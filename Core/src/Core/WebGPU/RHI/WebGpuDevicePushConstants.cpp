/// Push-constant emulation for the WebGPU backend.
///
/// WebGPU has no push constants and no extension this backend targets that adds them. What it does
/// have is a uniform buffer bound with a dynamic offset, which gives the same shape: a small block
/// of constants, rebound per draw, with no descriptor churn. The shader library's
/// `SFT_EMULATE_PUSH_CONSTANTS` path declares exactly that -- the same struct, bound at
/// `[[vk::binding(0, 3)]]` -- so a shader compiled for WGSL reads its constants from here while the
/// SPIR-V and DXIL builds of the same file keep using real push constants.
///
/// The ring below is what makes rebinding per draw safe. A slice is never reused while a submission
/// that reads it is still outstanding: WebGPU's queue is in-order, so recording the cursor at each
/// submit and reclaiming when that submit completes is enough to know exactly which bytes are free.
/// When nothing can be reclaimed the ring grows instead of stalling, and the old buffer is retired
/// rather than freed, because commands already recorded still name it.

#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace SFT::Core::WebGpu {

    namespace {

        /// Bytes the ring starts at. Sized so an ordinary frame never grows it: at a 256-byte slice
        /// stride this is 256 push-constant sets, and the whole UI plus deferred pass issues far
        /// fewer than that.
        constexpr u64 initial_ring_capacity = 1024u * 1024u;

        /// Rounds `value` up to a multiple of `alignment`.
        ///
        /// @param value Value consumed by the operation.
        /// @param alignment Alignment to round up to; must be a power of two.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr u64 align_up(u64 value, u64 alignment) noexcept {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

    } // namespace

    /// Returns an empty bind group layout, for padding a pipeline layout's unused lower groups up to
    /// the reserved index.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUBindGroupLayout WebGpuDevice::empty_bind_group_layout() noexcept {
        auto state = push_constants_.lock();
        (void)state;
        if (empty_bind_group_layout_ != nullptr) {
            return empty_bind_group_layout_;
        }
        WGPUBindGroupLayoutDescriptor desc{};
        desc.label = wgpu_string("sturdy empty group");
        desc.entryCount = 0;
        desc.entries = nullptr;
        empty_bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &desc);
        return empty_bind_group_layout_;
    }

    /// Returns the bind group layout the reserved push-constant group is declared with.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUBindGroupLayout WebGpuDevice::push_constant_bind_group_layout() noexcept {
        auto state = push_constants_.lock();
        (void)state;
        return push_constant_bind_group_layout_locked();
    }

    /// Creates the reserved group's layout on first use. The ring lock must already be held.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    WGPUBindGroupLayout WebGpuDevice::push_constant_bind_group_layout_locked() noexcept {
        if (push_constant_bind_group_layout_ != nullptr) {
            return push_constant_bind_group_layout_;
        }

        WGPUBindGroupLayoutEntry entry{};
        entry.binding = 0;
        // Every stage, because a single block is routinely read by both halves of a pipeline (the
        // draw constants in gbuffer_geometry.slang, for one) and WebGPU has no way to say "whichever
        // stages the shader happens to use".
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        entry.buffer.type = WGPUBufferBindingType_Uniform;
        entry.buffer.hasDynamicOffset = 1;
        // The binding covers one block, not the whole ring: the dynamic offset selects which slice,
        // and a shader must not be able to read past its own.
        entry.buffer.minBindingSize = push_constant_block_size;

        WGPUBindGroupLayoutDescriptor desc{};
        desc.label = wgpu_string("sturdy push constants");
        desc.entryCount = 1;
        desc.entries = &entry;
        push_constant_bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &desc);
        return push_constant_bind_group_layout_;
    }

    /// Copies `data` into a fresh ring slice and returns what to bind for it.
    ///
    /// @param data The whole push-constant block, as the encoder's shadow copy holds it.
    ///
    /// @return Returns the group and dynamic offset to bind, or `std::nullopt` when no slice could
    ///         be obtained.
    /// @note This function does not throw exceptions.
    std::optional<WebGpuDevice::PushConstantBinding> WebGpuDevice::allocate_push_constant_slice(
        span<const std::byte> data) noexcept {
        auto state = push_constants_.lock();

        WGPUBindGroupLayout layout = push_constant_bind_group_layout_locked();
        if (layout == nullptr) {
            return std::nullopt;
        }

        if (state->stride == 0) {
            // A dynamic offset must be a multiple of the device's alignment, so that -- not the
            // block size -- is the real slice stride.
            const u64 alignment = std::max<u64>(limits_.min_uniform_buffer_offset_alignment, 1u);
            state->stride = static_cast<u32>(align_up(push_constant_block_size, alignment));
        }

        if (state->ring == nullptr) {
            if (!grow_push_constant_ring(*state, layout, initial_ring_capacity)) {
                return std::nullopt;
            }
        } else if (state->cursor + state->stride > state->capacity) {
            // Out of room: start a fresh ring instead of wrapping this one. A slice is read when the command buffer that
            // named it *runs*, and a frame's command buffers are recorded on several threads and submitted separately, so
            // there is no moment at which "nothing outstanding" is knowable while recording; wrapping would overwrite
            // slices that recorded-but-unsubmitted draws still read, which scrambled their constants (broken UI text,
            // wrong colours and sizes). The old ring is only released, never destroyed: Dawn keeps it alive for as long
            // as a recorded command buffer or in-flight submission names it.
            if (!grow_push_constant_ring(*state, layout, state->capacity)) {
                return std::nullopt;
            }
        }

        const u64 offset = state->cursor;
        state->cursor += state->stride;

        // The binding is a whole block wide, so the whole block is written even when the caller set
        // fewer bytes -- the encoder's shadow copy supplies the rest, which is what keeps
        // push-constant semantics (a value persists until something overwrites it).
        if (state->shadow.size() != state->capacity) {
            state->shadow.resize(static_cast<usize>(state->capacity));
        }
        std::byte *slice = state->shadow.data() + offset;
        const usize copied = std::min<usize>(data.size(), push_constant_block_size);
        std::memcpy(slice, data.data(), copied);
        std::memset(slice + copied, 0, push_constant_block_size - copied);
        state->dirty_begin = std::min(state->dirty_begin, offset);
        state->dirty_end = std::max(state->dirty_end, offset + push_constant_block_size);
        return PushConstantBinding{.group = state->bind_group,
                                   .dynamic_offset = static_cast<u32>(offset)};
    }

    void WebGpuDevice::flush_push_constants_locked(PushConstantState &state) noexcept {
        if (state.ring != nullptr && state.dirty_begin < state.dirty_end && state.dirty_end <= state.shadow.size()) {
            wgpuQueueWriteBuffer(queue_, state.ring, state.dirty_begin, state.shadow.data() + state.dirty_begin,
                                 static_cast<usize>(state.dirty_end - state.dirty_begin));
        }
        state.dirty_begin = ~u64{0};
        state.dirty_end = 0;
    }

    void WebGpuDevice::flush_push_constants() noexcept {
        auto state = push_constants_.lock();
        flush_push_constants_locked(*state);
    }

    /// Replaces the ring with one of at least `required` bytes.
    ///
    /// @param state The locked ring state.
    /// @param layout Bind group layout the new group is created against.
    /// @param required Smallest capacity the new ring must have.
    ///
    /// @return Returns `true` when a new ring and its bind group were created.
    /// @note This function does not throw exceptions.
    bool WebGpuDevice::grow_push_constant_ring(PushConstantState &state, WGPUBindGroupLayout layout,
                                               u64 required) noexcept {
        const u64 capacity = std::max(align_up(required, initial_ring_capacity), initial_ring_capacity);

        WGPUBufferDescriptor buffer_desc{};
        buffer_desc.label = wgpu_string("sturdy push constant ring");
        buffer_desc.size = capacity;
        buffer_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        WGPUBuffer buffer = wgpuDeviceCreateBuffer(device_, &buffer_desc);
        if (buffer == nullptr) {
            Foundation::log_error(
                "WebGPU backend: could not grow the push-constant ring to {} bytes.", capacity);
            return false;
        }

        WGPUBindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = buffer;
        entry.offset = 0;
        entry.size = push_constant_block_size;

        WGPUBindGroupDescriptor group_desc{};
        group_desc.label = wgpu_string("sturdy push constants");
        group_desc.layout = layout;
        group_desc.entryCount = 1;
        group_desc.entries = &entry;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(device_, &group_desc);
        if (group == nullptr) {
            wgpuBufferDestroy(buffer);
            wgpuBufferRelease(buffer);
            Foundation::log_error("WebGPU backend: could not create the push-constant bind group.");
            return false;
        }

        // Anything composed for the outgoing ring must reach it before it is left behind: commands already recorded
        // against it read those slices when they run.
        flush_push_constants_locked(state);
        state.shadow.clear();

        // The outgoing ring and group may still be named by recorded or in-flight commands. Releasing (not destroying)
        // drops only our reference; Dawn frees them once nothing else holds one.
        if (state.bind_group != nullptr) {
            wgpuBindGroupRelease(state.bind_group);
        }
        if (state.ring != nullptr) {
            wgpuBufferRelease(state.ring);
        }

        state.ring = buffer;
        state.bind_group = group;
        state.capacity = capacity;
        state.cursor = 0;
        return true;
    }

    /// Drops the claims of submissions that have completed, and with them anything retired that no
    /// outstanding submission can still be reading.
    ///
    /// @param state The locked ring state.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::reclaim_push_constant_slices(PushConstantState &state) noexcept {
        while (!state.claims.empty()) {
            WGPUFutureWaitInfo wait{};
            wait.future = state.claims.front();
            // A zero timeout polls: it reports whether the submission is already done without
            // blocking, which is what makes this sweep cheap enough to run per allocation attempt.
            const WGPUWaitStatus status = wgpuInstanceWaitAny(instance_, 1, &wait, 0);
            if (status != WGPUWaitStatus_Success || wait.completed == 0) {
                break;
            }
            state.claims.erase(state.claims.begin());
        }

        // Nothing outstanding means nothing can still be reading a retired ring or group either.
        if (!state.claims.empty()) {
            return;
        }
        for (WGPUBindGroup group : state.retired_groups) {
            wgpuBindGroupRelease(group);
        }
        state.retired_groups.clear();
        for (WGPUBuffer ring : state.retired_rings) {
            wgpuBufferRelease(ring);
        }
        state.retired_rings.clear();
    }

    /// Records that everything allocated from the ring so far belongs to the submission just made.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::mark_push_constants_submitted() noexcept {
        auto state = push_constants_.lock();
        if (state->cursor == 0 && state->claims.empty()) {
            return;
        }

        WGPUQueueWorkDoneCallbackInfo info{};
        info.mode = WGPUCallbackMode_WaitAnyOnly;
        info.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *, void *) {};
        state->claims.push_back(wgpuQueueOnSubmittedWorkDone(queue_, info));

        // Sweep without blocking so a long-running process does not accumulate claims for work that
        // finished long ago.
        reclaim_push_constant_slices(*state);
    }

    /// Releases everything the push-constant emulation owns.
    ///
    /// Called from the device destructor, by which point the queue has been drained, so retired
    /// objects are released without consulting the claim queue.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_push_constant_state() noexcept {
        auto state = push_constants_.lock();
        for (WGPUBindGroup group : state->retired_groups) {
            wgpuBindGroupRelease(group);
        }
        state->retired_groups.clear();
        for (WGPUBuffer ring : state->retired_rings) {
            wgpuBufferRelease(ring);
        }
        state->retired_rings.clear();
        if (state->bind_group != nullptr) {
            wgpuBindGroupRelease(state->bind_group);
            state->bind_group = nullptr;
        }
        if (state->ring != nullptr) {
            wgpuBufferRelease(state->ring);
            state->ring = nullptr;
        }
        if (push_constant_bind_group_layout_ != nullptr) {
            wgpuBindGroupLayoutRelease(push_constant_bind_group_layout_);
            push_constant_bind_group_layout_ = nullptr;
        }
        if (empty_bind_group_layout_ != nullptr) {
            wgpuBindGroupLayoutRelease(empty_bind_group_layout_);
            empty_bind_group_layout_ = nullptr;
        }
    }

} // namespace SFT::Core::WebGpu
