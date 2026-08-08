#include "DirectStorageBackend.hpp"

// Must be defined before any Windows header pulls in <windef.h>'s min/max macros -- this file
// compares against std::numeric_limits<UINT32>::max(), which those macros break.
#if !defined(NOMINMAX)
    #define NOMINMAX
#endif

#include <Foundation/src/Foundation.hpp>

#include <wrl/client.h>

#include <dstorage.h>

#include <limits>
#include <mutex>

#include <tracy/Tracy.hpp>

using Microsoft::WRL::ComPtr;

namespace SFT::Core {

    namespace {

        // Process-wide: one IDStorageFactory and one memory-destination IDStorageQueue, lazily
        // created on first use and reused for every read_file_direct_storage() call thereafter --
        // DirectStorage's own guidance is to keep a small number of long-lived queues rather than
        // create one per request. `queue1` is the same object as `queue`, just also holding the
        // IDStorageQueue1 interface (added in DirectStorage 1.1) that EnqueueSetEvent lives on, so
        // completion can be waited on with WaitForSingleObject instead of a busy-poll loop.
        struct DirectStorageState {
            ComPtr<IDStorageFactory> factory;
            ComPtr<IDStorageQueue> queue;
            ComPtr<IDStorageQueue1> queue1;
            bool init_attempted = false;
            bool available = false;
        };

        DirectStorageState &state() {
            static DirectStorageState instance;
            return instance;
        }

        std::mutex &state_mutex() {
            static std::mutex instance;
            return instance;
        }

        // Callers must hold state_mutex().
        [[nodiscard]] bool ensure_initialized_locked() {
            DirectStorageState &s = state();
            if (s.init_attempted) {
                return s.available;
            }
            s.init_attempted = true;

            if (FAILED(DStorageGetFactory(IID_PPV_ARGS(&s.factory)))) {
                Foundation::log_warn("DirectStorage: DStorageGetFactory failed; falling back to standard file I/O.");
                return false;
            }

            DSTORAGE_QUEUE_DESC desc{};
            desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
            desc.Capacity = DSTORAGE_MAX_QUEUE_CAPACITY;
            desc.Priority = DSTORAGE_PRIORITY_NORMAL;
            desc.Name = "SturdyEngine texture streaming";
            // Memory-only destinations for this backend -- see DirectStorageBackend.hpp's own doc
            // comment for why this deliberately never becomes a D3D12 device.
            desc.Device = nullptr;

            if (FAILED(s.factory->CreateQueue(&desc, IID_PPV_ARGS(&s.queue)))) {
                Foundation::log_warn("DirectStorage: CreateQueue failed; falling back to standard file I/O.");
                s.factory.Reset();
                return false;
            }

            // IDStorageQueue1 (EnqueueSetEvent) is expected to be present on the 1.3.0 SDK this
            // engine fetches (see STURDY_DIRECTSTORAGE_VERSION) -- if a caller substitutes an older
            // runtime DLL at STURDY_PREFER_SYSTEM_DEPENDENCIES time, fall back to CPU I/O rather than
            // busy-poll IsComplete(), rather than special-casing a second wait strategy here.
            if (FAILED(s.queue.As(&s.queue1))) {
                Foundation::log_warn("DirectStorage: IDStorageQueue1 unavailable; falling back to standard file I/O.");
                s.queue.Reset();
                s.factory.Reset();
                return false;
            }

            s.available = true;
            return true;
        }

    } // namespace

    bool direct_storage_available() noexcept {
        std::lock_guard<std::mutex> lock(state_mutex());
        return ensure_initialized_locked();
    }

    RHI::RhiExpected<std::vector<std::byte>> read_file_direct_storage(const std::filesystem::path &path) {
        ZoneScopedN("Core::read_file_direct_storage");

        std::lock_guard<std::mutex> lock(state_mutex());
        if (!ensure_initialized_locked()) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::Unsupported,
                                              "read_file_direct_storage: DirectStorage is unavailable on this system."));
        }
        DirectStorageState &s = state();

        ComPtr<IDStorageFile> file;
        if (FAILED(s.factory->OpenFile(path.c_str(), IID_PPV_ARGS(&file)))) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: could not open '" + path.string() + "'."));
        }

        BY_HANDLE_FILE_INFORMATION info{};
        if (FAILED(file->GetFileInformation(&info))) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: could not stat '" + path.string() + "'."));
        }
        const u64 file_size = (static_cast<u64>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        // DSTORAGE_SOURCE_FILE::Size and DSTORAGE_DESTINATION_MEMORY::Size are both UINT32 --
        // DirectStorage requests are capped at 4GiB each; no texture asset this pipeline handles
        // comes close, so this is a hard reject rather than a chunking loop.
        if (file_size == 0 || file_size > std::numeric_limits<UINT32>::max()) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: '" + path.string() + "' is empty or exceeds the 4GiB per-request limit."));
        }

        std::vector<std::byte> bytes(static_cast<usize>(file_size));

        DSTORAGE_REQUEST request{};
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Source.File.Source = file.Get();
        request.Source.File.Offset = 0;
        request.Source.File.Size = static_cast<UINT32>(file_size);
        request.Destination.Memory.Buffer = bytes.data();
        request.Destination.Memory.Size = static_cast<UINT32>(file_size);
        request.UncompressedSize = 0; // uncompressed request -- see DSTORAGE_REQUEST::UncompressedSize's own doc comment.
        request.CancellationTag = 0;
        request.Name = nullptr;

        ComPtr<IDStorageStatusArray> status_array;
        if (FAILED(s.factory->CreateStatusArray(1, nullptr, IID_PPV_ARGS(&status_array)))) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: CreateStatusArray failed."));
        }

        // Manual-reset so a spurious extra wakeup (there won't be one -- only this one Submit()
        // targets this event -- but cheap to be defensive) can't be missed between the wait below
        // returning and this handle being closed.
        HANDLE completion_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (completion_event == nullptr) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: CreateEventW failed."));
        }

        s.queue->EnqueueRequest(&request);
        s.queue->EnqueueStatus(status_array.Get(), 0);
        s.queue1->EnqueueSetEvent(completion_event);
        s.queue->Submit();

        WaitForSingleObject(completion_event, INFINITE);
        CloseHandle(completion_event);

        const HRESULT result = status_array->GetHResult(0);
        if (FAILED(result)) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: request for '" + path.string() + "' failed."));
        }

        file->Close();
        return bytes;
    }

} // namespace SFT::Core
