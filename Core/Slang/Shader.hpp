#pragma once

#include "Foundation/Embed.hpp"
#include "Foundation/Types.hpp"

#include <array>
#include <concepts>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using std::array;
using std::convertible_to;
using std::expected;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::unexpected;
using std::vector;

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
        string message;
        string diagnostics;
    };

    using ShaderResult = expected<void, ShaderError>;

    template <typename Value>
    using ShaderExpected = expected<Value, ShaderError>;

    [[nodiscard]] unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics = {});

    enum class ShaderSourceKind {
        SourceString,
        File,
    };

    struct ShaderSource {
        ShaderSourceKind kind = ShaderSourceKind::SourceString;
        string module_name;
        string path;
        string source;

        [[nodiscard]] static ShaderSource from_source(string module_name, string source, string path = {});
        [[nodiscard]] static ShaderSource from_file(string path, string module_name = {});
    };

    template <typename StaticShader>
    concept StaticShaderSource = requires {
        { StaticShader::module_name } -> convertible_to<string_view>;
        { StaticShader::source } -> convertible_to<string_view>;
    };

    template <StaticShaderSource StaticShader>
    [[nodiscard]] ShaderSource shader_source_from_type() {
        ShaderSource source{};
        source.kind = ShaderSourceKind::SourceString;
        source.module_name = string{string_view{StaticShader::module_name}};
        source.source = string{string_view{StaticShader::source}};

        if constexpr (requires { { StaticShader::path } -> convertible_to<string_view>; }) {
            source.path = string{string_view{StaticShader::path}};
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
        string name;
        string value;
    };

    struct ShaderTarget {
        ShaderTargetFormat format = ShaderTargetFormat::Spirv;
        string profile = "spirv_1_5";
    };

    struct ShaderEntryPointRequest {
        string name;
        ShaderStage stage = ShaderStage::Unknown;
    };

    struct ShaderCompileOptions {
        vector<ShaderTarget> targets = {ShaderTarget{}};
        vector<ShaderEntryPointRequest> entry_points;
        vector<string> search_paths;
        vector<ShaderMacro> macros;
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
        string name;
        shared_ptr<ShaderTypeReflection> type;
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
        string name;
        string full_name;
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
        vector<ShaderFieldReflection> fields;
        vector<ShaderBindingRangeReflection> binding_ranges;
    };

    struct ShaderParameterReflection {
        string name;
        shared_ptr<ShaderTypeReflection> type;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        ShaderStage stage = ShaderStage::Unknown;
        u32 binding = 0;
        u32 binding_space = 0;
        u64 offset = 0;
        u64 size = 0;
        u64 stride = 0;
        string semantic_name;
        u32 semantic_index = 0;
        vector<ShaderParameterCategory> categories;
        vector<ShaderBindingRangeReflection> binding_ranges;
    };

    struct ShaderDescriptorRangeReflection {
        ShaderBindingType type = ShaderBindingType::Unknown;
        ShaderParameterCategory category = ShaderParameterCategory::None;
        u32 binding = 0;
        u32 count = 0;
    };

    struct ShaderDescriptorSetReflection {
        u32 space = 0;
        vector<ShaderDescriptorRangeReflection> ranges;
    };

    struct ShaderEntryPointReflection {
        string name;
        string name_override;
        ShaderStage stage = ShaderStage::Unknown;
        array<u32, 3> compute_thread_group_size = {0, 0, 0};
        u32 compute_wave_size = 0;
        b8 uses_sample_rate_input = false;
        b8 has_default_constant_buffer = false;
        vector<ShaderParameterReflection> parameters;
        vector<ShaderParameterReflection> result_parameters;
    };

    struct ShaderReflection {
        vector<ShaderParameterReflection> global_parameters;
        vector<ShaderEntryPointReflection> entry_points;
        vector<ShaderDescriptorSetReflection> descriptor_sets;
        vector<string> hashed_strings;
        string json;
        u32 global_constant_buffer_binding = 0;
        u64 global_constant_buffer_size = 0;
        i32 bindless_space_index = -1;
    };

    struct ShaderBytecode {
        ShaderTargetFormat target = ShaderTargetFormat::Spirv;
        string profile;
        string entry_point;
        vector<byte> bytes;
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
        [[nodiscard]] string_view module_name() const noexcept;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(usize entry_point_index, usize target_index = 0) const;
        [[nodiscard]] ShaderExpected<ShaderBytecode> entry_point_code(string_view entry_point_name, usize target_index = 0) const;

      private:
        friend class ShaderCompiler;
        explicit Shader(shared_ptr<ShaderState> state) noexcept;

        shared_ptr<ShaderState> state_;
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
        shared_ptr<ShaderCompilerState> state_;
    };

} // namespace SFT::Core::Slang
