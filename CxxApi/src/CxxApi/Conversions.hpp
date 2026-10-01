#pragma once

// Internal helpers converting between CxxApi's plain arrays and the engine's glm/string types.
// Not part of the bindable surface (never include from a function header).

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <Foundation/Foundation.hpp>

namespace SFT::CxxApi {

    [[nodiscard]] inline glm::vec2 to_vec2(const float (&a)[2]) noexcept { return {a[0], a[1]}; }
    [[nodiscard]] inline glm::vec3 to_vec3(const float (&a)[3]) noexcept { return {a[0], a[1], a[2]}; }
    [[nodiscard]] inline glm::vec4 to_vec4(const float (&a)[4]) noexcept { return {a[0], a[1], a[2], a[3]}; }
    [[nodiscard]] inline glm::vec2 to_vec2(std::array<float, 2> a) noexcept { return {a[0], a[1]}; }
    [[nodiscard]] inline glm::vec3 to_vec3(std::array<float, 3> a) noexcept { return {a[0], a[1], a[2]}; }
    [[nodiscard]] inline glm::vec4 to_vec4(std::array<float, 4> a) noexcept { return {a[0], a[1], a[2], a[3]}; }
    /// xyzw -> glm's wxyz constructor order.
    [[nodiscard]] inline glm::quat to_quat(const float (&a)[4]) noexcept { return glm::quat{a[3], a[0], a[1], a[2]}; }
    [[nodiscard]] inline glm::quat to_quat(std::array<float, 4> a) noexcept { return glm::quat{a[3], a[0], a[1], a[2]}; }
    [[nodiscard]] inline glm::mat4 to_mat4(const float (&a)[16]) noexcept {
        glm::mat4 m;
        std::memcpy(glm::value_ptr(m), a, sizeof(a));
        return m;
    }
    [[nodiscard]] inline glm::mat4 to_mat4(const std::array<float, 16> &a) noexcept {
        glm::mat4 m;
        std::memcpy(glm::value_ptr(m), a.data(), sizeof(float) * 16);
        return m;
    }

    [[nodiscard]] inline std::array<float, 2> to_array(glm::vec2 v) noexcept { return {v.x, v.y}; }
    [[nodiscard]] inline std::array<float, 3> to_array(glm::vec3 v) noexcept { return {v.x, v.y, v.z}; }
    [[nodiscard]] inline std::array<float, 4> to_array(glm::vec4 v) noexcept { return {v.x, v.y, v.z, v.w}; }
    [[nodiscard]] inline std::array<float, 4> to_array(glm::quat q) noexcept { return {q.x, q.y, q.z, q.w}; }
    [[nodiscard]] inline std::array<float, 16> to_array(const glm::mat4 &m) noexcept {
        std::array<float, 16> out{};
        std::memcpy(out.data(), glm::value_ptr(m), sizeof(out));
        return out;
    }

    inline void store(float (&out)[2], glm::vec2 v) noexcept { out[0] = v.x; out[1] = v.y; }
    inline void store(float (&out)[3], glm::vec3 v) noexcept { out[0] = v.x; out[1] = v.y; out[2] = v.z; }
    inline void store(float (&out)[4], glm::vec4 v) noexcept { out[0] = v.x; out[1] = v.y; out[2] = v.z; out[3] = v.w; }
    inline void store(float (&out)[4], glm::quat q) noexcept { out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w; }
    inline void store(float (&out)[16], const glm::mat4 &m) noexcept { std::memcpy(out, glm::value_ptr(m), sizeof(out)); }

    /// UTF-8 bytes -> UString; invalid UTF-8 becomes an empty string.
    [[nodiscard]] inline UString to_ustring(std::string_view utf8) {
        if (auto converted = UString::try_from_utf8(utf8)) {
            return std::move(*converted);
        }
        return UString{};
    }

    [[nodiscard]] inline std::unique_ptr<std::string> owned_string(std::string_view text) {
        return std::make_unique<std::string>(text);
    }

} // namespace SFT::CxxApi
