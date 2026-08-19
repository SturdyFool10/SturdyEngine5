#include <Renderer/Mesh.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

/// Returns the current or globally available vertices value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const GeometryVertex> Mesh::vertices() const noexcept { return vertices_; }

/// Returns the current or globally available indices value.
///
/// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
/// @note This function does not throw exceptions.
[[nodiscard]] span<const u32> Mesh::indices() const noexcept { return indices_; }

/// Returns the current or globally available label value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const UString &Mesh::label() const noexcept { return label_; }

/// Sets the label for this `Renderer`.
///
/// @param label `label` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void Mesh::set_label(UString label) noexcept { label_ = std::move(label); }

/// Sets the vertex color for this `Renderer`.
///
/// @param color `color` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void Mesh::set_vertex_color(const glm::vec4 &color) noexcept {
            ZoneScopedN("Mesh::set_vertex_color");
            for (GeometryVertex &vertex : vertices_) {
                vertex.color = color;
            }
        }

/// Returns the triangle count for this `Renderer`.
///
/// @return Returns the current triangle count value.
/// @note This function does not throw exceptions.
[[nodiscard]] usize Mesh::triangle_count() const noexcept {
            ZoneScopedN("Mesh::triangle_count");
            return !indices_.empty() ? indices_.size() / 3 : vertices_.size() / 3;
        }

/// Computes the estimated GPU bytes required by the supplied values.
///
/// @return Returns the current estimated GPU bytes value.
/// @note This function does not throw exceptions.
[[nodiscard]] u64 Mesh::estimated_gpu_bytes() const noexcept {
            ZoneScopedN("Mesh::estimated_gpu_bytes");
            return static_cast<u64>(vertices_.size()) * sizeof(GeometryVertex) +
                   static_cast<u64>(indices_.size()) * sizeof(u32);
        }

/// Reports whether GPU resident holds for this `Renderer`.
///
/// @return Returns the current is GPU resident value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool Mesh::is_gpu_resident() const noexcept { return gpu_resident_; }

/// Returns the GPU handle associated with this `Renderer`.
///
/// @return Returns the current GPU handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] MeshHandle Mesh::gpu_handle() const noexcept { return handle_; }

/// Marks uploaded using the supplied arguments and current state.
///
/// @param handle Handle identifying the target object or resource.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void Mesh::mark_uploaded(MeshHandle handle) noexcept {
            ZoneScopedN("Mesh::mark_uploaded");
            handle_ = handle;
            gpu_resident_ = true;
        }

/// Marks evicted using the supplied arguments and current state.
///
/// @return Returns the current mark evicted value.
/// @note This function does not throw exceptions.
void Mesh::mark_evicted() noexcept {
            ZoneScopedN("Mesh::mark_evicted");
            handle_ = {};
            gpu_resident_ = false;
        }

} // namespace SFT::Renderer
