#pragma once

#include <Audio/Decoder.hpp>
#include <Audio/Text.hpp>

#include <string_view>

/// Vorbis-style comments ("KEY=value"), as carried by Ogg Vorbis, Opus and FLAC. The reader sorts them into plain tags and the
/// loop points that game audio tools write (LOOPSTART, LOOPLENGTH, LOOPEND, in frames).
namespace SFT::Audio {

    class VorbisCommentReader {
      public:
        explicit VorbisCommentReader(AudioStreamInfo &info) noexcept
            : info_(info) {
        }

        /// Takes one raw "KEY=value" entry; entries without an '=' are ignored.
        void add(std::string_view entry) {
            const usize eq = entry.find('=');
            if (eq == std::string_view::npos) {
                return;
            }
            UString key = ascii_lower(text_from_bytes(entry.substr(0, eq)));
            const UString value = text_from_bytes(entry.substr(eq + 1));
            if (key == "loopstart"_ustr) {
                loop_start_ = number_or<u64>(value, 0);
                has_start_ = true;
            } else if (key == "looplength"_ustr) {
                loop_length_ = number_or<u64>(value, 0);
            } else if (key == "loopend"_ustr) {
                loop_end_ = number_or<u64>(value, 0);
            } else {
                info_.tags.emplace_back(std::move(key), value);
            }
        }

        /// Settles the loop region once every entry has been added. An end of 0 means "to the end of the stream", which is
        /// `info.total_frames` when the codec knows it.
        void finish() {
            if (!has_start_) {
                return;
            }
            const u64 end = loop_end_ > loop_start_ ? loop_end_ : (loop_length_ > 0 ? loop_start_ + loop_length_ : info_.total_frames);
            if (end > loop_start_) {
                info_.loop = LoopRegion{loop_start_, end};
            }
        }

      private:
        AudioStreamInfo &info_;
        u64 loop_start_ = 0, loop_length_ = 0, loop_end_ = 0;
        bool has_start_ = false;
    };

} // namespace SFT::Audio
