#include <Ecs/ComponentSchema.hpp>

#include <algorithm>
#include <format>

namespace SFT::Ecs {

    namespace {

        [[nodiscard]] std::vector<ComponentField> sorted_by_offset(std::span<const ComponentField> fields) {
            std::vector<ComponentField> sorted(fields.begin(), fields.end());
            std::ranges::stable_sort(sorted, [](const ComponentField &a, const ComponentField &b) { return a.offset < b.offset; });
            return sorted;
        }

        [[nodiscard]] std::string_view kind_name(FieldKind kind) noexcept {
            switch (kind) {
                case FieldKind::Bool: return "bool";
                case FieldKind::I8: return "i8";
                case FieldKind::I16: return "i16";
                case FieldKind::I32: return "i32";
                case FieldKind::I64: return "i64";
                case FieldKind::U8: return "u8";
                case FieldKind::U16: return "u16";
                case FieldKind::U32: return "u32";
                case FieldKind::U64: return "u64";
                case FieldKind::F32: return "f32";
                case FieldKind::F64: return "f64";
            }
            return "?";
        }

        [[nodiscard]] std::string describe_field(const ComponentField &field) {
            const std::string name = field.name.empty() ? std::string{"(unnamed)"} : field.name;
            return std::format("{} {}[{}] at offset {}", name, kind_name(field.kind), field.count, field.offset);
        }

        /// FNV-1a over the little-endian bytes of `value`.
        void mix(u64 &hash, u64 prime, u64 value) noexcept {
            for (int shift = 0; shift < 64; shift += 8) {
                hash = (hash ^ ((value >> shift) & 0xffu)) * prime;
            }
        }

    } // namespace

    std::optional<std::string> validate_component_fields(usize size, std::span<const ComponentField> fields) {
        const std::vector<ComponentField> sorted = sorted_by_offset(fields);
        u64 previous_end = 0;
        for (const ComponentField &field : sorted) {
            const u32 kind_size = field_kind_size(field.kind);
            if (kind_size == 0) {
                return std::format("field '{}' has an unknown kind", field.name);
            }
            if (field.count == 0) {
                return std::format("field '{}' has a count of zero", field.name);
            }
            const u64 end = static_cast<u64>(field.offset) + static_cast<u64>(kind_size) * field.count;
            if (end > size) {
                return std::format("field '{}' ends at byte {}, past the component's {} bytes", field.name, end, size);
            }
            if (field.offset < previous_end) {
                return std::format("field '{}' overlaps the field before it", field.name);
            }
            previous_end = end;
        }
        return std::nullopt;
    }

    LayoutFingerprint fingerprint_component_layout(usize size, usize align, std::span<const ComponentField> fields) {
        constexpr u64 prime_high = 1099511628211ull;
        constexpr u64 prime_low = prime_high + 2ull;
        u64 high = 14695981039346656037ull;
        u64 low = 7809847782465536322ull;
        const auto feed = [&](u64 value) {
            mix(high, prime_high, value);
            mix(low, prime_low, value ^ 0x9d9d9d9d9d9d9d9dull);
        };
        feed(size);
        feed(align);
        const std::vector<ComponentField> sorted = sorted_by_offset(fields);
        feed(sorted.size());
        for (const ComponentField &field : sorted) {
            feed(static_cast<u64>(field.kind));
            feed(field.offset);
            feed(field.count);
        }
        if (high == 0 && low == 0) {
            low = 1;
        }
        return LayoutFingerprint{.high = high, .low = low};
    }

    std::string describe_layout_difference(usize size_a, usize align_a, std::span<const ComponentField> fields_a, usize size_b, usize align_b,
                                           std::span<const ComponentField> fields_b) {
        if (size_a != size_b) {
            return std::format("size {} vs {}", size_a, size_b);
        }
        if (align_a != align_b) {
            return std::format("alignment {} vs {}", align_a, align_b);
        }
        const std::vector<ComponentField> a = sorted_by_offset(fields_a);
        const std::vector<ComponentField> b = sorted_by_offset(fields_b);
        for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            if (a[i].kind != b[i].kind || a[i].offset != b[i].offset || a[i].count != b[i].count) {
                return std::format("field {} is [{}] vs [{}]", i, describe_field(a[i]), describe_field(b[i]));
            }
        }
        if (a.size() != b.size()) {
            return std::format("{} field(s) vs {}", a.size(), b.size());
        }
        return {};
    }

} // namespace SFT::Ecs
