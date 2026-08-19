#include <Renderer/Text/Document.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <stdexcept>

namespace SFT::Text {
using std::make_shared;
using std::optional;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::vector;
namespace {

constexpr usize leaf_piece_capacity = 32;
constexpr usize branch_fanout = 32;
constexpr usize piece_target_bytes = 16 * 1024;


template <typename T, usize N>
class FixedVector {
  public:
    /// Constructs a `FixedVector` in its default state.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    FixedVector() = default;
    /// Constructs a `FixedVector` from the supplied initialization values.
    ///
    /// @param source Source value or resource.
    ///
    /// @pre `source.size() <= N`; debug builds assert if this precondition is violated.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    explicit FixedVector(vector<T> source) {
        assert(source.size() <= N);
        for (auto &item : source) storage_[count_++] = std::move(item);
    }
    /// Returns the size for this `FixedVector`.
    ///
    /// @return Returns the current size value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize size() const noexcept { return count_; }
    /// Reports whether this `FixedVector` contains no elements or payload.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    /// Accesses an element of the `FixedVector` by index or key.
    ///
    /// @param index Zero-based index of the target element or entry.
    ///
    /// @return Returns the selected element or a reference/proxy referring to it.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const T &operator[](usize index) const noexcept { return storage_[index]; }
    /// Returns the current or globally available front value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const T &front() const noexcept { return storage_[0]; }
    /// Returns the current or globally available back value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const T &back() const noexcept { return storage_[count_ - 1]; }
    /// Returns an iterator to the first element in the range.
    ///
    /// @return Returns an iterator referring to the first element.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const T *begin() const noexcept { return storage_.data(); }
    /// Returns the one-past-the-end iterator for the range.
    ///
    /// @return Returns the one-past-the-end iterator.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const T *end() const noexcept { return storage_.data() + count_; }

  private:
    std::array<T, N> storage_{};
    usize count_ = 0;
};

struct Piece {
    shared_ptr<const string> storage;
    usize start = 0;
    usize length = 0;
    TextSummary summary{};
};

/// Performs the continuation operation for `Text` using the supplied arguments.
///
/// @param byte `byte` value used by the operation.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool continuation(unsigned char byte) noexcept { return (byte & 0xc0u) == 0x80u; }


/// Decodes the supplied representation.
///
/// @param bytes Size of the relevant data in bytes.
/// @param available `available` value used by the operation.
/// @param scalar `scalar` value used by the operation.
/// @param width Width of the target extent.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool decode(const char *bytes, usize available, char32_t &scalar, usize &width) noexcept {
    if (available == 0) return false;
    const auto b0 = static_cast<unsigned char>(bytes[0]);
    if (b0 < 0x80u) { scalar = b0; width = 1; return true; }
    if (b0 >= 0xc2u && b0 <= 0xdfu && available >= 2 && continuation(static_cast<unsigned char>(bytes[1]))) {
        scalar = static_cast<char32_t>(((b0 & 0x1fu) << 6u) | (static_cast<unsigned char>(bytes[1]) & 0x3fu)); width = 2; return true;
    }
    if (b0 >= 0xe0u && b0 <= 0xefu && available >= 3 && continuation(static_cast<unsigned char>(bytes[1])) && continuation(static_cast<unsigned char>(bytes[2]))) {
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        if ((b0 == 0xe0u && b1 < 0xa0u) || (b0 == 0xedu && b1 >= 0xa0u)) return false;
        scalar = static_cast<char32_t>(((b0 & 0x0fu) << 12u) | ((b1 & 0x3fu) << 6u) | (static_cast<unsigned char>(bytes[2]) & 0x3fu)); width = 3; return true;
    }
    if (b0 >= 0xf0u && b0 <= 0xf4u && available >= 4 && continuation(static_cast<unsigned char>(bytes[1])) && continuation(static_cast<unsigned char>(bytes[2])) && continuation(static_cast<unsigned char>(bytes[3]))) {
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        if ((b0 == 0xf0u && b1 < 0x90u) || (b0 == 0xf4u && b1 > 0x8fu)) return false;
        scalar = static_cast<char32_t>(((b0 & 0x07u) << 18u) | ((b1 & 0x3fu) << 12u) | ((static_cast<unsigned char>(bytes[2]) & 0x3fu) << 6u) | (static_cast<unsigned char>(bytes[3]) & 0x3fu)); width = 4; return true;
    }
    return false;
}

/// Performs the summarize operation for `Text` using the supplied arguments.
///
/// @param text Text consumed by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<TextSummary> summarize(string_view text) noexcept {
    TextSummary result{};
    result.bytes = text.size();
    usize line_bytes = 0, line_scalars = 0, line_utf16 = 0;
    usize line_index = 0;
    bool have_first_line = false;
    for (usize i = 0; i < text.size();) {
        char32_t scalar{}; usize width{};
        if (!decode(text.data() + i, text.size() - i, scalar, width)) return nullopt;
        result.ascii = result.ascii && width == 1;
        ++result.scalars;
        result.utf16_code_units += scalar > 0xffff ? 2 : 1;
        if (scalar == U'\n') {
            if (!have_first_line) {
                result.first_line_bytes = line_bytes;
                result.first_line_scalars = line_scalars;
                result.first_line_utf16_code_units = line_utf16;
                have_first_line = true;
            }
            if (line_scalars > result.longest_line_scalars) { result.longest_line = line_index; result.longest_line_scalars = line_scalars; }
            ++result.newlines;
            ++line_index;
            line_bytes = line_scalars = line_utf16 = 0;
        } else {
            line_bytes += width;
            ++line_scalars;
            line_utf16 += scalar > 0xffff ? 2 : 1;
        }
        i += width;
    }
    if (!have_first_line) {
        result.first_line_bytes = line_bytes;
        result.first_line_scalars = line_scalars;
        result.first_line_utf16_code_units = line_utf16;
    }
    if (line_scalars > result.longest_line_scalars) { result.longest_line = line_index; result.longest_line_scalars = line_scalars; }
    result.last_line_bytes = line_bytes;
    result.last_line_scalars = line_scalars;
    result.last_line_utf16_code_units = line_utf16;
    return result;
}

/// Joins the associated asynchronous work and waits for completion.
///
/// @param lhs Left-hand operand.
/// @param rhs Right-hand operand.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] TextSummary join(TextSummary lhs, const TextSummary &rhs) noexcept {
    const bool rhs_has_line_break = rhs.newlines != 0;
    const bool lhs_has_line_break = lhs.newlines != 0;


    usize longest_line = lhs.longest_line;
    usize longest_line_scalars = lhs.longest_line_scalars;
    const usize boundary_scalars = lhs.last_line_scalars + rhs.first_line_scalars;
    if (boundary_scalars > longest_line_scalars) { longest_line = lhs.newlines; longest_line_scalars = boundary_scalars; }
    if (rhs.longest_line_scalars > longest_line_scalars) { longest_line = lhs.newlines + rhs.longest_line; longest_line_scalars = rhs.longest_line_scalars; }

    usize first_line_bytes = lhs.first_line_bytes;
    usize first_line_scalars = lhs.first_line_scalars;
    usize first_line_utf16 = lhs.first_line_utf16_code_units;
    if (!lhs_has_line_break) {
        first_line_bytes += rhs.first_line_bytes;
        first_line_scalars += rhs.first_line_scalars;
        first_line_utf16 += rhs.first_line_utf16_code_units;
    }

    lhs.bytes += rhs.bytes;
    lhs.scalars += rhs.scalars;
    lhs.utf16_code_units += rhs.utf16_code_units;
    lhs.newlines += rhs.newlines;
    lhs.last_line_bytes = rhs_has_line_break ? rhs.last_line_bytes : lhs.last_line_bytes + rhs.last_line_bytes;
    lhs.last_line_scalars = rhs_has_line_break ? rhs.last_line_scalars : lhs.last_line_scalars + rhs.last_line_scalars;
    lhs.last_line_utf16_code_units = rhs_has_line_break ? rhs.last_line_utf16_code_units : lhs.last_line_utf16_code_units + rhs.last_line_utf16_code_units;
    lhs.ascii = lhs.ascii && rhs.ascii;
    lhs.first_line_bytes = first_line_bytes;
    lhs.first_line_scalars = first_line_scalars;
    lhs.first_line_utf16_code_units = first_line_utf16;
    lhs.longest_line = longest_line;
    lhs.longest_line_scalars = longest_line_scalars;
    return lhs;
}

/// Performs the boundary operation for `Text` using the supplied arguments.
///
/// @param bytes Size of the relevant data in bytes.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the boolean result of the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool boundary(string_view bytes, usize offset) noexcept {
    return offset <= bytes.size() && (offset == 0 || offset == bytes.size() || !continuation(static_cast<unsigned char>(bytes[offset])));
}

/// Resolves the pieces associated with the supplied key, handle, or resource.
///
/// @param storage `storage` value used by the operation.
/// @param begin First position or element included in the operation.
/// @param length Requested or available size for the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @pre `next > cursor`; debug builds assert if this precondition is violated.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<Piece> pieces_for(shared_ptr<const string> storage, usize begin, usize length) {
    vector<Piece> result;
    const string_view all{*storage};
    usize cursor = begin;
    const usize end = begin + length;
    while (cursor < end) {
        usize next = std::min(cursor + piece_target_bytes, end);
        while (next < end && continuation(static_cast<unsigned char>(all[next]))) --next;
        assert(next > cursor);
        const string_view part = all.substr(cursor, next - cursor);
        result.push_back(Piece{storage, cursor, part.size(), *summarize(part)});
        cursor = next;
    }
    return result;
}

} // namespace

namespace Detail {
struct Node {
    TextSummary summary{};
    FixedVector<shared_ptr<const Node>, branch_fanout> children;
    FixedVector<Piece, leaf_piece_capacity> pieces;


    usize height = 1;
    /// Returns the current or globally available leaf value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool leaf() const noexcept { return children.empty(); }
};

struct DocumentState {
    shared_ptr<const Node> root;
    Revision revision{};
    shared_ptr<const DocumentState> parent;
    ChangeSet changes_from_parent{};


    EditKind kind_from_parent = EditKind::Standalone;
    bool continues_previous_group = false;
};
} // namespace Detail

namespace {
using Detail::Node;

/// Creates a leaf value from the supplied arguments.
///
/// @param pieces `pieces` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> make_leaf(vector<Piece> pieces) {
    auto node = make_shared<Node>();
    node->pieces = FixedVector<Piece, leaf_piece_capacity>{std::move(pieces)};
    for (const Piece &piece : node->pieces) node->summary = join(node->summary, piece.summary);
    return node;
}

/// Creates a branch value from the supplied arguments.
///
/// @param children `children` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> make_branch(vector<shared_ptr<const Node>> children) {
    auto node = make_shared<Node>();
    node->children = FixedVector<shared_ptr<const Node>, branch_fanout>{std::move(children)};
    for (const auto &child : node->children) node->summary = join(node->summary, child->summary);
    node->height = node->children.front()->height + 1;
    return node;
}


/// Builds level.
///
/// @param level `level` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> build_level(vector<shared_ptr<const Node>> level) {
    if (level.empty()) return make_leaf({});
    while (level.size() > 1) {
        vector<shared_ptr<const Node>> next;
        next.reserve((level.size() + branch_fanout - 1) / branch_fanout);
        for (usize i = 0; i < level.size(); i += branch_fanout) {
            next.push_back(make_branch(vector<shared_ptr<const Node>>{
                level.begin() + static_cast<isize>(i), level.begin() + static_cast<isize>(std::min(i + branch_fanout, level.size()))}));
        }
        level = std::move(next);
    }
    return level.front();
}

/// Builds tree.
///
/// @param pieces `pieces` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> build_tree(const vector<Piece> &pieces) {
    vector<shared_ptr<const Node>> leaves;
    for (usize i = 0; i < pieces.size(); i += leaf_piece_capacity) {
        leaves.push_back(make_leaf(vector<Piece>{pieces.begin() + static_cast<isize>(i), pieces.begin() + static_cast<isize>(std::min(i + leaf_piece_capacity, pieces.size()))}));
    }
    return build_level(std::move(leaves));
}

/// Collects pieces using the supplied arguments and current state.
///
/// @param node `node` value used by the operation.
/// @param out `out` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void collect_pieces(const shared_ptr<const Node> &node, vector<Piece> &out) {
    if (node->leaf()) { out.insert(out.end(), node->pieces.begin(), node->pieces.end()); return; }
    for (const auto &child : node->children) collect_pieces(child, out);
}


/// Performs the level from nodes operation for `Text` using the supplied arguments.
///
/// @param children `children` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<shared_ptr<const Node>> level_from_nodes(vector<shared_ptr<const Node>> children) {
    if (children.empty()) return {};
    if (children.size() <= branch_fanout) return {make_branch(std::move(children))};
    const usize mid = children.size() / 2;
    vector<shared_ptr<const Node>> first{children.begin(), children.begin() + static_cast<isize>(mid)};
    vector<shared_ptr<const Node>> second{children.begin() + static_cast<isize>(mid), children.end()};
    return {make_branch(std::move(first)), make_branch(std::move(second))};
}

/// Performs the level from pieces operation for `Text` using the supplied arguments.
///
/// @param pieces `pieces` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<shared_ptr<const Node>> level_from_pieces(vector<Piece> pieces) {
    if (pieces.empty()) return {};
    if (pieces.size() <= leaf_piece_capacity) return {make_leaf(std::move(pieces))};
    const usize mid = pieces.size() / 2;
    vector<Piece> first{pieces.begin(), pieces.begin() + static_cast<isize>(mid)};
    vector<Piece> second{pieces.begin() + static_cast<isize>(mid), pieces.end()};
    return {make_leaf(std::move(first)), make_leaf(std::move(second))};
}

/// Performs the concat core operation for `Text` using the supplied arguments.
///
/// @param left Left-hand operand.
/// @param right Right-hand operand.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<shared_ptr<const Node>> concat_core(const shared_ptr<const Node> &left,
                                                          const shared_ptr<const Node> &right) {
    if (left->height == right->height) {
        if (left->leaf()) {
            vector<Piece> pieces{left->pieces.begin(), left->pieces.end()};
            pieces.insert(pieces.end(), right->pieces.begin(), right->pieces.end());
            return level_from_pieces(std::move(pieces));
        }
        vector<shared_ptr<const Node>> children{left->children.begin(), left->children.end()};
        children.insert(children.end(), right->children.begin(), right->children.end());
        return level_from_nodes(std::move(children));
    }
    if (left->height > right->height) {
        vector<shared_ptr<const Node>> tail = concat_core(left->children.back(), right);
        vector<shared_ptr<const Node>> children{left->children.begin(), left->children.end() - 1};
        children.insert(children.end(), tail.begin(), tail.end());
        return level_from_nodes(std::move(children));
    }
    vector<shared_ptr<const Node>> head = concat_core(left, right->children.front());
    head.insert(head.end(), right->children.begin() + 1, right->children.end());
    return level_from_nodes(std::move(head));
}


/// Performs the concat operation for `Text` using the supplied arguments.
///
/// @param left Left-hand operand.
/// @param right Right-hand operand.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> concat(const shared_ptr<const Node> &left, const shared_ptr<const Node> &right) {
    if (left->summary.bytes == 0) return right;
    if (right->summary.bytes == 0) return left;
    vector<shared_ptr<const Node>> merged = concat_core(left, right);
    return merged.size() == 1 ? merged.front() : make_branch(std::move(merged));
}

/// Performs the wrap or empty operation for `Text` using the supplied arguments.
///
/// @param nodes `nodes` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] shared_ptr<const Node> wrap_or_empty(vector<shared_ptr<const Node>> nodes) {
    if (nodes.empty()) return make_leaf({});
    if (nodes.size() == 1) return nodes.front();
    return make_branch(std::move(nodes));
}

struct TreeSplit { shared_ptr<const Node> left; shared_ptr<const Node> right; };


/// Splits the supplied or associated value/state using the supplied arguments and current state.
///
/// @param node `node` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] TreeSplit split(const shared_ptr<const Node> &node, usize offset) {
    if (offset == 0) return {make_leaf({}), node};
    if (offset == node->summary.bytes) return {node, make_leaf({})};
    if (node->leaf()) {
        usize cursor = 0;
        for (usize i = 0; i < node->pieces.size(); ++i) {
            const Piece &piece = node->pieces[i];
            if (offset <= cursor + piece.length) {
                const usize local = offset - cursor;
                vector<Piece> left_pieces{node->pieces.begin(), node->pieces.begin() + static_cast<isize>(i)};
                vector<Piece> right_pieces{node->pieces.begin() + static_cast<isize>(i) + 1, node->pieces.end()};
                if (local > 0) {
                    const string_view bytes{piece.storage->data() + piece.start, local};
                    left_pieces.push_back(Piece{piece.storage, piece.start, local, *summarize(bytes)});
                }
                if (local < piece.length) {
                    const string_view bytes{piece.storage->data() + piece.start + local, piece.length - local};
                    right_pieces.insert(right_pieces.begin(), Piece{piece.storage, piece.start + local, piece.length - local, *summarize(bytes)});
                }
                return {make_leaf(std::move(left_pieces)), make_leaf(std::move(right_pieces))};
            }
            cursor += piece.length;
        }
        return {node, make_leaf({})};
    }
    usize cursor = 0;
    for (usize i = 0; i < node->children.size(); ++i) {
        const auto &child = node->children[i];
        if (offset == cursor) {
            return {wrap_or_empty(vector<shared_ptr<const Node>>{node->children.begin(), node->children.begin() + static_cast<isize>(i)}),
                    wrap_or_empty(vector<shared_ptr<const Node>>{node->children.begin() + static_cast<isize>(i), node->children.end()})};
        }
        if (offset < cursor + child->summary.bytes) {
            const TreeSplit inner = split(child, offset - cursor);
            vector<shared_ptr<const Node>> left_prefix{node->children.begin(), node->children.begin() + static_cast<isize>(i)};
            vector<shared_ptr<const Node>> right_suffix{node->children.begin() + static_cast<isize>(i) + 1, node->children.end()};
            return {concat(wrap_or_empty(std::move(left_prefix)), inner.left),
                    concat(inner.right, wrap_or_empty(std::move(right_suffix)))};
        }
        cursor += child->summary.bytes;
    }
    return {node, make_leaf({})};
}

/// Collects range using the supplied arguments and current state.
///
/// @param node `node` value used by the operation.
/// @param skip `skip` value used by the operation.
/// @param remaining `remaining` value used by the operation.
/// @param slice `slice` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void collect_range(const shared_ptr<const Node> &node, usize &skip, usize &remaining, TextSlice &slice) {
    if (remaining == 0) return;
    if (skip >= node->summary.bytes) { skip -= node->summary.bytes; return; }
    if (!node->leaf()) { for (const auto &child : node->children) collect_range(child, skip, remaining, slice); return; }
    for (const Piece &piece : node->pieces) {
        if (remaining == 0) break;
        if (skip >= piece.length) { skip -= piece.length; continue; }
        const usize start = skip;
        const usize count = std::min(remaining, piece.length - start);
        slice.append_chunk(string_view{piece.storage->data() + piece.start + start, count}, piece.storage);
        remaining -= count;
        skip = 0;
    }
}

/// Visits range using the supplied arguments and current state.
///
/// @param node `node` value used by the operation.
/// @param skip `skip` value used by the operation.
/// @param remaining `remaining` value used by the operation.
/// @param visitor `visitor` value used by the operation.
///
/// @return Returns the boolean result of the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
bool visit_range(const shared_ptr<const Node> &node, usize &skip, usize &remaining, const TextChunkVisitor &visitor) {
    if (remaining == 0) return true;
    if (skip >= node->summary.bytes) { skip -= node->summary.bytes; return true; }
    if (!node->leaf()) {
        for (const auto &child : node->children) if (!visit_range(child, skip, remaining, visitor)) return false;
        return true;
    }
    for (const Piece &piece : node->pieces) {
        if (remaining == 0) break;
        if (skip >= piece.length) { skip -= piece.length; continue; }
        const usize start = skip;
        const usize count = std::min(remaining, piece.length - start);
        if (!visitor(string_view{piece.storage->data() + piece.start + start, count})) return false;
        remaining -= count;
        skip = 0;
    }
    return true;
}

/// Performs the accumulate stats operation for `Text` using the supplied arguments.
///
/// @param node `node` value used by the operation.
/// @param stats `stats` value used by the operation.
/// @param depth Depth of the target extent.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
void accumulate_stats(const shared_ptr<const Node> &node, DocumentMemoryStats &stats, usize depth) {
    ++stats.tree_nodes;
    stats.maximum_tree_depth = std::max(stats.maximum_tree_depth, depth);
    if (node->leaf()) { stats.pieces += node->pieces.size(); return; }
    for (const auto &child : node->children) accumulate_stats(child, stats, depth + 1);
}

struct Location { TextSummary before{}; const Piece *piece = nullptr; usize local_byte = 0; };

[[nodiscard]] optional<Location> locate(const shared_ptr<const Node> &node, usize offset, TextSummary before = {}) {
    if (offset > node->summary.bytes) return nullopt;
    if (!node->leaf()) {
        for (const auto &child : node->children) {
            if (offset <= child->summary.bytes) return locate(child, offset, before);
            offset -= child->summary.bytes;
            before = join(before, child->summary);
        }
        return nullopt;
    }
    for (const Piece &piece : node->pieces) {
        if (offset <= piece.length) return Location{before, &piece, offset};
        offset -= piece.length;
        before = join(before, piece.summary);
    }
    return node->pieces.empty() && offset == 0 ? optional<Location>{Location{before, nullptr, 0}} : nullopt;
}

/// Performs the local prefix operation for `Text` using the supplied arguments.
///
/// @param location `location` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
[[nodiscard]] optional<TextSummary> local_prefix(const Location &location) {
    if (location.piece == nullptr) return TextSummary{};
    const string_view bytes{location.piece->storage->data() + location.piece->start, location.local_byte};
    return summarize(bytes);
}


/// Computes the newline end offset required by the supplied values.
///
/// @param node `node` value used by the operation.
/// @param newline_index Zero-based index of the target element or entry.
/// @param prefix_bytes `prefix_bytes` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<usize> newline_end_offset(const shared_ptr<const Node> &node, usize newline_index,
                                                 usize prefix_bytes = 0) {
    if (newline_index >= node->summary.newlines) return nullopt;
    if (!node->leaf()) {
        for (const auto &child : node->children) {
            if (newline_index < child->summary.newlines) return newline_end_offset(child, newline_index, prefix_bytes);
            newline_index -= child->summary.newlines;
            prefix_bytes += child->summary.bytes;
        }
        return nullopt;
    }
    for (const Piece &piece : node->pieces) {
        if (newline_index >= piece.summary.newlines) {
            newline_index -= piece.summary.newlines;
            prefix_bytes += piece.length;
            continue;
        }
        const string_view bytes{piece.storage->data() + piece.start, piece.length};
        for (usize i = 0; i < bytes.size(); ++i) {
            if (bytes[i] == '\n' && newline_index-- == 0) return prefix_bytes + i + 1;
        }
        return nullopt;
    }
    return nullopt;
}


/// Computes the line column to offset required by the supplied values.
///
/// @param root `root` value used by the operation.
/// @param line `line` value used by the operation.
/// @param column `column` value used by the operation.
/// @param utf16 `utf16` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<usize> line_column_to_offset(const shared_ptr<const Node> &root, usize line, usize column,
                                                    bool utf16) {
    const usize line_count = root->summary.newlines + 1;
    if (line >= line_count) return nullopt;
    const usize start = line == 0 ? 0 : *newline_end_offset(root, line - 1);
    const usize end = line < root->summary.newlines ? *newline_end_offset(root, line) - 1 : root->summary.bytes;
    TextSlice slice;
    usize skip = start, remaining = end - start;
    collect_range(root, skip, remaining, slice);
    usize consumed = 0, units = 0;
    for (const TextSlice::Chunk &chunk : slice.chunks()) {
        for (usize i = 0; i < chunk.bytes.size();) {
            if (units == column) return start + consumed;
            char32_t scalar{}; usize width{};
            if (!decode(chunk.bytes.data() + i, chunk.bytes.size() - i, scalar, width)) return nullopt;
            units += utf16 && scalar > 0xffff ? 2 : 1;
            if (units > column) return nullopt;
            i += width;
            consumed += width;
        }
    }
    return units == column ? optional<usize>{start + consumed} : nullopt;
}


/// Performs the raw byte at operation for `Text` using the supplied arguments.
///
/// @param node `node` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<unsigned char> raw_byte_at(const shared_ptr<const Node> &node, usize offset) {
    if (offset >= node->summary.bytes) return nullopt;
    if (!node->leaf()) {
        for (const auto &child : node->children) {
            if (offset < child->summary.bytes) return raw_byte_at(child, offset);
            offset -= child->summary.bytes;
        }
        return nullopt;
    }
    for (const Piece &piece : node->pieces) {
        if (offset < piece.length) return static_cast<unsigned char>(piece.storage->data()[piece.start + offset]);
        offset -= piece.length;
    }
    return nullopt;
}

/// Reports whether valid range is valid for the current operation.
///
/// @param state `state` value used by the operation.
/// @param range Range of values to process.
///
/// @return Returns the boolean result of the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] bool valid_range(const Detail::DocumentState &state, TextRange range) {
    if (range.start.value > range.end.value || range.end.value > state.root->summary.bytes) return false;
    const auto start = locate(state.root, range.start.value);
    const auto end = locate(state.root, range.end.value);
    return start && end && (!start->piece || boundary(string_view{start->piece->storage->data() + start->piece->start, start->piece->length}, start->local_byte)) &&
           (!end->piece || boundary(string_view{end->piece->storage->data() + end->piece->start, end->piece->length}, end->local_byte));
}


/// Performs the classify operation for `Text` using the supplied arguments.
///
/// @param scalar `scalar` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] CharClass classify(char32_t scalar) noexcept {
    if (scalar == U' ' || scalar == U'\t' || scalar == U'\n' || scalar == U'\r' || scalar == U'\f' || scalar == U'\v') return CharClass::Whitespace;
    if (scalar == U'_' || (scalar < 128 && std::isalnum(static_cast<int>(scalar)) != 0) || scalar >= 128) return CharClass::Word;
    return CharClass::Punctuation;
}

struct ScalarAt { char32_t scalar; ByteOffset start; ByteOffset end; };

/// Performs the next scalar at operation for `Text` using the supplied arguments.
///
/// @param snapshot `snapshot` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<ScalarAt> next_scalar_at(const DocumentSnapshot &snapshot, ByteOffset offset) {
    const usize size = snapshot.byte_size();
    if (offset.value >= size) return nullopt;
    const string bytes = snapshot.slice({offset, {std::min(offset.value + 4, size)}}).flatten();
    char32_t scalar{}; usize width{};
    if (!decode(bytes.data(), bytes.size(), scalar, width)) return nullopt;
    return ScalarAt{scalar, offset, {offset.value + width}};
}

/// Performs the previous scalar at operation for `Text` using the supplied arguments.
///
/// @param snapshot `snapshot` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
[[nodiscard]] optional<ScalarAt> previous_scalar_at(const DocumentSnapshot &snapshot, ByteOffset offset) {
    if (offset.value == 0) return nullopt;
    const usize begin = offset.value > 4 ? offset.value - 4 : 0;
    const string bytes = snapshot.slice({{begin}, offset}).flatten();
    usize local = bytes.size() - 1;
    while (local > 0 && continuation(static_cast<unsigned char>(bytes[local]))) --local;
    char32_t scalar{}; usize width{};
    if (!decode(bytes.data() + local, bytes.size() - local, scalar, width)) return nullopt;
    return ScalarAt{scalar, {begin + local}, offset};
}

/// Performs the matching of operation for `Text` using the supplied arguments.
///
/// @param c `c` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<char> matching_of(char c) noexcept {
    switch (c) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '{': return '}'; case '}': return '{';
    default: return nullopt;
    }
}
/// Reports whether open bracket holds for this `Text`.
///
/// @param c `c` value used by the operation.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
[[nodiscard]] bool is_open_bracket(char c) noexcept { return c == '(' || c == '[' || c == '{'; }


/// Reports whether coalesce holds for this `Text`.
///
/// @param previous `previous` value used by the operation.
/// @param kind `kind` value used by the operation.
/// @param edit `edit` value used by the operation.
///
/// @return Returns `true` when the stated condition holds; otherwise returns `false`.
/// @note This function does not throw exceptions.
[[nodiscard]] bool can_coalesce(const Detail::DocumentState &previous, EditKind kind, const Splice &edit) noexcept {
    if (kind == EditKind::Standalone) return false;
    if (previous.kind_from_parent != kind) return false;
    if (previous.changes_from_parent.changes.size() != 1) return false;
    const Change &last = previous.changes_from_parent.changes.front();
    if (kind == EditKind::Typing) {
        return edit.range.start.value == edit.range.end.value && edit.range.start.value == last.new_range.end.value;
    }
    return edit.replacement.empty() && edit.range.start.value != edit.range.end.value && edit.range.end.value == last.new_range.start.value;
}


/// Performs the compose group change operation for `Text` using the supplied arguments.
///
/// @param oldest_to_newest `oldest_to_newest` value used by the operation.
///
/// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] Change compose_group_change(const vector<shared_ptr<const Detail::DocumentState>> &oldest_to_newest) {
    if (oldest_to_newest.size() == 1) return oldest_to_newest.front()->changes_from_parent.changes.front();
    const EditKind kind = oldest_to_newest.front()->kind_from_parent;
    const Change &oldest = oldest_to_newest.front()->changes_from_parent.changes.front();
    const Change &newest = oldest_to_newest.back()->changes_from_parent.changes.front();
    Change combined{};
    if (kind == EditKind::Typing) {
        combined.old_range = oldest.old_range;
        combined.new_range = {oldest.old_range.start, newest.new_range.end};
        string inserted;
        for (const auto &state : oldest_to_newest) inserted += state->changes_from_parent.changes.front().inserted_text.cpp_string();
        combined.inserted_text = UString{inserted};
    } else {
        combined.old_range = {newest.old_range.start, oldest.old_range.end};
        combined.new_range = {combined.old_range.start, combined.old_range.start};
        string deleted;
        for (auto it = oldest_to_newest.rbegin(); it != oldest_to_newest.rend(); ++it) deleted += (*it)->changes_from_parent.changes.front().deleted_text.cpp_string();
        combined.deleted_text = UString{deleted};
    }
    return combined;
}

} // namespace

/// Converts the value to string representation.
///
/// @param error Error value describing the failure.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
string_view to_string(DocumentError error) noexcept {
    switch (error) {
    case DocumentError::InvalidUtf8: return "invalid UTF-8";
    case DocumentError::InvalidRange: return "invalid text range";
    case DocumentError::InvalidBoundary: return "range is not on a UTF-8 scalar boundary";
    case DocumentError::RevisionMismatch: return "transaction base revision does not match document";
    case DocumentError::OverlappingSplices: return "transaction contains overlapping splices";
    }
    return "unknown document error";
}

/// Returns the current or globally available flatten value.
///
/// @return Returns the current flatten value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
string TextSlice::flatten() const {
    string result; result.reserve(byte_size_);
    for (const Chunk &chunk : chunks_) result.append(chunk.bytes);
    return result;
}

/// Returns the current or globally available revision value.
///
/// @return Returns the current revision value.
/// @note This function does not throw exceptions.
Revision DocumentSnapshot::revision() const noexcept { return state_ ? state_->revision : Revision{}; }
/// Returns the current or globally available summary value.
///
/// @return Returns the current summary value.
/// @note This function does not throw exceptions.
TextSummary DocumentSnapshot::summary() const noexcept { return state_ ? state_->root->summary : TextSummary{}; }
/// Returns the byte size for this `Text`.
///
/// @return Returns the current byte size value.
/// @note This function does not throw exceptions.
usize DocumentSnapshot::byte_size() const noexcept { return summary().bytes; }
/// Returns the line count for this `Text`.
///
/// @return Returns the current line count value.
/// @note This function does not throw exceptions.
usize DocumentSnapshot::line_count() const noexcept { return state_ ? state_->root->summary.newlines + 1 : 0; }

/// Performs the line range operation for `Text` using the supplied arguments.
///
/// @param line `line` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextRange> DocumentSnapshot::line_range(usize line) const {
    if (!state_ || line >= line_count()) return nullopt;
    const usize start = line == 0 ? 0 : *newline_end_offset(state_->root, line - 1);
    const usize end = line < state_->root->summary.newlines ? *newline_end_offset(state_->root, line) - 1 : byte_size();
    return TextRange{{start}, {end}};
}

/// Performs the slice operation for `Text` using the supplied arguments.
///
/// @param range Range of values to process.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
TextSlice DocumentSnapshot::slice(TextRange range) const {
    TextSlice result;
    if (!state_ || !valid_range(*state_, range)) return result;
    usize skip = range.start.value, remaining = range.end.value - range.start.value;
    collect_range(state_->root, skip, remaining, result);
    return result;
}

/// Returns the current or globally available flatten value.
///
/// @return Returns the current flatten value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
string DocumentSnapshot::flatten() const { return slice({{}, {byte_size()}}).flatten(); }

/// Performs the offset to point operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextPoint> DocumentSnapshot::offset_to_point(ByteOffset offset) const {
    if (!state_ || offset.value > byte_size()) return nullopt;
    const auto found = locate(state_->root, offset.value); if (!found) return nullopt;
    const auto local = local_prefix(*found); if (!local) return nullopt;
    const TextSummary prefix = join(found->before, *local);
    return TextPoint{.line = prefix.newlines, .scalar_column = prefix.last_line_scalars};
}

/// Performs the offset to UTF-16 operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<Utf16Point> DocumentSnapshot::offset_to_utf16(ByteOffset offset) const {
    if (!state_ || offset.value > byte_size()) return nullopt;
    const auto found = locate(state_->root, offset.value); if (!found) return nullopt;
    const auto local = local_prefix(*found); if (!local) return nullopt;
    const TextSummary prefix = join(found->before, *local);
    return Utf16Point{.line = prefix.newlines, .code_unit_column = prefix.last_line_utf16_code_units};
}

/// Computes the point to offset required by the supplied values.
///
/// @param point `point` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ByteOffset> DocumentSnapshot::point_to_offset(TextPoint point) const {
    if (!state_) return nullopt;
    const auto offset = line_column_to_offset(state_->root, point.line, point.scalar_column,           false);
    return offset ? optional<ByteOffset>{ByteOffset{*offset}} : nullopt;
}

/// Computes the UTF-16 to offset required by the supplied values.
///
/// @param point `point` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ByteOffset> DocumentSnapshot::utf16_to_offset(Utf16Point point) const {
    if (!state_) return nullopt;
    const auto offset = line_column_to_offset(state_->root, point.line, point.code_unit_column,           true);
    return offset ? optional<ByteOffset>{ByteOffset{*offset}} : nullopt;
}

/// Performs the anchor at operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param bias `bias` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<Anchor> DocumentSnapshot::anchor_at(ByteOffset offset, AnchorBias bias) const {
    if (!state_ || !valid_range(*state_, {offset, offset})) return nullopt;
    Anchor anchor; anchor.revision_ = revision(); anchor.offset_ = offset; anchor.bias_ = bias; anchor.valid_ = true; return anchor;
}

/// Resolves the requested value into the concrete value used by the engine.
///
/// @param anchor `anchor` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ByteOffset> DocumentSnapshot::resolve(const Anchor &anchor) const {
    if (!state_ || !anchor.valid_) return nullopt;


    vector<const ChangeSet *> hops;
    const Detail::DocumentState *cursor = state_.get();
    while (cursor != nullptr && cursor->revision != anchor.revision_) {
        hops.push_back(&cursor->changes_from_parent);
        cursor = cursor->parent.get();
    }
    if (cursor == nullptr) return nullopt;
    ByteOffset offset = anchor.offset_;
    for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
        const auto translated = Document::translate(offset, anchor.bias_, **it);
        if (!translated) return nullopt;
        offset = *translated;
    }
    return offset;
}

/// Resolves the requested value into the concrete value used by the engine.
///
/// @param range Range of values to process.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextRange> DocumentSnapshot::resolve(const AnchorRange &range) const {
    const auto start = resolve(range.start);
    const auto end = resolve(range.end);
    return start && end && start->value <= end->value ? optional<TextRange>{TextRange{*start, *end}} : nullopt;
}

/// Computes the clip offset required by the supplied values.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param bias `bias` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
ByteOffset DocumentSnapshot::clip_offset(ByteOffset offset, Bias bias) const noexcept {
    if (!state_) return {};
    const usize size = byte_size();
    usize value = std::min(offset.value, size);
    const auto is_continuation_at = [&](usize position) {
        const auto byte = raw_byte_at(state_->root, position);
        return byte && continuation(*byte);
    };
    if (bias == Bias::Before) {
        while (value > 0 && is_continuation_at(value)) --value;
    } else {
        while (value < size && is_continuation_at(value)) ++value;
    }
    return ByteOffset{value};
}

/// Returns the current or globally available max point value.
///
/// @return Returns the current max point value.
/// @note This function does not throw exceptions.
TextPoint DocumentSnapshot::max_point() const noexcept {
    const TextSummary s = summary();
    return TextPoint{.line = s.newlines, .scalar_column = s.last_line_scalars};
}

/// Returns the current or globally available longest line value.
///
/// @return Returns the current longest line value.
/// @note This function does not throw exceptions.
LongestLine DocumentSnapshot::longest_line() const noexcept {
    const TextSummary s = summary();
    return LongestLine{.line = s.longest_line, .scalars = s.longest_line_scalars};
}

/// Performs the char class at operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
CharClass DocumentSnapshot::char_class_at(ByteOffset offset) const {
    const auto scalar = next_scalar_at(*this, clip_offset(offset, Bias::After));
    return scalar ? classify(scalar->scalar) : CharClass::Whitespace;
}

/// Performs the word range at operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextRange> DocumentSnapshot::word_range_at(ByteOffset offset) const {
    if (!state_) return nullopt;
    offset = clip_offset(offset, Bias::After);
    auto here = next_scalar_at(*this, offset);
    CharClass run_class = here ? classify(here->scalar) : CharClass::Whitespace;
    if (run_class == CharClass::Whitespace) {


        const auto before = previous_scalar_at(*this, offset);
        if (!before || classify(before->scalar) == CharClass::Whitespace) return TextRange{offset, offset};
        run_class = classify(before->scalar);
        offset = before->start;
        here = next_scalar_at(*this, offset);
    }
    if (!here) return TextRange{offset, offset};
    ByteOffset start = offset;
    ByteOffset end = here->end;
    for (auto previous = previous_scalar_at(*this, start); previous && classify(previous->scalar) == run_class;
         previous = previous_scalar_at(*this, start)) {
        start = previous->start;
    }
    for (auto next = next_scalar_at(*this, end); next && classify(next->scalar) == run_class; next = next_scalar_at(*this, end)) {
        end = next->end;
    }
    return TextRange{start, end};
}

/// Performs the next word boundary operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ByteOffset DocumentSnapshot::next_word_boundary(ByteOffset offset) const {
    if (!state_) return offset;
    offset = clip_offset(offset, Bias::After);
    auto current = next_scalar_at(*this, offset);
    while (current && classify(current->scalar) == CharClass::Whitespace) {
        offset = current->end;
        current = next_scalar_at(*this, offset);
    }
    if (!current) return offset;
    const CharClass run_class = classify(current->scalar);
    while (current && classify(current->scalar) == run_class) {
        offset = current->end;
        current = next_scalar_at(*this, offset);
    }
    return offset;
}

/// Performs the previous word boundary operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ByteOffset DocumentSnapshot::previous_word_boundary(ByteOffset offset) const {
    if (!state_) return offset;
    offset = clip_offset(offset, Bias::Before);
    auto current = previous_scalar_at(*this, offset);
    while (current && classify(current->scalar) == CharClass::Whitespace) {
        offset = current->start;
        current = previous_scalar_at(*this, offset);
    }
    if (!current) return offset;
    const CharClass run_class = classify(current->scalar);
    while (current && classify(current->scalar) == run_class) {
        offset = current->start;
        current = previous_scalar_at(*this, offset);
    }
    return offset;
}

/// Performs the line indent operation for `Text` using the supplied arguments.
///
/// @param line `line` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
LineIndent DocumentSnapshot::line_indent(usize line) const {
    LineIndent result{};
    const auto range = line_range(line);
    if (!range) return result;
    const string bytes = slice(*range).flatten();
    usize i = 0;
    while (i < bytes.size()) {
        char32_t scalar{}; usize width{};
        if (!decode(bytes.data() + i, bytes.size() - i, scalar, width)) break;
        if (scalar == U' ') result.has_spaces = true;
        else if (scalar == U'\t') result.has_tabs = true;
        else break;
        result.whitespace_bytes += width;
        ++result.whitespace_scalars;
        i += width;
    }
    return result;
}

/// Reports whether line blank holds for this `Text`.
///
/// @param line `line` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
bool DocumentSnapshot::is_line_blank(usize line) const {
    const auto range = line_range(line);
    if (!range) return true;
    const usize length = range->end.value - range->start.value;
    return length == 0 || line_indent(line).whitespace_bytes == length;
}

/// Performs the matching bracket operation for `Text` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ByteOffset> DocumentSnapshot::matching_bracket(ByteOffset offset) const {
    if (!state_ || offset.value >= byte_size()) return nullopt;
    const string one = slice({offset, {offset.value + 1}}).flatten();
    if (one.size() != 1) return nullopt;
    const char bracket = one.front();
    const auto target = matching_of(bracket);
    if (!target) return nullopt;
    constexpr usize window = 4096;
    isize depth = 0;
    if (is_open_bracket(bracket)) {
        usize position = offset.value + 1;
        const usize size = byte_size();
        while (position < size) {
            const string chunk = slice({{position}, {std::min(position + window, size)}}).flatten();
            for (usize i = 0; i < chunk.size(); ++i) {
                if (chunk[i] == bracket) ++depth;
                else if (chunk[i] == *target) { if (depth == 0) return ByteOffset{position + i}; --depth; }
            }
            position += chunk.size();
        }
        return nullopt;
    }
    usize position = offset.value;
    while (position > 0) {
        const usize window_start = position > window ? position - window : 0;
        const string chunk = slice({{window_start}, {position}}).flatten();
        for (usize i = chunk.size(); i-- > 0;) {
            if (chunk[i] == bracket) ++depth;
            else if (chunk[i] == *target) { if (depth == 0) return ByteOffset{window_start + i}; --depth; }
        }
        position = window_start;
    }
    return nullopt;
}

namespace {
/// Performs the fold char operation for `Text` using the supplied arguments.
///
/// @param c `c` value used by the operation.
/// @param case_sensitive `case_sensitive` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] char fold_char(char c, bool case_sensitive) noexcept {
    return case_sensitive ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}
} // namespace

/// Finds the requested entry in the available state.
///
/// @param pattern `pattern` value used by the operation.
/// @param from `from` value used by the operation.
/// @param forward `forward` value used by the operation.
/// @param case_sensitive `case_sensitive` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextRange> DocumentSnapshot::find(string_view pattern, ByteOffset from, bool forward, bool case_sensitive) const {
    if (!state_ || pattern.empty()) return nullopt;
    const usize size = byte_size();
    optional<TextRange> found;
    string carry;
    usize base = 0;
    const auto scan = [&](string_view chunk) {
        string window = carry;
        window.append(chunk);
        for (usize i = 0; i + pattern.size() <= window.size(); ++i) {
            bool matches = true;
            for (usize p = 0; p < pattern.size() && matches; ++p) matches = fold_char(window[i + p], case_sensitive) == fold_char(pattern[p], case_sensitive);
            if (matches) {
                const usize absolute = base - carry.size() + i;
                found = TextRange{{absolute}, {absolute + pattern.size()}};
                if (forward) return false;
            }
        }
        base += chunk.size();
        carry = window.size() >= pattern.size() - 1 ? window.substr(window.size() - (pattern.size() - 1)) : window;
        return true;
    };
    if (forward) {
        const usize start = std::min(from.value, size);
        base = start;
        usize skip = start, remaining = size - start;
        visit_range(state_->root, skip, remaining, scan);
    } else {
        const usize end = std::min(from.value, size);
        usize skip = 0, remaining = end;
        visit_range(state_->root, skip, remaining, scan);
    }
    return found;
}

/// Finds all in the available state.
///
/// @param pattern `pattern` value used by the operation.
/// @param case_sensitive `case_sensitive` value used by the operation.
///
/// @return Returns the located entry/position; the type-specific sentinel or empty state indicates that no match was found.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
vector<TextRange> DocumentSnapshot::find_all(string_view pattern, bool case_sensitive) const {
    vector<TextRange> results;
    if (!state_ || pattern.empty()) return results;
    string carry;
    usize base = 0;


    usize next_allowed = 0;
    usize skip = 0, remaining = byte_size();
    visit_range(state_->root, skip, remaining, [&](string_view chunk) {
        string window = carry;
        window.append(chunk);
        usize i = 0;
        while (i + pattern.size() <= window.size()) {
            const usize absolute = base - carry.size() + i;
            if (absolute < next_allowed) { ++i; continue; }
            bool matches = true;
            for (usize p = 0; p < pattern.size() && matches; ++p) matches = fold_char(window[i + p], case_sensitive) == fold_char(pattern[p], case_sensitive);
            if (matches) {
                results.push_back(TextRange{{absolute}, {absolute + pattern.size()}});
                next_allowed = absolute + pattern.size();
                i += pattern.size();
            } else {
                ++i;
            }
        }
        base += chunk.size();
        carry = window.size() >= pattern.size() - 1 ? window.substr(window.size() - (pattern.size() - 1)) : window;
        return true;
    });
    return results;
}

/// Returns the current or globally available detect indent style value.
///
/// @return Returns the current detect indent style value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
IndentStyle DocumentSnapshot::detect_indent_style() const {
    if (!state_) return {};
    usize tab_lines = 0;
    vector<usize> space_run_lengths;
    const usize sample = std::min<usize>(line_count(), 200);
    for (usize line = 0; line < sample; ++line) {
        const LineIndent indent = line_indent(line);
        if (indent.whitespace_scalars == 0) continue;
        if (indent.has_tabs && !indent.has_spaces) ++tab_lines;
        else if (indent.has_spaces && !indent.has_tabs) space_run_lengths.push_back(indent.whitespace_scalars);
    }
    if (tab_lines > space_run_lengths.size()) return IndentStyle{.kind = IndentKind::Tabs, .width = 4};
    if (space_run_lengths.empty()) return IndentStyle{};


    const usize width = std::clamp<usize>(*std::ranges::min_element(space_run_lengths), 1, 8);
    return IndentStyle{.kind = IndentKind::Spaces, .width = width};
}

/// Returns the current or globally available detect line ending value.
///
/// @return Returns the current detect line ending value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
LineEnding DocumentSnapshot::detect_line_ending() const {
    if (!state_) return LineEnding::Lf;
    usize crlf = 0, lf = 0;
    const usize sample = std::min<usize>(summary().newlines, 200);
    for (usize line = 0; line < sample; ++line) {
        const auto range = line_range(line);
        if (!range) continue;
        const bool has_cr = range->end.value > range->start.value &&
                            slice({{range->end.value - 1}, range->end}).flatten() == "\r";
        (has_cr ? crlf : lf)++;
    }
    return crlf > lf ? LineEnding::CrLf : LineEnding::Lf;
}

/// Performs the document operation for `Text` using the supplied arguments.
///
/// @param initial_utf8 `initial_utf8` value used by the operation.
///
/// @throws `invalid_argument` if `!info`.
Document::Document(string_view initial_utf8) {
    const auto info = summarize(initial_utf8);
    if (!info) throw invalid_argument{"Text::Document requires strict UTF-8."};
    auto storage = make_shared<const string>(initial_utf8);
    auto state = make_shared<Detail::DocumentState>();
    state->root = build_tree(pieces_for(storage, 0, storage->size()));
    state->revision = Revision{1};
    state_ = std::move(state);
}

/// Returns the current or globally available snapshot value.
///
/// @return Returns the current snapshot value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DocumentSnapshot Document::snapshot() const { return DocumentSnapshot{state_}; }
/// Returns the current or globally available revision value.
///
/// @return Returns the current revision value.
/// @note This function does not throw exceptions.
Revision Document::revision() const noexcept { return state_->revision; }

/// Applies the supplied operation or state to `Text`.
///
/// @param transaction `transaction` value used by the operation.
/// @param kind `kind` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `DocumentError::RevisionMismatch`, `DocumentError::InvalidRange`, `DocumentError::OverlappingSplices`, `DocumentError::InvalidUtf8`.
expected<ApplyResult, DocumentError> Document::apply(const EditTransaction &transaction, EditKind kind) {
    if (transaction.base_revision() != revision()) return unexpected(DocumentError::RevisionMismatch);
    vector<Splice> edits = transaction.splices();
    std::ranges::sort(edits, {}, [](const Splice &splice) { return splice.range.start.value; });
    usize previous_end = 0;
    for (const Splice &edit : edits) {
        if (!valid_range(*state_, edit.range)) return unexpected(DocumentError::InvalidRange);
        if (edit.range.start.value < previous_end) return unexpected(DocumentError::OverlappingSplices);
        if (!summarize(edit.replacement)) return unexpected(DocumentError::InvalidUtf8);
        previous_end = edit.range.end.value;
    }
    if (edits.empty()) return ApplyResult{snapshot(), ChangeSet{.before = revision(), .after = revision(), .changes = {}}};
    const bool continues_group = edits.size() == 1 && can_coalesce(*state_, kind, edits.front());

    string inserted_slab;
    for (const Splice &edit : edits) inserted_slab.append(edit.replacement);
    auto insert_storage = make_shared<const string>(std::move(inserted_slab));


    shared_ptr<const Node> result = make_leaf({});
    shared_ptr<const Node> remainder = state_->root;
    usize slab_cursor = 0;

    ChangeSet changes{.before = revision(), .after = Revision{revision().value + 1}, .changes = {}};
    usize base_cursor = 0, delta = 0;
    for (const Splice &edit : edits) {
        const TreeSplit prefix_split = split(remainder, edit.range.start.value - base_cursor);
        result = concat(result, prefix_split.left);
        const TreeSplit removed_split = split(prefix_split.right, edit.range.end.value - edit.range.start.value);
        remainder = removed_split.right;

        const usize replacement_start = slab_cursor;
        slab_cursor += edit.replacement.size();
        if (!edit.replacement.empty()) {
            vector<Piece> inserted = pieces_for(insert_storage, replacement_start, edit.replacement.size());
            result = concat(result, build_tree(inserted));
        }
        const usize new_start = edit.range.start.value + delta;
        changes.changes.push_back(Change{.old_range = edit.range,
                                         .new_range = {{new_start}, {new_start + edit.replacement.size()}},
                                         .inserted_text = UString{edit.replacement},
                                         .deleted_text = UString{snapshot().slice(edit.range).flatten()}});
        delta += edit.replacement.size() - (edit.range.end.value - edit.range.start.value);
        base_cursor = edit.range.end.value;
    }
    result = concat(result, remainder);

    auto next_state = make_shared<Detail::DocumentState>();
    next_state->root = result;
    next_state->revision = changes.after;
    next_state->parent = state_;
    next_state->changes_from_parent = changes;
    next_state->kind_from_parent = kind;
    next_state->continues_previous_group = continues_group;
    state_ = next_state;
    redo_.clear();
    return ApplyResult{DocumentSnapshot{std::move(next_state)}, std::move(changes)};
}

/// Returns the current or globally available undo value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ApplyResult> Document::undo() {
    if (!state_ || !state_->parent) return nullopt;


    vector<shared_ptr<const Detail::DocumentState>> popped;
    shared_ptr<Detail::DocumentState> cursor = state_;
    while (cursor && cursor->parent) {
        const bool keep_going = cursor->continues_previous_group;
        popped.push_back(cursor);
        cursor = const_pointer_cast<Detail::DocumentState>(cursor->parent);
        if (!keep_going) break;
    }
    state_ = cursor;
    for (const auto &popped_state : popped) redo_.push_back(const_pointer_cast<Detail::DocumentState>(popped_state));

    ChangeSet changes{};
    if (popped.size() == 1) {
        changes = popped.front()->changes_from_parent;
    } else {
        const vector<shared_ptr<const Detail::DocumentState>> oldest_to_newest{popped.rbegin(), popped.rend()};
        changes.before = state_->revision;
        changes.after = popped.front()->revision;
        changes.changes = {compose_group_change(oldest_to_newest)};
    }
    std::ranges::reverse(changes.changes);
    for (Change &change : changes.changes) {
        std::swap(change.old_range, change.new_range);
        std::swap(change.inserted_text, change.deleted_text);
    }
    std::swap(changes.before, changes.after);
    return ApplyResult{snapshot(), std::move(changes)};
}

/// Returns the current or globally available redo value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<ApplyResult> Document::redo() {
    if (redo_.empty() || redo_.back()->parent.get() != state_.get()) return nullopt;


    vector<shared_ptr<const Detail::DocumentState>> restored;
    do {
        shared_ptr<Detail::DocumentState> next = redo_.back();
        redo_.pop_back();
        state_ = next;
        restored.push_back(next);
    } while (!redo_.empty() && redo_.back()->continues_previous_group && redo_.back()->parent.get() == state_.get());

    ChangeSet changes{};
    if (restored.size() == 1) {
        changes = restored.front()->changes_from_parent;
    } else {
        changes.before = restored.front()->changes_from_parent.before;
        changes.after = restored.back()->revision;
        changes.changes = {compose_group_change(restored)};
    }
    return ApplyResult{snapshot(), std::move(changes)};
}

/// Returns the current or globally available memory stats value.
///
/// @return Returns the current memory stats value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
DocumentMemoryStats Document::memory_stats() const {
    DocumentMemoryStats stats{};
    if (!state_) return stats;
    stats.logical_bytes = state_->root->summary.bytes;
    accumulate_stats(state_->root, stats, 1);
    vector<Piece> pieces;
    collect_pieces(state_->root, pieces);
    vector<const string *> seen;
    for (const Piece &piece : pieces) {
        if (std::ranges::find(seen, piece.storage.get()) == seen.end()) {
            seen.push_back(piece.storage.get());
            stats.backing_storage_bytes += piece.storage->size();
        }
    }
    for (auto cursor = state_; cursor && cursor->parent; cursor = const_pointer_cast<Detail::DocumentState>(cursor->parent)) ++stats.undo_revisions;
    stats.redo_revisions = redo_.size();
    return stats;
}

/// Visits the supplied or associated value/state using the supplied arguments and current state.
///
/// @param range Range of values to process.
/// @param visitor `visitor` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
bool Document::visit(TextRange range, const TextChunkVisitor &visitor) const {
    if (!state_ || !visitor || !valid_range(*state_, range)) return false;
    usize skip = range.start.value;
    usize remaining = range.end.value - range.start.value;
    return visit_range(state_->root, skip, remaining, visitor) && remaining == 0;
}

/// Translates the supplied or associated value/state using the supplied arguments and current state.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param bias `bias` value used by the operation.
/// @param changes `changes` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
optional<ByteOffset> Document::translate(ByteOffset offset, AnchorBias bias, const ChangeSet &changes) {
    isize delta = 0;
    for (const Change &change : changes.changes) {
        const usize start = change.old_range.start.value;
        const usize end = change.old_range.end.value;
        if (offset.value < start) break;
        if (offset.value > end || (offset.value == end && start != end)) {
            delta += static_cast<isize>(change.new_range.end.value - change.new_range.start.value) - static_cast<isize>(end - start);
            continue;
        }
        return ByteOffset{bias == AnchorBias::Before ? change.new_range.start.value : change.new_range.end.value};
    }
    return ByteOffset{static_cast<usize>(static_cast<isize>(offset.value) + delta)};
}

/// Translates the supplied or associated value/state using the supplied arguments and current state.
///
/// @param range Range of values to process.
/// @param changes `changes` value used by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
optional<TextRange> Document::translate(TextRange range, const ChangeSet &changes) {
    const auto start = translate(range.start, AnchorBias::Before, changes);
    const auto end = translate(range.end, AnchorBias::After, changes);
    return start && end && start->value <= end->value ? optional<TextRange>{TextRange{*start, *end}} : nullopt;
}

} // namespace SFT::Text

namespace SFT::Text {

    /// Returns the current or globally available revision value.
    ///
    /// @return Returns the current revision value.
    /// @note This function does not throw exceptions.
    Revision Anchor::revision() const noexcept { return revision_; }

    /// Returns the current or globally available bias value.
    ///
    /// @return Returns the current bias value.
    /// @note This function does not throw exceptions.
    AnchorBias Anchor::bias() const noexcept { return bias_; }

    /// Returns the current or globally available valid value.
    ///
    /// @return Returns the current valid value.
    /// @note This function does not throw exceptions.
    bool Anchor::valid() const noexcept { return valid_; }

    /// Replaces the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param range Range of values to process.
    /// @param replacement `replacement` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void EditTransaction::replace(TextRange range, string_view replacement) { splices_.push_back({range, replacement}); }

    /// Returns the current or globally available base revision value.
    ///
    /// @return Returns the current base revision value.
    /// @note This function does not throw exceptions.
    Revision EditTransaction::base_revision() const noexcept { return base_revision_; }

    /// Returns the current or globally available splices value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const vector<Splice> &EditTransaction::splices() const noexcept { return splices_; }

    /// Returns the current or globally available chunks value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const vector<TextSlice::Chunk> &TextSlice::chunks() const noexcept { return chunks_; }

    /// Returns the byte size for this `Text`.
    ///
    /// @return Returns the current byte size value.
    /// @note This function does not throw exceptions.
    usize TextSlice::byte_size() const noexcept { return byte_size_; }

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param bytes Size of the relevant data in bytes.
    /// @param owner Owner/context identifier used for validation or diagnostics.
    ///
    /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void TextSlice::append_chunk(string_view bytes, shared_ptr<const string> owner) {
        chunks_.push_back({bytes});
        owners_.push_back(std::move(owner));
        byte_size_ += bytes.size();
    }

} // namespace SFT::Text


namespace SFT::Text {

    /// Performs the edit transaction operation for `Text` using the supplied arguments.
    ///
    /// @param base_revision `base_revision` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    EditTransaction::EditTransaction(Revision base_revision) : base_revision_(base_revision) {}

} // namespace SFT::Text


namespace SFT::Text {

    /// Performs the document snapshot operation for `Text` using the supplied arguments.
    ///
    /// @param state `state` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    DocumentSnapshot::DocumentSnapshot(shared_ptr<const Detail::DocumentState> state) : state_(std::move(state)) {}

} // namespace SFT::Text

