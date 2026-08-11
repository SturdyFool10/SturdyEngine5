#include "Mesh.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

[[nodiscard]] span<const GeometryVertex> Mesh::vertices() const noexcept { return vertices_; }

[[nodiscard]] span<const u32> Mesh::indices() const noexcept { return indices_; }

[[nodiscard]] const UString &Mesh::label() const noexcept { return label_; }

void Mesh::set_label(UString label) noexcept { label_ = std::move(label); }

void Mesh::set_vertex_color(const glm::vec4 &color) noexcept {
            ZoneScopedN("Mesh::set_vertex_color");
            for (GeometryVertex &vertex : vertices_) {
                vertex.color = color;
            }
        }

[[nodiscard]] usize Mesh::triangle_count() const noexcept {
            ZoneScopedN("Mesh::triangle_count");
            return !indices_.empty() ? indices_.size() / 3 : vertices_.size() / 3;
        }

[[nodiscard]] u64 Mesh::estimated_gpu_bytes() const noexcept {
            ZoneScopedN("Mesh::estimated_gpu_bytes");
            return static_cast<u64>(vertices_.size()) * sizeof(GeometryVertex) +
                   static_cast<u64>(indices_.size()) * sizeof(u32);
        }

[[nodiscard]] bool Mesh::is_gpu_resident() const noexcept { return gpu_resident_; }

[[nodiscard]] MeshHandle Mesh::gpu_handle() const noexcept { return handle_; }

void Mesh::mark_uploaded(MeshHandle handle) noexcept {
            ZoneScopedN("Mesh::mark_uploaded");
            handle_ = handle;
            gpu_resident_ = true;
        }

void Mesh::mark_evicted() noexcept {
            ZoneScopedN("Mesh::mark_evicted");
            handle_ = {};
            gpu_resident_ = false;
        }

} // namespace SFT::Renderer
