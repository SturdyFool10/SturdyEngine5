#include <Renderer/src/Renderer/Material.hpp>


namespace SFT::Renderer {

    /// Performs the material parameter type of operation for `Renderer` using the supplied arguments.
    ///
    /// @param uniform `uniform` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    MaterialParameterType material_parameter_type_of(const ReflectedUniform &uniform) noexcept {
        const bool is_float = uniform.scalar == slang::ShaderScalarType::Float32 ||
                              uniform.scalar == slang::ShaderScalarType::Float16 ||
                              uniform.scalar == slang::ShaderScalarType::Float64;
        if (uniform.rows > 1) {
            if (uniform.rows == 4 && uniform.columns == 4) return MaterialParameterType::Mat4;
            if (uniform.rows == 3 && uniform.columns == 3) return MaterialParameterType::Mat3;
            return MaterialParameterType::Unknown;
        }
        const u32 components = uniform.columns == 0 ? 1u : uniform.columns;
        if (is_float) {
            switch (components) {
                case 1: return MaterialParameterType::Float;
                case 2: return MaterialParameterType::Vec2;
                case 3: return MaterialParameterType::Vec3;
                case 4: return MaterialParameterType::Vec4;
                default: return MaterialParameterType::Unknown;
            }
        }
        if (components == 1) {
            if (uniform.scalar == slang::ShaderScalarType::Int32) return MaterialParameterType::Int;
            if (uniform.scalar == slang::ShaderScalarType::UInt32) return MaterialParameterType::UInt;
        }
        return MaterialParameterType::Unknown;
    }

} // namespace SFT::Renderer

