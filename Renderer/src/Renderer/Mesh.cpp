#include "Mesh.hpp"

namespace SFT::Renderer {

[[nodiscard]] span<const GeometryVertex> Mesh::vertices() const noexcept { return vertices_; }

[[nodiscard]] span<const u32> Mesh::indices() const noexcept { return indices_; }

[[nodiscard]] const string &Mesh::label() const noexcept { return label_; }

void Mesh::set_label(string label) noexcept { label_ = std::move(label); }

void Mesh::set_vertex_color(const glm::vec4 &color) noexcept {
            for (GeometryVertex &vertex : vertices_) {
                vertex.color = color;
            }
        }

[[nodiscard]] bool Mesh::is_gpu_resident() const noexcept { return gpu_resident_; }

[[nodiscard]] MeshHandle Mesh::gpu_handle() const noexcept { return handle_; }

void Mesh::mark_uploaded(MeshHandle handle) noexcept {
            handle_ = handle;
            gpu_resident_ = true;
        }

void Mesh::mark_evicted() noexcept {
            handle_ = {};
            gpu_resident_ = false;
        }

} // namespace SFT::Renderer
