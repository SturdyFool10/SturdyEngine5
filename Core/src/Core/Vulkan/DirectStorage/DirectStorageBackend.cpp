#include "DirectStorageBackend.hpp"


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


        struct DirectStorageState {
            ComPtr<IDStorageFactory> factory;
            ComPtr<IDStorageQueue> queue;
            ComPtr<IDStorageQueue1> queue1;
            bool init_attempted = false;
            bool available = false;
        };

        /// Returns the current or globally available state value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        DirectStorageState &state() {
            static DirectStorageState instance;
            return instance;
        }

        /// Returns the current or globally available state mutex value.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        std::mutex &state_mutex() {
            static std::mutex instance;
            return instance;
        }


        /// Finds or creates the initialized locked required by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


            desc.Device = nullptr;

            if (FAILED(s.factory->CreateQueue(&desc, IID_PPV_ARGS(&s.queue)))) {
                Foundation::log_warn("DirectStorage: CreateQueue failed; falling back to standard file I/O.");
                s.factory.Reset();
                return false;
            }


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

    /// Returns the current or globally available direct storage available value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool direct_storage_available() noexcept {
        std::lock_guard<std::mutex> lock(state_mutex());
        return ensure_initialized_locked();
    }

    /// Reads file direct storage from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::OperationFailed`.
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
        request.UncompressedSize = 0;
        request.CancellationTag = 0;
        request.Name = nullptr;

        ComPtr<IDStorageStatusArray> status_array;
        if (FAILED(s.factory->CreateStatusArray(1, nullptr, IID_PPV_ARGS(&status_array)))) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_direct_storage: CreateStatusArray failed."));
        }


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
