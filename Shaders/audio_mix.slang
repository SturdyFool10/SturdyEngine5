// Compute-mode audio mixing (Audio/src/Audio/Compute.hpp, AudioGpu/GpuComputeMixer.cpp).
//
// Three kernels, dispatched back to back for one audio block:
//   voiceMain   one thread per voice: resample its sample (Catmull-Rom), optionally average the channels to one signal,
//               optionally run the air-absorption low-pass, writing `signals[signal][frame]`.
//   tapMain     one thread per (item, frame): an item is a run of at most kTapsPerItem taps that all feed one destination;
//               each tap scales a signal plane by its gain ramp. Partial sums go to `partial[item][frame]`.
//   reduceMain  one thread per (destination, frame): adds a destination's items into `mix[destination][frame]`.
// The arithmetic is exactly Audio's reference_voice_signals / reference_tap, so a device and the CPU agree to rounding.
// Records are plain 4-byte words so the layout is the same under std140, std430 and WGSL.

struct GpuVoice {
    uint sample;      // id into sampleTable
    uint flags;       // bit 0 loop, bit 1 mix down to one signal, bit 2 low-pass
    uint firstFrame;
    uint validFrames;
    int base;
    float frac;
    float step;
    uint firstSignal;
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
    uint pad;
};

struct GpuSample {
    uint offset;      // first float of the sample in samplePool
    uint frames;
    uint channels;
    uint pad;
};

struct GpuTap {
    uint signal;
    float gainFrom;
    float gainTo;
    uint pad;
};

struct GpuItem {
    uint firstTap;
    uint tapCount;
    uint destination;
    uint pad;
};

struct GpuRange {
    uint firstItem;
    uint itemCount;
};

struct GpuFilterState {
    float z1;
    float z2;
};

struct MixParams {
    uint frames;
    uint voiceCount;
    uint itemCount;
    uint destinationCount;
    // Threads per row of the 2D dispatches (so more than 65535 groups are possible).
    uint rowThreads;
    uint pad0;
    uint pad1;
    uint pad2;
};

ConstantBuffer<MixParams> params;
StructuredBuffer<float> samplePool;
StructuredBuffer<GpuSample> sampleTable;
StructuredBuffer<GpuVoice> voices;
RWStructuredBuffer<float> signals;
RWStructuredBuffer<GpuFilterState> filterState;
StructuredBuffer<GpuTap> taps;
StructuredBuffer<GpuItem> items;
StructuredBuffer<GpuRange> ranges;
RWStructuredBuffer<float> partial;
RWStructuredBuffer<float> mix;

float fetchSample(GpuSample info, int frame, uint channel, bool looping) {
    int length = int(info.frames);
    if (looping) {
        frame = frame % length;
        if (frame < 0) {
            frame += length;
        }
    } else if (frame < 0 || frame >= length) {
        return 0.0;
    }
    return samplePool[info.offset + uint(frame) * info.channels + channel];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void voiceMain(uint3 id : SV_DispatchThreadID) {
    uint v = id.x + id.y * params.rowThreads;
    if (v >= params.voiceCount) {
        return;
    }
    GpuVoice voice = voices[v];
    GpuSample info = sampleTable[voice.sample];
    uint frames = params.frames;
    bool looping = (voice.flags & 1u) != 0u;
    bool mixDown = (voice.flags & 2u) != 0u;
    bool filtered = (voice.flags & 4u) != 0u;
    uint channels = info.channels;
    uint planes = mixDown ? 1u : channels;

    for (uint p = 0u; p < planes; p++) {
        uint plane = (voice.firstSignal + p) * frames;
        for (uint f = 0u; f < frames; f++) {
            signals[plane + f] = 0.0;
        }
    }
    uint first = min(frames, voice.firstFrame);
    uint last = min(frames, voice.firstFrame + voice.validFrames);
    float inverse = 1.0 / float(max(channels, 1u));
    for (uint i = first; i < last; i++) {
        float position = voice.frac + float(i - voice.firstFrame) * voice.step;
        float whole = floor(position);
        float t = position - whole;
        int i1 = voice.base + int(whole);
        float sum = 0.0;
        for (uint c = 0u; c < channels; c++) {
            float p0 = fetchSample(info, i1 - 1, c, looping);
            float p1 = fetchSample(info, i1, c, looping);
            float p2 = fetchSample(info, i1 + 1, c, looping);
            float p3 = fetchSample(info, i1 + 2, c, looping);
            float a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
            float b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            float d = -0.5 * p0 + 0.5 * p2;
            float value = ((a * t + b) * t + d) * t + p1;
            if (mixDown) {
                sum += value;
            } else {
                signals[(voice.firstSignal + c) * frames + i] = value;
            }
        }
        if (mixDown) {
            signals[voice.firstSignal * frames + i] = sum * inverse;
        }
    }

    GpuFilterState state;
    state.z1 = voice.z1;
    state.z2 = voice.z2;
    if (mixDown && filtered) {
        float z1 = voice.z1;
        float z2 = voice.z2;
        uint plane = voice.firstSignal * frames;
        for (uint i = 0u; i < frames; i++) {
            float x = signals[plane + i];
            float y = voice.b0 * x + z1;
            z1 = voice.b1 * x - voice.a1 * y + z2;
            z2 = voice.b2 * x - voice.a2 * y;
            signals[plane + i] = y;
        }
        state.z1 = z1;
        state.z2 = z2;
    }
    filterState[v] = state;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void tapMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.x + id.y * params.rowThreads;
    uint frames = params.frames;
    if (index >= params.itemCount * frames) {
        return;
    }
    uint item = index / frames;
    uint f = index - item * frames;
    GpuItem work = items[item];
    float sum = 0.0;
    for (uint k = 0u; k < work.tapCount; k++) {
        GpuTap tap = taps[work.firstTap + k];
        float ramp = (tap.gainTo - tap.gainFrom) / float(frames);
        sum += signals[tap.signal * frames + f] * (tap.gainFrom + ramp * float(f + 1u));
    }
    partial[index] = sum;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void reduceMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.x + id.y * params.rowThreads;
    uint frames = params.frames;
    if (index >= params.destinationCount * frames) {
        return;
    }
    uint destination = index / frames;
    uint f = index - destination * frames;
    GpuRange range = ranges[destination];
    float sum = 0.0;
    for (uint k = 0u; k < range.itemCount; k++) {
        sum += partial[(range.firstItem + k) * frames + f];
    }
    mix[index] = sum;
}
