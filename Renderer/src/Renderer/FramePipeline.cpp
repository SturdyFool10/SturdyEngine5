#include <Renderer/FramePipeline.hpp>

#include <algorithm>

namespace SFT::Renderer {

    namespace {
        [[nodiscard]] std::unexpected<FramePipelineError> fail(FramePipelineErrorCode code, std::string message) {
            return std::unexpected(FramePipelineError{.code = code, .message = std::move(message)});
        }
    } // namespace

    FramePipeline::Entry *FramePipeline::find(std::string_view name) noexcept {
        const auto found = std::ranges::find(entries_, name, &Entry::name);
        return found == entries_.end() ? nullptr : &*found;
    }

    const FramePipeline::Entry *FramePipeline::find(std::string_view name) const noexcept {
        const auto found = std::ranges::find(entries_, name, &Entry::name);
        return found == entries_.end() ? nullptr : &*found;
    }

    FramePipelineExpected<void> FramePipeline::add(std::string name, FrameFeatureFn build) {
        if (name.empty()) {
            return fail(FramePipelineErrorCode::EmptyName, "a frame feature needs a name");
        }
        if (find(name) != nullptr) {
            return fail(FramePipelineErrorCode::DuplicateName, "a frame feature named '" + name + "' already exists");
        }
        entries_.push_back(Entry{std::move(name), std::move(build), true});
        return {};
    }

    FramePipelineExpected<void> FramePipeline::insert_before(std::string_view anchor, std::string name, FrameFeatureFn build) {
        if (name.empty()) {
            return fail(FramePipelineErrorCode::EmptyName, "a frame feature needs a name");
        }
        if (find(name) != nullptr) {
            return fail(FramePipelineErrorCode::DuplicateName, "a frame feature named '" + name + "' already exists");
        }
        const auto position = std::ranges::find(entries_, anchor, &Entry::name);
        if (position == entries_.end()) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{anchor} + "'");
        }
        entries_.insert(position, Entry{std::move(name), std::move(build), true});
        return {};
    }

    FramePipelineExpected<void> FramePipeline::insert_after(std::string_view anchor, std::string name, FrameFeatureFn build) {
        if (name.empty()) {
            return fail(FramePipelineErrorCode::EmptyName, "a frame feature needs a name");
        }
        if (find(name) != nullptr) {
            return fail(FramePipelineErrorCode::DuplicateName, "a frame feature named '" + name + "' already exists");
        }
        const auto position = std::ranges::find(entries_, anchor, &Entry::name);
        if (position == entries_.end()) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{anchor} + "'");
        }
        entries_.insert(position + 1, Entry{std::move(name), std::move(build), true});
        return {};
    }

    FramePipelineExpected<void> FramePipeline::replace(std::string_view name, FrameFeatureFn build) {
        Entry *entry = find(name);
        if (entry == nullptr) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{name} + "'");
        }
        entry->build = std::move(build);
        return {};
    }

    FramePipelineExpected<void> FramePipeline::remove(std::string_view name) {
        const auto position = std::ranges::find(entries_, name, &Entry::name);
        if (position == entries_.end()) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{name} + "'");
        }
        entries_.erase(position);
        return {};
    }

    FramePipelineExpected<void> FramePipeline::set_enabled(std::string_view name, bool enabled) {
        Entry *entry = find(name);
        if (entry == nullptr) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{name} + "'");
        }
        entry->enabled = enabled;
        return {};
    }

    FramePipelineExpected<void> FramePipeline::wrap(
        std::string_view name, std::function<Core::RendererResult(FrameBuildContext &, const FrameFeatureFn &)> wrapper) {
        Entry *entry = find(name);
        if (entry == nullptr) {
            return fail(FramePipelineErrorCode::UnknownName, "no frame feature named '" + std::string{name} + "'");
        }
        entry->build = [inner = std::move(entry->build), wrapper = std::move(wrapper)](FrameBuildContext &context) {
            return wrapper(context, inner);
        };
        return {};
    }

    bool FramePipeline::contains(std::string_view name) const noexcept { return find(name) != nullptr; }

    bool FramePipeline::enabled(std::string_view name) const noexcept {
        const Entry *entry = find(name);
        return entry != nullptr && entry->enabled;
    }

    std::vector<std::string> FramePipeline::names() const {
        std::vector<std::string> result;
        result.reserve(entries_.size());
        for (const Entry &entry : entries_) {
            result.push_back(entry.name);
        }
        return result;
    }

    Core::RendererResult FramePipeline::build(FrameBuildContext &context) const {
        for (const Entry &entry : entries_) {
            if (!entry.enabled) {
                continue;
            }
            if (Core::RendererResult built = entry.build(context); !built.has_value()) {
                return built;
            }
        }
        return {};
    }

} // namespace SFT::Renderer
