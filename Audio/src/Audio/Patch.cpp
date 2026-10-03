#include <Audio/Patch.hpp>

#include <array>

#include <algorithm>
#include <cmath>
#include <memory>

namespace SFT::Audio {

    namespace {

        enum class Kind : u8 { Constant, Param, Oscillator, Noise, Envelope, Filter, Add, Multiply, Mix, Scale, Saturate, Delay, MidiToHz };

        struct NodeDef {
            Kind kind = Kind::Constant;
            std::array<PatchNode, 3> input{no_patch_node, no_patch_node, no_patch_node};
            f32 a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f; // kind-specific constants
            u32 selector = 0;                           // waveform / band / pink flag
            u32 seed = 1;
            u32 param_index = 0;
        };

    } // namespace

    struct PatchDef {
        std::vector<NodeDef> nodes;
        std::vector<UString> param_names;
        std::vector<f32> param_defaults;
        PatchNode output = 0;
    };

    struct PatchBuilder::Impl {
        std::vector<NodeDef> nodes;
        std::vector<UString> param_names;
        std::vector<f32> param_defaults;

        PatchNode add(NodeDef node) {
            nodes.push_back(node);
            return static_cast<PatchNode>(nodes.size() - 1);
        }
        // Inputs must name earlier nodes (or be absent); anything else becomes "absent" so a bad wire cannot form a cycle.
        [[nodiscard]] PatchNode valid(PatchNode n) const noexcept { return n < nodes.size() ? n : no_patch_node; }
    };

    PatchBuilder::PatchBuilder() : impl_(std::make_unique<Impl>()) {}
    PatchBuilder::~PatchBuilder() = default;
    PatchBuilder::PatchBuilder(PatchBuilder &&) noexcept = default;
    PatchBuilder &PatchBuilder::operator=(PatchBuilder &&) noexcept = default;

    PatchNode PatchBuilder::constant(f32 value) {
        NodeDef n;
        n.kind = Kind::Constant;
        n.a = value;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::param(const UString &name, f32 default_value) {
        for (usize i = 0; i < impl_->param_names.size(); ++i) {
            if (impl_->param_names[i] == name) {
                NodeDef n;
                n.kind = Kind::Param;
                n.param_index = static_cast<u32>(i);
                return impl_->add(n);
            }
        }
        NodeDef n;
        n.kind = Kind::Param;
        n.param_index = static_cast<u32>(impl_->param_names.size());
        impl_->param_names.push_back(name);
        impl_->param_defaults.push_back(default_value);
        return impl_->add(n);
    }

    PatchNode PatchBuilder::oscillator(Waveform waveform, PatchNode frequency, PatchNode pulse_width) {
        NodeDef n;
        n.kind = Kind::Oscillator;
        n.selector = static_cast<u32>(waveform);
        n.input = {impl_->valid(frequency), impl_->valid(pulse_width), no_patch_node};
        return impl_->add(n);
    }

    PatchNode PatchBuilder::noise(bool pink, u32 seed) {
        NodeDef n;
        n.kind = Kind::Noise;
        n.selector = pink ? 1u : 0u;
        n.seed = seed;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::envelope(PatchNode gate, f32 attack, f32 decay, f32 sustain, f32 release) {
        NodeDef n;
        n.kind = Kind::Envelope;
        n.input = {impl_->valid(gate), no_patch_node, no_patch_node};
        n.a = attack;
        n.b = decay;
        n.c = sustain;
        n.d = release;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::filter(Band band, PatchNode input, PatchNode cutoff, f32 q) {
        NodeDef n;
        n.kind = Kind::Filter;
        n.selector = static_cast<u32>(band);
        n.input = {impl_->valid(input), impl_->valid(cutoff), no_patch_node};
        n.a = q;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::add(PatchNode a, PatchNode b) {
        NodeDef n;
        n.kind = Kind::Add;
        n.input = {impl_->valid(a), impl_->valid(b), no_patch_node};
        return impl_->add(n);
    }

    PatchNode PatchBuilder::multiply(PatchNode a, PatchNode b) {
        NodeDef n;
        n.kind = Kind::Multiply;
        n.input = {impl_->valid(a), impl_->valid(b), no_patch_node};
        return impl_->add(n);
    }

    PatchNode PatchBuilder::mix(PatchNode a, PatchNode b, PatchNode t) {
        NodeDef n;
        n.kind = Kind::Mix;
        n.input = {impl_->valid(a), impl_->valid(b), impl_->valid(t)};
        return impl_->add(n);
    }

    PatchNode PatchBuilder::scale(PatchNode input, f32 scale, f32 offset) {
        NodeDef n;
        n.kind = Kind::Scale;
        n.input = {impl_->valid(input), no_patch_node, no_patch_node};
        n.a = scale;
        n.b = offset;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::saturate(PatchNode input, f32 drive) {
        NodeDef n;
        n.kind = Kind::Saturate;
        n.input = {impl_->valid(input), no_patch_node, no_patch_node};
        n.a = drive;
        return impl_->add(n);
    }

    PatchNode PatchBuilder::delay(PatchNode input, f32 max_seconds, PatchNode time, f32 feedback) {
        NodeDef n;
        n.kind = Kind::Delay;
        n.input = {impl_->valid(input), impl_->valid(time), no_patch_node};
        n.a = std::max(max_seconds, 0.001f);
        n.b = std::clamp(feedback, 0.0f, 0.98f);
        return impl_->add(n);
    }

    PatchNode PatchBuilder::midi_to_hz(PatchNode note) {
        NodeDef n;
        n.kind = Kind::MidiToHz;
        n.input = {impl_->valid(note), no_patch_node, no_patch_node};
        return impl_->add(n);
    }

    std::shared_ptr<const PatchDef> PatchBuilder::build(PatchNode output) {
        auto def = std::make_shared<PatchDef>();
        def->nodes = impl_->nodes;
        def->param_names = impl_->param_names;
        def->param_defaults = impl_->param_defaults;
        def->output = output < def->nodes.size() ? output : (def->nodes.empty() ? 0 : static_cast<PatchNode>(def->nodes.size() - 1));
        return def;
    }

    // ---- runtime ---------------------------------------------------------------------------------------------------

    struct PatchSource::Runtime {
        struct State {
            Oscillator oscillator;
            Noise noise;
            Envelope envelope;
            StateVariableFilter filter;
            DelayLine delay;
            f32 smoothed = 0.0f;
            f32 value = 0.0f;
            u32 control_counter = 0;
            f32 last_cutoff = -1.0f;
            bool gate_was_on = false;
        };
        std::vector<State> states;
        std::vector<std::atomic<f32>> params;
        f32 smoothing = 0.01f;

        Runtime(usize node_count, usize param_count) : states(node_count), params(param_count) {}
    };

    PatchSource::PatchSource(std::shared_ptr<const PatchDef> def, u32 sample_rate)
        : def_(std::move(def)), runtime_(std::make_unique<Runtime>(def_->nodes.size(), def_->param_names.size())), sample_rate_(sample_rate) {
        const f32 sr = static_cast<f32>(sample_rate);
        runtime_->smoothing = 1.0f - std::exp(-1.0f / (0.005f * sr));
        for (usize i = 0; i < def_->param_names.size(); ++i) {
            runtime_->params[i].store(def_->param_defaults[i], std::memory_order_relaxed);
        }
        for (usize i = 0; i < def_->nodes.size(); ++i) {
            const NodeDef &n = def_->nodes[i];
            Runtime::State &s = runtime_->states[i];
            switch (n.kind) {
                case Kind::Oscillator: s.oscillator.set(static_cast<Waveform>(n.selector), sr, 440.0f); break;
                case Kind::Noise: s.noise = Noise(n.seed); break;
                case Kind::Envelope: s.envelope.set(sr, n.a, n.b, n.c, n.d); break;
                case Kind::Delay: s.delay.resize(static_cast<u32>(n.a * sr) + 2); break;
                case Kind::Param: s.smoothed = def_->param_defaults[n.param_index]; break;
                default: break;
            }
        }
    }

    PatchSource::~PatchSource() = default;

    usize PatchSource::param_count() const noexcept { return def_->param_names.size(); }

    bool PatchSource::set_param(const ustr &name, f32 value) noexcept {
        for (usize i = 0; i < def_->param_names.size(); ++i) {
            if (def_->param_names[i] == name) {
                runtime_->params[i].store(value, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    u32 PatchSource::read(AudioBuffer &out, u32 frames) {
        if (finished_ || out.channels() == 0) {
            return 0;
        }
        u32 count = frames;
        if (duration_frames_ > 0) {
            count = static_cast<u32>(std::min<u64>(frames, duration_frames_ - std::min(duration_frames_, produced_)));
        }
        f32 *dst = out.data(0);
        const f32 sr = static_cast<f32>(sample_rate_);
        const usize node_count = def_->nodes.size();

        for (u32 n = 0; n < count; ++n) {
            for (usize i = 0; i < node_count; ++i) {
                const NodeDef &node = def_->nodes[i];
                Runtime::State &s = runtime_->states[i];
                const auto in = [&](usize slot, f32 fallback = 0.0f) {
                    return node.input[slot] == no_patch_node ? fallback : runtime_->states[node.input[slot]].value;
                };
                switch (node.kind) {
                    case Kind::Constant: s.value = node.a; break;
                    case Kind::Param: {
                        const f32 target = runtime_->params[node.param_index].load(std::memory_order_relaxed);
                        s.smoothed += (target - s.smoothed) * runtime_->smoothing;
                        s.value = s.smoothed;
                        break;
                    }
                    case Kind::Oscillator:
                        s.oscillator.set_frequency(std::max(in(0, 440.0f), 0.0f));
                        s.value = s.oscillator.next();
                        break;
                    case Kind::Noise: s.value = node.selector == 1u ? s.noise.pink() : s.noise.white(); break;
                    case Kind::Envelope: {
                        const bool gate = in(0, 0.0f) > 0.5f;
                        if (gate && !s.gate_was_on) {
                            s.envelope.gate_on();
                        } else if (!gate && s.gate_was_on) {
                            s.envelope.gate_off();
                        }
                        s.gate_was_on = gate;
                        s.value = s.envelope.next();
                        break;
                    }
                    case Kind::Filter: {
                        // Coefficients are refreshed every 8 samples (control rate) or when the cutoff jumps.
                        const f32 cutoff = std::max(in(1, 1000.0f), 20.0f);
                        if (s.control_counter++ % 8u == 0u && std::fabs(cutoff - s.last_cutoff) > 0.5f) {
                            s.filter.set(sr, cutoff, node.a);
                            s.last_cutoff = cutoff;
                        }
                        const auto o = s.filter.process(in(0));
                        s.value = node.selector == 0u ? o.low : (node.selector == 1u ? o.band : o.high);
                        break;
                    }
                    case Kind::Add: s.value = in(0) + in(1); break;
                    case Kind::Multiply: s.value = in(0) * in(1, 1.0f); break;
                    case Kind::Mix: {
                        const f32 a = in(0), b = in(1), t = std::clamp(in(2, 0.5f), 0.0f, 1.0f);
                        s.value = a + (b - a) * t;
                        break;
                    }
                    case Kind::Scale: s.value = in(0) * node.a + node.b; break;
                    case Kind::Saturate: s.value = std::tanh(in(0) * node.a); break;
                    case Kind::Delay: {
                        const f32 time = std::clamp(in(1, 0.1f), 1.0f / sr, node.a);
                        const f32 delayed = s.delay.read(std::max(1.0f, time * sr));
                        s.delay.write(in(0) + delayed * node.b);
                        s.value = delayed;
                        break;
                    }
                    case Kind::MidiToHz: s.value = 440.0f * std::exp2((in(0) - 69.0f) / 12.0f); break;
                }
            }
            dst[n] = runtime_->states[def_->output].value;
        }
        produced_ += count;
        if (duration_frames_ > 0 && produced_ >= duration_frames_) {
            finished_ = true;
        }
        return count;
    }

} // namespace SFT::Audio
