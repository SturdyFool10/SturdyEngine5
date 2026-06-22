#pragma once

#include "Foundation/Types.hpp"

#include <array>
#include <concepts>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Core::Slang {

    inline constexpr u64 shader_unbounded_size = ~u64{0};
    inline constexpr u64 shader_unknown_size = shader_unbounded_size - 1;

    enum class ShaderErrorCode {
        InitializationFailed,
        InvalidArgument,
        FileReadFailed,
        CompilationFailed,
        ReflectionFailed,
        EntryPointNotFound,
        CodeGenerationFailed,
        OutOfMemory,
        OperationFailed,
    };

    struct ShaderError {
        ShaderErrorCode code = ShaderErrorCode::OperationFailed;
        std::string message;
        std::string diagnostics;
    };

    using ShaderResult = std::expected<void, ShaderError>;

    template <typename Value>
    using ShaderExpected = std::expected<Value, ShaderError>;

    [[nodiscard]] std::unexpected<ShaderError> shader_error(ShaderErrorCode code, std::string message, std::string diagnostics = {});

    enum class ShaderSourceKind {
        SourceString,
        File,
    };

    struct ShaderSource {
        ShaderSourceKind kind = ShaderSourceKind::SourceString;
        std::string module_name;
        std::string path;
        std::string source;

        [[nodiscard]] static ShaderSource from_source(std::string module_name, std::string source, std::string path = {});
        [[nodiscard]] static ShaderSource from_file(std::string path, std::string module_name = {});
    };

    template <typename StaticShader>
    concept StaticShaderSource = requires {
        { StaticShader::module_name } -> std::convertible_to<std::string_view>;
        { StaticShader::source } -> std::convertible_to<std::string_view>;
    };

    template <StaticShaderSource StaticShader>
    [[nodiscard]] ShaderSource shader_source_from_type() {
        ShaderSource source{};
        source.kind = ShaderSourceKind::SourceString;
        source.module_name = std::string{std::string_view{StaticShader::module_name}};
        source.source = std::string{std::string_view{StaticShader::source}};

        if constexpr (requires { { StaticShader::path } -> std::convertible_to<std::string_view>; }) {
            source.path = std::string{std::string_view{StaticShader::path}};
        }

        return source;
    }

    enum class ShaderTargetFormat {
        Spirv,
        Dxil,
        Hlsl,
        Glsl,
        Metal,
        Wgsl,
    };

    enum class ShaderStage {
        Unknown,
        Vertex,
        Hull,
        Domain,
        Geometry,
        Fragment,
        Compute,
        RayGeneration,
        Intersection,
        AnyHit,
        ClosestHit,
        Miss,
        Callable,
        Mesh,
        Amplification,
        Dispatch,
    };

    struct ShaderMacro {
        std::string name;
        std::string value;
    };

    struct ShaderTarget {
        ShaderTargetFormat format = ShaderTargetFormat::Spirv;
        std::string profile = "spirv_1_5";
    };

    struct ShaderEntryPointRequest {
        std::string name;
        ShaderStage stage = ShaderStage::Unknown;
    };

    struct ShaderCompileOptions {
        std::vector<ShaderTarget> targets = {ShaderTarget{}};
        std::vector<ShaderEntryPointRequest> entry_points;
        std::vector<std::string> search_paths;
        std::vector<ShaderMacro> macros;
        b8 allow_glsl_syntax = false;
        b8 skip_spirv_validation = false;
        b8 enable_effect_annotations = false;
    };

    enum class ShaderTypeKind {
        Unknown,
        Struct,
        Array,
        Matrix,
        Vector,
        Scalar,
        ConstantBuffer,
        Resource,
        SamplerState,
        TextureBuffer,
        ShaderStorageBuffer,
        ParameterBlock,
        GenericTypeParameter,
        Interface,
        OutputStream,
        MeshOutput,
        Specialized,
        Feedback,
        Pointer,
        DynamicResource,
        Enum,
    };

    enum class ShaderScalarType {
        None,
        Void,
        Bool,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float16,
        Float32,
        Float64,
        Int8,
        UInt8,
        Int16,
        UInt16,
        IntPtr,
        UIntPtr,
        BFloat16,
        FloatE4M3,
        FloatE5M2,
    };

    enum class ShaderParameterCategory {
        None,
        Mixed,
        ConstantBuffer,
        ShaderResource,
        UnorderedAccess,
        VaryingInput,
        VaryingOutput,
        SamplerState,
        Uniform,
        DescriptorTableSlot,
        SpecializationConstant,
        PushConstantBuffer,
        RegisterSpace,
        Generic,
        RayPayload,
        HitAttributes,
        CallablePayload,
        ShaderRecord,
        ExistentialTypeParam,
        ExistentialObjectParam,
        SubElementRegisterSpace,
        Subpass,
        MetalArgumentBufferElement,
        MetalAttribute,
        MetalPayload,
    };

    enum class ShaderBindingType {
        Unknown,
        Sampler,
        Texture,
        ConstantBuffer,
        ParameterBlock,
        TypedBuffer,
        RawBuffer,
        CombinedTextureSampler,
        InputRenderTarget,
        InlineUniformData,
        RayTracingAccelerationStructure,
        VaryingInput,
        VaryingOutput,
        ExistentialValue,
        PushConstant,
        MutableTexture,
        MutableTypedBuffer,
        MutableRawBuffer,
    };

    enum class ShaderResourceShape {
        Unknown,
        Texture1D,
        Texture2D,
        Texture3D,
        TextureCube,
        TextureBuffer,
        StructuredBuffer,
        ByteAddressBuffer,
        AccelerationStructure,
        TextureSubpass,
    };

    enum class ShaderResourceAccess {
        None,
        Read,
        ReadWrite,
        RasterOrdered,
        Append,
        Consume,
        Write,
        Feedback,
        Unknown,
    };

    enum class ShaderMatrixLayout {
        Unknown,
        RowMajor,
        ColumnMajor,
    };

    struct ShaderTypeReflection;

    struct ShaderFieldReflection {
        std::string name;
        std::shared_ptr<ShaderTypeReflection> type;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
    };

    struct ShaderBindingRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 descriptor_set = 0;
        u32 descriptor_range_index = 0;
        u32 descriptor_range_count = 0;
        u32 binding = 0;
        u32 count = 0;
        u32 image_format = 0;
        b8 specializable = false;
    };

    struct ShaderTypeReflection {
        std::string name;
        std::string full_name;
        ShaderTypeKind kind = ShaderTypeKind::Unknown;
        ShaderScalarType scalar_type = ShaderScalarType::None;
        ShaderResourceShape resource_shape = ShaderResourceShape::Unknown;
        ShaderResourceAccess resource_access = ShaderResourceAccess::Unknown;
        ShaderMatrixLayout matrix_layout = ShaderMatrixLayout::Unknown;
        u32 row_count = 0;
        u32 column_count = 0;
        u64 element_count = 0;
        u64 size = 0;
        u64 stride = 0;
        i32 alignment = 0;
        std::vector<ShaderFieldReflection> fields;
        std::vector<ShaderBindingRangeReflection> binding_ranges;
    };

    struct ShaderParameterReflection {
        std::string name;
        std::shared_ptr<ShaderTypeReflection> type;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        ShaderStage stage = ShaderStage::Unknown;
        u32 binding = 0;
        u32 binding_space = 0;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
        std::string semantic_name;
        u32 semantic_index = 0;
        std::vector<ShaderParameterCategory> categories;
        std::vector<ShaderBindingRangeReflection> binding_ranges;
    };

    struct ShaderDescriptorRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 binding = 0;
        u32 count = 0;
    };

    struct ShaderDescriptorSetReflection {
        u32 space = 0;
        std::vector<ShaderDescriptorRangeReflection> ranges;
    };

    struct ShaderEntryPointReflection {
        std::string name;
        std::string name_override;
        ShaderStage stage = ShaderStage::Unknown;
        std::array<u32, 3> compute_thread_group_size = {0, 0, 0};
        u32 compute_wave_size = 0;
        b8 uses_sample_rate_input = false;
        b8 has_default_constant_buffer = false;
        std::vector<ShaderParameterReflection> parameters;
        std::vector<ShaderParameterReflection> result_parameters;
    };

    struct ShaderReflection {
        std::vector<ShaderParameterReflection> global_parameters;
        std::vector<ShaderEntryPointReflection> entry_points;
        std::vector<ShaderDescriptorSetReflection> descriptor_sets;
        std::vector<std::string> hashed_strings;
        std::string json;
        u32 global_constant_buffer_binding = 0;
        u64 global_constant_buffer_size = 0;
        i32 bindless_space_index = -1;
    };

    struct ShaderBytecode {
        ShaderTargetFormat target = ShaderTargetFormat::Spirv;
        std::string profile;
        std::string entry_point;
        std::vector<byte> bytes;
    };

    struct ShaderCompilerState;
    struct ShaderState;

    class Shader {
      public:
        Shader() = default;
        ~Shader();

        Shader(const Shader &) = default;
        Shader &operator=(const Shader &) = default;
        Shader(Shader &&) noexcept = default;
        Shader &operator=(Shader &&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const ShaderReflection &reflection() const noexcept;
        [[nodiscard]] std::string_view module_name() const noexcept;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, usize target_index = 0) const;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(std::string_view entry_point_name, usize target_index = 0) const;

      private:
        friend class ShaderCompiler;
        explicit Shader(std::shared_ptr<ShaderState> state) noexcept;

        std::shared_ptr<ShaderState> state_;
    };

    class ShaderCompiler {
      public:
        ShaderCompiler();
        ~ShaderCompiler();

        ShaderCompiler(const ShaderCompiler &) = default;
        ShaderCompiler &operator=(const ShaderCompiler &) = default;
        ShaderCompiler(ShaderCompiler &&) noexcept = default;
        ShaderCompiler &operator=(ShaderCompiler &&) noexcept = default;

        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderSource &source, const ShaderCompileOptions &options = {});

        template <StaticShaderSource StaticShader>
        [[nodiscard]] ShaderExpected<Shader> compile(const ShaderCompileOptions &options = {}) {
            return compile(shader_source_from_type<StaticShader>(), options);
        }

      private:
        std::shared_ptr<ShaderCompilerState> state_;
    };

} // namespace SFT::Core::Slang
