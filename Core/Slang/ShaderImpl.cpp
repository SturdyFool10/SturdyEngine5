module;

#pragma region Imports
#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :Shader;
import :ShaderError;
import :ShaderSource;
import :ShaderTypes;
import :ShaderReflection;
import Sturdy.Foundation;

using std::bad_alloc;
using std::exception;
using std::find_if;
using std::ifstream;
using std::ios;
using std::lock_guard;
using std::make_shared;
using std::memcpy;
using std::mutex;
using std::numeric_limits;
using std::shared_ptr;
using std::streamoff;
using std::string;
using std::string_view;
using std::unexpected;
using std::vector;
using std::filesystem::path;

using filesystem_path = path;

namespace SFT::Core::Slang {

    namespace {

        [[nodiscard]] string blob_string(slang::IBlob *blob) {
            if (!blob || !blob->getBufferPointer() || blob->getBufferSize() == 0) {
                return {};
            }

            const auto *data = static_cast<const char *>(blob->getBufferPointer());
            usize size = blob->getBufferSize();
            if (size > 0 && data[size - 1] == '\0') {
                --size;
            }
            return string{data, size};
        }

        [[nodiscard]] string path_stem_or_module_name(const string &path) {
            filesystem_path file_path{path};
            string stem = file_path.stem().string();
            if (!stem.empty()) {
                return stem;
            }

            return "runtime_shader";
        }

        [[nodiscard]] ShaderExpected<string> read_text_file(const string &path) {
            ifstream file(path, ios::binary);
            if (!file) {
                return shader_error(ShaderErrorCode::FileReadFailed, "Failed to open Slang shader file: " + path);
            }

            string contents;
            file.seekg(0, ios::end);
            const streamoff size = file.tellg();
            if (size > 0) {
                contents.resize(static_cast<usize>(size));
                file.seekg(0, ios::beg);
                file.read(contents.data(), size);
            }

            if (!file && !file.eof()) {
                return shader_error(ShaderErrorCode::FileReadFailed, "Failed to read Slang shader file: " + path);
            }

            return contents;
        }

        [[nodiscard]] SlangCompileTarget to_slang_target(ShaderTargetFormat format) noexcept {
            switch (format) {
                case ShaderTargetFormat::Spirv:
                    return SLANG_SPIRV;
                case ShaderTargetFormat::Dxil:
                    return SLANG_DXIL;
                case ShaderTargetFormat::Hlsl:
                    return SLANG_HLSL;
                case ShaderTargetFormat::Glsl:
                    return SLANG_GLSL;
                case ShaderTargetFormat::Metal:
                    return SLANG_METAL;
                case ShaderTargetFormat::Wgsl:
                    return SLANG_WGSL;
            }

            return SLANG_TARGET_UNKNOWN;
        }

        [[nodiscard]] const char *default_profile(ShaderTargetFormat format) noexcept {
            switch (format) {
                case ShaderTargetFormat::Spirv:
                    return "spirv_1_5";
                case ShaderTargetFormat::Dxil:
                case ShaderTargetFormat::Hlsl:
                    return "sm_6_6";
                case ShaderTargetFormat::Glsl:
                    return "GLSL_460";
                case ShaderTargetFormat::Metal:
                    return "metal";
                case ShaderTargetFormat::Wgsl:
                    return "";
            }

            return "";
        }

        [[nodiscard]] SlangOptimizationLevel to_slang_optimization_level(ShaderOptimizationLevel level) noexcept {
            switch (level) {
                case ShaderOptimizationLevel::None:
                    return SLANG_OPTIMIZATION_LEVEL_NONE;
                case ShaderOptimizationLevel::Default:
                    return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
                case ShaderOptimizationLevel::High:
                    return SLANG_OPTIMIZATION_LEVEL_HIGH;
                case ShaderOptimizationLevel::Maximal:
                    return SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
            }

            return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
        }

        [[nodiscard]] SlangStage to_slang_stage(ShaderStage stage) noexcept {
            switch (stage) {
                case ShaderStage::Vertex:
                    return SLANG_STAGE_VERTEX;
                case ShaderStage::Hull:
                    return SLANG_STAGE_HULL;
                case ShaderStage::Domain:
                    return SLANG_STAGE_DOMAIN;
                case ShaderStage::Geometry:
                    return SLANG_STAGE_GEOMETRY;
                case ShaderStage::Fragment:
                    return SLANG_STAGE_FRAGMENT;
                case ShaderStage::Compute:
                    return SLANG_STAGE_COMPUTE;
                case ShaderStage::RayGeneration:
                    return SLANG_STAGE_RAY_GENERATION;
                case ShaderStage::Intersection:
                    return SLANG_STAGE_INTERSECTION;
                case ShaderStage::AnyHit:
                    return SLANG_STAGE_ANY_HIT;
                case ShaderStage::ClosestHit:
                    return SLANG_STAGE_CLOSEST_HIT;
                case ShaderStage::Miss:
                    return SLANG_STAGE_MISS;
                case ShaderStage::Callable:
                    return SLANG_STAGE_CALLABLE;
                case ShaderStage::Mesh:
                    return SLANG_STAGE_MESH;
                case ShaderStage::Amplification:
                    return SLANG_STAGE_AMPLIFICATION;
                case ShaderStage::Dispatch:
                    return SLANG_STAGE_DISPATCH;
                case ShaderStage::Unknown:
                    break;
            }

            return SLANG_STAGE_NONE;
        }

        [[nodiscard]] ShaderStage from_slang_stage(SlangStage stage) noexcept {
            switch (stage) {
                case SLANG_STAGE_VERTEX:
                    return ShaderStage::Vertex;
                case SLANG_STAGE_HULL:
                    return ShaderStage::Hull;
                case SLANG_STAGE_DOMAIN:
                    return ShaderStage::Domain;
                case SLANG_STAGE_GEOMETRY:
                    return ShaderStage::Geometry;
                case SLANG_STAGE_FRAGMENT:
                    return ShaderStage::Fragment;
                case SLANG_STAGE_COMPUTE:
                    return ShaderStage::Compute;
                case SLANG_STAGE_RAY_GENERATION:
                    return ShaderStage::RayGeneration;
                case SLANG_STAGE_INTERSECTION:
                    return ShaderStage::Intersection;
                case SLANG_STAGE_ANY_HIT:
                    return ShaderStage::AnyHit;
                case SLANG_STAGE_CLOSEST_HIT:
                    return ShaderStage::ClosestHit;
                case SLANG_STAGE_MISS:
                    return ShaderStage::Miss;
                case SLANG_STAGE_CALLABLE:
                    return ShaderStage::Callable;
                case SLANG_STAGE_MESH:
                    return ShaderStage::Mesh;
                case SLANG_STAGE_AMPLIFICATION:
                    return ShaderStage::Amplification;
                case SLANG_STAGE_DISPATCH:
                    return ShaderStage::Dispatch;
                default:
                    return ShaderStage::Unknown;
            }
        }

        [[nodiscard]] ShaderTypeKind from_slang_type_kind(slang::TypeReflection::Kind kind) noexcept {
            using Kind = slang::TypeReflection::Kind;
            switch (kind) {
                case Kind::Struct:
                    return ShaderTypeKind::Struct;
                case Kind::Array:
                    return ShaderTypeKind::Array;
                case Kind::Matrix:
                    return ShaderTypeKind::Matrix;
                case Kind::Vector:
                    return ShaderTypeKind::Vector;
                case Kind::Scalar:
                    return ShaderTypeKind::Scalar;
                case Kind::ConstantBuffer:
                    return ShaderTypeKind::ConstantBuffer;
                case Kind::Resource:
                    return ShaderTypeKind::Resource;
                case Kind::SamplerState:
                    return ShaderTypeKind::SamplerState;
                case Kind::TextureBuffer:
                    return ShaderTypeKind::TextureBuffer;
                case Kind::ShaderStorageBuffer:
                    return ShaderTypeKind::ShaderStorageBuffer;
                case Kind::ParameterBlock:
                    return ShaderTypeKind::ParameterBlock;
                case Kind::GenericTypeParameter:
                    return ShaderTypeKind::GenericTypeParameter;
                case Kind::Interface:
                    return ShaderTypeKind::Interface;
                case Kind::OutputStream:
                    return ShaderTypeKind::OutputStream;
                case Kind::Specialized:
                    return ShaderTypeKind::Specialized;
                case Kind::Feedback:
                    return ShaderTypeKind::Feedback;
                case Kind::Pointer:
                    return ShaderTypeKind::Pointer;
                case Kind::DynamicResource:
                    return ShaderTypeKind::DynamicResource;
                case Kind::MeshOutput:
                    return ShaderTypeKind::MeshOutput;
                case Kind::Enum:
                    return ShaderTypeKind::Enum;
                case Kind::None:
                    break;
            }

            return ShaderTypeKind::Unknown;
        }

        [[nodiscard]] ShaderScalarType from_slang_scalar_type(slang::TypeReflection::ScalarType scalar) noexcept {
            switch (scalar) {
                case slang::TypeReflection::Void:
                    return ShaderScalarType::Void;
                case slang::TypeReflection::Bool:
                    return ShaderScalarType::Bool;
                case slang::TypeReflection::Int32:
                    return ShaderScalarType::Int32;
                case slang::TypeReflection::UInt32:
                    return ShaderScalarType::UInt32;
                case slang::TypeReflection::Int64:
                    return ShaderScalarType::Int64;
                case slang::TypeReflection::UInt64:
                    return ShaderScalarType::UInt64;
                case slang::TypeReflection::Float16:
                    return ShaderScalarType::Float16;
                case slang::TypeReflection::Float32:
                    return ShaderScalarType::Float32;
                case slang::TypeReflection::Float64:
                    return ShaderScalarType::Float64;
                case slang::TypeReflection::Int8:
                    return ShaderScalarType::Int8;
                case slang::TypeReflection::UInt8:
                    return ShaderScalarType::UInt8;
                case slang::TypeReflection::Int16:
                    return ShaderScalarType::Int16;
                case slang::TypeReflection::UInt16:
                    return ShaderScalarType::UInt16;
                case slang::TypeReflection::IntPtr:
                    return ShaderScalarType::IntPtr;
                case slang::TypeReflection::UIntPtr:
                    return ShaderScalarType::UIntPtr;
                case slang::TypeReflection::BFloat16:
                    return ShaderScalarType::BFloat16;
                case slang::TypeReflection::FloatE4M3:
                    return ShaderScalarType::FloatE4M3;
                case slang::TypeReflection::FloatE5M2:
                    return ShaderScalarType::FloatE5M2;
                case slang::TypeReflection::None:
                    break;
            }

            return ShaderScalarType::None;
        }

        [[nodiscard]] ShaderParameterCategory from_slang_category(slang::ParameterCategory category) noexcept {
            switch (category) {
                case slang::ParameterCategory::Mixed:
                    return ShaderParameterCategory::Mixed;
                case slang::ParameterCategory::ConstantBuffer:
                    return ShaderParameterCategory::ConstantBuffer;
                case slang::ParameterCategory::ShaderResource:
                    return ShaderParameterCategory::ShaderResource;
                case slang::ParameterCategory::UnorderedAccess:
                    return ShaderParameterCategory::UnorderedAccess;
                case slang::ParameterCategory::VaryingInput:
                    return ShaderParameterCategory::VaryingInput;
                case slang::ParameterCategory::VaryingOutput:
                    return ShaderParameterCategory::VaryingOutput;
                case slang::ParameterCategory::SamplerState:
                    return ShaderParameterCategory::SamplerState;
                case slang::ParameterCategory::Uniform:
                    return ShaderParameterCategory::Uniform;
                case slang::ParameterCategory::DescriptorTableSlot:
                    return ShaderParameterCategory::DescriptorTableSlot;
                case slang::ParameterCategory::SpecializationConstant:
                    return ShaderParameterCategory::SpecializationConstant;
                case slang::ParameterCategory::PushConstantBuffer:
                    return ShaderParameterCategory::PushConstantBuffer;
                case slang::ParameterCategory::RegisterSpace:
                    return ShaderParameterCategory::RegisterSpace;
                case slang::ParameterCategory::GenericResource:
                    return ShaderParameterCategory::Generic;
                case slang::ParameterCategory::RayPayload:
                    return ShaderParameterCategory::RayPayload;
                case slang::ParameterCategory::HitAttributes:
                    return ShaderParameterCategory::HitAttributes;
                case slang::ParameterCategory::CallablePayload:
                    return ShaderParameterCategory::CallablePayload;
                case slang::ParameterCategory::ShaderRecord:
                    return ShaderParameterCategory::ShaderRecord;
                case slang::ParameterCategory::ExistentialTypeParam:
                    return ShaderParameterCategory::ExistentialTypeParam;
                case slang::ParameterCategory::ExistentialObjectParam:
                    return ShaderParameterCategory::ExistentialObjectParam;
                case slang::ParameterCategory::SubElementRegisterSpace:
                    return ShaderParameterCategory::SubElementRegisterSpace;
                case slang::ParameterCategory::InputAttachmentIndex:
                    return ShaderParameterCategory::Subpass;
                case slang::ParameterCategory::MetalArgumentBufferElement:
                    return ShaderParameterCategory::MetalArgumentBufferElement;
                case slang::ParameterCategory::MetalAttribute:
                    return ShaderParameterCategory::MetalAttribute;
                case slang::ParameterCategory::MetalPayload:
                    return ShaderParameterCategory::MetalPayload;
                case slang::ParameterCategory::None:
                    break;
            }

            return ShaderParameterCategory::None;
        }

        [[nodiscard]] ShaderBindingType from_slang_binding_type(slang::BindingType type) noexcept {
            switch (type) {
                case slang::BindingType::Sampler:
                    return ShaderBindingType::Sampler;
                case slang::BindingType::Texture:
                    return ShaderBindingType::Texture;
                case slang::BindingType::ConstantBuffer:
                    return ShaderBindingType::ConstantBuffer;
                case slang::BindingType::ParameterBlock:
                    return ShaderBindingType::ParameterBlock;
                case slang::BindingType::TypedBuffer:
                    return ShaderBindingType::TypedBuffer;
                case slang::BindingType::RawBuffer:
                    return ShaderBindingType::RawBuffer;
                case slang::BindingType::CombinedTextureSampler:
                    return ShaderBindingType::CombinedTextureSampler;
                case slang::BindingType::InputRenderTarget:
                    return ShaderBindingType::InputRenderTarget;
                case slang::BindingType::InlineUniformData:
                    return ShaderBindingType::InlineUniformData;
                case slang::BindingType::RayTracingAccelerationStructure:
                    return ShaderBindingType::RayTracingAccelerationStructure;
                case slang::BindingType::VaryingInput:
                    return ShaderBindingType::VaryingInput;
                case slang::BindingType::VaryingOutput:
                    return ShaderBindingType::VaryingOutput;
                case slang::BindingType::ExistentialValue:
                    return ShaderBindingType::ExistentialValue;
                case slang::BindingType::PushConstant:
                    return ShaderBindingType::PushConstant;
                case slang::BindingType::MutableTexture:
                    return ShaderBindingType::MutableTexture;
                case slang::BindingType::MutableTypedBuffer:
                    return ShaderBindingType::MutableTypedBuffer;
                case slang::BindingType::MutableRawBuffer:
                    return ShaderBindingType::MutableRawBuffer;
                case slang::BindingType::Unknown:
                case slang::BindingType::MutableFlag:
                case slang::BindingType::BaseMask:
                case slang::BindingType::ExtMask:
                    break;
            }

            return ShaderBindingType::Unknown;
        }

        [[nodiscard]] ShaderResourceShape from_slang_resource_shape(SlangResourceShape shape) noexcept {
            switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
                case SLANG_TEXTURE_1D:
                    return ShaderResourceShape::Texture1D;
                case SLANG_TEXTURE_2D:
                    return ShaderResourceShape::Texture2D;
                case SLANG_TEXTURE_3D:
                    return ShaderResourceShape::Texture3D;
                case SLANG_TEXTURE_CUBE:
                    return ShaderResourceShape::TextureCube;
                case SLANG_TEXTURE_BUFFER:
                    return ShaderResourceShape::TextureBuffer;
                case SLANG_STRUCTURED_BUFFER:
                    return ShaderResourceShape::StructuredBuffer;
                case SLANG_BYTE_ADDRESS_BUFFER:
                    return ShaderResourceShape::ByteAddressBuffer;
                case SLANG_ACCELERATION_STRUCTURE:
                    return ShaderResourceShape::AccelerationStructure;
                case SLANG_TEXTURE_SUBPASS:
                    return ShaderResourceShape::TextureSubpass;
                default:
                    return ShaderResourceShape::Unknown;
            }
        }

        [[nodiscard]] ShaderResourceAccess from_slang_resource_access(SlangResourceAccess access) noexcept {
            switch (access) {
                case SLANG_RESOURCE_ACCESS_NONE:
                    return ShaderResourceAccess::None;
                case SLANG_RESOURCE_ACCESS_READ:
                    return ShaderResourceAccess::Read;
                case SLANG_RESOURCE_ACCESS_READ_WRITE:
                    return ShaderResourceAccess::ReadWrite;
                case SLANG_RESOURCE_ACCESS_RASTER_ORDERED:
                    return ShaderResourceAccess::RasterOrdered;
                case SLANG_RESOURCE_ACCESS_APPEND:
                    return ShaderResourceAccess::Append;
                case SLANG_RESOURCE_ACCESS_CONSUME:
                    return ShaderResourceAccess::Consume;
                case SLANG_RESOURCE_ACCESS_WRITE:
                    return ShaderResourceAccess::Write;
                case SLANG_RESOURCE_ACCESS_FEEDBACK:
                    return ShaderResourceAccess::Feedback;
                default:
                    return ShaderResourceAccess::Unknown;
            }
        }

        [[nodiscard]] ShaderMatrixLayout from_slang_matrix_layout(SlangMatrixLayoutMode mode) noexcept {
            switch (mode) {
                case SLANG_MATRIX_LAYOUT_ROW_MAJOR:
                    return ShaderMatrixLayout::RowMajor;
                case SLANG_MATRIX_LAYOUT_COLUMN_MAJOR:
                    return ShaderMatrixLayout::ColumnMajor;
                default:
                    return ShaderMatrixLayout::Unknown;
            }
        }

        [[nodiscard]] u64 normalize_size(size_t value) noexcept {
            if (value == SLANG_UNKNOWN_SIZE) {
                return shader_unknown_size;
            }
            if (value == SLANG_UNBOUNDED_SIZE) {
                return shader_unbounded_size;
            }

            return static_cast<u64>(value);
        }

        [[nodiscard]] u32 normalize_u32(size_t value) noexcept {
            if (value == SLANG_UNKNOWN_SIZE || value == SLANG_UNBOUNDED_SIZE || value > numeric_limits<u32>::max()) {
                return numeric_limits<u32>::max();
            }

            return static_cast<u32>(value);
        }

        [[nodiscard]] u32 normalize_slang_unsigned(unsigned value) noexcept {
            if (value == static_cast<unsigned>(SLANG_UNKNOWN_SIZE) || value == static_cast<unsigned>(SLANG_UNBOUNDED_SIZE)) {
                return numeric_limits<u32>::max();
            }

            return value;
        }

        [[nodiscard]] u32 normalize_slang_int(SlangInt value) noexcept {
            if (value < 0 || static_cast<unsigned long long>(value) > numeric_limits<u32>::max()) {
                return numeric_limits<u32>::max();
            }

            return static_cast<u32>(value);
        }

        [[nodiscard]] string type_full_name(slang::TypeReflection *type) {
            if (!type) {
                return {};
            }

            ::Slang::ComPtr<slang::IBlob> name_blob;
            if (SLANG_SUCCEEDED(type->getFullName(name_blob.writeRef()))) {
                string name = blob_string(name_blob);
                if (!name.empty()) {
                    return name;
                }
            }

            const char *name = type->getName();
            return name ? string{name} : string{};
        }

        [[nodiscard]] ShaderBindingRangeReflection parse_binding_range(slang::TypeLayoutReflection *type_layout, SlangInt range_index) {
            ShaderBindingRangeReflection range{};
            if (!type_layout) {
                return range;
            }

            range.type = from_slang_binding_type(type_layout->getBindingRangeType(range_index));
            range.descriptor_set = normalize_slang_int(type_layout->getBindingRangeDescriptorSetIndex(range_index));
            range.descriptor_range_index = normalize_slang_int(type_layout->getBindingRangeFirstDescriptorRangeIndex(range_index));
            range.descriptor_range_count = normalize_slang_int(type_layout->getBindingRangeDescriptorRangeCount(range_index));
            range.count = normalize_slang_int(type_layout->getBindingRangeBindingCount(range_index));
            // Only *storage* image/texel-buffer ranges carry a decorated image format, and only those
            // have a non-null leaf variable inside Slang. getBindingRangeImageFormat unconditionally
            // dereferences that leaf variable (`leafVar->findModifier<FormatAttribute>()` in
            // slang-reflection-api.cpp), so calling it for a *sampled* Texture/TypedBuffer — whose leaf
            // var is null — segfaults inside Slang. Restrict the query to the mutable (RW) kinds; sampled
            // textures have no storage format to report anyway.
            if (range.type == ShaderBindingType::MutableTexture ||
                range.type == ShaderBindingType::MutableTypedBuffer) {
                range.image_format = static_cast<u32>(type_layout->getBindingRangeImageFormat(range_index));
            }
            range.specializable = type_layout->isBindingRangeSpecializable(range_index);

            if (range.descriptor_range_count > 0 && range.descriptor_range_count != numeric_limits<u32>::max() &&
                range.descriptor_set != numeric_limits<u32>::max() &&
                range.descriptor_range_index != numeric_limits<u32>::max()) {
                range.binding = normalize_slang_int(type_layout->getDescriptorSetDescriptorRangeIndexOffset(
                    static_cast<SlangInt>(range.descriptor_set),
                    static_cast<SlangInt>(range.descriptor_range_index)));
            }

            if (slang::TypeLayoutReflection *leaf = type_layout->getBindingRangeLeafTypeLayout(range_index)) {
                range.category = from_slang_category(leaf->getParameterCategory());
            }

            return range;
        }

        [[nodiscard]] shared_ptr<ShaderTypeReflection> parse_type_layout(slang::TypeLayoutReflection *type_layout, u32 depth = 0) {
            auto type = make_shared<ShaderTypeReflection>();
            if (!type_layout) {
                return type;
            }

            slang::TypeReflection *slang_type = type_layout->getType();
            type->name = type_layout->getName() ? type_layout->getName() : "";
            type->full_name = type_full_name(slang_type);
            type->kind = from_slang_type_kind(type_layout->getKind());
            type->scalar_type = from_slang_scalar_type(type_layout->getScalarType());
            type->resource_shape = from_slang_resource_shape(type_layout->getResourceShape());
            type->resource_access = from_slang_resource_access(type_layout->getResourceAccess());
            type->matrix_layout = from_slang_matrix_layout(type_layout->getMatrixLayoutMode());
            type->row_count = type_layout->getRowCount();
            type->column_count = type_layout->getColumnCount();
            type->element_count = normalize_size(type_layout->getElementCount());
            type->size = normalize_size(type_layout->getSize());
            type->stride = normalize_size(type_layout->getStride());
            type->alignment = type_layout->getAlignment();

            const SlangInt binding_range_count = type_layout->getBindingRangeCount();
            for (SlangInt index = 0; index < binding_range_count; ++index) {
                type->binding_ranges.push_back(parse_binding_range(type_layout, index));
            }

            // Keep recursive reflection bounded. Deep generated/generic layouts can still be
            // inspected via the raw JSON if needed.
            if (depth >= 16) {
                return type;
            }

            const unsigned int field_count = type_layout->getFieldCount();
            type->fields.reserve(field_count);
            for (unsigned int field_index = 0; field_index < field_count; ++field_index) {
                slang::VariableLayoutReflection *field_layout = type_layout->getFieldByIndex(field_index);
                if (!field_layout) {
                    continue;
                }

                ShaderFieldReflection field{};
                field.name = field_layout->getName() ? field_layout->getName() : "";
                field.type = parse_type_layout(field_layout->getTypeLayout(), depth + 1);
                field.offset = normalize_size(field_layout->getOffset());
                if (field.type) {
                    field.size = field.type->size;
                    field.stride = field.type->stride;
                }
                type->fields.push_back(std::move(field));
            }

            return type;
        }

        [[nodiscard]] ShaderParameterReflection parse_parameter_layout(slang::VariableLayoutReflection *layout) {
            ShaderParameterReflection parameter{};
            if (!layout) {
                return parameter;
            }

            parameter.name = layout->getName() ? layout->getName() : "";
            parameter.type = parse_type_layout(layout->getTypeLayout());
            parameter.category = from_slang_category(layout->getCategory());
            parameter.stage = from_slang_stage(layout->getStage());
            parameter.binding = normalize_slang_unsigned(layout->getBindingIndex());
            parameter.binding_space = normalize_slang_unsigned(layout->getBindingSpace());
            parameter.offset = normalize_size(layout->getOffset());
            parameter.semantic_name = layout->getSemanticName() ? layout->getSemanticName() : "";
            parameter.semantic_index = normalize_u32(layout->getSemanticIndex());

            const unsigned int category_count = layout->getCategoryCount();
            parameter.categories.reserve(category_count);
            for (unsigned int index = 0; index < category_count; ++index) {
                parameter.categories.push_back(from_slang_category(layout->getCategoryByIndex(index)));
            }

            if (parameter.type) {
                parameter.size = parameter.type->size;
                parameter.stride = parameter.type->stride;
                parameter.binding_ranges = parameter.type->binding_ranges;
            }

            return parameter;
        }

        void append_fields_as_parameters(vector<ShaderParameterReflection> &out, slang::VariableLayoutReflection *layout) {
            if (!layout || !layout->getTypeLayout()) {
                return;
            }

            slang::TypeLayoutReflection *type_layout = layout->getTypeLayout();
            const unsigned int field_count = type_layout->getFieldCount();
            if (field_count == 0) {
                out.push_back(parse_parameter_layout(layout));
                return;
            }

            out.reserve(out.size() + field_count);
            for (unsigned int index = 0; index < field_count; ++index) {
                if (slang::VariableLayoutReflection *field = type_layout->getFieldByIndex(index)) {
                    out.push_back(parse_parameter_layout(field));
                }
            }
        }

        [[nodiscard]] ShaderDescriptorSetReflection parse_descriptor_set(slang::TypeLayoutReflection *layout, SlangInt set_index) {
            ShaderDescriptorSetReflection set{};
            if (!layout) {
                return set;
            }

            set.space = normalize_slang_int(layout->getDescriptorSetSpaceOffset(set_index));

            const SlangInt range_count = layout->getDescriptorSetDescriptorRangeCount(set_index);
            set.ranges.reserve(range_count > 0 ? static_cast<usize>(range_count) : 0);
            for (SlangInt range_index = 0; range_index < range_count; ++range_index) {
                ShaderDescriptorRangeReflection range{};
                range.type = from_slang_binding_type(layout->getDescriptorSetDescriptorRangeType(set_index, range_index));
                range.category = from_slang_category(layout->getDescriptorSetDescriptorRangeCategory(set_index, range_index));
                range.binding = normalize_slang_int(layout->getDescriptorSetDescriptorRangeIndexOffset(set_index, range_index));
                range.count = normalize_slang_int(layout->getDescriptorSetDescriptorRangeDescriptorCount(set_index, range_index));
                set.ranges.push_back(range);
            }

            return set;
        }

        [[nodiscard]] ShaderExpected<ShaderReflection> parse_reflection(slang::IComponentType *linked_program, usize target_index) {
            ::Slang::ComPtr<slang::IBlob> diagnostics;
            slang::ProgramLayout *layout = linked_program->getLayout(static_cast<SlangInt>(target_index), diagnostics.writeRef());
            if (!layout) {
                return shader_error(ShaderErrorCode::ReflectionFailed, "Slang failed to produce shader reflection.", blob_string(diagnostics));
            }

            ShaderReflection reflection{};
            reflection.global_constant_buffer_binding = normalize_u32(layout->getGlobalConstantBufferBinding());
            reflection.global_constant_buffer_size = normalize_size(layout->getGlobalConstantBufferSize());
            reflection.bindless_space_index = static_cast<i32>(layout->getBindlessSpaceIndex());

            const unsigned global_count = layout->getParameterCount();
            reflection.global_parameters.reserve(global_count);
            for (unsigned index = 0; index < global_count; ++index) {
                reflection.global_parameters.push_back(parse_parameter_layout(layout->getParameterByIndex(index)));
            }

            const SlangUInt entry_point_count = layout->getEntryPointCount();
            reflection.entry_points.reserve(entry_point_count);
            for (SlangUInt index = 0; index < entry_point_count; ++index) {
                slang::EntryPointReflection *entry_point = layout->getEntryPointByIndex(index);
                if (!entry_point) {
                    continue;
                }

                ShaderEntryPointReflection entry{};
                entry.name = entry_point->getName() ? entry_point->getName() : "";
                entry.name_override = entry_point->getNameOverride() ? entry_point->getNameOverride() : "";
                entry.stage = from_slang_stage(entry_point->getStage());
                entry.uses_sample_rate_input = entry_point->usesAnySampleRateInput();
                entry.has_default_constant_buffer = entry_point->hasDefaultConstantBuffer();

                SlangUInt thread_group_size[3] = {};
                entry_point->getComputeThreadGroupSize(3, thread_group_size);
                entry.compute_thread_group_size = {
                    static_cast<u32>(thread_group_size[0]),
                    static_cast<u32>(thread_group_size[1]),
                    static_cast<u32>(thread_group_size[2]),
                };

                SlangUInt wave_size = 0;
                entry_point->getComputeWaveSize(&wave_size);
                entry.compute_wave_size = static_cast<u32>(wave_size);

                const unsigned parameter_count = entry_point->getParameterCount();
                entry.parameters.reserve(parameter_count);
                for (unsigned parameter_index = 0; parameter_index < parameter_count; ++parameter_index) {
                    entry.parameters.push_back(parse_parameter_layout(entry_point->getParameterByIndex(parameter_index)));
                }

                append_fields_as_parameters(entry.result_parameters, entry_point->getResultVarLayout());
                reflection.entry_points.push_back(std::move(entry));
            }

            if (slang::TypeLayoutReflection *global_params = layout->getGlobalParamsTypeLayout()) {
                const SlangInt set_count = global_params->getDescriptorSetCount();
                reflection.descriptor_sets.reserve(set_count > 0 ? static_cast<usize>(set_count) : 0);
                for (SlangInt set_index = 0; set_index < set_count; ++set_index) {
                    reflection.descriptor_sets.push_back(parse_descriptor_set(global_params, set_index));
                }
            }

            const SlangUInt hashed_count = layout->getHashedStringCount();
            reflection.hashed_strings.reserve(hashed_count);
            for (SlangUInt index = 0; index < hashed_count; ++index) {
                size_t count = 0;
                const char *string = layout->getHashedString(index, &count);
                if (string) {
                    reflection.hashed_strings.emplace_back(string, count);
                }
            }

            ::Slang::ComPtr<slang::IBlob> json_blob;
            if (SLANG_SUCCEEDED(layout->toJson(json_blob.writeRef()))) {
                reflection.json = blob_string(json_blob);
            }

            return reflection;
        }

        [[nodiscard]] ShaderExpected<vector<::Slang::ComPtr<slang::IEntryPoint>>> resolve_entry_points(
            slang::IModule *module,
            const ShaderCompileOptions &options) {
            vector<::Slang::ComPtr<slang::IEntryPoint>> entry_points;

            if (options.entry_points.empty()) {
                const SlangInt32 count = module->getDefinedEntryPointCount();
                entry_points.reserve(count > 0 ? static_cast<usize>(count) : 0);
                for (SlangInt32 index = 0; index < count; ++index) {
                    ::Slang::ComPtr<slang::IEntryPoint> entry_point;
                    const SlangResult result = module->getDefinedEntryPoint(index, entry_point.writeRef());
                    if (SLANG_FAILED(result)) {
                        return shader_error(ShaderErrorCode::EntryPointNotFound, "Failed to read a Slang module entry point.");
                    }
                    entry_points.push_back(std::move(entry_point));
                }

                return entry_points;
            }

            entry_points.reserve(options.entry_points.size());
            for (const ShaderEntryPointRequest &request : options.entry_points) {
                if (request.name.empty()) {
                    return shader_error(ShaderErrorCode::InvalidArgument, "Slang shader entry point name cannot be empty.");
                }

                ::Slang::ComPtr<slang::IBlob> diagnostics;
                ::Slang::ComPtr<slang::IEntryPoint> entry_point;
                SlangResult result = SLANG_FAIL;
                if (request.stage == ShaderStage::Unknown) {
                    result = module->findEntryPointByName(request.name.c_str(), entry_point.writeRef());
                } else {
                    result = module->findAndCheckEntryPoint(request.name.c_str(), to_slang_stage(request.stage), entry_point.writeRef(), diagnostics.writeRef());
                }

                if (SLANG_FAILED(result) || !entry_point.get()) {
                    return shader_error(
                        ShaderErrorCode::EntryPointNotFound,
                        "Failed to find Slang shader entry point: " + request.name,
                        blob_string(diagnostics));
                }

                entry_points.push_back(std::move(entry_point));
            }

            return entry_points;
        }

        [[nodiscard]] ShaderExpected<vector<slang::TargetDesc>> build_target_descs(
            slang::IGlobalSession *global_session,
            const vector<ShaderTarget> &targets,
            const ShaderCompileOptions &options) {
            if (targets.empty()) {
                return shader_error(ShaderErrorCode::InvalidArgument, "At least one Slang shader target is required.");
            }

            vector<slang::TargetDesc> target_descs;
            target_descs.reserve(targets.size());
            for (const ShaderTarget &target : targets) {
                const char *profile = target.profile.empty() ? default_profile(target.format) : target.profile.c_str();
                slang::TargetDesc desc{};
                desc.format = to_slang_target(target.format);
                desc.profile = global_session->findProfile(profile);
                desc.flags = kDefaultTargetFlags;

                if (desc.format == SLANG_TARGET_UNKNOWN) {
                    return shader_error(ShaderErrorCode::InvalidArgument, "Unsupported Slang shader target.");
                }

                if (options.skip_spirv_validation && target.format == ShaderTargetFormat::Spirv) {
                    // SessionDesc also carries this setting; keep the target desc simple for now.
                }

                target_descs.push_back(desc);
            }

            return target_descs;
        }

    } // namespace

    struct ShaderCompilerState {
        mutex mutex;
        ::Slang::ComPtr<slang::IGlobalSession> global_session;
    };

    struct ShaderState {
        string module_name;
        vector<ShaderTarget> targets;
        ShaderReflection reflection;
        ::Slang::ComPtr<slang::ISession> session;
        ::Slang::ComPtr<slang::IModule> module;
        ::Slang::ComPtr<slang::IComponentType> linked_program;
    };

    unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics) {
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

    namespace {

        // Front-end only: create a session and load the source as a Slang module. This is the work
        // shared by compile() and reflect() — it stops before entry-point composition and linking,
        // which are the parts that make a program able to emit target code. The caller must hold the
        // compiler mutex and pass a live global session.
        [[nodiscard]] ShaderExpected<shared_ptr<ShaderState>> load_shader_module(
            slang::IGlobalSession *global_session,
            const ShaderSource &source,
            const ShaderCompileOptions &options) {
            string module_name = source.module_name;
            string path = source.path;
            string source_text = source.source;
            vector<string> effective_search_paths = options.search_paths;

            if (source.kind == ShaderSourceKind::File) {
                if (path.empty()) {
                    return shader_error(ShaderErrorCode::InvalidArgument, "Slang shader file path cannot be empty.");
                }

                if (module_name.empty()) {
                    module_name = path_stem_or_module_name(path);
                }

                auto loaded = read_text_file(path);
                if (!loaded) {
                    return unexpected(loaded.error());
                }
                source_text = std::move(*loaded);

                const filesystem_path parent_path = filesystem_path{path}.parent_path();
                if (!parent_path.empty()) {
                    effective_search_paths.push_back(parent_path.string());
                }
            } else if (module_name.empty()) {
                module_name = "runtime_shader";
            }

            if (source_text.empty()) {
                return shader_error(ShaderErrorCode::InvalidArgument, "Slang shader source cannot be empty.");
            }
            if (path.empty()) {
                path = module_name + ".slang";
            }

            auto target_descs = build_target_descs(global_session, options.targets, options);
            if (!target_descs) {
                return unexpected(target_descs.error());
            }

            vector<const char *> search_paths;
            search_paths.reserve(effective_search_paths.size());
            for (const string &search_path : effective_search_paths) {
                search_paths.push_back(search_path.c_str());
            }

            vector<slang::PreprocessorMacroDesc> macros;
            macros.reserve(options.macros.size());
            for (const ShaderMacro &macro : options.macros) {
                if (!macro.name.empty()) {
                    macros.push_back(slang::PreprocessorMacroDesc{macro.name.c_str(), macro.value.c_str()});
                }
            }

            // Session-wide compiler options. Optimization level is applied here so it covers every
            // target/entry point produced from this session.
            const slang::CompilerOptionEntry compiler_options[] = {
                slang::CompilerOptionEntry{
                    slang::CompilerOptionName::Optimization,
                    slang::CompilerOptionValue{
                        slang::CompilerOptionValueKind::Int,
                        static_cast<std::int32_t>(to_slang_optimization_level(options.optimization)),
                        0,
                        nullptr,
                        nullptr,
                    },
                },
                // Without this, Slang's SPIR-V backend renames every entry point to "main" (matching
                // GLSL/HLSL convention), so a VkPipelineShaderStageCreateInfo::pName built from the
                // reflected entry point name (e.g. "vertexMain") fails to resolve at pipeline creation.
                slang::CompilerOptionEntry{
                    slang::CompilerOptionName::VulkanUseEntryPointName,
                    slang::CompilerOptionValue{
                        slang::CompilerOptionValueKind::Int,
                        1,
                        0,
                        nullptr,
                        nullptr,
                    },
                },
            };

            slang::SessionDesc session_desc{};
            session_desc.targets = target_descs->data();
            session_desc.targetCount = static_cast<SlangInt>(target_descs->size());
            session_desc.searchPaths = search_paths.empty() ? nullptr : search_paths.data();
            session_desc.searchPathCount = static_cast<SlangInt>(search_paths.size());
            session_desc.preprocessorMacros = macros.empty() ? nullptr : macros.data();
            session_desc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
            session_desc.compilerOptionEntries = const_cast<slang::CompilerOptionEntry *>(compiler_options);
            session_desc.compilerOptionEntryCount = static_cast<std::uint32_t>(sizeof(compiler_options) / sizeof(compiler_options[0]));
            session_desc.allowGLSLSyntax = static_cast<bool>(options.allow_glsl_syntax);
            session_desc.skipSPIRVValidation = static_cast<bool>(options.skip_spirv_validation);
            session_desc.enableEffectAnnotations = static_cast<bool>(options.enable_effect_annotations);

            auto shader_state = make_shared<ShaderState>();
            shader_state->module_name = module_name;
            shader_state->targets = options.targets;
            for (ShaderTarget &target : shader_state->targets) {
                if (target.profile.empty()) {
                    target.profile = default_profile(target.format);
                }
            }

            SlangResult result = global_session->createSession(session_desc, shader_state->session.writeRef());
            if (SLANG_FAILED(result) || !shader_state->session.get()) {
                return shader_error(ShaderErrorCode::InitializationFailed, "Failed to create Slang session.");
            }

            ::Slang::ComPtr<slang::IBlob> diagnostics;
            shader_state->module = shader_state->session->loadModuleFromSourceString(
                module_name.c_str(),
                path.c_str(),
                source_text.c_str(),
                diagnostics.writeRef());
            if (!shader_state->module.get()) {
                return shader_error(ShaderErrorCode::CompilationFailed, "Failed to load Slang shader module: " + module_name, blob_string(diagnostics));
            }

            return shader_state;
        }

    } // namespace

    ShaderSource ShaderSource::from_source(string module_name, string source, string path) {
        ShaderSource shader_source{};
        shader_source.kind = ShaderSourceKind::SourceString;
        shader_source.module_name = std::move(module_name);
        shader_source.path = std::move(path);
        shader_source.source = std::move(source);
        return shader_source;
    }

    ShaderSource ShaderSource::from_file(string path, string module_name) {
        ShaderSource shader_source{};
        shader_source.kind = ShaderSourceKind::File;
        shader_source.module_name = std::move(module_name);
        shader_source.path = std::move(path);
        return shader_source;
    }

    Shader::Shader(shared_ptr<ShaderState> state) noexcept
        : state_(std::move(state)) {
    }

    Shader::~Shader() = default;

    Shader::operator bool() const noexcept {
        return static_cast<bool>(state_);
    }

    const ShaderReflection &Shader::reflection() const noexcept {
        static const ShaderReflection empty{};
        return state_ ? state_->reflection : empty;
    }

    string_view Shader::module_name() const noexcept {
        return state_ ? string_view{state_->module_name} : string_view{};
    }

    ShaderExpected<ShaderBytecode> Shader::entry_point_code(usize entry_point_index, usize target_index) const {
        if (!state_ || !state_->linked_program.get()) {
            return shader_error(ShaderErrorCode::OperationFailed, "Cannot get bytecode from an empty Slang shader.");
        }
        if (entry_point_index >= state_->reflection.entry_points.size()) {
            return shader_error(ShaderErrorCode::InvalidArgument, "Slang shader entry point index is out of range.");
        }
        if (target_index >= state_->targets.size()) {
            return shader_error(ShaderErrorCode::InvalidArgument, "Slang shader target index is out of range.");
        }

        ::Slang::ComPtr<slang::IBlob> code;
        ::Slang::ComPtr<slang::IBlob> diagnostics;
        const SlangResult result = state_->linked_program->getEntryPointCode(
            static_cast<SlangInt>(entry_point_index),
            static_cast<SlangInt>(target_index),
            code.writeRef(),
            diagnostics.writeRef());

        if (SLANG_FAILED(result) || !code.get()) {
            return shader_error(ShaderErrorCode::CodeGenerationFailed, "Slang failed to generate entry point code.", blob_string(diagnostics));
        }

        ShaderBytecode bytecode{};
        bytecode.target = state_->targets[target_index].format;
        bytecode.profile = state_->targets[target_index].profile.empty() ? default_profile(bytecode.target) : state_->targets[target_index].profile;
        bytecode.entry_point = state_->reflection.entry_points[entry_point_index].name;

        const usize size = code->getBufferSize();
        bytecode.bytes.resize(size);
        if (size > 0) {
            memcpy(bytecode.bytes.data(), code->getBufferPointer(), size);
        }

        return bytecode;
    }

    ShaderExpected<ShaderBytecode> Shader::entry_point_code(string_view entry_point_name, usize target_index) const {
        if (!state_) {
            return shader_error(ShaderErrorCode::OperationFailed, "Cannot get bytecode from an empty Slang shader.");
        }

        const auto found = find_if(
            state_->reflection.entry_points.begin(),
            state_->reflection.entry_points.end(),
            [entry_point_name](const ShaderEntryPointReflection &entry_point) {
                return entry_point.name == entry_point_name || entry_point.name_override == entry_point_name;
            });

        if (found == state_->reflection.entry_points.end()) {
            return shader_error(ShaderErrorCode::EntryPointNotFound, "Slang shader entry point was not reflected: " + string{entry_point_name});
        }

        return entry_point_code(static_cast<usize>(found - state_->reflection.entry_points.begin()), target_index);
    }

    ShaderCompiler::ShaderCompiler()
        : state_(make_shared<ShaderCompilerState>()) {
    }

    ShaderCompiler::~ShaderCompiler() = default;

    ShaderExpected<ShaderReflection> ShaderCompiler::reflect(const ShaderSource &source, const ShaderCompileOptions &options) {
        if (!state_) {
            return shader_error(ShaderErrorCode::InitializationFailed, "Slang shader compiler state is unavailable.");
        }

        try {
            lock_guard lock(state_->mutex);

            if (!state_->global_session.get()) {
                const SlangResult result = slang::createGlobalSession(state_->global_session.writeRef());
                if (SLANG_FAILED(result) || !state_->global_session.get()) {
                    return shader_error(ShaderErrorCode::InitializationFailed, "Failed to create Slang global session.");
                }
            }

            // Reflection only — load the module and read its layout. No entry-point composition,
            // no link, no target codegen. This is the lightweight path used to inventory shaders.
            auto shader_state = load_shader_module(state_->global_session.get(), source, options);
            if (!shader_state) {
                return unexpected(shader_state.error());
            }

            return parse_reflection((*shader_state)->module.get(), 0);
        } catch (const bad_alloc &) {
            return shader_error(ShaderErrorCode::OutOfMemory, "Out of memory while reflecting Slang shader.");
        } catch (const exception &exception) {
            return shader_error(ShaderErrorCode::OperationFailed, string{"Unexpected exception while reflecting Slang shader: "} + exception.what());
        } catch (...) {
            return shader_error(ShaderErrorCode::OperationFailed, "Unexpected exception while reflecting Slang shader.");
        }
    }

    ShaderExpected<Shader> ShaderCompiler::compile(const ShaderSource &source, const ShaderCompileOptions &options) {
        if (!state_) {
            return shader_error(ShaderErrorCode::InitializationFailed, "Slang shader compiler state is unavailable.");
        }

        try {
            lock_guard lock(state_->mutex);

            if (!state_->global_session.get()) {
                const SlangResult result = slang::createGlobalSession(state_->global_session.writeRef());
                if (SLANG_FAILED(result) || !state_->global_session.get()) {
                    return shader_error(ShaderErrorCode::InitializationFailed, "Failed to create Slang global session.");
                }
            }

            auto loaded_state = load_shader_module(state_->global_session.get(), source, options);
            if (!loaded_state) {
                return unexpected(loaded_state.error());
            }
            shared_ptr<ShaderState> shader_state = std::move(*loaded_state);

            ::Slang::ComPtr<slang::IBlob> diagnostics;
            SlangResult result = SLANG_OK;

            auto entry_points = resolve_entry_points(shader_state->module.get(), options);
            if (!entry_points) {
                return unexpected(entry_points.error());
            }

            vector<slang::IComponentType *> components;
            components.reserve(entry_points->size() + 1);
            components.push_back(shader_state->module.get());
            for (const ::Slang::ComPtr<slang::IEntryPoint> &entry_point : *entry_points) {
                components.push_back(entry_point.get());
            }

            ::Slang::ComPtr<slang::IComponentType> composed_program;
            diagnostics.setNull();
            result = shader_state->session->createCompositeComponentType(
                components.data(),
                static_cast<SlangInt>(components.size()),
                composed_program.writeRef(),
                diagnostics.writeRef());
            if (SLANG_FAILED(result) || !composed_program.get()) {
                return shader_error(ShaderErrorCode::CompilationFailed, "Failed to compose Slang shader program.", blob_string(diagnostics));
            }

            diagnostics.setNull();
            result = composed_program->link(shader_state->linked_program.writeRef(), diagnostics.writeRef());
            if (SLANG_FAILED(result) || !shader_state->linked_program.get()) {
                return shader_error(ShaderErrorCode::CompilationFailed, "Failed to link Slang shader program.", blob_string(diagnostics));
            }

            auto reflection = parse_reflection(shader_state->linked_program.get(), 0);
            if (!reflection) {
                return unexpected(reflection.error());
            }
            shader_state->reflection = std::move(*reflection);

            return Shader{std::move(shader_state)};
        } catch (const bad_alloc &) {
            return shader_error(ShaderErrorCode::OutOfMemory, "Out of memory while compiling Slang shader.");
        } catch (const exception &exception) {
            return shader_error(ShaderErrorCode::OperationFailed, string{"Unexpected exception while compiling Slang shader: "} + exception.what());
        } catch (...) {
            return shader_error(ShaderErrorCode::OperationFailed, "Unexpected exception while compiling Slang shader.");
        }
    }

} // namespace SFT::Core::Slang
