#include <Audio/Hdf5.hpp>

#include <Audio/Inflate.hpp>
#include <Audio/Text.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <set>

namespace SFT::Audio {

    namespace {
        constexpr u64 kUndefined = ~0ull;

        /// Bounds-checked little-endian reader; a read past the end sets `failed` and yields zeros.
        struct Reader {
            std::span<const std::byte> data;
            usize pos = 0;
            bool failed = false;

            [[nodiscard]] bool has(usize n) const noexcept { return pos <= data.size() && n <= data.size() - pos; }
            u64 uint(u32 bytes) {
                if (!has(bytes) || bytes > 8) {
                    failed = true;
                    pos = data.size();
                    return 0;
                }
                u64 v = 0;
                for (u32 i = 0; i < bytes; ++i) v |= static_cast<u64>(static_cast<u8>(data[pos + i])) << (8 * i);
                pos += bytes;
                return v;
            }
            void skip(usize n) {
                if (!has(n)) failed = true;
                pos = std::min(data.size(), pos + n);
            }
            std::span<const std::byte> bytes(usize n) {
                if (!has(n)) {
                    failed = true;
                    pos = data.size();
                    return {};
                }
                const auto out = data.subspan(pos, n);
                pos += n;
                return out;
            }
        };

        struct Message {
            u32 type = 0;
            u32 flags = 0;
            std::span<const std::byte> body;
        };

        struct DataType {
            enum class Kind { Integer, Float, String, Other } kind = Kind::Other;
            u32 size = 0;
            bool big_endian = false;
            bool is_signed = false;
        };

        struct Filter {
            u32 id = 0;
            std::vector<u32> values;
        };

        struct Layout {
            enum class Kind { None, Compact, Contiguous, Chunked } kind = Kind::None;
            u64 address = kUndefined; ///< data (contiguous) or chunk B-tree (chunked)
            u64 size = 0;
            std::vector<u32> chunk_dims; ///< including the trailing element-size entry for chunked
            std::span<const std::byte> compact;
        };

        struct ObjectInfo {
            std::vector<u64> shape;
            bool has_dataspace = false;
            DataType type;
            Layout layout;
            std::vector<Filter> filters;
            std::vector<Message> messages;
        };

        f64 convert(std::span<const std::byte> element, const DataType &t) {
            u64 raw = 0;
            for (u32 i = 0; i < t.size && i < 8; ++i) {
                const u32 at = t.big_endian ? t.size - 1 - i : i;
                raw |= static_cast<u64>(static_cast<u8>(element[at])) << (8 * i);
            }
            if (t.kind == DataType::Kind::Float) {
                if (t.size == 4) return std::bit_cast<f32>(static_cast<u32>(raw));
                if (t.size == 8) return std::bit_cast<f64>(raw);
                return 0.0;
            }
            if (t.is_signed && t.size < 8) {
                const u64 sign = u64{1} << (8 * t.size - 1);
                return static_cast<f64>(static_cast<i64>((raw ^ sign) - sign));
            }
            if (t.is_signed) return static_cast<f64>(static_cast<i64>(raw));
            return static_cast<f64>(raw);
        }
    } // namespace

    struct Hdf5File::Impl {
        std::span<const std::byte> file;
        u32 offset_size = 8, length_size = 8;
        u64 base = 0;
        u64 root = kUndefined;
        UString error;

        [[nodiscard]] Reader at(u64 address) const {
            Reader r{file, 0};
            if (address == kUndefined || address + base > file.size()) {
                r.failed = true;
                r.pos = file.size();
            } else {
                r.pos = static_cast<usize>(address + base);
            }
            return r;
        }
        u64 offset(Reader &r) const {
            const u64 v = r.uint(offset_size);
            const u64 all_ones = offset_size == 8 ? ~0ull : (u64{1} << (8 * offset_size)) - 1;
            return v == all_ones ? kUndefined : v;
        }

        // ---- object headers ----------------------------------------------------------------------------------------------

        void collect_v1(Reader r, usize bytes, std::vector<Message> &out, int depth) const {
            const usize end = r.pos + bytes;
            while (r.pos + 8 <= end && !r.failed) {
                Message m;
                m.type = static_cast<u32>(r.uint(2));
                const u32 size = static_cast<u32>(r.uint(2));
                m.flags = static_cast<u32>(r.uint(1));
                r.skip(3);
                m.body = r.bytes(size);
                if (r.failed) return;
                if (m.type == 0x10 && depth < 16) {
                    Reader c{m.body, 0};
                    const u64 address = offset(c);
                    const u64 length = c.uint(length_size);
                    Reader block = at(address);
                    if (!block.failed && block.has(static_cast<usize>(length))) collect_v1(block, static_cast<usize>(length), out, depth + 1);
                } else {
                    out.push_back(m);
                }
            }
        }

        void collect_v2(Reader r, usize end, bool track_order, std::vector<Message> &out, int depth) const {
            while (r.pos + 4 <= end && !r.failed) {
                Message m;
                m.type = static_cast<u32>(r.uint(1));
                const u32 size = static_cast<u32>(r.uint(2));
                m.flags = static_cast<u32>(r.uint(1));
                if (track_order) r.skip(2);
                m.body = r.bytes(size);
                if (r.failed) return;
                if (m.type == 0x10 && depth < 16) {
                    Reader c{m.body, 0};
                    const u64 address = offset(c);
                    const u64 length = c.uint(length_size);
                    Reader block = at(address);
                    if (block.failed || length < 8 || !block.has(static_cast<usize>(length))) continue;
                    const usize block_end = block.pos + static_cast<usize>(length) - 4; // checksum
                    block.skip(4);                                                         // "OCHK"
                    collect_v2(block, block_end, track_order, out, depth + 1);
                } else {
                    out.push_back(m);
                }
            }
        }

        std::expected<std::vector<Message>, UString> messages(u64 address) const {
            Reader r = at(address);
            std::vector<Message> out;
            if (r.failed) return std::unexpected("hdf5: an object header lies outside the file");
            const auto sig = r.bytes(4);
            if (!r.failed && std::memcmp(sig.data(), "OHDR", 4) == 0) {
                const u32 version = static_cast<u32>(r.uint(1));
                const u32 flags = static_cast<u32>(r.uint(1));
                if (version != 2) return std::unexpected("hdf5: unknown object header version");
                if (flags & 0x20) r.skip(16);
                if (flags & 0x10) r.skip(4);
                const u32 size_bytes = 1u << (flags & 3);
                const u64 chunk = r.uint(size_bytes);
                if (r.failed || !r.has(static_cast<usize>(chunk))) return std::unexpected("hdf5: a damaged object header");
                collect_v2(r, r.pos + static_cast<usize>(chunk), (flags & 0x04) != 0, out, 0);
                return out;
            }
            r = at(address);
            const u32 version = static_cast<u32>(r.uint(1));
            if (version != 1) return std::unexpected("hdf5: unknown object header version");
            r.skip(1);
            r.uint(2); // message count
            r.uint(4); // reference count
            const u64 size = r.uint(4);
            r.skip(4); // padding to 8 bytes
            if (r.failed || !r.has(static_cast<usize>(size))) return std::unexpected("hdf5: a damaged object header");
            collect_v1(r, static_cast<usize>(size), out, 0);
            return out;
        }

        // ---- messages ----------------------------------------------------------------------------------------------------

        [[nodiscard]] static DataType parse_datatype(std::span<const std::byte> body, usize *consumed = nullptr) {
            Reader r{body, 0};
            DataType t;
            const u32 first = static_cast<u32>(r.uint(1));
            const u32 cls = first & 0x0F;
            const u32 bits = static_cast<u32>(r.uint(3));
            t.size = static_cast<u32>(r.uint(4));
            t.big_endian = (bits & 1) != 0;
            if (cls == 0) {
                t.kind = DataType::Kind::Integer;
                t.is_signed = (bits & 0x08) != 0;
                r.skip(4);
            } else if (cls == 1) {
                t.kind = DataType::Kind::Float;
                r.skip(12);
            } else if (cls == 3) {
                t.kind = DataType::Kind::String;
            }
            if (consumed != nullptr) *consumed = r.pos;
            return t;
        }

        [[nodiscard]] std::vector<u64> parse_dataspace(std::span<const std::byte> body, bool &ok) const {
            Reader r{body, 0};
            const u32 version = static_cast<u32>(r.uint(1));
            const u32 dims = static_cast<u32>(r.uint(1));
            const u32 flags = static_cast<u32>(r.uint(1));
            (void)flags;
            std::vector<u64> shape;
            ok = true;
            if (version == 1) {
                r.skip(5);
            } else if (version == 2) {
                const u32 type = static_cast<u32>(r.uint(1));
                if (type == 0 || type == 2) { // scalar / null
                    return shape;
                }
            } else {
                ok = false;
                return shape;
            }
            for (u32 i = 0; i < dims; ++i) shape.push_back(r.uint(length_size));
            ok = !r.failed;
            return shape;
        }

        [[nodiscard]] static std::vector<Filter> parse_filters(std::span<const std::byte> body) {
            Reader r{body, 0};
            const u32 version = static_cast<u32>(r.uint(1));
            const u32 count = static_cast<u32>(r.uint(1));
            std::vector<Filter> out;
            if (version == 1) r.skip(6);
            for (u32 i = 0; i < count && !r.failed; ++i) {
                Filter f;
                f.id = static_cast<u32>(r.uint(2));
                u32 name_length = 0;
                if (version == 1 || f.id >= 256) name_length = static_cast<u32>(r.uint(2));
                r.skip(2); // flags
                const u32 values = static_cast<u32>(r.uint(2));
                if (version == 1) name_length = (name_length + 7) & ~7u;
                r.skip(name_length);
                for (u32 v = 0; v < values; ++v) f.values.push_back(static_cast<u32>(r.uint(4)));
                if (version == 1 && (values % 2) == 1) r.skip(4);
                out.push_back(std::move(f));
            }
            return out;
        }

        [[nodiscard]] std::expected<Layout, UString> parse_layout(std::span<const std::byte> body) const {
            Reader r{body, 0};
            const u32 version = static_cast<u32>(r.uint(1));
            Layout l;
            if (version != 3) return std::unexpected(UString{std::format("hdf5: data layout version {} is not supported (version 3 is)", version)});
            const u32 cls = static_cast<u32>(r.uint(1));
            if (cls == 0) {
                l.kind = Layout::Kind::Compact;
                const u32 size = static_cast<u32>(r.uint(2));
                l.compact = r.bytes(size);
            } else if (cls == 1) {
                l.kind = Layout::Kind::Contiguous;
                l.address = offset(r);
                l.size = r.uint(length_size);
            } else if (cls == 2) {
                l.kind = Layout::Kind::Chunked;
                const u32 dims = static_cast<u32>(r.uint(1));
                l.address = offset(r);
                for (u32 i = 0; i < dims; ++i) l.chunk_dims.push_back(static_cast<u32>(r.uint(4)));
            } else {
                return std::unexpected("hdf5: unknown data layout class");
            }
            if (r.failed) return std::unexpected("hdf5: a damaged data layout message");
            return l;
        }

        std::expected<ObjectInfo, UString> object(u64 address) const {
            auto msgs = messages(address);
            if (!msgs) return std::unexpected(std::move(msgs.error()));
            ObjectInfo info;
            for (const Message &m : *msgs) {
                if (m.type == 0x01) {
                    bool ok = false;
                    info.shape = parse_dataspace(m.body, ok);
                    if (!ok) return std::unexpected("hdf5: unsupported dataspace");
                    info.has_dataspace = true;
                } else if (m.type == 0x03) {
                    info.type = parse_datatype(m.body);
                } else if (m.type == 0x08) {
                    auto layout = parse_layout(m.body);
                    if (!layout) return std::unexpected(std::move(layout.error()));
                    info.layout = *layout;
                } else if (m.type == 0x0B) {
                    info.filters = parse_filters(m.body);
                }
            }
            info.messages = std::move(*msgs);
            return info;
        }

        // ---- groups ------------------------------------------------------------------------------------------------------

        /// Decodes a link message body (also the payload of dense link records); returns name and object address for hard links.
        [[nodiscard]] std::optional<std::pair<std::string, u64>> parse_link(std::span<const std::byte> body) const {
            Reader r{body, 0};
            const u32 version = static_cast<u32>(r.uint(1));
            if (version != 1) return std::nullopt;
            const u32 flags = static_cast<u32>(r.uint(1));
            u32 type = 0;
            if (flags & 0x08) type = static_cast<u32>(r.uint(1));
            if (flags & 0x04) r.skip(8);
            if (flags & 0x10) r.skip(1);
            const u64 name_length = r.uint(1u << (flags & 3));
            const auto name = r.bytes(static_cast<usize>(name_length));
            if (r.failed || type != 0) return std::nullopt; // soft and external links are not followed
            const u64 address = offset(r);
            return std::pair{std::string(reinterpret_cast<const char *>(name.data()), name.size()), address};
        }

        // Old-style: a B-tree of symbol nodes with names in a local heap.
        void symbol_table_links(u64 btree, u64 heap, std::map<std::string, u64> &out, int depth = 0) const {
            if (depth > 16) return;
            Reader h = at(heap);
            h.skip(4 + 4);
            h.uint(length_size);
            h.uint(length_size);
            const u64 data_address = offset(h);
            Reader t = at(btree);
            const auto sig = t.bytes(4);
            if (t.failed || std::memcmp(sig.data(), "TREE", 4) != 0) return;
            t.skip(1); // node type
            const u32 level = static_cast<u32>(t.uint(1));
            const u32 used = static_cast<u32>(t.uint(2));
            offset(t);
            offset(t);
            t.uint(length_size); // first key
            for (u32 i = 0; i < used && !t.failed; ++i) {
                const u64 child = offset(t);
                t.uint(length_size); // key after
                if (level > 0) {
                    symbol_table_links(child, heap, out, depth + 1);
                    continue;
                }
                Reader s = at(child);
                const auto ssig = s.bytes(4);
                if (s.failed || std::memcmp(ssig.data(), "SNOD", 4) != 0) continue;
                s.skip(2);
                const u32 entries = static_cast<u32>(s.uint(2));
                for (u32 e = 0; e < entries && !s.failed; ++e) {
                    const u64 name_offset = s.uint(length_size);
                    const u64 address = offset(s);
                    s.skip(4 + 4 + 16);
                    Reader n = at(data_address + name_offset);
                    std::string name;
                    while (!n.failed && n.has(1)) {
                        const char c = static_cast<char>(n.uint(1));
                        if (c == 0) break;
                        name.push_back(c);
                    }
                    out[name] = address;
                }
            }
        }

        struct FractalHeap {
            u32 id_length = 0, offset_bits = 0, length_bytes = 0, width = 0, io_filter_length = 0;
            u64 start_block = 0, max_direct = 0, root = kUndefined, max_managed = 0;
            u32 current_rows = 0;
            bool checksummed = false;
        };

        std::optional<FractalHeap> read_heap(u64 address) const {
            Reader r = at(address);
            const auto sig = r.bytes(4);
            if (r.failed || std::memcmp(sig.data(), "FRHP", 4) != 0) return std::nullopt;
            r.skip(1);
            FractalHeap h;
            h.id_length = static_cast<u32>(r.uint(2));
            h.io_filter_length = static_cast<u32>(r.uint(2));
            const u32 flags = static_cast<u32>(r.uint(1));
            h.checksummed = (flags & 2) != 0;
            h.max_managed = r.uint(4);
            r.uint(length_size);  // next huge id
            offset(r);            // huge object b-tree
            r.uint(length_size);  // free space
            offset(r);            // free space manager
            r.uint(length_size);  // managed space
            r.uint(length_size);  // allocated managed space
            r.uint(length_size);  // iterator offset
            r.uint(length_size);  // managed objects
            r.uint(length_size);  // huge size
            r.uint(length_size);  // huge count
            r.uint(length_size);  // tiny size
            r.uint(length_size);  // tiny count
            h.width = static_cast<u32>(r.uint(2));
            h.start_block = r.uint(length_size);
            h.max_direct = r.uint(length_size);
            h.offset_bits = static_cast<u32>(r.uint(2));
            r.uint(2); // starting rows in root indirect block
            h.root = offset(r);
            h.current_rows = static_cast<u32>(r.uint(2));
            if (r.failed) return std::nullopt;
            const u64 largest = std::min(h.max_direct, h.max_managed);
            h.length_bytes = 1;
            while ((u64{1} << (8 * h.length_bytes)) <= largest && h.length_bytes < 8) ++h.length_bytes;
            return h;
        }

        /// The bytes of one managed object in the heap, by heap ID.
        std::optional<std::span<const std::byte>> heap_object(const FractalHeap &h, std::span<const std::byte> id) const {
            if (id.empty() || ((static_cast<u8>(id[0]) >> 4) & 3) != 0) return std::nullopt; // only managed objects
            Reader r{id, 1};
            const u32 offset_bytes = (h.offset_bits + 7) / 8;
            const u64 heap_offset = r.uint(offset_bytes);
            const u64 length = r.uint(h.length_bytes);
            if (r.failed) return std::nullopt;
            const u64 header = 5 + offset_size + offset_bytes + (h.checksummed ? 4 : 0);
            (void)header;
            // Find the direct block covering `heap_offset`.
            const auto in_direct = [&](u64 block_address, u64 block_offset, u64 block_size) -> std::optional<std::span<const std::byte>> {
                if (heap_offset < block_offset || heap_offset + length > block_offset + block_size) return std::nullopt;
                const u64 position = block_address + base + (heap_offset - block_offset);
                if (position + length > file.size()) return std::nullopt;
                return file.subspan(static_cast<usize>(position), static_cast<usize>(length));
            };
            if (h.current_rows == 0) return in_direct(h.root, 0, h.start_block);
            Reader b = at(h.root);
            const auto sig = b.bytes(4);
            if (b.failed || std::memcmp(sig.data(), "FHIB", 4) != 0) return std::nullopt;
            b.skip(1);
            offset(b);
            b.skip(offset_bytes);
            u64 block_offset = 0;
            u32 direct_rows = 2;
            for (u64 s = h.start_block; s < h.max_direct; s <<= 1) ++direct_rows;
            for (u32 row = 0; row < h.current_rows; ++row) {
                const u64 size = row == 0 ? h.start_block : (h.start_block << (row - 1));
                for (u32 col = 0; col < h.width; ++col) {
                    const u64 child = offset(b);
                    if (h.io_filter_length != 0 && row < direct_rows) b.skip(length_size + 4);
                    if (b.failed) return std::nullopt;
                    if (row >= direct_rows) return std::nullopt; // nested indirect blocks: not needed for link storage this small
                    if (child != kUndefined) {
                        if (auto found = in_direct(child, block_offset, size)) return found;
                    }
                    block_offset += size;
                }
            }
            return std::nullopt;
        }

        // B-tree v2 (type 5: link names), walked for its heap IDs.
        void btree2_records(u64 address, std::vector<std::vector<std::byte>> &ids, u32 id_begin, u32 id_length, u32 record_size, u32 node_size, u32 depth, u32 records, u32 max_leaf,
                            u32 max_size, const std::vector<u64> &cumulative) const {
            Reader r = at(address);
            const auto sig = r.bytes(4);
            if (r.failed) return;
            const bool leaf = std::memcmp(sig.data(), "BTLF", 4) == 0;
            if (!leaf && std::memcmp(sig.data(), "BTIN", 4) != 0) return;
            r.skip(2); // version, type
            std::vector<std::vector<std::byte>> mine;
            for (u32 i = 0; i < records && !r.failed; ++i) {
                const auto rec = r.bytes(record_size);
                if (rec.size() >= id_begin + id_length) ids.emplace_back(rec.begin() + id_begin, rec.begin() + id_begin + id_length);
            }
            if (leaf || depth == 0) return;
            u32 total_bytes = 0;
            if (depth > 1) {
                total_bytes = 1;
                while ((u64{1} << (8 * total_bytes)) <= cumulative[depth - 1] && total_bytes < 8) ++total_bytes;
            }
            for (u32 i = 0; i <= records && !r.failed; ++i) {
                const u64 child = offset(r);
                const u32 child_records = static_cast<u32>(r.uint(max_size));
                if (depth > 1) r.skip(total_bytes);
                btree2_records(child, ids, id_begin, id_length, record_size, node_size, depth - 1, child_records, max_leaf, max_size, cumulative);
            }
        }

        /// Heap IDs from a name-index B-tree v2 (type 5: link names; type 8: attribute names).
        std::vector<std::vector<std::byte>> dense_ids(u64 btree) const {
            std::vector<std::vector<std::byte>> ids;
            Reader r = at(btree);
            const auto sig = r.bytes(4);
            if (r.failed || std::memcmp(sig.data(), "BTHD", 4) != 0) return ids;
            r.skip(1);
            const u32 type = static_cast<u32>(r.uint(1));
            const u32 node_size = static_cast<u32>(r.uint(4));
            const u32 record_size = static_cast<u32>(r.uint(2));
            const u32 depth = static_cast<u32>(r.uint(2));
            r.skip(2);
            const u64 root = offset(r);
            const u32 root_records = static_cast<u32>(r.uint(2));
            if (r.failed || (type != 5 && type != 8) || root == kUndefined || record_size < 11) return ids;
            const u32 id_begin = type == 5 ? 4 : 0, id_length = type == 5 ? 7 : 8;
            const u32 max_leaf = (node_size - 10) / record_size;
            u32 max_size = 1;
            while ((u64{1} << (8 * max_size)) <= max_leaf && max_size < 8) ++max_size;
            std::vector<u64> cumulative(depth + 1);
            cumulative[0] = max_leaf;
            for (u32 d = 1; d <= depth; ++d) {
                const u32 total_bytes = d > 1 ? 4 : 0; // generous: only used for sizing the skip, recomputed per level above
                const u32 pointer = offset_size + max_size + total_bytes;
                const u32 max_rec = (node_size - 10 - pointer) / (record_size + pointer);
                cumulative[d] = (static_cast<u64>(max_rec) + 1) * cumulative[d - 1] + max_rec;
            }
            btree2_records(root, ids, id_begin, id_length, record_size, node_size, depth, root_records, max_leaf, max_size, cumulative);
            return ids;
        }

        /// Child objects of the group whose object header is `info`.
        std::map<std::string, u64> children(const ObjectInfo &info) const {
            std::map<std::string, u64> out;
            for (const Message &m : info.messages) {
                if (m.type == 0x06) {
                    if (auto link = parse_link(m.body)) out[link->first] = link->second;
                } else if (m.type == 0x11) {
                    Reader r{m.body, 0};
                    const u64 btree = offset(r);
                    const u64 heap = offset(r);
                    symbol_table_links(btree, heap, out);
                } else if (m.type == 0x02) {
                    Reader r{m.body, 0};
                    r.skip(1);
                    const u32 flags = static_cast<u32>(r.uint(1));
                    if (flags & 1) r.skip(8);
                    const u64 heap_address = offset(r);
                    const u64 btree_address = offset(r);
                    if (heap_address == kUndefined || btree_address == kUndefined || r.failed) continue;
                    const auto heap = read_heap(heap_address);
                    if (!heap) continue;
                    for (const auto &id : dense_ids(btree_address)) {
                        if (const auto body = heap_object(*heap, id)) {
                            if (auto link = parse_link(*body)) out[link->first] = link->second;
                        }
                    }
                }
            }
            return out;
        }

        std::optional<u64> resolve(std::string_view path) const {
            u64 current = root;
            usize start = 0;
            while (start <= path.size()) {
                const usize slash = path.find('/', start);
                const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
                if (!part.empty()) {
                    const auto info = object(current);
                    if (!info) return std::nullopt;
                    const auto kids = children(*info);
                    const auto it = kids.find(std::string(part));
                    if (it == kids.end()) return std::nullopt;
                    current = it->second;
                }
                if (slash == std::string_view::npos) break;
                start = slash + 1;
            }
            return current;
        }

        // ---- data --------------------------------------------------------------------------------------------------------

        struct ChunkEntry {
            std::vector<u64> offsets;
            u32 size = 0, mask = 0;
            u64 address = 0;
        };

        void chunk_tree(u64 address, u32 dims, std::vector<ChunkEntry> &out, int depth = 0) const {
            if (depth > 16) return;
            Reader r = at(address);
            const auto sig = r.bytes(4);
            if (r.failed || std::memcmp(sig.data(), "TREE", 4) != 0) return;
            r.skip(1);
            const u32 level = static_cast<u32>(r.uint(1));
            const u32 used = static_cast<u32>(r.uint(2));
            offset(r);
            offset(r);
            const auto read_key = [&](ChunkEntry &e) {
                e.size = static_cast<u32>(r.uint(4));
                e.mask = static_cast<u32>(r.uint(4));
                for (u32 d = 0; d < dims; ++d) e.offsets.push_back(r.uint(8));
            };
            for (u32 i = 0; i < used && !r.failed; ++i) {
                ChunkEntry e;
                read_key(e);
                e.address = offset(r);
                if (level > 0) chunk_tree(e.address, dims, out, depth + 1);
                else out.push_back(std::move(e));
            }
        }

        std::expected<std::vector<std::byte>, UString> unfilter(std::span<const std::byte> raw, const std::vector<Filter> &filters, u32 mask, usize expected) const {
            std::vector<std::byte> data(raw.begin(), raw.end());
            for (usize i = filters.size(); i-- > 0;) {
                if ((mask >> i) & 1) continue; // skipped for this chunk
                const Filter &f = filters[i];
                if (f.id == 1) {
                    auto inflated = inflate(data, true, expected);
                    if (!inflated) return std::unexpected(std::move(inflated.error()));
                    data = std::move(*inflated);
                } else if (f.id == 2) {
                    const usize width = f.values.empty() ? 1 : f.values[0];
                    if (width > 1 && data.size() >= width) {
                        const usize count = data.size() / width;
                        std::vector<std::byte> out(data.size());
                        for (usize b = 0; b < width; ++b) {
                            for (usize e = 0; e < count; ++e) out[e * width + b] = data[b * count + e];
                        }
                        std::copy(data.begin() + static_cast<std::ptrdiff_t>(count * width), data.end(), out.begin() + static_cast<std::ptrdiff_t>(count * width));
                        data = std::move(out);
                    }
                } else if (f.id == 3) {
                    if (data.size() >= 4) data.resize(data.size() - 4);
                } else {
                    return std::unexpected(UString{std::format("hdf5: the compression filter {} is not supported", f.id)});
                }
            }
            return data;
        }

        std::expected<Hdf5Array, UString> read_array(const ObjectInfo &info) const {
            if (!info.has_dataspace || info.layout.kind == Layout::Kind::None) return std::unexpected("hdf5: the object is not a dataset");
            if (info.type.kind != DataType::Kind::Integer && info.type.kind != DataType::Kind::Float) return std::unexpected("hdf5: only numeric datasets can be read");
            Hdf5Array out;
            out.shape = info.shape;
            usize count = 1;
            for (const u64 d : info.shape) count *= static_cast<usize>(d);
            const u32 width = info.type.size;
            if (width == 0 || width > 8) return std::unexpected("hdf5: unsupported element size");
            if (count > (usize{1} << 31)) return std::unexpected("hdf5: the dataset is too large");
            out.values.assign(count, 0.0);
            if (info.layout.kind == Layout::Kind::Compact || info.layout.kind == Layout::Kind::Contiguous) {
                std::span<const std::byte> bytes = info.layout.compact;
                if (info.layout.kind == Layout::Kind::Contiguous) {
                    if (info.layout.address == kUndefined) return out; // never written: all zero
                    Reader r = at(info.layout.address);
                    bytes = r.bytes(count * width);
                    if (r.failed) return std::unexpected("hdf5: dataset data runs past the end of the file");
                }
                for (usize i = 0; i < count && (i + 1) * width <= bytes.size(); ++i) out.values[i] = convert(bytes.subspan(i * width, width), info.type);
                return out;
            }
            // Chunked.
            const u32 dims = static_cast<u32>(info.shape.size());
            if (info.layout.chunk_dims.size() != dims + 1 || dims == 0) return std::unexpected("hdf5: inconsistent chunk dimensions");
            std::vector<u64> chunk(info.layout.chunk_dims.begin(), info.layout.chunk_dims.end() - 1);
            usize chunk_elements = 1;
            for (const u64 c : chunk) chunk_elements *= static_cast<usize>(c);
            std::vector<ChunkEntry> entries;
            if (info.layout.address != kUndefined) chunk_tree(info.layout.address, dims + 1, entries);
            std::vector<u64> strides(dims, 1);
            for (u32 d = dims - 1; d-- > 0;) strides[d] = strides[d + 1] * info.shape[d + 1];
            for (const ChunkEntry &e : entries) {
                Reader r = at(e.address);
                const auto raw = r.bytes(e.size);
                if (r.failed) return std::unexpected("hdf5: a chunk runs past the end of the file");
                auto data = unfilter(raw, info.filters, e.mask, chunk_elements * width);
                if (!data) return std::unexpected(std::move(data.error()));
                if (data->size() < chunk_elements * width) return std::unexpected("hdf5: a chunk is shorter than its shape says");
                // Scatter the chunk's elements to their places, clipping the part of an edge chunk that lies outside the array.
                std::vector<u64> index(dims, 0);
                for (usize element = 0; element < chunk_elements; ++element) {
                    bool inside = true;
                    usize target = 0;
                    for (u32 d = 0; d < dims; ++d) {
                        const u64 position = e.offsets[d] + index[d];
                        if (position >= info.shape[d]) inside = false;
                        target += static_cast<usize>(position * strides[d]);
                    }
                    if (inside) out.values[target] = convert(std::span<const std::byte>(*data).subspan(element * width, width), info.type);
                    for (u32 d = dims; d-- > 0;) {
                        if (++index[d] < chunk[d]) break;
                        index[d] = 0;
                    }
                }
            }
            return out;
        }

        std::optional<std::pair<DataType, std::span<const std::byte>>> attribute(const ObjectInfo &info, std::string_view name, std::vector<u64> *shape) const {
            std::vector<std::span<const std::byte>> bodies;
            for (const Message &m : info.messages) {
                if (m.type == 0x0C) {
                    bodies.push_back(m.body);
                } else if (m.type == 0x15) { // attribute info: attributes stored densely in a fractal heap
                    Reader a{m.body, 0};
                    a.skip(1);
                    const u32 flags = static_cast<u32>(a.uint(1));
                    if (flags & 1) a.skip(2);
                    const u64 heap_address = offset(a);
                    const u64 btree_address = offset(a);
                    if (a.failed || heap_address == kUndefined || btree_address == kUndefined) continue;
                    if (const auto heap = read_heap(heap_address)) {
                        for (const auto &id : dense_ids(btree_address)) {
                            if (const auto body = heap_object(*heap, id)) bodies.push_back(*body);
                        }
                    }
                }
            }
            for (const std::span<const std::byte> body : bodies) {
                Reader r{body, 0};
                const u32 version = static_cast<u32>(r.uint(1));
                if (version < 1 || version > 3) continue;
                const u32 flags = static_cast<u32>(r.uint(1));
                (void)flags;
                const u32 name_size = static_cast<u32>(r.uint(2));
                const u32 type_size = static_cast<u32>(r.uint(2));
                const u32 space_size = static_cast<u32>(r.uint(2));
                if (version == 3) r.skip(1);
                const auto pad = [&](u32 n) { return version == 1 ? (n + 7) & ~7u : n; };
                const auto attr_name = r.bytes(name_size);
                r.skip(pad(name_size) - name_size);
                const auto type_bytes = r.bytes(type_size);
                r.skip(pad(type_size) - type_size);
                const auto space_bytes = r.bytes(space_size);
                r.skip(pad(space_size) - space_size);
                if (r.failed) continue;
                std::string_view attr(reinterpret_cast<const char *>(attr_name.data()), attr_name.size());
                while (!attr.empty() && attr.back() == '\0') attr.remove_suffix(1);
                if (attr != name) continue;
                bool ok = false;
                std::vector<u64> dims = parse_dataspace(space_bytes, ok);
                if (!ok) continue;
                if (shape != nullptr) *shape = dims;
                return std::pair{parse_datatype(type_bytes), body.subspan(r.pos)};
            }
            return std::nullopt;
        }
    };

    Hdf5File::Hdf5File(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    Hdf5File::~Hdf5File() = default;

    std::expected<std::unique_ptr<Hdf5File>, UString> Hdf5File::open(std::span<const std::byte> file) {
        static constexpr u8 kSignature[8] = {0x89, 'H', 'D', 'F', 0x0D, 0x0A, 0x1A, 0x0A};
        usize start = 0;
        bool found = false;
        for (usize candidate = 0; candidate + 8 <= file.size(); candidate = candidate == 0 ? 512 : candidate * 2) {
            if (std::memcmp(file.data() + candidate, kSignature, 8) == 0) {
                start = candidate;
                found = true;
                break;
            }
        }
        if (!found) return std::unexpected("hdf5: not an HDF5 file");
        auto impl = std::make_unique<Impl>();
        impl->file = file;
        Reader r{file, start + 8};
        const u32 version = static_cast<u32>(r.uint(1));
        if (version <= 1) {
            r.skip(3); // free space, root table, reserved
            r.skip(1); // shared header version
            impl->offset_size = static_cast<u32>(r.uint(1));
            impl->length_size = static_cast<u32>(r.uint(1));
            r.skip(1 + 2 + 2 + 4);
            if (version == 1) r.skip(4);
            impl->base = r.uint(impl->offset_size);
            r.uint(impl->offset_size);
            r.uint(impl->offset_size);
            r.uint(impl->offset_size);
            r.uint(impl->offset_size); // root link name offset
            impl->root = r.uint(impl->offset_size);
        } else if (version == 2 || version == 3) {
            impl->offset_size = static_cast<u32>(r.uint(1));
            impl->length_size = static_cast<u32>(r.uint(1));
            r.skip(1);
            impl->base = r.uint(impl->offset_size);
            r.uint(impl->offset_size);
            r.uint(impl->offset_size);
            impl->root = r.uint(impl->offset_size);
        } else {
            return std::unexpected("hdf5: unknown superblock version");
        }
        if (r.failed || impl->offset_size == 0 || impl->offset_size > 8 || impl->length_size == 0 || impl->length_size > 8) return std::unexpected("hdf5: a damaged superblock");
        impl->base += start;
        return std::unique_ptr<Hdf5File>(new Hdf5File(std::move(impl)));
    }

    bool Hdf5File::exists(std::string_view path) const { return impl_->resolve(path).has_value(); }

    std::expected<Hdf5Array, UString> Hdf5File::read(std::string_view path) const {
        const auto address = impl_->resolve(path);
        if (!address) return std::unexpected(UString{std::format("hdf5: no object '{}'", path)});
        const auto info = impl_->object(*address);
        if (!info) return std::unexpected(info.error());
        return impl_->read_array(*info);
    }

    std::optional<UString> Hdf5File::string_attribute(std::string_view path, std::string_view name) const {
        const auto address = impl_->resolve(path);
        if (!address) return std::nullopt;
        const auto info = impl_->object(*address);
        if (!info) return std::nullopt;
        const auto attr = impl_->attribute(*info, name, nullptr);
        if (!attr || attr->first.kind != DataType::Kind::String) return std::nullopt;
        std::string_view text(reinterpret_cast<const char *>(attr->second.data()), std::min<usize>(attr->first.size, attr->second.size()));
        const usize nul = text.find('\0');
        if (nul != std::string_view::npos) text = text.substr(0, nul);
        return text_from_bytes(text);
    }

    std::optional<std::vector<f64>> Hdf5File::number_attribute(std::string_view path, std::string_view name) const {
        const auto address = impl_->resolve(path);
        if (!address) return std::nullopt;
        const auto info = impl_->object(*address);
        if (!info) return std::nullopt;
        std::vector<u64> shape;
        const auto attr = impl_->attribute(*info, name, &shape);
        if (!attr || (attr->first.kind != DataType::Kind::Integer && attr->first.kind != DataType::Kind::Float) || attr->first.size == 0) return std::nullopt;
        usize count = 1;
        for (const u64 d : shape) count *= static_cast<usize>(d);
        std::vector<f64> out;
        for (usize i = 0; i < count && (i + 1) * attr->first.size <= attr->second.size(); ++i) out.push_back(convert(attr->second.subspan(i * attr->first.size, attr->first.size), attr->first));
        return out;
    }

} // namespace SFT::Audio
