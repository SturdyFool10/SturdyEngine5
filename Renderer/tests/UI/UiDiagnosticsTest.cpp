/// CPU-only checks for the UI's error reporting, memory growth and runtime debug view, through the public `UI::Context` API:
/// a layout bigger than the context starts with is truncated once, reported with enough detail to find it (element path, call
/// sites in both languages, native stack), and fits from the next frame; repeats collapse into one counted report; a fixed-size
/// context stays truncated and says so; the debug view can be switched on and off while the UI runs.

#include <Renderer/UI/UI.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#undef assert
#define assert(condition)               \
    ((condition) ? static_cast<void>(0) \
                 : (std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition), std::abort()))

namespace {

    using SFT::UI::Context;
    using SFT::UI::ElementDecl;
    using SFT::UI::SizingAxis;
    using SFT::UI::TextStyle;
    using SFT::UI::UiErrorCode;
    using SFT::UI::UiErrorReport;
    using SFT::UI::UiSeverity;

    Context make(Context::Config config = {}) {
        config.load_default_font = false;
        auto created = Context::create(config);
        assert(created.has_value());
        return std::move(*created);
    }

    const UiErrorReport *find(const Context &context, UiErrorCode code) {
        for (const UiErrorReport &report : context.diagnostics().reports()) {
            if (report.code == code) {
                return &report;
            }
        }
        return nullptr;
    }

    /// One frame with `rows` rows, each a styled element with a label.
    void build_frame(Context &context, int rows) {
        context.begin_layout({800.0f, 600.0f});
        {
            auto root = context.element(ElementDecl{.sizing = {SizingAxis::grow(), SizingAxis::grow()},
                                                    .direction = SFT::UI::LayoutDirection::TopToBottom,
                                                    .id = "root"_ustr});
            for (int i = 0; i < rows; ++i) {
                auto row = context.element(ElementDecl{.sizing = {SizingAxis::grow(), SizingAxis::fixed(10.0f)},
                                                       .background_color = SFT::UI::Color{0.1, 0.2, 0.3, 1.0}});
                const SFT::UString label{("row " + std::to_string(i)).c_str()};
                context.text(label, TextStyle{});
            }
        }
        (void)context.finish_frame();
    }

    /// One frame with a deliberate duplicate id.
    void context_dup_frame(Context &context) {
        context.begin_layout({400.0f, 400.0f});
        {
            auto a = context.element(ElementDecl{.id = "dup"_ustr});
            auto b = context.element(ElementDecl{.id = "dup"_ustr});
        }
        (void)context.finish_frame();
    }

    void growth_fits_from_the_next_frame() {
        Context context = make(Context::Config{.max_element_count = 64});
        SFT::Foundation::set_foreign_call_site("rust", "src/game/inventory.rs", 118, 13, "draw_inventory");
        build_frame(context, 2000); // far more than 64 elements

        const UiErrorReport *limit = find(context, UiErrorCode::ElementLimit);
        assert(limit != nullptr);
        assert(limit->occurrences >= 1);
        assert(limit->value > 64 && limit->limit > 0);
        assert(limit->message.find("grow") != std::string::npos);
        // Where: the engine check, the UI call in C++, the announced foreign site, the element chain, and a native stack.
        assert(limit->engine_source.find("clay.h") != std::string::npos);
        assert(limit->api_call_site.valid() && limit->api_call_site.file.find("UiDiagnosticsTest.cpp") != std::string::npos);
        assert(limit->foreign_call_site.valid() && limit->foreign_call_site.file == "src/game/inventory.rs" && limit->foreign_call_site.line == 118);
        assert(!limit->element_path.empty() && limit->element_path.front() == "<root>");
        bool named_root = false;
        for (const std::string &part : limit->element_path) {
            named_root = named_root || part == "root";
        }
        assert(named_root);
        if (SFT::Foundation::trace_detail() != SFT::Foundation::TraceDetail::Off) {
            assert(!limit->trace.empty());
        }
        const std::string text = SFT::UI::format_report(*limit);
        assert(text.find("src/game/inventory.rs:118") != std::string::npos);
        assert(text.find("element path") != std::string::npos);
        assert(context.memory_stats().growable);

        // The context resizes between frames: after a couple of frames the same UI fits completely.
        for (int frame = 0; frame < 4; ++frame) {
            build_frame(context, 2000);
        }
        const Context::MemoryStats stats = context.memory_stats();
        assert(!stats.truncated_last_frame);
        assert(stats.grow_count >= 1);
        assert(stats.elements_used > 4000 && stats.elements_used < stats.elements_capacity);
        const UiErrorReport *resized = find(context, UiErrorCode::Resized);
        assert(resized != nullptr && resized->severity == UiSeverity::Info);
        SFT::Foundation::clear_foreign_call_site();
    }

    void repeats_collapse_into_one_report() {
        Context context = make();
        std::vector<std::pair<UiErrorCode, u64>> seen;
        context.diagnostics().set_callback([&](const UiErrorReport &report) { seen.emplace_back(report.code, report.occurrences); });
        context.diagnostics().set_log_severity(UiSeverity::Error); // keep the test output quiet
        for (int frame = 0; frame < 40; ++frame) {
            context.begin_layout({400.0f, 400.0f});
            {
                auto a = context.element(ElementDecl{.id = "dup"_ustr});
                auto b = context.element(ElementDecl{.id = "dup"_ustr}); // same id, same frame: a bug in the caller
            }
            (void)context.finish_frame();
        }
        const UiErrorReport *duplicate = find(context, UiErrorCode::DuplicateId);
        assert(duplicate != nullptr);
        assert(duplicate->occurrences == 40);
        assert(duplicate->first_frame < duplicate->last_frame);
        size_t duplicate_reports = 0;
        for (const UiErrorReport &report : context.diagnostics().reports()) {
            duplicate_reports += report.code == UiErrorCode::DuplicateId ? 1 : 0;
        }
        assert(duplicate_reports == 1);
        // The callback hears the first sighting and then only power-of-two escalations, not every frame.
        size_t duplicate_calls = 0;
        for (const auto &entry : seen) {
            duplicate_calls += entry.first == UiErrorCode::DuplicateId ? 1 : 0;
        }
        assert(duplicate_calls >= 3 && duplicate_calls <= 8);
        assert(context.diagnostics().has_problems());
        auto taken = context.diagnostics().take_reports();
        assert(!taken.empty() && context.diagnostics().reports().empty() && !context.diagnostics().has_problems());
    }

    void fixed_context_stays_truncated_and_says_so() {
        Context::Config config;
        config.arena_capacity_bytes = 3 * 1024 * 1024;
        config.max_element_count = 64;
        Context context = make(config);
        context.diagnostics().set_log_severity(UiSeverity::Info);
        for (int frame = 0; frame < 3; ++frame) {
            build_frame(context, 500);
        }
        assert(!context.memory_stats().growable);
        assert(context.memory_stats().truncated_last_frame);
        const UiErrorReport *limit = find(context, UiErrorCode::ElementLimit);
        assert(limit != nullptr && limit->severity == UiSeverity::Error);
        assert(limit->message.find("cannot grow") != std::string::npos);
        assert(limit->occurrences == 3);

        // A fixed arena too small for its own element count fails to create, with a message instead of an exception.
        Context::Config tiny;
        tiny.arena_capacity_bytes = 4096;
        auto created = Context::create(tiny);
        assert(!created.has_value());
    }

    void memory_limit_truncates_with_a_clear_error() {
        Context::Config config;
        config.max_element_count = 64;
        config.memory_limit_bytes = 600 * 1024;
        Context context = make(config);
        context.diagnostics().set_log_severity(UiSeverity::Error);
        for (int frame = 0; frame < 6; ++frame) {
            build_frame(context, 3000);
        }
        const UiErrorReport *limit = find(context, UiErrorCode::MemoryLimitReached);
        assert(limit != nullptr && limit->severity == UiSeverity::Error);
        assert(limit->message.find("limit") != std::string::npos);
        assert(limit->limit == 600 * 1024 && limit->value > limit->limit);
        assert(context.memory_stats().truncated_last_frame);
    }

    void debug_view_toggles_at_runtime() {
        Context context = make();
        context.diagnostics().set_log_severity(UiSeverity::Error);
        assert(!context.debug_view_enabled()); // the engine never turns it on by itself
        for (int frame = 0; frame < 30; ++frame) {
            context.set_debug_view_enabled(frame % 6 < 3);
            assert(context.debug_view_enabled() == (frame % 6 < 3));
            build_frame(context, 20);
        }
        context.set_debug_view_enabled(false);
        build_frame(context, 20);
        assert(!context.debug_view_enabled());
        assert(find(context, UiErrorCode::InternalBoundsViolation) == nullptr);
    }

    void reports_survive_moving_the_context() {
        Context first = make(Context::Config{.max_element_count = 64});
        first.diagnostics().set_log_severity(UiSeverity::Error);
        Context moved = std::move(first);
        build_frame(moved, 500);
        assert(find(moved, UiErrorCode::ElementLimit) != nullptr);
    }

    void assignment_keeps_the_hosts_diagnostics_and_debug_view() {
        // The engine move-assigns a freshly built context over the live one (device change, HDR toggle). What the application set up on the
        // live one -- its callback, its collected reports, its debug-view choice -- must survive that, and the new layout context must
        // report into the same store.
        Context live = make();
        live.diagnostics().set_log_severity(UiSeverity::Error);
        int callback_calls = 0;
        live.diagnostics().set_callback([&](const UiErrorReport &) { ++callback_calls; });
        live.set_debug_view_enabled(true);
        for (int frame = 0; frame < 3; ++frame) {
            context_dup_frame(live);
        }
        assert(find(live, UiErrorCode::DuplicateId) != nullptr);
        const u64 before = find(live, UiErrorCode::DuplicateId)->occurrences;

        Context replacement = make(Context::Config{.max_element_count = 64});
        live = std::move(replacement);
        assert(live.debug_view_enabled());                 // choice carried over
        assert(find(live, UiErrorCode::DuplicateId) != nullptr); // reports kept
        const int calls_before = callback_calls;
        build_frame(live, 500);                            // the new context overflows its small budget...
        assert(find(live, UiErrorCode::ElementLimit) != nullptr); // ...and reports into the kept store
        assert(callback_calls > calls_before);             // ...through the kept callback
        context_dup_frame(live);
        assert(find(live, UiErrorCode::DuplicateId)->occurrences > before);
    }

} // namespace

int main() {
    growth_fits_from_the_next_frame();
    repeats_collapse_into_one_report();
    fixed_context_stays_truncated_and_says_so();
    memory_limit_truncates_with_a_clear_error();
    debug_view_toggles_at_runtime();
    reports_survive_moving_the_context();
    assignment_keeps_the_hosts_diagnostics_and_debug_view();
    std::puts("UiDiagnosticsTest: all checks passed.");
    return 0;
}
