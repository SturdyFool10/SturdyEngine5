# Sturdy.Audio — the engine-owned audio graph: planar buffers, a DSP node library, data sources (decoded buffers,
# streamed files, generators, synthesis patches), a codec registry, VBAP spatialisation, voices with virtualisation, waypoints
# (markers) with jumps and callbacks, buses with effects, and a lock-free command path between the game thread and whatever
# pulls blocks (an `IAudioDevice` sink on miniaudio, WASAPI, a file writer, or a test). Nothing here opens a device or includes
# a platform audio header in the mixing path.
#
# Codecs: WAV/FLAC/MP3 (miniaudio), AIFF (in-house), Ogg Vorbis (stb_vorbis), Opus (libopus + libogg), and AAC/M4A/ALAC/WMA
# through the OS where it has a framework (Media Foundation, AudioToolbox) so no royalty-bearing codec is bundled. Encoders:
# WAV/AIFF in-house, FLAC (libFLAC), Ogg Vorbis (libvorbis), Opus (libopus), AAC/MP3 through the OS where it can.
sturdy_inactive_os_dirs(_sturdy_audio_platform_dirs
    DIRS Platform/Linux Platform/MacOS Platform/Web Platform/Windows)

# Codec switches come from cmake/SturdyCodecs.cmake (everything on by default). Each contributes its library and a 0/1
# define; the sources check the defines, so a disabled codec is recognised but reports itself as not built.
set(_sturdy_audio_private_deps Sturdy::miniaudio Sturdy::StbImage Sturdy::Json)
set(_sturdy_audio_private_defines)
foreach(_codec IN LISTS STURDY_AUDIO_CODEC_LIST)
    if(STURDY_AUDIO_${_codec})
        list(APPEND _sturdy_audio_private_defines STURDY_AUDIO_${_codec}=1)
    else()
        list(APPEND _sturdy_audio_private_defines STURDY_AUDIO_${_codec}=0)
    endif()
endforeach()
if(STURDY_AUDIO_NEEDS_OGG)
    list(APPEND _sturdy_audio_private_deps Sturdy::Ogg)
endif()
if(STURDY_AUDIO_OPUS)
    list(APPEND _sturdy_audio_private_deps Sturdy::Opus)
endif()
set(_sturdy_audio_encode_flac 0)
set(_sturdy_audio_encode_vorbis 0)
set(_sturdy_audio_encode_opus 0)
if(STURDY_AUDIO_ENCODERS AND STURDY_AUDIO_OPUS)
    set(_sturdy_audio_encode_opus 1)
endif()
if(STURDY_AUDIO_ENCODERS AND STURDY_AUDIO_FLAC)
    set(_sturdy_audio_encode_flac 1)
    list(APPEND _sturdy_audio_private_deps Sturdy::Flac)
endif()
if(STURDY_AUDIO_ENCODERS AND STURDY_AUDIO_VORBIS)
    set(_sturdy_audio_encode_vorbis 1)
    list(APPEND _sturdy_audio_private_deps Sturdy::VorbisEnc Sturdy::Vorbis)
endif()
# The encoder switches are public so tests (and tools) can tell which formats this build can write.
set(_sturdy_audio_public_defines
    STURDY_AUDIO_ENCODE_FLAC=${_sturdy_audio_encode_flac}
    STURDY_AUDIO_ENCODE_VORBIS=${_sturdy_audio_encode_vorbis}
    STURDY_AUDIO_ENCODE_OPUS=${_sturdy_audio_encode_opus})

sturdy_add_package(Audio
    ENGINE_OPTIMIZED

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_audio_platform_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::GLM
        Sturdy::Async
        Sturdy::Physics

    PRIVATE_DEPS
        ${_sturdy_audio_private_deps}

    PUBLIC_DEFINES
        ${_sturdy_audio_public_defines}

    PRIVATE_DEFINES
        ${_sturdy_audio_private_defines}
)

if(STURDY_OS STREQUAL "Windows")
    target_link_libraries(Audio PRIVATE mfplat mfreadwrite mfuuid shlwapi ole32 avrt ws2_32)
elseif(STURDY_OS STREQUAL "MacOS")
    target_link_libraries(Audio PRIVATE "-framework AudioToolbox" "-framework CoreFoundation")
endif()

sturdy_add_tests(Audio TIMEOUT 30)
