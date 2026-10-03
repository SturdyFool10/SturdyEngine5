#include <Audio/Inflate.hpp>

#include <array>
#include <cstdint>

namespace SFT::Audio {

    namespace {
        class Bits {
          public:
            explicit Bits(std::span<const std::byte> data) : data_(data) {}
            [[nodiscard]] bool ok() const noexcept { return !overrun_; }
            u32 get(u32 count) {
                u32 value = 0;
                for (u32 i = 0; i < count; ++i) value |= static_cast<u32>(bit()) << i;
                return value;
            }
            int bit() {
                if (byte_ >= data_.size()) {
                    overrun_ = true;
                    return 0;
                }
                const int b = (static_cast<u8>(data_[byte_]) >> bit_) & 1;
                if (++bit_ == 8) {
                    bit_ = 0;
                    ++byte_;
                }
                return b;
            }
            void align() {
                if (bit_ != 0) {
                    bit_ = 0;
                    ++byte_;
                }
            }
            [[nodiscard]] usize position() const noexcept { return byte_; }
            void skip(usize n) { byte_ += n; }
            [[nodiscard]] std::span<const std::byte> rest() const noexcept { return byte_ <= data_.size() ? data_.subspan(byte_) : std::span<const std::byte>{}; }
            void fail() noexcept { overrun_ = true; }

          private:
            std::span<const std::byte> data_;
            usize byte_ = 0;
            u32 bit_ = 0;
            bool overrun_ = false;
        };

        /// Canonical Huffman decoding by code length counts (the "puff" scheme): small, no tables to build.
        struct Huffman {
            std::array<u16, 16> count{};
            std::array<u16, 288> symbol{};

            bool build(std::span<const u8> lengths) {
                count.fill(0);
                for (const u8 l : lengths) ++count[l];
                if (count[0] == lengths.size()) return true; // no codes: legal for an unused distance tree
                i32 left = 1;
                for (int len = 1; len < 16; ++len) {
                    left <<= 1;
                    left -= count[len];
                    if (left < 0) return false; // over-subscribed
                }
                std::array<u16, 16> offsets{};
                for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<u16>(offsets[len] + count[len]);
                for (usize s = 0; s < lengths.size(); ++s) {
                    if (lengths[s] != 0) symbol[offsets[lengths[s]]++] = static_cast<u16>(s);
                }
                return true;
            }

            int decode(Bits &bits) const {
                int code = 0, first = 0, index = 0;
                for (int len = 1; len < 16; ++len) {
                    code |= bits.bit();
                    const int n = count[len];
                    if (code - n < first) return symbol[index + (code - first)];
                    index += n;
                    first += n;
                    first <<= 1;
                    code <<= 1;
                    if (!bits.ok()) return -1;
                }
                return -1;
            }
        };

        constexpr std::array<u16, 29> kLengthBase = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        constexpr std::array<u8, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        constexpr std::array<u16, 30> kDistBase = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        constexpr std::array<u8, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

        bool codes(Bits &bits, std::vector<std::byte> &out, const Huffman &lit, const Huffman &dist, usize limit) {
            for (;;) {
                const int symbol = lit.decode(bits);
                if (symbol < 0) return false;
                if (symbol < 256) {
                    out.push_back(static_cast<std::byte>(symbol));
                } else if (symbol == 256) {
                    return true;
                } else {
                    const int s = symbol - 257;
                    if (s >= 29) return false;
                    const u32 length = kLengthBase[s] + bits.get(kLengthExtra[s]);
                    const int d = dist.decode(bits);
                    if (d < 0 || d >= 30) return false;
                    const u32 distance = kDistBase[d] + bits.get(kDistExtra[d]);
                    if (distance > out.size()) return false;
                    for (u32 i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
                }
                if (!bits.ok() || out.size() > limit) return false;
            }
        }
    } // namespace

    std::expected<std::vector<std::byte>, UString> inflate(std::span<const std::byte> compressed, bool zlib_header, usize expected_size) {
        Bits bits(compressed);
        if (zlib_header) {
            if (compressed.size() < 2 || ((static_cast<u32>(static_cast<u8>(compressed[0])) << 8) | static_cast<u8>(compressed[1])) % 31 != 0 || (static_cast<u8>(compressed[0]) & 0x0F) != 8) {
                return std::unexpected("audio: bad zlib header");
            }
            bits.skip(2);
        }
        const usize limit = expected_size != 0 ? expected_size + 65536 : usize{1} << 31;
        std::vector<std::byte> out;
        if (expected_size != 0) out.reserve(expected_size);
        bool last = false;
        while (!last) {
            last = bits.bit() != 0;
            const u32 type = bits.get(2);
            if (!bits.ok()) return std::unexpected("audio: truncated deflate stream");
            if (type == 0) {
                bits.align();
                const auto rest = bits.rest();
                if (rest.size() < 4) return std::unexpected("audio: truncated stored block");
                const u32 length = static_cast<u8>(rest[0]) | (static_cast<u32>(static_cast<u8>(rest[1])) << 8);
                const u32 inverse = static_cast<u8>(rest[2]) | (static_cast<u32>(static_cast<u8>(rest[3])) << 8);
                if ((length ^ 0xFFFF) != inverse || rest.size() < 4 + static_cast<usize>(length)) return std::unexpected("audio: damaged stored block");
                out.insert(out.end(), rest.begin() + 4, rest.begin() + 4 + length);
                bits.skip(4 + length);
            } else if (type == 1 || type == 2) {
                Huffman lit, dist;
                std::array<u8, 288 + 32> lengths{};
                if (type == 1) {
                    for (int i = 0; i < 144; ++i) lengths[i] = 8;
                    for (int i = 144; i < 256; ++i) lengths[i] = 9;
                    for (int i = 256; i < 280; ++i) lengths[i] = 7;
                    for (int i = 280; i < 288; ++i) lengths[i] = 8;
                    lit.build(std::span<const u8>(lengths.data(), 288));
                    std::array<u8, 30> fixed_dist{};
                    fixed_dist.fill(5);
                    dist.build(fixed_dist);
                } else {
                    const u32 nlen = bits.get(5) + 257, ndist = bits.get(5) + 1, ncode = bits.get(4) + 4;
                    if (nlen > 286 || ndist > 30) return std::unexpected("audio: damaged dynamic Huffman header");
                    constexpr std::array<u8, 19> order = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                    std::array<u8, 19> code_lengths{};
                    for (u32 i = 0; i < ncode; ++i) code_lengths[order[i]] = static_cast<u8>(bits.get(3));
                    Huffman code_tree;
                    if (!code_tree.build(code_lengths)) return std::unexpected("audio: damaged Huffman code lengths");
                    u32 index = 0;
                    while (index < nlen + ndist) {
                        const int symbol = code_tree.decode(bits);
                        if (symbol < 0) return std::unexpected("audio: damaged Huffman table");
                        if (symbol < 16) {
                            lengths[index++] = static_cast<u8>(symbol);
                        } else {
                            u8 previous = 0;
                            u32 repeat;
                            if (symbol == 16) {
                                if (index == 0) return std::unexpected("audio: damaged Huffman table");
                                previous = lengths[index - 1];
                                repeat = 3 + bits.get(2);
                            } else if (symbol == 17) {
                                repeat = 3 + bits.get(3);
                            } else {
                                repeat = 11 + bits.get(7);
                            }
                            if (index + repeat > nlen + ndist) return std::unexpected("audio: damaged Huffman table");
                            while (repeat-- > 0) lengths[index++] = previous;
                        }
                    }
                    if (!lit.build(std::span<const u8>(lengths.data(), nlen)) || !dist.build(std::span<const u8>(lengths.data() + nlen, ndist))) {
                        return std::unexpected("audio: damaged Huffman table");
                    }
                }
                if (!codes(bits, out, lit, dist, limit)) return std::unexpected("audio: damaged deflate data");
            } else {
                return std::unexpected("audio: unknown deflate block type");
            }
            if (!bits.ok()) return std::unexpected("audio: truncated deflate stream");
        }
        return out;
    }

} // namespace SFT::Audio
