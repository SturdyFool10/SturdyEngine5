#include "ShaderCache.hpp"

#pragma region Imports
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
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






        constexpr u32 shader_cache_format_version = 2;
        constexpr u32 shader_cache_magic = 0x53484341u;



        constexpr u32 shader_reflection_cache_format_version = 3;
        constexpr u32 shader_reflection_cache_magic = 0x53484352u;



        constexpr usize max_shader_cache_file_bytes = 128ull * 1024ull * 1024ull;
        constexpr u32 max_shader_cache_collection_items = 65'536;
        constexpr usize max_shader_reflection_nesting_depth = 64;

        [[nodiscard]] optional<string> read_cache_file(const path &file_path) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(file_path, ec);
            if (ec || size > max_shader_cache_file_bytes) {
                return std::nullopt;
            }

            ifstream file(file_path, ios::binary);
            if (!file) {
                return std::nullopt;
            }

            string contents(static_cast<usize>(size), '\0');
            if (!contents.empty() &&
                !file.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
                return std::nullopt;
            }
            return contents;
        }

        // --- Binary writer/reader: flat, no alignment padding, little-endian (this cache never
        // crosses machines/processes with different endianness — it's a same-build local artifact). ---

        class ByteWriter {
          public:
            void write_bytes(const void *data, usize size) {
                if (size == 0) {
                    return;
                }
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
            [[nodiscard]] usize remaining() const noexcept { return failed_ ? 0 : size_ - offset_; }

            void fail() noexcept { failed_ = true; }

            void read_bytes(void *out, usize size) {
                if (failed_ || size > remaining()) {
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
                if (failed_ || len > remaining()) {
                    failed_ = true;
                    return {};
                }
                string value(reinterpret_cast<const char *>(data_ + offset_), len);
                offset_ += len;
                return value;
            }

            [[nodiscard]] vector<byte> read_bytes_vector() {
                const u32 len = read<u32>();
                if (failed_ || len > remaining()) {
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






        void write_type_reflection(ByteWriter &w, const ShaderTypeReflection &type);
        [[nodiscard]] shared_ptr<ShaderTypeReflection> read_type_reflection(ByteReader &r, usize depth = 0);

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

        [[nodiscard]] bool valid_collection_count(ByteReader &r, u32 count, usize minimum_item_bytes) {
            if (!r.ok() || count > max_shader_cache_collection_items || minimum_item_bytes == 0 ||
                static_cast<usize>(count) > r.remaining() / minimum_item_bytes) {
                r.fail();
                return false;
            }
            return true;
        }

        template <typename T, typename ReadOne>
        [[nodiscard]] vector<T> read_vector(ByteReader &r, usize minimum_item_bytes, ReadOne read_one) {
            const u32 count = r.read<u32>();
            vector<T> items;
            if (!valid_collection_count(r, count, minimum_item_bytes)) {
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

        [[nodiscard]] ShaderFieldReflection read_field(ByteReader &r, usize depth) {
            ShaderFieldReflection field{};
            field.name = r.read_string();
            if (r.read<u8>() != 0) {
                field.type = read_type_reflection(r, depth);
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

        [[nodiscard]] shared_ptr<ShaderTypeReflection> read_type_reflection(ByteReader &r, usize depth) {
            if (!r.ok() || depth >= max_shader_reflection_nesting_depth) {
                r.fail();
                return {};
            }

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
            type->fields = read_vector<ShaderFieldReflection>(
                r, sizeof(u32) + sizeof(u8) + sizeof(u64) * 3,
                [depth](ByteReader &reader) { return read_field(reader, depth + 1); });
            type->binding_ranges = read_vector<ShaderBindingRangeReflection>(
                r, sizeof(u32) * 8 + sizeof(u8), read_binding_range);
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
            if (!valid_collection_count(r, category_count, sizeof(u32))) {
                return param;
            }
            param.categories.reserve(category_count);
            for (u32 i = 0; i < category_count && r.ok(); ++i) {
                param.categories.push_back(static_cast<ShaderParameterCategory>(r.read<u32>()));
            }
            param.binding_ranges = read_vector<ShaderBindingRangeReflection>(
                r, sizeof(u32) * 8 + sizeof(u8), read_binding_range);
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
            set.ranges = read_vector<ShaderDescriptorRangeReflection>(r, sizeof(u32) * 4, read_descriptor_range);
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
            constexpr usize minimum_parameter_bytes = sizeof(u32) * 9 + sizeof(u64) * 3 + sizeof(u8);
            entry_point.parameters = read_vector<ShaderParameterReflection>(r, minimum_parameter_bytes, read_parameter);
            entry_point.result_parameters = read_vector<ShaderParameterReflection>(r, minimum_parameter_bytes, read_parameter);
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
            constexpr usize minimum_parameter_bytes = sizeof(u32) * 9 + sizeof(u64) * 3 + sizeof(u8);
            constexpr usize minimum_entry_point_bytes = sizeof(u32) * 9 + sizeof(u8) * 2;
            reflection.global_parameters = read_vector<ShaderParameterReflection>(r, minimum_parameter_bytes, read_parameter);
            reflection.entry_points = read_vector<ShaderEntryPointReflection>(r, minimum_entry_point_bytes, read_entry_point);
            reflection.descriptor_sets = read_vector<ShaderDescriptorSetReflection>(r, sizeof(u32) * 2, read_descriptor_set);
            const u32 hashed_string_count = r.read<u32>();
            if (!valid_collection_count(r, hashed_string_count, sizeof(u32))) {
                return reflection;
            }
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

        void write_target_artifact(ByteWriter &w, const ShaderCacheTargetArtifact &artifact) {
            write_target(w, artifact.target);
            write_reflection(w, artifact.reflection);
            write_vector<ShaderBytecode>(w, artifact.bytecode, write_bytecode);
        }

        [[nodiscard]] ShaderCacheTargetArtifact read_target_artifact(ByteReader &r) {
            ShaderCacheTargetArtifact artifact{};
            artifact.target = read_target(r);
            artifact.reflection = read_reflection(r);
            artifact.bytecode = read_vector<ShaderBytecode>(r, sizeof(u32) * 4, read_bytecode);
            return artifact;
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

    [[nodiscard]] bool shader_cache_entry_is_fresh(
        const path &directory, u64 key, const path &shader_source_path) noexcept {
        std::error_code error;
        const std::filesystem::file_time_type cache_time =
            std::filesystem::last_write_time(cache_file_path(directory, key), error);
        if (error) {
            return false;
        }
        const std::filesystem::file_time_type source_time =
            std::filesystem::last_write_time(shader_source_path, error);
        if (error) {
            return false;
        }
        return cache_time >= source_time;
    }

    [[nodiscard]] optional<ShaderCacheEntry> load_shader_cache_entry(
        const path &directory, u64 key) {
        try {
            const optional<string> contents = read_cache_file(cache_file_path(directory, key));
            if (!contents || contents->size() < sizeof(u32) * 2) {
                return std::nullopt;
            }

            ByteReader r(reinterpret_cast<const byte *>(contents->data()), contents->size());
            if (r.read<u32>() != shader_cache_magic || r.read<u32>() != shader_cache_format_version) {
                return std::nullopt;
            }

            ShaderCacheEntry entry{};
            entry.module_name = r.read_string();
            entry.artifacts = read_vector<ShaderCacheTargetArtifact>(
                r, sizeof(u32) * 2, read_target_artifact);
            if (!r.ok()) {
                return std::nullopt;
            }
            return entry;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool store_shader_cache_entry(
        const path &directory, u64 key, const ShaderCacheEntry &entry) {
        try {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                return false;
            }

            ByteWriter w;
            w.write(shader_cache_magic);
            w.write(shader_cache_format_version);
            w.write_string(entry.module_name);
            write_vector<ShaderCacheTargetArtifact>(w, entry.artifacts, write_target_artifact);
            if (w.buffer().size() > max_shader_cache_file_bytes) {
                return false;
            }



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
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] optional<ShaderReflection> load_shader_reflection_cache_entry(
        const path &directory, u64 key) {
        try {
            const optional<string> contents = read_cache_file(reflection_cache_file_path(directory, key));
            if (!contents || contents->size() < sizeof(u32) * 2) {
                return std::nullopt;
            }

            ByteReader r(reinterpret_cast<const byte *>(contents->data()), contents->size());
            if (r.read<u32>() != shader_reflection_cache_magic || r.read<u32>() != shader_reflection_cache_format_version) {
                return std::nullopt;
            }

            ShaderReflection reflection = read_reflection(r);
            if (!r.ok()) {
                return std::nullopt;
            }
            return reflection;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool store_shader_reflection_cache_entry(
        const path &directory, u64 key, const ShaderReflection &reflection) {
        try {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec) {
                return false;
            }

            ByteWriter w;
            w.write(shader_reflection_cache_magic);
            w.write(shader_reflection_cache_format_version);
            write_reflection(w, reflection);
            if (w.buffer().size() > max_shader_cache_file_bytes) {
                return false;
            }

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
        } catch (...) {
            return false;
        }
    }

} // namespace SFT::Core::Slang
