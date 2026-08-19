

#include <Core/D3D12/RHI/D3D12Device.hpp>

#pragma region Imports
#include <Core/D3D12/RHI/D3D12Convert.hpp>

#include <D3D12MemAlloc.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {


        constexpr const wchar_t *pipeline_library_cache_path = L".cache/d3d12_pipeline_library.bin";

        /// Reads pipeline library blob from the associated source.
        ///
        /// @return Returns the current read pipeline library blob value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<std::byte> read_pipeline_library_blob() {
            std::error_code ec;
            const std::filesystem::path path(pipeline_library_cache_path);
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size == 0) {
                return {};
            }
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return {};
            }
            vector<std::byte> data(static_cast<usize>(size));
            if (!file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()))) {
                return {};
            }
            return data;
        }


        /// Loads pipeline library.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] ComPtr<ID3D12PipelineLibrary1> load_pipeline_library(ID3D12Device *device) {
            ComPtr<ID3D12Device1> device1;
            if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) {
                return nullptr;
            }
            const vector<std::byte> blob = read_pipeline_library_blob();
            ComPtr<ID3D12PipelineLibrary1> library;
            if (!blob.empty() &&
                SUCCEEDED(device1->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&library)))) {
                return library;
            }
            if (FAILED(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)))) {
                return nullptr;
            }
            return library;
        }


        /// Saves pipeline library.
        ///
        /// @param library `library` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void save_pipeline_library(ID3D12PipelineLibrary1 *library) {
            if (library == nullptr) {
                return;
            }
            const usize size = library->GetSerializedSize();
            if (size == 0) {
                return;
            }
            vector<std::byte> buffer(size);
            if (FAILED(library->Serialize(buffer.data(), buffer.size()))) {
                return;
            }
            std::error_code ec;
            const std::filesystem::path path(pipeline_library_cache_path);
            std::filesystem::create_directories(path.parent_path(), ec);


            const std::filesystem::path temp_path = path.wstring() + L".tmp";
            {
                std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    return;
                }
                file.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                if (!file) {
                    return;
                }
            }
            std::filesystem::rename(temp_path, path, ec);
            if (ec) {
                std::filesystem::remove(temp_path, ec);
            }
        }

        /// Creates a queue from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param type Type value to inspect, select, or convert.
        /// @param label `label` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] rhi::RhiExpected<ComPtr<ID3D12CommandQueue>> create_queue(
            ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, const char *label) {
            const D3D12_COMMAND_QUEUE_DESC desc{
                .Type = type,
                .Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL,
                .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
                .NodeMask = 0,
            };
            ComPtr<ID3D12CommandQueue> queue;
            if (const HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)); FAILED(hr)) {
                return hresult_error(hr, "D3D12Device::initialize (CreateCommandQueue)");
            }
            set_debug_name(queue.Get(), label);
            return queue;
        }

    } // namespace

    /// Performs the d3 d12 device operation for `D3D12` using the supplied arguments.
    ///
    /// @param info Description of the resource or operation to perform.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12Device::D3D12Device(D3D12DeviceCreateInfo &&info)
        : factory_(std::move(info.factory)), adapter_(std::move(info.adapter)), device_(std::move(info.device)),
          adapter_info_(std::move(info.adapter_info)), limits_(info.limits),
          feature_report_(std::move(info.feature_report)), feature_properties_(info.feature_properties),
          queue_infos_(std::move(info.queue_infos)), enabled_extensions_(std::move(info.enabled_extensions)),
          enhanced_barriers_(info.enhanced_barriers), debug_layer_enabled_(info.debug_layer_enabled),
          allow_tearing_(info.allow_tearing), pipeline_library_supported_(info.pipeline_library_supported),
          gpu_upload_heap_supported_(info.gpu_upload_heap_supported) {
        enabled_features_ = feature_report_.enabled_features();
    }

    /// Destroys the `D3D12` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    D3D12Device::~D3D12Device() {


        wait_idle();

        if (d3d12ma_allocator_ != nullptr) {
            d3d12ma_allocator_->Release();
            d3d12ma_allocator_ = nullptr;
        }


        if (pipeline_library_supported_) {
            save_pipeline_library(pipeline_library_.lock()->Get());
        }

        for (auto *fence_holder : {&graphics_blocking_fence_, &compute_blocking_fence_, &copy_blocking_fence_}) {
            auto fence = fence_holder->lock();
            if (fence->event != nullptr) {
                CloseHandle(fence->event);
                fence->event = nullptr;
            }
        }

        fences_.for_each([](FenceRecord &record) {
            if (record.event != nullptr) {
                CloseHandle(record.event);
                record.event = nullptr;
            }
        });

        swapchains_.for_each([this](SwapchainRecord &record) { destroy_swapchain_textures(record); });
    }

    /// Initializes the `D3D12` for use.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::initialize() {
        ZoneScopedN("D3D12Device::initialize");
        if (device_ == nullptr) {
            return operation_failed("D3D12Device::initialize: no ID3D12Device was supplied.");
        }


        (void)device_.As(&device5_);
        (void)device_.As(&device8_);
        (void)device_.As(&device10_);


        if (pipeline_library_supported_) {
            if (ComPtr<ID3D12PipelineLibrary1> library = load_pipeline_library(device_.Get());
                library != nullptr) {
                *pipeline_library_.lock() = std::move(library);
            } else {
                pipeline_library_supported_ = false;
            }
        }

        if (auto queue = create_queue(device_.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, "Sturdy graphics queue")) {
            graphics_queue_ = std::move(*queue);
        } else {
            return std::unexpected(queue.error());
        }


        if (auto queue = create_queue(device_.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, "Sturdy compute queue")) {
            compute_queue_ = std::move(*queue);
        }
        if (auto queue = create_queue(device_.Get(), D3D12_COMMAND_LIST_TYPE_COPY, "Sturdy copy queue")) {
            copy_queue_ = std::move(*queue);
        }

        for (auto *fence_holder : {&graphics_blocking_fence_, &compute_blocking_fence_, &copy_blocking_fence_}) {
            auto fence = fence_holder->lock();
            if (const HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence->fence));
                FAILED(hr)) {
                return hresult_error(hr, "D3D12Device::initialize (CreateFence)");
            }
            fence->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (fence->event == nullptr) {
                return operation_failed("D3D12Device::initialize: CreateEventW failed for a blocking fence.");
            }
        }

        // D3D12MA suballocates buffers/textures onto shared heaps via CreatePlacedResource instead of
        // every resource paying for its own dedicated CreateCommittedResource heap (the D3D12
        // counterpart to Sturdy::VMA on the Vulkan side; see VulkanBackendDevice.cpp's initializeVMA).
        // Left null on failure -- create_buffer()/create_texture() null-check it and fall back to
        // CreateCommittedResource, so a failed allocator create degrades performance, not correctness.
        {
            const D3D12MA::ALLOCATOR_DESC allocator_desc{
                .Flags = D3D12MA::ALLOCATOR_FLAG_NONE,
                .pDevice = device_.Get(),
                .PreferredBlockSize = 0,
                .pAllocationCallbacks = nullptr,
                .pAdapter = adapter_.Get(),
            };
            if (const HRESULT hr = D3D12MA::CreateAllocator(&allocator_desc, &d3d12ma_allocator_); FAILED(hr)) {
                d3d12ma_allocator_ = nullptr;
                Foundation::log_warn(
                    "D3D12: D3D12MA::CreateAllocator failed (hr=0x{:08X}); every buffer/texture will fall "
                    "back to its own dedicated CreateCommittedResource allocation.",
                    static_cast<u32>(hr));
            }
        }

        struct AllocatorSetup {
            CpuDescriptorAllocator *allocator;
            D3D12_DESCRIPTOR_HEAP_TYPE type;
            u32 capacity;
        };
        for (const AllocatorSetup &setup : {
                 AllocatorSetup{&cpu_resource_descriptors_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                default_cpu_descriptor_chunk_capacity},
                 AllocatorSetup{&cpu_sampler_descriptors_, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE},
                 AllocatorSetup{&cpu_rtv_descriptors_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, default_cpu_rtv_chunk_capacity},
                 AllocatorSetup{&cpu_dsv_descriptors_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, default_cpu_dsv_chunk_capacity},
             }) {
            if (auto initialized = setup.allocator->initialize(device_.Get(), setup.type, setup.capacity);
                !initialized) {
                return initialized;
            }
        }

        if (auto initialized = bundle_resource_descriptors_.initialize(
                device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, default_persistent_bundle_resource_descriptors);
            !initialized) {
            return initialized;
        }
        if (auto initialized = bundle_sampler_descriptors_.initialize(
                device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, default_persistent_bundle_sampler_descriptors);
            !initialized) {
            return initialized;
        }


        UINT64 timestamp_frequency = 0;
        if (SUCCEEDED(graphics_queue_->GetTimestampFrequency(&timestamp_frequency)) && timestamp_frequency != 0) {
            limits_.timestamp_period_ns = 1'000'000'000.0f / static_cast<f32>(timestamp_frequency);
        } else {
            limits_.timestamp_period_ns = 0.0f;
            limits_.timestamp_valid_bits = 0;
        }

        return {};
    }

    // ─── Introspection ───────────────────────────────────────────────────────────

    rhi::BackendType D3D12Device::backend_type() const noexcept { return rhi::BackendType::D3D12; }

    const rhi::AdapterInfo &D3D12Device::adapter_info() const noexcept { return adapter_info_; }

    const rhi::DeviceLimits &D3D12Device::limits() const noexcept { return limits_; }

    const rhi::FeatureNegotiationReport &D3D12Device::feature_negotiation_report() const noexcept {
        return feature_report_;
    }

    const rhi::FeatureSet &D3D12Device::enabled_features() const noexcept { return enabled_features_; }

    const rhi::FeatureProperties &D3D12Device::feature_properties() const noexcept { return feature_properties_; }

    span<const rhi::QueueInfo> D3D12Device::queue_infos() const noexcept { return queue_infos_; }

    span<const rhi::ExtensionId> D3D12Device::enabled_extensions() const noexcept { return enabled_extensions_; }

    rhi::RhiDeviceExtension *D3D12Device::extension_interface(rhi::ExtensionId) noexcept {
        // No D3D12-specific extension interfaces are published yet. Returning nullptr is the documented
        // safe-fail for an extension that is not enabled, and is correct for every id while the
        // supported-extension list is empty.
        return nullptr;
    }

    // ─── Queues ──────────────────────────────────────────────────────────────────

    ID3D12CommandQueue *D3D12Device::queue_for_lane(rhi::QueueLane lane) const noexcept {
        switch (lane.queue) {
            case rhi::QueueClass::Compute:
                return compute_queue_ != nullptr ? compute_queue_.Get() : graphics_queue_.Get();
            case rhi::QueueClass::Transfer:
                return copy_queue_ != nullptr ? copy_queue_.Get() : graphics_queue_.Get();
            case rhi::QueueClass::Graphics:
            case rhi::QueueClass::Sparse:
                return graphics_queue_.Get();
            case rhi::QueueClass::VideoDecode:
            case rhi::QueueClass::VideoEncode:
                // No video queues are created (no video feature is reported supported), so a lane
                // naming one is a caller error validate_queue_lane() rejects before this is reached.
                return nullptr;
        }
        return graphics_queue_.Get();
    }

    rhi::RhiResult D3D12Device::validate_queue_lane(rhi::QueueLane lane, const char *operation) const {
        const auto it = std::ranges::find_if(queue_infos_,
                                             [&](const rhi::QueueInfo &info) { return info.queue == lane.queue; });
        if (it == queue_infos_.end()) {
            return invalid_argument(std::string(operation) + ": this device exposes no queue of the requested class.");
        }
        if (lane.index >= it->lane_count) {
            return invalid_argument(std::string(operation) + ": queue lane index " + std::to_string(lane.index) +
                                    " is out of range for a class exposing " + std::to_string(it->lane_count) +
                                    " lane(s).");
        }
        return {};
    }

    rhi::RhiResult D3D12Device::execute_and_wait(ID3D12GraphicsCommandList *list, rhi::QueueClass queue_class) {
        ZoneScopedN("D3D12Device::execute_and_wait");
        ID3D12CommandQueue *queue = queue_for_lane(rhi::QueueLane{queue_class, 0});
        if (queue == nullptr || list == nullptr) {
            return operation_failed("D3D12Device::execute_and_wait: no queue or command list.");
        }

        auto *fence_holder = &graphics_blocking_fence_;
        if (queue == compute_queue_.Get()) {
            fence_holder = &compute_blocking_fence_;
        } else if (queue == copy_queue_.Get()) {
            fence_holder = &copy_blocking_fence_;
        }

        ID3D12CommandList *lists[] = {list};
        auto fence = fence_holder->lock();
        queue->ExecuteCommandLists(1, lists);
        const u64 value = ++fence->next_value;
        if (const HRESULT hr = queue->Signal(fence->fence.Get(), value); FAILED(hr)) {
            return hresult_error(hr, "D3D12Device::execute_and_wait (Signal)");
        }
        if (fence->fence->GetCompletedValue() >= value) {
            return {};
        }
        if (const HRESULT hr = fence->fence->SetEventOnCompletion(value, fence->event); FAILED(hr)) {
            return hresult_error(hr, "D3D12Device::execute_and_wait (SetEventOnCompletion)");
        }
        WaitForSingleObject(fence->event, INFINITE);
        return {};
    }

    void D3D12Device::wait_idle() noexcept {
        ZoneScopedN("D3D12Device::wait_idle");
        struct QueueFence {
            ID3D12CommandQueue *queue;
            Async::Mutex<BlockingFence> *fence;
        };
        for (const QueueFence &entry : {QueueFence{graphics_queue_.Get(), &graphics_blocking_fence_},
                                        QueueFence{compute_queue_.Get(), &compute_blocking_fence_},
                                        QueueFence{copy_queue_.Get(), &copy_blocking_fence_}}) {
            if (entry.queue == nullptr) {
                continue;
            }
            auto fence = entry.fence->lock();
            if (fence->fence == nullptr || fence->event == nullptr) {
                continue;
            }
            const u64 value = ++fence->next_value;
            if (FAILED(entry.queue->Signal(fence->fence.Get(), value))) {
                continue;
            }
            if (fence->fence->GetCompletedValue() >= value) {
                continue;
            }
            if (SUCCEEDED(fence->fence->SetEventOnCompletion(value, fence->event))) {
                WaitForSingleObject(fence->event, INFINITE);
            }
        }
    }

    // ─── Command-list recycling ──────────────────────────────────────────────────

    rhi::RhiExpected<ShaderVisibleDescriptorHeap> D3D12Device::create_shader_visible_heap(
        D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity) {
        ShaderVisibleDescriptorHeap heap;
        if (auto initialized = heap.initialize(device_.Get(), type, capacity); !initialized) {
            return std::unexpected(initialized.error());
        }
        return heap;
    }

    std::optional<CommandBufferRecord> D3D12Device::checkout_command_buffer(D3D12_COMMAND_LIST_TYPE list_type) noexcept {
        auto free_list = command_buffer_free_list_.lock();
        auto it = free_list->find(static_cast<u32>(list_type));
        if (it == free_list->end() || it->second.empty()) {
            return std::nullopt;
        }
        CommandBufferRecord record = std::move(it->second.back());
        it->second.pop_back();
        return record;
    }

    void D3D12Device::return_command_buffer(CommandBufferRecord &&record) noexcept {
        if (record.allocator == nullptr || record.list == nullptr) {
            return;
        }
        // Resetting the allocator recycles every command it backed, which is only legal once the GPU
        // is done with them — the exact condition the RHI already places on destroy_command_buffer().
        // A failed reset means the allocator is in an indeterminate state, so the record is dropped
        // rather than handed to the next caller.
        if (FAILED(record.allocator->Reset())) {
            return;
        }
        record.resource_heap.reset();
        record.sampler_heap.reset();
        record.retired_heaps.clear();
        record.transient_uploads.clear();
        for (DescriptorRange descriptor : record.transient_resource_descriptors) {
            cpu_resource_descriptors_.release(descriptor);
        }
        for (DescriptorRange descriptor : record.transient_rtv_descriptors) {
            cpu_rtv_descriptors_.release(descriptor);
        }
        for (DescriptorRange descriptor : record.transient_dsv_descriptors) {
            cpu_dsv_descriptors_.release(descriptor);
        }
        record.transient_resource_descriptors.clear();
        record.transient_rtv_descriptors.clear();
        record.transient_dsv_descriptors.clear();
        const u32 key = static_cast<u32>(record.list_type);
        command_buffer_free_list_.lock()->operator[](key).push_back(std::move(record));
    }

    rhi::RhiExpected<CommandBufferRecord> D3D12Device::acquire_command_buffer(rhi::QueueLane lane) {
        ZoneScopedN("D3D12Device::acquire_command_buffer");
        const D3D12_COMMAND_LIST_TYPE list_type = to_d3d12(lane.queue);

        if (auto recycled = checkout_command_buffer(list_type)) {
            // A record may have become unusable after it was returned (for example, a failed frame can
            // abandon a list while the runtime is still unwinding it). Never let a cache miss turn into
            // a renderer failure: dropping this COM-owned record is safe, and a fresh allocator/list
            // pair below is independent of it.
            if (SUCCEEDED(recycled->list->Reset(recycled->allocator.Get(), nullptr))) {
                recycled->queue = lane;
                return std::move(*recycled);
            }
        }

        CommandBufferRecord record{};
        record.queue = lane;
        record.list_type = list_type;
        if (const HRESULT hr = device_->CreateCommandAllocator(list_type, IID_PPV_ARGS(&record.allocator));
            FAILED(hr)) {
            return hresult_error(hr, "D3D12Device::acquire_command_buffer (CreateCommandAllocator)");
        }
        if (const HRESULT hr = device_->CreateCommandList(0, list_type, record.allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&record.list));
            FAILED(hr)) {
            return hresult_error(hr, "D3D12Device::acquire_command_buffer (CreateCommandList)");
        }

        // Copy queues never bind descriptor heaps (there is nothing on a copy engine that reads a
        // descriptor), so allocating shader-visible heaps for one would be pure waste — and a sampler
        // heap in particular is a scarce, hardware-capped resource.
        if (list_type != D3D12_COMMAND_LIST_TYPE_COPY) {
            if (auto heap = create_shader_visible_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                       default_shader_visible_resource_descriptors)) {
                record.resource_heap = std::move(*heap);
            } else {
                return std::unexpected(heap.error());
            }
            if (auto heap = create_shader_visible_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                       default_shader_visible_sampler_descriptors)) {
                record.sampler_heap = std::move(*heap);
            } else {
                return std::unexpected(heap.error());
            }
        }
        return record;
    }

    void D3D12Device::destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_command_buffer");
        if (auto record = command_buffers_.extract(handle)) {
            return_command_buffer(std::move(*record));
        }
    }

    void D3D12Device::destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept {
        if (auto record = render_bundles_.extract(handle)) {
            for (const DescriptorRange &range : record->resource_table_ranges) {
                bundle_resource_descriptors_.release(range);
            }
            for (const DescriptorRange &range : record->sampler_table_ranges) {
                bundle_sampler_descriptors_.release(range);
            }
        }
    }

} // namespace SFT::D3D12
