#include "ShaderCache.hpp"

#pragma region Imports
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <system_error>
#include <type_traits>
#pragma endregion

using std::ifstream;
using std::ios;
using std::make_shared;
using std::ofstream;
using std::filesystem::path;

namespace SFT::Core::Slang {

    namespace {

        // Bumped whenever the on-disk layout below changes — a mismatch is just treated as a miss
        // (see load_shader_cache_entry()), so old entries are silently ignored rather than crashing.
        constexpr u32 shader_cache_format_version = 1;
        constexpr u32 shader_cache_magic = 0x53484341u; // "SHCA"

        // Separate magic/version/file-suffix from the compiled-bytecode cache above -- see
        // load/store_shader_reflection_cache_entry()'s doc comment (ShaderCache.hpp) for why.
        constexpr u32 shader_reflection_cache_format_version = 1;
        constexpr u32 shader_reflection_cache_magic = 0x53484352u; // "SHCR"

        // --- Binary writer/reader: flat, no alignment padding, little-endian (this cache never
        // crosses machines/processes with different endianness — it's a same-build local artifact). ---

        class ByteWriter {
          public:
            void write_bytes(const void *data, usize size) {
                const auto *bytes = static_cast<const byte *>(data);
                buffer_.insert(buffer_.end(), bytes, bytes + size);
            }

            template <typename T>
                requires std::is_trivially_copyable_v<T>
            void write(const T &value) {
                write_bytes(&value, sizeof(T));
            }

            void write_string(string_view value) {
                write(static_cast<u32>(value.size()));
                write_bytes(value.data(), value.size());
            }

            void write_bytes_vector(const vector<byte> &value) {
                write(static_cast<u32>(value.size()));
                if (!value.empty()) {
                    write_bytes(value.data(), value.size());
                }
            }

            [[nodiscard]] const vector<byte> &buffer() const noexcept { return buffer_; }

          private:
            vector<byte> buffer_;
        };

        class ByteReader {
          public:
            ByteReader(const byte *data, usize size) : data_(data), size_(size) {}

            [[nodiscard]] bool ok() const noexcept { return !failed_; }

            void read_bytes(void *out, usize size) {
                if (failed_ || offset_ + size > size_) {
                    failed_ = true;
                    return;
                }
                std::memcpy(out, data_ + offset_, size);
                offset_ += size;
            }

            template <typename T>
                requires std::is_trivially_copyable_v<T>
            [[nodiscard]] T read() {
                T value{};
                read_bytes(&value, sizeof(T));
                return value;
            }

            [[nodiscard]] string read_string() {
                const u32 len = read<u32>();
                if (failed_ || offset_ + len > size_) {
                    failed_ = true;
                    return {};
                }
                string value(reinterpret_cast<const char *>(data_ + offset_), len);
                offset_ += len;
                return value;
            }

            [[nodiscard]] vector<byte> read_bytes_vector() {
                const u32 len = read<u32>();
                if (failed_ || offset_ + len > size_) {
                    failed_ = true;
                    return {};
                }
                vector<byte> value(len);
                if (len > 0) {
                    std::memcpy(value.data(), data_ + offset_, len);
                }
                offset_ += len;
                return value;
            }

          private:
            const byte *data_;
            usize size_;
            usize offset_ = 0;
            bool failed_ = false;
        };

        // --- ShaderReflection struct-tree (de)serialization — mirrors ShaderReflection.hpp's shape
        // exactly. ShaderTypeReflection's shared_ptr fields form a tree in practice (shader type
        // graphs don't recurse), so this duplicates any incidentally-shared subtree rather than
        // building a dedup/back-reference table — simpler, and correct as long as that holds. ---

        void write_type_reflection(ByteWriter &w, const ShaderTypeReflection &type);
        [[nodiscard]] shared_ptr<ShaderTypeReflection> read_type_reflection(ByteReader &r);

        void write_binding_range(ByteWriter &w, const ShaderBindingRangeReflection &range) {
            w.write(static_cast<u32>(range.type));
            w.write(static_cast<u32>(range.category));
            w.write(range.descriptor_set);
            w.write(range.descriptor_range_index);
            w.write(range.descriptor_range_count);
            w.write(range.binding);
            w.write(range.count);
            w.write(range.image_format);
            w.write(static_cast<u8>(range.specializable ? 1 : 0));
        }

        [[nodiscard]] ShaderBindingRangeReflection read_binding_range(ByteReader &r) {
            ShaderBindingRangeReflection range{};
            range.type = static_cast<ShaderBindingType>(r.read<u32>());
            range.category = static_cast<ShaderParameterCategory>(r.read<u32>());
            range.descriptor_set = r.read<u32>();
            range.descriptor_range_index = r.read<u32>();
            range.descriptor_range_count = r.read<u32>();
            range.binding = r.read<u32>();
            range.count = r.read<u32>();
            range.image_format = r.read<u32>();
            range.specializable = r.read<u8>() != 0;
            return range;
        }

        template <typename T, typename WriteOne>
        void write_vector(ByteWriter &w, const vector<T> &items, WriteOne write_one) {
            w.write(static_cast<u32>(items.size()));
            for (const T &item : items) {
                write_one(w, item);
            }
        }

        template <typename T, typename ReadOne>
        [[nodiscard]] vector<T> read_vector(ByteReader &r, ReadOne read_one) {
            const u32 count = r.read<u32>();
            vector<T> items;
            if (!r.ok()) {
                return items;
            }
            items.reserve(count);
            for (u32 i = 0; i < count && r.ok(); ++i) {
                items.push_back(read_one(r));
            }
            return items;
        }

        void write_field(ByteWriter &w, const ShaderFieldReflection &field) {
            w.write_string(field.name);
            w.write(static_cast<u8>(field.type ? 1 : 0));
            if (field.type) {
                write_type_reflection(w, *field.type);
            }
            w.write(field.offset);
            w.write(field.size);
            w.write(field.stride);
        }

        [[nodiscard]] ShaderFieldReflection read_field(ByteReader &r) {
            ShaderFieldReflection field{};
            field.name = r.read_string();
            if (r.read<u8>() != 0) {
                field.type = read_type_reflection(r);
            }
            field.offset = r.read<u64>();
            field.size = r.read<u64>();
            field.stride = r.read<u64>();
            return field;
        }

        void write_type_reflection(ByteWriter &w, const ShaderTypeReflection &type) {
            w.write_string(type.name);
            w.write_string(type.full_name);
            w.write(static_cast<u32>(type.kind));
            w.write(static_cast<u32>(type.scalar_type));
            w.write(static_cast<u32>(type.resource_shape));
            w.write(static_cast<u32>(type.resource_access));
            w.write(static_cast<u32>(type.matrix_layout));
            w.write(type.row_count);
            w.write(type.column_count);
            w.write(type.element_count);
            w.write(type.size);
            w.write(type.stride);
            w.write(type.alignment);
            write_vector<ShaderFieldReflection>(w, type.fields, write_field);
            write_vector<ShaderBindingRangeReflection>(w, type.binding_ranges, write_binding_range);
        }

        [[nodiscard]] shared_ptr<ShaderTypeReflection> read_type_reflection(ByteReader &r) {
            auto type = make_shared<ShaderTypeReflection>();
            type->name = r.read_string();
            type->full_name = r.read_string();
            type->kind = static_cast<ShaderTypeKind>(r.read<u32>());
            type->scalar_type = static_cast<ShaderScalarType>(r.read<u32>());
            type->resource_shape = static_cast<ShaderResourceShape>(r.read<u32>());
            type->resource_access = static_cast<ShaderResourceAccess>(r.read<u32>());
            type->matrix_layout = static_cast<ShaderMatrixLayout>(r.read<u32>());
            type->row_count = r.read<u32>();
            type->column_count = r.read<u32>();
            type->element_count = r.read<u64>();
            type->size = r.read<u64>();
            type->stride = r.read<u64>();
            type->alignment = r.read<i32>();
            type->fields = read_vector<ShaderFieldReflection>(r, read_field);
            type->binding_ranges = read_vector<ShaderBindingRangeReflection>(r, read_binding_range);
            return type;
        }

        void write_parameter(ByteWriter &w, const ShaderParameterReflection &param) {
            w.write_string(param.name);
            w.write(static_cast<u8>(param.type ? 1 : 0));
            if (param.type) {
                write_type_reflection(w, *param.type);
            }
            w.write(static_cast<u32>(param.category));
            w.write(static_cast<u32>(param.stage));
            w.write(param.binding);
            w.write(param.binding_space);
            w.write(param.offset);
            w.write(param.size);
            w.write(param.stride);
            w.write_string(param.semantic_name);
            w.write(param.semantic_index);
            w.write(static_cast<u32>(param.categories.size()));
            for (const ShaderParameterCategory category : param.categories) {
                w.write(static_cast<u32>(category));
            }
            write_vector<ShaderBindingRangeReflection>(w, param.binding_ranges, write_binding_range);
        }

        [[nodiscard]] ShaderParameterReflection read_parameter(ByteReader &r) {
            ShaderParameterReflection param{};
            param.name = r.read_string();
            if (r.read<u8>() != 0) {
                param.type = read_type_reflection(r);
            }
            param.category = static_cast<ShaderParameterCategory>(r.read<u32>());
            param.stage = static_cast<ShaderStage>(r.read<u32>());
            param.binding = r.read<u32>();
            param.binding_space = r.read<u32>();
            param.offset = r.read<u64>();
            param.size = r.read<u64>();
            param.stride = r.read<u64>();
            param.semantic_name = r.read_string();
            param.semantic_index = r.read<u32>();
            const u32 category_count = r.read<u32>();
            param.categories.reserve(category_count);
            for (u32 i = 0; i < category_count && r.ok(); ++i) {
                param.categories.push_back(static_cast<ShaderParameterCategory>(r.read<u32>()));
            }
            param.binding_ranges = read_vector<ShaderBindingRangeReflection>(r, read_binding_range);
            return param;
        }

        void write_descriptor_range(ByteWriter &w, const ShaderDescriptorRangeReflection &range) {
            w.write(static_cast<u32>(range.type));
            w.write(static_cast<u32>(range.category));
            w.write(range.binding);
            w.write(range.count);
        }

        [[nodiscard]] ShaderDescriptorRangeReflection read_descriptor_range(ByteReader &r) {
            ShaderDescriptorRangeReflection range{};
            range.type = static_cast<ShaderBindingType>(r.read<u32>());
            range.category = static_cast<ShaderParameterCategory>(r.read<u32>());
            range.binding = r.read<u32>();
            range.count = r.read<u32>();
            return range;
        }

        void write_descriptor_set(ByteWriter &w, const ShaderDescriptorSetReflection &set) {
            w.write(set.space);
            write_vector<ShaderDescriptorRangeReflection>(w, set.ranges, write_descriptor_range);
        }

        [[nodiscard]] ShaderDescriptorSetReflection read_descriptor_set(ByteReader &r) {
            ShaderDescriptorSetReflection set{};
            set.space = r.read<u32>();
            set.ranges = read_vector<ShaderDescriptorRangeReflection>(r, read_descriptor_range);
            return set;
        }

        void write_entry_point(ByteWriter &w, const ShaderEntryPointReflection &entry_point) {
            w.write_string(entry_point.name);
            w.write_string(entry_point.name_override);
            w.write(static_cast<u32>(entry_point.stage));
            w.write(entry_point.compute_thread_group_size[0]);
            w.write(entry_point.compute_thread_group_size[1]);
            w.write(entry_point.compute_thread_group_size[2]);
            w.write(entry_point.compute_wave_size);
            w.write(static_cast<u8>(entry_point.uses_sample_rate_input ? 1 : 0));
            w.write(static_cast<u8>(entry_point.has_default_constant_buffer ? 1 : 0));
            write_vector<ShaderParameterReflection>(w, entry_point.parameters, write_parameter);
            write_vector<ShaderParameterReflection>(w, entry_point.result_parameters, write_parameter);
        }

        [[nodiscard]] ShaderEntryPointReflection read_entry_point(ByteReader &r) {
            ShaderEntryPointReflection entry_point{};
            entry_point.name = r.read_string();
            entry_point.name_override = r.read_string();
            entry_point.stage = static_cast<ShaderStage>(r.read<u32>());
            entry_point.compute_thread_group_size[0] = r.read<u32>();
            entry_point.compute_thread_group_size[1] = r.read<u32>();
            entry_point.compute_thread_group_size[2] = r.read<u32>();
            entry_point.compute_wave_size = r.read<u32>();
            entry_point.uses_sample_rate_input = r.read<u8>() != 0;
            entry_point.has_default_constant_buffer = r.read<u8>() != 0;
            entry_point.parameters = read_vector<ShaderParameterReflection>(r, read_parameter);
            entry_point.result_parameters = read_vector<ShaderParameterReflection>(r, read_parameter);
            return entry_point;
        }

        void write_reflection(ByteWriter &w, const ShaderReflection &reflection) {
            write_vector<ShaderParameterReflection>(w, reflection.global_parameters, write_parameter);
            write_vector<ShaderEntryPointReflection>(w, reflection.entry_points, write_entry_point);
            write_vector<ShaderDescriptorSetReflection>(w, reflection.descriptor_sets, write_descriptor_set);
            w.write(static_cast<u32>(reflection.hashed_strings.size()));
            for (const string &hashed_string : reflection.hashed_strings) {
                w.write_string(hashed_string);
            }
            w.write_string(reflection.json);
            w.write(reflection.global_constant_buffer_binding);
            w.write(reflection.global_constant_buffer_size);
            w.write(reflection.bindless_space_index);
        }

        [[nodiscard]] ShaderReflection read_reflection(ByteReader &r) {
            ShaderReflection reflection{};
            reflection.global_parameters = read_vector<ShaderParameterReflection>(r, read_parameter);
            reflection.entry_points = read_vector<ShaderEntryPointReflection>(r, read_entry_point);
            reflection.descriptor_sets = read_vector<ShaderDescriptorSetReflection>(r, read_descriptor_set);
            const u32 hashed_string_count = r.read<u32>();
            reflection.hashed_strings.reserve(hashed_string_count);
            for (u32 i = 0; i < hashed_string_count && r.ok(); ++i) {
                reflection.hashed_strings.push_back(r.read_string());
            }
            reflection.json = r.read_string();
            reflection.global_constant_buffer_binding = r.read<u32>();
            reflection.global_constant_buffer_size = r.read<u64>();
            reflection.bindless_space_index = r.read<i32>();
            return reflection;
        }

        void write_target(ByteWriter &w, const ShaderTarget &target) {
            w.write(static_cast<u32>(target.format));
            w.write_string(target.profile);
        }

        [[nodiscard]] ShaderTarget read_target(ByteReader &r) {
            ShaderTarget target{};
            target.format = static_cast<ShaderTargetFormat>(r.read<u32>());
            target.profile = r.read_string();
            return target;
        }

        void write_bytecode(ByteWriter &w, const ShaderBytecode &bytecode) {
            w.write(static_cast<u32>(bytecode.target));
            w.write_string(bytecode.profile);
            w.write_string(bytecode.entry_point);
            w.write_bytes_vector(bytecode.bytes);
        }

        [[nodiscard]] ShaderBytecode read_bytecode(ByteReader &r) {
            ShaderBytecode bytecode{};
            bytecode.target = static_cast<ShaderTargetFormat>(r.read<u32>());
            bytecode.profile = r.read_string();
            bytecode.entry_point = r.read_string();
            bytecode.bytes = r.read_bytes_vector();
            return bytecode;
        }

        [[nodiscard]] path cache_file_path(const path &directory, u64 key) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
            return directory / (string{hex} + ".sc");
        }

        [[nodiscard]] path reflection_cache_file_path(const path &directory, u64 key) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(key));
            return directory / (string{hex} + ".sr");
        }

    } // namespace

    [[nodiscard]] u64 compute_shader_cache_key(
        string_view module_name,
        string_view source_text,
        string_view variant_canonical,
        const ShaderCompileOptions &options) noexcept {
        u64 value = 0xcbf29ce484222325ull;
        const auto mix_byte = [&value](u8 byte) { value = (value ^ byte) * 0x100000001b3ull; };
        const auto mix_text = [&](string_view text) {
            for (const char c : text) {
                mix_byte(static_cast<u8>(c));
            }
            mix_byte(0);
        };
        const auto mix_u32 = [&](u32 v) {
            for (int i = 0; i < 4; ++i) {
                mix_byte(static_cast<u8>((v >> (i * 8)) & 0xFFu));
            }
        };

        mix_u32(shader_cache_format_version);
        mix_text(module_name);
        mix_text(source_text);
        mix_text(variant_canonical);
        mix_u32(static_cast<u32>(options.targets.size()));
        for (const ShaderTarget &target : options.targets) {
            mix_u32(static_cast<u32>(target.format));
            mix_text(target.profile);
        }
        mix_u32(static_cast<u32>(options.optimization));
        mix_byte(static_cast<u8>(options.allow_glsl_syntax ? 1 : 0));
        mix_byte(static_cast<u8>(options.skip_spirv_validation ? 1 : 0));
        mix_byte(static_cast<u8>(options.enable_effect_annotations ? 1 : 0));
        for (const string &search_path : options.search_paths) {
            mix_text(search_path);
        }
        for (const ShaderEntryPointRequest &entry_point : options.entry_points) {
            mix_text(entry_point.name);
            mix_u32(static_cast<u32>(entry_point.stage));
        }
        return value;
    }

    [[nodiscard]] optional<ShaderCacheEntry> load_shader_cache_entry(
        const path &directory, u64 key) {
        ifstream file(cache_file_path(directory, key), ios::binary);
        if (!file) {
            return std::nullopt;
        }

        string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (contents.size() < sizeof(u32) * 2) {
            return std::nullopt;
        }

        ByteReader r(reinterpret_cast<const byte *>(contents.data()), contents.size());
        if (r.read<u32>() != shader_cache_magic || r.read<u32>() != shader_cache_format_version) {
            return std::nullopt;
        }

        ShaderCacheEntry entry{};
        entry.module_name = r.read_string();
        entry.targets = read_vector<ShaderTarget>(r, read_target);
        entry.reflection = read_reflection(r);
        entry.bytecode = read_vector<ShaderBytecode>(r, read_bytecode);
        if (!r.ok()) {
            return std::nullopt;
        }
        return entry;
    }

    bool store_shader_cache_entry(
        const path &directory, u64 key, const ShaderCacheEntry &entry) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }

        ByteWriter w;
        w.write(shader_cache_magic);
        w.write(shader_cache_format_version);
        w.write_string(entry.module_name);
        write_vector<ShaderTarget>(w, entry.targets, write_target);
        write_reflection(w, entry.reflection);
        write_vector<ShaderBytecode>(w, entry.bytecode, write_bytecode);

        // Write to a temp file then rename, so a crash/power-loss mid-write never leaves a truncated
        // .sc file that load_shader_cache_entry() could mistake for a valid (if corrupt) entry.
        const path final_path = cache_file_path(directory, key);
        const path temp_path = final_path.string() + ".tmp";
        {
            ofstream file(temp_path, ios::binary | ios::trunc);
            if (!file) {
                return false;
            }
            file.write(reinterpret_cast<const char *>(w.buffer().data()), static_cast<std::streamsize>(w.buffer().size()));
            if (!file) {
                return false;
            }
        }
        std::filesystem::rename(temp_path, final_path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
        return true;
    }

    [[nodiscard]] optional<ShaderReflection> load_shader_reflection_cache_entry(
        const path &directory, u64 key) {
        ifstream file(reflection_cache_file_path(directory, key), ios::binary);
        if (!file) {
            return std::nullopt;
        }

        string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (contents.size() < sizeof(u32) * 2) {
            return std::nullopt;
        }

        ByteReader r(reinterpret_cast<const byte *>(contents.data()), contents.size());
        if (r.read<u32>() != shader_reflection_cache_magic || r.read<u32>() != shader_reflection_cache_format_version) {
            return std::nullopt;
        }

        ShaderReflection reflection = read_reflection(r);
        if (!r.ok()) {
            return std::nullopt;
        }
        return reflection;
    }

    bool store_shader_reflection_cache_entry(
        const path &directory, u64 key, const ShaderReflection &reflection) {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            return false;
        }

        ByteWriter w;
        w.write(shader_reflection_cache_magic);
        w.write(shader_reflection_cache_format_version);
        write_reflection(w, reflection);

        const path final_path = reflection_cache_file_path(directory, key);
        const path temp_path = final_path.string() + ".tmp";
        {
            ofstream file(temp_path, ios::binary | ios::trunc);
            if (!file) {
                return false;
            }
            file.write(reinterpret_cast<const char *>(w.buffer().data()), static_cast<std::streamsize>(w.buffer().size()));
            if (!file) {
                return false;
            }
        }
        std::filesystem::rename(temp_path, final_path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
        return true;
    }

} // namespace SFT::Core::Slang
