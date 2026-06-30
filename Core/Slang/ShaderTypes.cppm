module;

#include <string>
#include <vector>

export module Sturdy.Core:ShaderTypes;

import Sturdy.Foundation;

using std::string;
using std::vector;

export namespace SFT::Core::Slang {

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

    enum class ShaderOptimizationLevel {
        None,    // Don't optimize at all.
        Default, // Balance code quality and compilation time.
        High,    // Optimize aggressively.
        Maximal, // Include optimizations that may take a very long time / severe space-speed tradeoffs.
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
        ShaderOptimizationLevel optimization = ShaderOptimizationLevel::Default;
        b8 allow_glsl_syntax = false;
        b8 skip_spirv_validation = false;
        b8 enable_effect_annotations = false;
    };

    struct ShaderBytecode {
        ShaderTargetFormat target = ShaderTargetFormat::Spirv;
        string profile;
        string entry_point;
        vector<byte> bytes;
    };

} // namespace SFT::Core::Slang
