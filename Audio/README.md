# Audio

An engine-owned audio graph: no platform audio API in the mixing path, optional acoustics, any speaker layout, several
outputs at once. Device sinks and platform spatial renderers plug in at the edges.

## What it does

| Need | How |
|---|---|
| **Procedural sound** | `PatchBuilder` graphs (oscillators, noise, ADSR, filters, delays, math, live parameters) play through the same voices as samples; `CallbackSource`, `RingSource` (fed from any thread) and raw `DataSource` implementations for anything else. DSP library: biquads, SVF, PolyBLEP oscillators, FDN reverb, Karplus-Strong, modal banks, limiter, compressor. |
| **3D spatial audio** | VBAP onto any layout, third-order ambisonic encode/decode, distance models, Doppler, air absorption, binaural (built-in spherical-head filter; `BinauralFilter` is the slot for SOFA HRTFs). |
| **Surround** | `SpeakerLayout` presets: mono, stereo, quad, 5.1, 7.1, 5.1.4, 7.1.4, 9.1.6; any custom list of speaker angles works. |
| **Atmos-class output** | `OutputDesc::Kind::Objects`: a static bed plus up to N positioned mono objects with ADM-style metadata (position, spread, gain) per block. No licensed code involved: hand `AudioEngine::objects()` to Windows Spatial Audio, a Dolby-capable renderer or an IAMF writer. Without a consumer the same output pans into its bed. |
| **Multiple outputs** | `AudioEngineConfig::outputs` (TV + headset + second speaker); a voice can play on buses of several outputs. `DeviceSink` opens one device per output. |
| **Cross platform** | miniaudio (WASAPI, CoreAudio, ALSA/Pulse/PipeWire, AAudio, Web Audio) behind `DeviceSink`; the mixer itself is plain C++ and also renders offline or into a test. |
| **Raytraced audio, optional** | `AcousticsProvider`: `NullAcoustics` (default: distance only) or `RaycastAcoustics` over any `AudioRayScene` (a CPU `TriangleBvh` is included; adapt the renderer's BVH or hardware ray queries by implementing one method). Gives occlusion by material, diffraction detours, and a room estimate for reverb sends. Games that do not call `AudioWorld::set_acoustics` pay nothing. |
| **World vs listener-local sources** | `SourceSpace` / `AudioAttachment`: `World` (fixed point), `Entity` (follows an entity's transform plus a local offset, with Doppler) or `Listener` (head-locked, never trails the player). |
| **Contact sounds** | `ContactSoundSystem` turns `Physics::ContactEvent`s into modal impacts keyed by the shared material table. |

## Using it from the engine

```cpp
engine.audio().enable(Audio::AudioEngineConfig{
    .outputs = {Audio::OutputDesc{.kind = Audio::OutputDesc::Kind::Speakers, .name = "main",
                                  .layout = Audio::SpeakerLayout::surround_7_1()}}});

world.spawn(Engine::WorldTransform{...}, Engine::AudioListener{});                       // the camera
world.spawn(Engine::WorldTransform{...},                                                 // a radio, parented by following
            Engine::AudioSource{.sound = Engine::sound_buffer(assets, radio), .loop = true,
                                .attachment = Engine::AudioAttachment::Entity});
world.spawn(Engine::WorldTransform{}, Engine::AudioSource{.sound = voice_line,           // the player's own voice line
                                .attachment = Engine::AudioAttachment::Listener, .offset = {0, 0, -0.3f}});

// Optional raytraced acoustics:
engine.audio().set_acoustics(std::make_shared<Audio::RaycastAcoustics>(bvh, materials));
```

Threading: game-thread calls enqueue commands on a lock-free ring and the audio thread applies them at block
boundaries, never allocating or blocking; retired voices and effects travel back on a second ring and are freed in
`AudioEngine::pump()`.

## Codecs

Everything is on by default and can be switched off at configure time: `-DSTURDY_AUDIO_CODECS=OFF` drops the whole family
(WAV and AIFF stay), or switch single ones with `STURDY_AUDIO_MP3`, `_FLAC`, `_VORBIS`, `_OPUS`, `_PLATFORM`, and
`STURDY_AUDIO_ENCODERS` for the writers. Image codecs follow the same pattern (`STURDY_IMAGE_CODECS`, `STURDY_IMAGE_WEBP`, `_AVIF`,
`_JXL`, `_JP2`, `_TIFF`, `_EXR`), and the GLFW window provider is now on by default (`-DSTURDY_BUILD_GLFW_WINDOW_PROVIDER=OFF`).

| Format | Read | Write |
|---|---|---|
| WAV (PCM 8/16/24/32, float, ADPCM; RF64; speaker masks, cue points, loops, INFO tags) | miniaudio | in-house (RF64 beyond 4 GB) |
| Wave64 (very long multichannel recordings) | miniaudio | in-house |
| AIFF / AIFF-C (PCM, float, markers) | in-house | in-house |
| FLAC | miniaudio | libFLAC (0-8 effort, up to 8 channels) |
| MP3 | miniaudio | libmp3lame loaded at run time (LGPL, never linked), `STURDY_LAME_PATH` to point at it |
| Ogg Vorbis (any channel count, loop tags) | stb_vorbis | libvorbis (quality or ABR/CBR bitrate) |
| Opus (1-255 channels: stereo, surround, ambisonics, discrete) | libopus + libogg, sample-accurate seek | libopus (bitrate, VBR, complexity, frame size) |
| AAC / M4A / ALAC / WMA | the OS (Media Foundation, AudioToolbox) | AAC via the OS; ALAC on macOS |
| raw PCM | - | in-house |

One options struct covers every writer (`EncodeOptions`: format, sample format, dither, bitrate, quality, VBR, per-codec settings,
sample-rate and channel conversion, gain, tags, markers, loop). `encode_buffer` writes a decoded buffer, `open_encoder` takes a
live stream block by block, `transcode` converts any file to any format in constant memory, and `AudioRecorder` records a
microphone or network stream to any of them.

Cue points and loop regions come out of the file as named markers. Short sounds are decoded once (`SoundManager::play_file`,
cached); long ones stream from a shared decode pool (`stream_file`); files at or above 1 MiB are memory mapped, so a file larger than RAM
costs address space, not memory. All file access goes through `Foundation::Io`: read-ahead hints, mapping, positional reads
from any thread, atomic writes, and io_uring/DirectStorage for big bulk loads.

## Controlling sounds from code

Every sound is a `SoundHandle`; every source can be pitched (sources that cannot resample themselves are wrapped in a rate
converter), faded, paused, sought and watched:

```cpp
Audio::SoundManager sounds(engine);                      // or AudioWorld::sounds()
auto theme = *sounds.stream_file("music/boss.ogg", {.spatial = false});
theme.set_pitch_semitones(+2);                           // key change (speed follows pitch)
theme.fade_volume(0.3f, 2.0f);                           // duck over two seconds
theme.seek(32.0);                                        // jump anywhere (a few ms of fade hides the seam)

// Waypoints: names, positions, and what happens there, sample accurately.
theme.add_marker("drop", 64.0);
theme.on_marker("drop", [](const Audio::MarkerHit &hit) { camera_shake(); });
theme.jump_at_marker("verse_end", /*target=*/16.0);      // loop a section until told otherwise
theme.set_loop(16.0, 32.0);                              // or a loop region (-1 = forever, n = n times then continue)
theme.stop_at_marker("outro");

sounds.update();                                         // once per frame: runs callbacks on the game thread
```

`PlayParams` carries the same things up front (`markers`, `loop`, `start_seconds`, `start_delay_seconds`, `start_at_seconds` on
the engine clock, `fade_in_seconds`, `declick_seconds`); `SoundCallbacks` adds `on_started/on_marker/on_loop/on_jump/on_paused/
on_finished`. `VoiceStatus` (`handle.position_seconds()`, `playing()`, ...) is a lock-free readout, safe to poll every frame.

## Game-audio workloads

- **Sound cues** (`SoundCue`/`CuePlayer`): variations with weights, round-robin/no-repeat/layered selection, random volume and pitch,
  instance limits (reject or steal the oldest), cooldowns. A hundred footsteps stop sounding like one sample.
- **Music** (`MusicPlayer`): crossfades, beat/bar-quantised switches, gapless queued tracks (the successor is scheduled on the engine
  clock before the current one ends), stingers on the next beat, `beat_position()` for rhythm gameplay.
- **Ducking** (`add_ducking`): dialogue pulls music down and releases it, per bus, with attack and release.
- **Scheduling**: `start_at_seconds` / `start_delay_seconds` start any sound at an exact sample of the engine clock.
- **Metering and visualisers**: `bus_levels`, `copy_output_tap`, `SpectrumAnalyzer` (FFT, log bands, centroid).
- **Capture**: `CaptureDevice` turns a microphone into a source (voice chat, recording); `RingSource` feeds PCM from any thread.
- **Offline rendering**: `render_offline`/`render_offline_to_wav` render an engine faster than real time, scripted by clock time.
- **In ECS**: `AudioSource` has `markers`, `loop_region`, `start_seconds`, a `handle` for gameplay control, and sends `AudioSourceEvent`
  (marker / loop / jump / finished) through the ECS event stream.

## Channels

A source's channels have a meaning (`ChannelLayoutInfo`): speaker roles (from WAVE masks, Vorbis/Opus mapping, 5.1 and so on), an
ambisonic sound field (ACN/SN3D, orders 1-3), or independent signals (a 32-channel interface). Non-spatial voices are folded onto
the output by role (ITU-R BS.775 downmix), discrete channels pass through by index, and ambisonic sources turn with the listener and
decode to whatever speakers (or headphones) are in use. Helpers: `make_channel_matrix`, `AmbisonicRotator`, `ambisonic_a_to_b_format`
(tetrahedral mics), `wave_channel_mask`.

## Effects and filters

Fifteen effects share one interface and run on a bus, on a single voice (`PlayParams::effects`), or offline (`apply_effects`):
filter (low/high/band-pass, notch, all-pass, peak, shelves, Butterworth slopes up to 48 dB/oct), 8-band equalizer, DC blocker,
noise gate (hysteresis, hold, look-ahead, side-chain filter), expander, compressor (soft knee, RMS/peak, look-ahead, parallel
mix), look-ahead limiter, de-esser, hum filter, delay, chorus/flanger/phaser/tremolo/vibrato, distortion/bit crusher, stereo
width, gain/pan, partitioned-FFT convolution (`make_convolution`), and noise reduction. Every control is a named, ranged parameter, settable by
name from code or data (`EffectSpec`, `engine.set_effect_parameter(bus, 0, "gain", -6.0f)`). Spectral noise reduction is `EffectKind::NoiseReduction` (`Denoise.hpp`): a spectral Wiener-style reducer that tracks the noise floor per bin on its own, or
uses a profile learned from a noise-only stretch (`NoiseReducer::begin_learning`/`end_learning`, `learn_noise_profile`, offline `reduce_noise`).

## Live audio: microphones and network

`CaptureDevice` opens any input at the channel count the hardware offers (a stereo mic, a 32-channel interface, an ambisonic
array) and exposes it as a lock-free `LiveSource` with a latency cushion and clock-drift correction; `add_tap()` gives recorders and meters
their own copies, `take_channel_peak` drives level meters. Audio over IP is RTP over UDP, unicast or multicast, uncompressed
(L16/L24, the AES67 payload) or Opus, for up to 255 channels: `RtpSender`/`RtpReceiver`, `NetworkOutputSink` (send a secondary engine
output as it is mixed), SDP in and out, a jitter buffer with reordering and loss concealment. The packet layer works without
sockets (`RtpPacketizer`, `RtpStreamDecoder`) so other transports can carry it. RTCP (`Rtcp.hpp`) runs beside it on port + 1: sender reports with NTP/RTP timestamps and CNAME,
receiver reports with loss and jitter, round-trip time from LSR/DLSR (`RtpSender::peer_stats()`, `RtpReceiver::sender_info()`), BYE on shutdown. PTP clocking and HTTP radio streams are not implemented.

## Editing, loudness and display

`audio::edit` has slice/remove/insert/concat (with crossfades), reverse, repeat, mix, fades, gain envelopes, normalise (peak or LUFS), DC removal,
silence detection and trimming, channel and rate conversion, mid/side, varispeed, WSOLA time stretch, pitch shift and quantise, all keeping
markers aligned. `measure_loudness`/`LoudnessMeter` give BS.1770/R128 loudness, range and true peak. For display, `PeakPyramid` summarises
any length of audio at every zoom (build it while recording, from a file in one streaming pass, cached on disk), and the renderers draw waveforms
(envelope, RMS core, line, bars; stacked/overlaid channels, dB scale, markers, selection, playhead), spectrograms (linear/log/mel, six
colour maps), oscilloscopes, goniometers and bar meters into RGBA images (`write_png` for files).

## Scale and hardware

Thousands of simultaneous voices are normal: 3000 playing 2D sounds mix in about 0.3 ms per 256-frame block on one thread, 3000 spatialised ones in
3.5 ms (1 ms with helper threads). Voices are slots with generations (stale commands cannot hit a successor) and are recycled; mixing
spreads over helper threads once enough voices are audible; voices too quiet to hear cost nothing. Inner loops run on AVX2+FMA (x86-64), NEON
(ARM64) or the compiler's RVV vectorisation (RISC-V builds with the V extension), chosen at start-up; denormals are flushed and the audio
threads ask the OS for audio scheduling (MMCSS, SCHED_FIFO, time-constraint policy).

Beyond the SIMD kernels and helper threads, the mixer avoids work wherever it can: positions the game sets go straight into a seqlock table (no command),
per-voice gain/direction maths runs in parallel once voices are numerous, buses nothing fed (and whose effects have stopped ringing) are skipped with their
effects, a sample played at its own rate is copied rather than interpolated, resampling at a rational ratio uses an exact polyphase table, streamed files share
a decode thread pool, and the acoustics scheduler only re-traces voices whose geometry or position changed, ranked by staleness over distance within a ray budget.
Acoustic queries use ordered, early-out BVH traversal over packed triangles, any-hit rays for occlusion, and a cache of the listener's bounce paths.

### Compute mode (GPU mixing)

For tens of thousands of voices, `AudioEngineConfig::compute_backend` hands the arithmetic of every plain loaded-sample voice to a `ComputeMixBackend`
(`Audio/Compute.hpp`): resample (Catmull-Rom), mix down, air-absorption low-pass, gain-ramped sum into bus channels. Voices with effects, markers, aux sends, a
delay line, binaural/object/ambisonic output, streams or live sources stay on the CPU mixer, as does everything in blocks with fewer than `compute_min_voices`
eligible voices. The mixer plans each block (advancing sources exactly as `read` would), submits at the start, mixes the rest on the CPU, and adds the result at the end.
`make_cpu_compute_backend()` runs the same arithmetic serially (the reference); the `AudioGpu` package's `GpuComputeMixer` runs it on any RHI device
(three kernels in `Shaders/audio_mix.slang`: per-voice, per-run-of-taps partial sums, per-destination reduce; samples live in a video-memory pool).
`Engine::create_gpu_audio_mixer(engine.rhi_device())` makes one. Measured on an RX 9070 with 8192 audible voices: 12 ms/block on one CPU thread, 2.6 ms through the
device, matching the CPU to 4e-6. A sample becomes usable on the device a block or two after first use (it plays on the CPU meanwhile). Limits: the sample pool
is 128 MiB by default (bigger samples stay on the CPU), and the backend borrows the device, so drop it before switching or destroying the graphics backend.

## Custom sinks

`AudioSink` (`Sink.hpp`) is the interface for anything that wants the mixed output: implement `open(format)`, `write(interleaved, frames)` and
`close()`, or wrap a lambda in `CallbackSink`. A `SinkPump` carries one output of an engine to it on a thread of its own: a secondary output
follows whoever pulls the primary, and the primary output can be driven by the pump itself (real-time paced, or free-running for offline
rendering), so a custom sink can replace the device. `NetworkOutputSink` is built on the same pump.

## Speaker layouts

Outputs may have up to 32 channels. Presets: mono, stereo, 3.0, quad, 5.0, 5.1, 6.1, 7.1, 7.1.2, 5.1.4, 7.1.4, 9.1.4, 9.1.6 and NHK 22.2 (24 channels,
two LFEs, floor layer). `SpeakerLayout::from_channel_count(n)` gives the conventional layout for any count a device reports (an even
ring for unusual ones) and `SpeakerLayout::custom` takes azimuth/elevation pairs and works out the channel roles.

## Containers

`Demuxer` (`Demux.hpp`) reads Matroska/WebM and MP4/MOV (progressive and fragmented) from memory or a mapped file and hands out the audio packets as views
into the file, with timestamps, keyframe flags, codec names and codec setup data (OpusHead, AudioSpecificConfig, STREAMINFO ...): the input a video player
needs for `MediaAudioStream`. As a decoder backend it also opens such files as audio: Opus, Vorbis, FLAC, MP3 and PCM inside them decode through the
normal registry (the packets are re-wrapped as the codec's own file format in memory). AAC in a container needs an OS decoder (Media Foundation, AudioToolbox);
this build bundles none.

## Measured HRTFs

`HrtfSet::from_sofa_file` (`Hrtf.hpp`) loads SOFA `SimpleFreeFieldHRIR` sets (MIT KEMAR, CIPIC, ARI, LISTEN ...) through a small read-only HDF5 reader (`Hdf5.hpp`:
superblock 0-3, object headers 1-2, old and new groups incl. dense storage, chunked data with deflate/shuffle/fletcher32), resamples them to the engine rate and
splits each response into a delay-free impulse response and an onset delay, so blending neighbouring directions keeps the interaural time difference clean.
`OutputDesc::binaural = make_hrtf_filter_factory(set)` plugs it into a Binaural output. Checked against the MIT KEMAR set (710 directions): the far ear is
about 8 dB down and 0.85 ms late for a source at 90 degrees. Each tap costs a multiply-add per sample per binaural voice (256 by default).

## C ABI

`FFI/src/FFI/Sturdy.h` (ABI 0.31) exposes audio to Rust, C# and C: `sturdy_audio_enable` (any layout up to 32 channels), sounds and voices, entity-attached sources
(world, entity-following, listener-locked), user-fed streams for voice chat and video, microphones, custom sinks (a callback receiving the mixed output) and effects by
catalogue name. `FfiAudioTest` covers it headless.

## Audio for video

`MediaAudioStream` takes decoded audio with timestamps from a demuxer/decoder thread (bounded ring: the soundtrack is never held whole,
so it can be larger than RAM), fills timeline gaps with silence, trims overlaps, flushes on seek, and reports `playback_pts()`, the media
time the speakers are playing. `MediaClock` follows it for the video side (`until`, `should_drop`), and `DeviceSink::output_latency_seconds()` supplies the
device delay. The `Demuxer` (see Containers) supplies the packets.

## Raytraced acoustics

`RaycastAcoustics` (optional; the default is plain distance attenuation) shapes each source from the actual geometry. Six effects, each with its own
settings struct and an on/off switch, globally (`RaycastAcousticsSettings`, changeable at run time with `set_settings`) and per source
(`AudioSource::acoustic_effects` mask plus `muffling_scale`, `reverb_scale`, `delay_scale`, `doppler_scale`):

| Effect | What it does | Main controls |
|---|---|---|
| Muffling | walls dull and quieten a sound by what each band transmits; with no route at all it is faint and very dull | `strength`, `min/max_cutoff_hz`, `min_gain`, `no_path_cutoff_hz`, `no_path_gain`, occlusion rays, diffraction probes |
| Directionality | the arrival direction is the line of sight averaged with where the sound last bounced (the surface nearest the listener on each reflected path, energy-weighted) | `bounce_blend` (0 line of sight only, 1 bounce only), `min_path_power` |
| Propagation delay | distance divided by the speed of sound, along the shortest route that exists (direct, reflected or round a corner) | `speed_of_sound`, `delay_scale`, `max_delay_seconds` |
| Reverb | the listener's room is surveyed with rays: decay time per band (Eyring), send level, extra send for muffled sources; `AudioWorld::set_reverb_bus` makes a reverb bus follow it | `room_rays`, `rt60_scale`, `send_scale`, `min/max_rt60`, `occluded_boost` |
| Material bounces | rays from the listener bounce up to N times, losing energy per band by each material's reflectivity and scattering diffusely or like a mirror; mirror echoes are found exactly with the source's image in each wall | `rays`, `max_bounces`, `gain`, `specular_paths`, `jitter` |
| Doppler | pitch shift from the speeds along the path the sound arrives by | `mode` (Off, Pitch = resample, Delay = let the changing delay bend it), `factor`, `min/max_ratio` |

Materials (`AcousticMaterial`) have absorption and transmission per band (low, mid, high) and `scattering` (0 mirror, 1 diffuse); `with_defaults()` has concrete, brick,
wood, glass, drywall, carpet, metal, fabric and foliage. The geometry is a `TriangleBvh` with `BvhSettings`: `voxel_size` is the resolution, in metres per voxel: triangles smaller than a voxel are merged into larger quads by
material and facing (0 keeps every triangle; 0.05 furniture detail, 0.25 cluttered rooms, 1.0 buildings), `max_triangles` is a hard budget, and the tree builds with a median or
binned-SAH split and configurable leaf size. Any other ray source plugs in through `AudioRayScene`. With pitch Doppler the delay carries only a detour's excess, so the pitch is not bent twice.

### Baked fields and background evaluation

`AcousticField::bake(acoustics, {min, max, cell_size})` measures the reverberant character of a space ahead of time (in parallel, one room survey per grid cell),
`RaycastAcoustics::set_baked_field` makes the provider read the listener's room from it by trilinear lookup instead of tracing room rays, and the field serialises to
bytes to ship with the level. In the engine the acoustics evaluation itself runs on a scheduler worker (`AudioWorld::set_acoustics_async`, on by default): a frame never waits
for rays, results are applied at the next `end_frame` after the job finishes, and `AudioWorld::edit_acoustics(fn)` is the safe way to change a provider's settings or scene.

## Data-driven audio and tempo maps

`AudioData.hpp` loads synthesis patches, sound cues, effect chains and tempo maps from JSON (comments allowed; errors name the offending node, parameter or file).
`TempoMap` (`Tempo.hpp`) handles music whose tempo or time signature changes: beats and seconds convert both ways across changes, bar lines restart at a new signature, and
`beat_markers` lays named waypoints on the grid. `MusicTrack::tempo` replaces the single `bpm` for quantised transitions, stingers and `beat_position()`.

## Not done yet

A Windows `ISpatialAudioClient` object sink, MP3/WMA export through the OS, PTP clocking, a software AAC decoder (AAC in containers needs an OS codec),
SOFA conventions other than free-field HRIR, and putting streamed voices and per-voice effects on the GPU mixer (they stay on the CPU: resampling a loaded sample is
what the device is good at). The Windows and macOS platform codecs (AAC/ALAC export, M4A/WMA import) were written without a
machine to build them on; the Linux build covers everything else and is tested.
