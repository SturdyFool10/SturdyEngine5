/// FramePipeline: naming, ordering, replacement, wrapping, enabling. CPU only; the features here just
/// record that they ran.

#include <Renderer/FramePipeline.hpp>

#include <iostream>

namespace {
    using namespace SFT::Renderer;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
    std::string ran;
} // namespace

int main() {
    // A context is only ever handed through; features here never touch it.
    FrameBuildContext *no_context = nullptr;
    const auto mark = [](char c) {
        return [c](FrameBuildContext &) -> SFT::Core::RendererResult {
            ran += c;
            return {};
        };
    };
    const auto run = [&](const FramePipeline &pipeline) {
        ran.clear();
        (void)pipeline.build(*no_context);
        return ran;
    };

    FramePipeline pipeline;
    check(pipeline.add("a", mark('a')).has_value() && pipeline.add("b", mark('b')).has_value() && pipeline.add("c", mark('c')).has_value(),
          "adding features must succeed");
    check(run(pipeline) == "abc", "features run in registration order");
    check(!pipeline.add("a", mark('x')).has_value(), "a duplicate name is rejected");
    check(!pipeline.add("", mark('x')).has_value(), "an empty name is rejected");

    check(pipeline.insert_after("a", "a2", mark('1')).has_value() && pipeline.insert_before("a", "z", mark('0')).has_value(), "inserting must succeed");
    check(run(pipeline) == "0a1bc", "inserted features land where asked");
    check(!pipeline.insert_after("nope", "q", mark('q')).has_value(), "inserting next to an unknown feature fails");

    check(pipeline.replace("b", mark('B')).has_value() && run(pipeline) == "0a1Bc", "replace keeps position, swaps behaviour");
    check(pipeline.set_enabled("a", false).has_value() && run(pipeline) == "01Bc" && !pipeline.enabled("a"), "a disabled feature is skipped");
    check(pipeline.set_enabled("a", true).has_value() && run(pipeline) == "0a1Bc", "re-enabling restores it");

    check(pipeline.wrap("c", [](FrameBuildContext &context, const FrameFeatureFn &inner) {
                     ran += '<';
                     const auto result = inner(context);
                     ran += '>';
                     return result;
                 }).has_value(),
          "wrapping must succeed");
    check(run(pipeline) == "0a1B<c>", "a wrapper surrounds the original");

    check(pipeline.remove("z").has_value() && run(pipeline) == "a1B<c>", "removing drops a feature");
    check(!pipeline.remove("z").has_value(), "removing twice fails");

    // The first failure stops the frame.
    FramePipeline failing;
    (void)failing.add("ok", mark('k'));
    (void)failing.add("bad", [](FrameBuildContext &) -> SFT::Core::RendererResult {
        return SFT::Core::graphics_backend_error(SFT::Core::GraphicsBackendErrorCode::OperationFailed, "boom");
    });
    (void)failing.add("never", mark('n'));
    ran.clear();
    check(!failing.build(*no_context).has_value() && ran == "k", "a failing feature stops the rest");

    const auto names = pipeline.names();
    check(names.size() == 4 && names.front() == "a", "names() lists execution order");
    return failures == 0 ? 0 : 1;
}
