# Codec selection. Everything is on by default; each codec can be dropped for a leaner build or because a platform cannot
# build it. A master switch turns a whole family off at once, and the per-codec switches then have no effect:
#
#   cmake -DSTURDY_IMAGE_CODECS=OFF -DSTURDY_IMAGE_WEBP=ON ...   # master wins: nothing is built (use the per-codec switches
#                                                                  with the master left ON to drop individual codecs)
#
# Effective values are plain (non-cache) variables so the cache keeps what the user typed. A file in a disabled format is
# still recognised and fails with an error that names the missing codec instead of a generic decode failure.

option(STURDY_IMAGE_CODECS "Build the optional image codecs (WebP, AVIF, JPEG XL, JPEG 2000, TIFF, OpenEXR). PNG/JPEG/GIF/BMP/TGA/PSD/HDR/PNM stay in stb_image." ON)
set(STURDY_IMAGE_CODEC_LIST WEBP AVIF JXL JP2 TIFF EXR)
foreach(_codec IN LISTS STURDY_IMAGE_CODEC_LIST)
    option(STURDY_IMAGE_${_codec} "Build the ${_codec} image codec." ON)
    if(NOT STURDY_IMAGE_CODECS)
        set(STURDY_IMAGE_${_codec} OFF)
    endif()
endforeach()

option(STURDY_AUDIO_CODECS "Build the optional audio codecs. WAV and AIFF stay in-house." ON)
# MP3 and FLAC ride on miniaudio's decoders, Vorbis on stb_vorbis, Opus on libopus (+libogg), the encoders on libFLAC,
# libvorbis/libogg and libopus, and PLATFORM is the OS framework (AAC/M4A/ALAC/WMA; Media Foundation, AudioToolbox).
set(STURDY_AUDIO_CODEC_LIST MP3 FLAC VORBIS OPUS PLATFORM)
foreach(_codec IN LISTS STURDY_AUDIO_CODEC_LIST)
    option(STURDY_AUDIO_${_codec} "Build the ${_codec} audio codec." ON)
    if(NOT STURDY_AUDIO_CODECS)
        set(STURDY_AUDIO_${_codec} OFF)
    endif()
endforeach()
option(STURDY_AUDIO_ENCODERS "Build the audio encoders (FLAC, Ogg Vorbis, Opus); WAV/AIFF writers are always present." ON)
if(NOT STURDY_AUDIO_CODECS)
    set(STURDY_AUDIO_ENCODERS OFF)
endif()
# The encoders need the same libraries as the decoders they mirror.
if(NOT STURDY_AUDIO_FLAC AND NOT STURDY_AUDIO_VORBIS AND NOT STURDY_AUDIO_OPUS)
    set(STURDY_AUDIO_ENCODERS OFF)
endif()
# libogg is shared by Vorbis and Opus (container) and FLAC-in-Ogg.
if(STURDY_AUDIO_VORBIS OR STURDY_AUDIO_OPUS)
    set(STURDY_AUDIO_NEEDS_OGG ON)
else()
    set(STURDY_AUDIO_NEEDS_OGG OFF)
endif()
