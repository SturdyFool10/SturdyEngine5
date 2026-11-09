#include <Core/Slang/BundledDxc.hpp>

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <slang.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace SFT::Core::Slang {

    bool dxil_container_is_signed(std::span<const std::byte> dxil) noexcept {
        // "DXBC", then a 16-byte hash that the validator (dxil.dll) fills in; DXC leaves it zero when it could not validate.
        if (dxil.size() < 20) {
            return false;
        }
        constexpr std::byte magic[4] = {std::byte{'D'}, std::byte{'X'}, std::byte{'B'}, std::byte{'C'}};
        if (!std::equal(std::begin(magic), std::end(magic), dxil.begin())) {
            return false;
        }
        return std::any_of(dxil.begin() + 4, dxil.begin() + 20, [](std::byte b) { return b != std::byte{0}; });
    }

    std::string DxcStatus::describe() const {
        std::string text = summary;
        for (const std::string &detail : details) {
            text += "\n  - " + detail;
        }
        return text;
    }

#if defined(_WIN32)

    namespace {

        std::mutex &status_mutex() {
            static auto *mutex = new std::mutex();
            return *mutex;
        }

        DxcStatus &shared_status() {
            static auto *status = new DxcStatus();
            return *status;
        }

        void log_status(const DxcStatus &status) {
            switch (status.health) {
                case DxcHealth::Ok:
                    Foundation::log_info("DXC: {}", status.summary);
                    break;
                case DxcHealth::Degraded:
                    Foundation::log_warn("DXC (degraded): {}", status.describe());
                    break;
                case DxcHealth::Failed:
                    Foundation::log_error("DXC (failed): {}", status.describe());
                    break;
                case DxcHealth::NotApplicable:
                    break;
            }
        }

    } // namespace


    namespace {

        namespace fs = std::filesystem;

        [[nodiscard]] std::string utf8(const fs::path &path) {
            const std::u8string text = path.u8string();
            return std::string{reinterpret_cast<const char *>(text.data()), text.size()};
        }

        /// "error 5 (Access is denied.)": the number alone is not something a user can act on.
        [[nodiscard]] std::string win32_error_text(DWORD code) {
            wchar_t *buffer = nullptr;
            const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                                nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
            std::string text = std::format("error {}", code);
            if (length != 0 && buffer != nullptr) {
                std::wstring message{buffer, length};
                LocalFree(buffer);
                while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' ')) {
                    message.pop_back();
                }
                const int size = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
                std::string narrow(static_cast<size_t>(std::max(size, 0)), '\0');
                if (size > 0) {
                    WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), narrow.data(), size, nullptr, nullptr);
                }
                text += " (" + narrow + ")";
            }
            return text;
        }

        [[nodiscard]] fs::path program_directory() {
            std::wstring buffer(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length >= buffer.size()) {
                return {};
            }
            buffer.resize(length);
            return fs::path{buffer}.parent_path();
        }

        [[nodiscard]] bool file_matches(const fs::path &path, std::span<const std::byte> bytes) {
            std::error_code ec;
            if (fs::file_size(path, ec) != bytes.size() || ec) {
                return false;
            }
            std::ifstream in{path, std::ios::binary};
            if (!in) {
                return false;
            }
            std::vector<char> existing(bytes.size());
            in.read(existing.data(), static_cast<std::streamsize>(existing.size()));
            return in && std::equal(existing.begin(), existing.end(), reinterpret_cast<const char *>(bytes.data()));
        }

        /// Writes `bytes` to `path` unless it already holds exactly them. Written beside and renamed over, so a process starting at the same
        /// moment, or one killed half way through, never sees (or leaves behind) a truncated DLL. On failure `why` says which step and why.
        [[nodiscard]] bool write_verified(const fs::path &path, std::span<const std::byte> bytes, std::string &why) {
            if (file_matches(path, bytes)) {
                return true;
            }
            std::error_code ec;
            fs::path temporary = path;
            temporary += ".tmp" + std::to_string(GetCurrentProcessId());
            {
                std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
                if (!out) {
                    why = std::format("could not create '{}' (is the folder writable?)", utf8(temporary));
                    return false;
                }
                out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                out.flush();
                if (!out) {
                    why = std::format("could not write {} bytes to '{}' (disk full?)", bytes.size(), utf8(temporary));
                    fs::remove(temporary, ec);
                    return false;
                }
            }
            fs::rename(temporary, path, ec);
            if (ec) {
                // Another process won the race and has the file open (Windows refuses to replace a loaded DLL): fine if it is the right one.
                fs::remove(temporary, ec);
                if (file_matches(path, bytes)) {
                    return true;
                }
                why = std::format("could not move the written file into place as '{}', and what is there is not the embedded copy", utf8(path));
                return false;
            }
            return true;
        }

        [[nodiscard]] DxcStatus setup_once() {
            DxcStatus status;
            std::error_code ec;
            const fs::path beside_program = program_directory();
            const bool has_compiler_beside = !beside_program.empty() && fs::exists(beside_program / "dxcompiler.dll", ec);
            const bool has_signer_beside = !beside_program.empty() && fs::exists(beside_program / "dxil.dll", ec);
            if (has_compiler_beside && has_signer_beside) {
                status.health = DxcHealth::Ok;
                status.source = DxcSource::BesideProgram;
                status.compiler_directory = utf8(beside_program);
                status.compiler_path = utf8(beside_program / "dxcompiler.dll");
                status.signer_path = utf8(beside_program / "dxil.dll");
                status.summary = "using the dxcompiler.dll and dxil.dll beside the program";
                return status;
            }
            std::vector<std::string> notes;
            if (has_compiler_beside != has_signer_beside) {
                notes.push_back(std::format("'{}' exists beside the program but '{}' does not, so the pair is ignored (DXIL it makes would be unsigned)",
                                            has_compiler_beside ? "dxcompiler.dll" : "dxil.dll", has_compiler_beside ? "dxil.dll" : "dxcompiler.dll"));
            }

            const BundledDxc bundle = bundled_dxc();
            if (!bundle.available()) {
                // Nothing embedded: Slang will look for a DXC on the machine, which may or may not be there; the DXIL probe finds out.
                status.health = DxcHealth::Degraded;
                status.source = DxcSource::SystemSearch;
                status.summary = std::format("no DXC is embedded in this build ({}); relying on one Slang can find on this machine",
                                             bundle.unavailable_reason.empty() ? std::string_view{"reason not recorded"} : bundle.unavailable_reason);
                status.details = std::move(notes);
                return status;
            }
            status.bundle_version = std::string{bundle.version};
            status.source = DxcSource::Bundled;

            const fs::path root =
                Foundation::cache_subdirectory("dxc", fs::temp_directory_path(ec) / "sturdy-dxc") / std::string{bundle.version};
            std::error_code create_error;
            fs::create_directories(root, create_error);
            if (create_error) {
                status.health = DxcHealth::Failed;
                status.source = DxcSource::None;
                status.summary = std::format("could not create the folder for the embedded DXC, '{}': {}", utf8(root), create_error.message());
                status.details = std::move(notes);
                return status;
            }
            const fs::path compiler = root / "dxcompiler.dll";
            const fs::path signer = root / "dxil.dll";
            status.compiler_directory = utf8(root);
            status.compiler_path = utf8(compiler);
            status.signer_path = utf8(signer);

            std::string why;
            if (!write_verified(signer, bundle.dxil, why)) {
                status.health = DxcHealth::Failed;
                status.source = DxcSource::None;
                status.summary = "could not write the embedded dxil.dll: " + why;
                status.details = std::move(notes);
                return status;
            }
            if (!write_verified(compiler, bundle.dxcompiler, why)) {
                status.health = DxcHealth::Failed;
                status.source = DxcSource::None;
                status.summary = "could not write the embedded dxcompiler.dll: " + why;
                status.details = std::move(notes);
                return status;
            }
            // dxcompiler looks dxil.dll up by name when it validates; a module that is already loaded under that name is what that lookup
            // returns, so loading ours first by full path makes it the one used no matter what else is on the search path.
            if (LoadLibraryW(signer.c_str()) == nullptr) {
                status.health = DxcHealth::Failed;
                status.source = DxcSource::None;
                status.summary = std::format("could not load '{}': {} (a security product may have blocked it, or the file is damaged); "
                                             "DXIL would be unsigned and rejected by D3D12",
                                             status.signer_path, win32_error_text(GetLastError()));
                status.details = std::move(notes);
                return status;
            }
            status.health = DxcHealth::Ok;
            status.summary = std::format("using the embedded DXC {} from '{}'", bundle.version, utf8(root));
            status.details = std::move(notes);
            if (!status.details.empty()) {
                status.health = DxcHealth::Degraded;
                status.summary += " (worked around a problem, see details)";
            }
            return status;
        }

    } // namespace

    DxcStatus bundled_dxc_status() {
        static std::once_flag once;
        std::call_once(once, [] {
            DxcStatus status = setup_once();
            log_status(status);
            const std::lock_guard<std::mutex> lock{status_mutex()};
            shared_status() = std::move(status);
        });
        const std::lock_guard<std::mutex> lock{status_mutex()};
        return shared_status();
    }

    DxcStatus prepare_bundled_dxc(slang::IGlobalSession *session) {
        DxcStatus status = bundled_dxc_status();
        if (session == nullptr || status.compiler_directory.empty() || status.source == DxcSource::SystemSearch || status.source == DxcSource::None) {
            return status;
        }
        // Setting the path reports nothing; whether Slang can really load a DXC from there is asked afterwards.
        session->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, status.compiler_directory.c_str());
        const SlangResult result = session->checkPassThroughSupport(SLANG_PASS_THROUGH_DXC);
        if (SLANG_FAILED(result)) {
            const std::string detail =
                std::format("Slang could not load the DXC in '{}' (result 0x{:08x}); DXIL generation will fail unless it finds another DXC on this machine",
                            status.compiler_directory, static_cast<unsigned>(result));
            const std::lock_guard<std::mutex> lock{status_mutex()};
            DxcStatus &shared = shared_status();
            if (shared.health != DxcHealth::Degraded || std::find(shared.details.begin(), shared.details.end(), detail) == shared.details.end()) {
                shared.health = DxcHealth::Degraded;
                shared.details.push_back(detail);
                shared.summary += " (Slang could not load it)";
                Foundation::log_warn("DXC (degraded): {}", detail);
            }
            return shared;
        }
        return status;
    }

#else

    DxcStatus bundled_dxc_status() {
        DxcStatus status;
        status.health = DxcHealth::NotApplicable;
        status.summary = "DXC is only used on Windows";
        return status;
    }

    DxcStatus prepare_bundled_dxc(slang::IGlobalSession *) { return bundled_dxc_status(); }

#endif

} // namespace SFT::Core::Slang
