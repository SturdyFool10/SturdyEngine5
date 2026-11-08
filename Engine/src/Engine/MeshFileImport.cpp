#include <Engine/MeshFileImport.hpp>

#include <Engine/AssetManager.hpp>
#include <Engine/ImportCommon.hpp>

#include <Foundation/FileIo.hpp>
#include <Renderer/Mesh.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] AssetError fail(AssetErrorCode code, std::string message, std::string_view name) {
            return AssetError{.code = code, .message = UString{std::move(message)}, .source = std::filesystem::path{std::string{name}}};
        }

        struct Geometry {
            std::vector<Renderer::GeometryVertex> vertices;
            std::vector<u32> indices;
            bool has_normals = false;
            bool has_uvs = false;
        };

        /// Registers `geometry` as a model with a default material and a Z-up -> Y-up instance.
        [[nodiscard]] AssetExpected<GltfImportResult> make_scene(AssetManager &assets, Geometry geometry, std::string_view name,
                                                                 Asset shader) {
            if (geometry.vertices.empty() || geometry.indices.size() < 3) {
                return std::unexpected(fail(AssetErrorCode::InvalidDescription, "The file contains no triangles.", name));
            }
            if (!geometry.has_normals) {
                Detail::generate_normals(geometry.vertices, geometry.indices);
            }
            if (geometry.has_uvs) {
                Detail::generate_tangents(geometry.vertices, geometry.indices);
            }
            const std::string label{name};
            ModelAssetDesc desc;
            desc.label = UString{label};
            ModelPrimitiveDesc primitive{
                .mesh = Renderer::Mesh::from_vertices(geometry.vertices, geometry.indices, label.c_str()),
                .shader = shader,
            };
            AssetExpected<Asset> flat_normal = assets.create_texture(TextureAssetDesc{
                .width = 1, .height = 1, .color_space = TextureColorSpace::Linear,
                .pixels = std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}},
                .label = UString{"mesh flat normal"_ustr}});
            if (flat_normal) {
                primitive.textures.push_back(ModelTextureBinding{.slot = UString{"normal_texture"_ustr}, .texture = *flat_normal});
            }
            desc.primitives.push_back(std::move(primitive));
            AssetExpected<Asset> model = assets.create_model(std::move(desc));
            if (!model) {
                return std::unexpected(model.error());
            }
            if (AssetResult set = Detail::apply_material_values(assets, *model, 0, Detail::ImportMaterialValues{}); !set) {
                (void)assets.unload(*model);
                return std::unexpected(set.error());
            }
            GltfImportResult out;
            out.models.push_back(*model);
            out.instances.push_back(GltfNodeInstance{
                .name = UString{label},
                .model = *model,
                .world_transform = glm::rotate(glm::mat4{1.0f}, glm::radians(-90.0f), glm::vec3{1.0f, 0.0f, 0.0f}),
            });
            return out;
        }

        // ---- PLY -----------------------------------------------------------------------------------------------

        enum class PlyType : u8 { I8, U8, I16, U16, I32, U32, F32, F64 };

        [[nodiscard]] std::optional<PlyType> ply_type(std::string_view t) {
            if (t == "char" || t == "int8") return PlyType::I8;
            if (t == "uchar" || t == "uint8") return PlyType::U8;
            if (t == "short" || t == "int16") return PlyType::I16;
            if (t == "ushort" || t == "uint16") return PlyType::U16;
            if (t == "int" || t == "int32") return PlyType::I32;
            if (t == "uint" || t == "uint32") return PlyType::U32;
            if (t == "float" || t == "float32") return PlyType::F32;
            if (t == "double" || t == "float64") return PlyType::F64;
            return std::nullopt;
        }

        [[nodiscard]] usize ply_size(PlyType t) {
            switch (t) {
            case PlyType::I8: case PlyType::U8: return 1;
            case PlyType::I16: case PlyType::U16: return 2;
            case PlyType::I32: case PlyType::U32: case PlyType::F32: return 4;
            case PlyType::F64: return 8;
            }
            return 0;
        }

        struct PlyProperty {
            std::string name;
            PlyType type = PlyType::F32;
            bool is_list = false;
            PlyType count_type = PlyType::U8;
        };

        struct PlyElement {
            std::string name;
            usize count = 0;
            std::vector<PlyProperty> properties;
        };

        /// Reads the values of a PLY body, binary or ASCII, behind one interface.
        class PlyReader {
          public:
            PlyReader(std::span<const std::byte> body, bool ascii, bool big_endian)
                : body_(body), ascii_(ascii), big_(big_endian) {}

            [[nodiscard]] bool read(PlyType type, f64 &out) {
                if (ascii_) {
                    while (at_ < body_.size() && std::isspace(static_cast<unsigned char>(body_[at_]))) ++at_;
                    if (at_ >= body_.size()) return false;
                    const char *begin = reinterpret_cast<const char *>(body_.data()) + at_;
                    const char *end = reinterpret_cast<const char *>(body_.data()) + body_.size();
                    const char *stop = begin;
                    while (stop < end && !std::isspace(static_cast<unsigned char>(*stop))) ++stop;
                    const std::string token(begin, stop);
                    char *parsed = nullptr;
                    out = std::strtod(token.c_str(), &parsed);
                    at_ += static_cast<usize>(stop - begin);
                    return parsed != token.c_str();
                }
                const usize size = ply_size(type);
                if (at_ + size > body_.size()) return false;
                unsigned char raw[8];
                std::memcpy(raw, body_.data() + at_, size);
                at_ += size;
                if (big_) std::reverse(raw, raw + size);
                switch (type) {
                case PlyType::I8: out = static_cast<i8>(raw[0]); break;
                case PlyType::U8: out = raw[0]; break;
                case PlyType::I16: { i16 v; std::memcpy(&v, raw, 2); out = v; break; }
                case PlyType::U16: { u16 v; std::memcpy(&v, raw, 2); out = v; break; }
                case PlyType::I32: { i32 v; std::memcpy(&v, raw, 4); out = v; break; }
                case PlyType::U32: { u32 v; std::memcpy(&v, raw, 4); out = v; break; }
                case PlyType::F32: { f32 v; std::memcpy(&v, raw, 4); out = v; break; }
                case PlyType::F64: { f64 v; std::memcpy(&v, raw, 8); out = v; break; }
                }
                return true;
            }

          private:
            std::span<const std::byte> body_;
            usize at_ = 0;
            bool ascii_;
            bool big_;
        };

    } // namespace

    AssetExpected<GltfImportResult> import_ply_bytes(AssetManager &assets, std::span<const std::byte> bytes,
                                                     std::string_view name, Asset shader) {
        const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
        if (!text.starts_with("ply")) {
            return std::unexpected(fail(AssetErrorCode::DecodeFailure, "Not a PLY file.", name));
        }
        const usize header_end_marker = text.find("end_header");
        if (header_end_marker == std::string_view::npos) {
            return std::unexpected(fail(AssetErrorCode::DecodeFailure, "The PLY header is not terminated.", name));
        }
        usize body_start = text.find('\n', header_end_marker);
        if (body_start == std::string_view::npos) {
            return std::unexpected(fail(AssetErrorCode::DecodeFailure, "The PLY file has no data.", name));
        }
        ++body_start;

        bool ascii = false, big = false;
        std::vector<PlyElement> elements;
        usize line_start = 0;
        while (line_start < header_end_marker) {
            usize line_end = text.find('\n', line_start);
            if (line_end == std::string_view::npos || line_end > header_end_marker) line_end = header_end_marker;
            std::string line(text.substr(line_start, line_end - line_start));
            line_start = line_end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> words;
            for (usize i = 0; i < line.size();) {
                while (i < line.size() && line[i] == ' ') ++i;
                usize j = i;
                while (j < line.size() && line[j] != ' ') ++j;
                if (j > i) words.emplace_back(line.substr(i, j - i));
                i = j;
            }
            if (words.empty()) continue;
            if (words[0] == "format" && words.size() >= 2) {
                ascii = words[1] == "ascii";
                big = words[1] == "binary_big_endian";
            } else if (words[0] == "element" && words.size() >= 3) {
                elements.push_back(PlyElement{.name = words[1], .count = static_cast<usize>(std::strtoull(words[2].c_str(), nullptr, 10))});
            } else if (words[0] == "property" && !elements.empty()) {
                PlyProperty property;
                if (words.size() >= 5 && words[1] == "list") {
                    const auto count_type = ply_type(words[2]);
                    const auto item_type = ply_type(words[3]);
                    if (!count_type || !item_type) {
                        return std::unexpected(fail(AssetErrorCode::Unsupported, "Unknown PLY list type.", name));
                    }
                    property = PlyProperty{.name = words[4], .type = *item_type, .is_list = true, .count_type = *count_type};
                } else if (words.size() >= 3) {
                    const auto type = ply_type(words[1]);
                    if (!type) {
                        return std::unexpected(fail(AssetErrorCode::Unsupported, "Unknown PLY property type '" + words[1] + "'.", name));
                    }
                    property = PlyProperty{.name = words[2], .type = *type};
                }
                elements.back().properties.push_back(std::move(property));
            }
        }

        PlyReader reader(bytes.subspan(body_start), ascii, big);
        Geometry geometry;
        for (const PlyElement &element : elements) {
            const bool is_vertex = element.name == "vertex";
            const bool is_face = element.name == "face";
            if (is_vertex) geometry.vertices.reserve(element.count);
            for (usize row = 0; row < element.count; ++row) {
                Renderer::GeometryVertex vertex;
                std::vector<u32> polygon;
                for (const PlyProperty &property : element.properties) {
                    f64 value = 0.0;
                    if (property.is_list) {
                        f64 count = 0.0;
                        if (!reader.read(property.count_type, count)) {
                            return std::unexpected(fail(AssetErrorCode::DecodeFailure, "The PLY data is truncated.", name));
                        }
                        const bool wanted = is_face && (property.name == "vertex_indices" || property.name == "vertex_index");
                        for (i64 k = 0; k < static_cast<i64>(count); ++k) {
                            if (!reader.read(property.type, value)) {
                                return std::unexpected(fail(AssetErrorCode::DecodeFailure, "The PLY data is truncated.", name));
                            }
                            if (wanted) polygon.push_back(static_cast<u32>(value));
                        }
                        continue;
                    }
                    if (!reader.read(property.type, value)) {
                        return std::unexpected(fail(AssetErrorCode::DecodeFailure, "The PLY data is truncated.", name));
                    }
                    if (!is_vertex) continue;
                    const bool byte_colour = property.type == PlyType::U8;
                    const f32 v = static_cast<f32>(value);
                    const std::string &n = property.name;
                    if (n == "x") vertex.position.x = v;
                    else if (n == "y") vertex.position.y = v;
                    else if (n == "z") vertex.position.z = v;
                    else if (n == "nx") { vertex.normal.x = v; geometry.has_normals = true; }
                    else if (n == "ny") vertex.normal.y = v;
                    else if (n == "nz") vertex.normal.z = v;
                    else if (n == "s" || n == "u" || n == "texture_u" || n == "texture_s") { vertex.uv.x = v; geometry.has_uvs = true; }
                    else if (n == "t" || n == "v" || n == "texture_v" || n == "texture_t") vertex.uv.y = v;
                    else if (n == "red" || n == "r") vertex.color.r = byte_colour ? v / 255.0f : v;
                    else if (n == "green" || n == "g") vertex.color.g = byte_colour ? v / 255.0f : v;
                    else if (n == "blue" || n == "b") vertex.color.b = byte_colour ? v / 255.0f : v;
                    else if (n == "alpha" || n == "a") vertex.color.a = byte_colour ? v / 255.0f : v;
                }
                if (is_vertex) {
                    geometry.vertices.push_back(vertex);
                } else if (is_face) {
                    for (usize k = 2; k < polygon.size(); ++k) { // fan: convex polygons, which is what PLY holds
                        geometry.indices.insert(geometry.indices.end(), {polygon[0], polygon[k - 1], polygon[k]});
                    }
                }
            }
        }
        for (u32 index : geometry.indices) {
            if (index >= geometry.vertices.size()) {
                return std::unexpected(fail(AssetErrorCode::DecodeFailure, "A PLY face refers to a vertex that does not exist.", name));
            }
        }
        return make_scene(assets, std::move(geometry), name, shader);
    }

    // ---- STL ---------------------------------------------------------------------------------------------------

    AssetExpected<GltfImportResult> import_stl_bytes(AssetManager &assets, std::span<const std::byte> bytes,
                                                     std::string_view name, Asset shader) {
        Geometry geometry;
        geometry.has_normals = true;
        const auto emit = [&](const glm::vec3 &a, const glm::vec3 &b, const glm::vec3 &c, glm::vec3 normal) {
            const glm::vec3 computed = glm::cross(b - a, c - a);
            if (glm::dot(normal, normal) < 1.0e-12f) {
                normal = glm::dot(computed, computed) > 0.0f ? glm::normalize(computed) : glm::vec3{0.0f, 0.0f, 1.0f};
            }
            const u32 base = static_cast<u32>(geometry.vertices.size());
            for (const glm::vec3 &p : {a, b, c}) {
                Renderer::GeometryVertex vertex;
                vertex.position = p;
                vertex.normal = normal;
                geometry.vertices.push_back(vertex);
            }
            geometry.indices.insert(geometry.indices.end(), {base, base + 1, base + 2});
        };

        u32 declared = 0;
        const bool binary_size_matches = bytes.size() >= 84 && [&] {
            std::memcpy(&declared, bytes.data() + 80, 4);
            return bytes.size() == 84 + static_cast<usize>(declared) * 50;
        }();
        if (binary_size_matches) {
            for (u32 i = 0; i < declared; ++i) {
                f32 f[12];
                std::memcpy(f, bytes.data() + 84 + static_cast<usize>(i) * 50, 48);
                emit({f[3], f[4], f[5]}, {f[6], f[7], f[8]}, {f[9], f[10], f[11]}, {f[0], f[1], f[2]});
            }
        } else {
            const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            if (text.find("solid") == std::string::npos || text.find("facet") == std::string::npos) {
                return std::unexpected(fail(AssetErrorCode::DecodeFailure, "Not a valid STL file.", name));
            }
            glm::vec3 normal{0.0f};
            std::vector<glm::vec3> corners;
            usize at = 0;
            while (at < text.size()) {
                usize end = text.find('\n', at);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(at, end - at);
                at = end + 1;
                const auto number_after = [&](const char *key, glm::vec3 &out) {
                    const usize p = line.find(key);
                    if (p == std::string::npos) return false;
                    return std::sscanf(line.c_str() + p + std::strlen(key), "%f %f %f", &out.x, &out.y, &out.z) == 3;
                };
                if (line.find("facet normal") != std::string::npos) {
                    corners.clear();
                    number_after("normal", normal);
                } else if (glm::vec3 vertex; line.find("vertex") != std::string::npos && number_after("vertex", vertex)) {
                    corners.push_back(vertex);
                } else if (line.find("endfacet") != std::string::npos && corners.size() == 3) {
                    emit(corners[0], corners[1], corners[2], normal);
                }
            }
        }
        return make_scene(assets, std::move(geometry), name, shader);
    }

    AssetExpected<GltfImportResult> import_ply(AssetManager &assets, const std::filesystem::path &source, Asset shader) {
        auto bytes = Foundation::Io::read_file(source);
        if (!bytes) {
            return std::unexpected(AssetError{.code = AssetErrorCode::IoFailure, .message = UString{bytes.error()}, .source = source});
        }
        return import_ply_bytes(assets, *bytes, source.stem().string(), shader);
    }

    AssetExpected<GltfImportResult> import_stl(AssetManager &assets, const std::filesystem::path &source, Asset shader) {
        auto bytes = Foundation::Io::read_file(source);
        if (!bytes) {
            return std::unexpected(AssetError{.code = AssetErrorCode::IoFailure, .message = UString{bytes.error()}, .source = source});
        }
        return import_stl_bytes(assets, *bytes, source.stem().string(), shader);
    }

} // namespace SFT::Engine
