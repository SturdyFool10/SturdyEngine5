#include <Engine/ModelImport.hpp>

#include <Engine/AssetManager.hpp>

#include <Animation/Bvh.hpp>

#include <algorithm>
#include <cctype>
#include <Foundation/FileIo.hpp>
#include <fstream>
#include <iterator>
#include <string>

namespace SFT::Engine {

    namespace {

        [[nodiscard]] std::string extension_of(const std::filesystem::path &path) {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        [[nodiscard]] AssetError import_error(AssetErrorCode code, std::string message, const std::filesystem::path &source) {
            return AssetError{.code = code, .message = UString{message}, .source = source};
        }

        [[nodiscard]] std::string supported_list() {
            std::string list;
            for (std::string_view e : supported_model_extensions()) list += (list.empty() ? "" : ", ") + std::string{e};
            for (std::string_view e : supported_animation_extensions()) list += ", " + std::string{e} + " (animation only)";
            return list;
        }

        // The skin with the most clips is the character the file is about; otherwise the whole node hierarchy.
        [[nodiscard]] ImportedAnimations animations_of(const ImportedScene &scene) {
            ImportedAnimations result;
            const GltfSkin *best = nullptr;
            for (const GltfSkin &skin : scene.skins) {
                if (best == nullptr || skin.clips.size() > best->clips.size()) best = &skin;
            }
            if (best != nullptr && !best->clips.empty()) {
                result.skeleton = best->skeleton;
                result.clips = best->clips;
            } else {
                result.skeleton = scene.scene_skeleton;
                result.clips = scene.scene_clips;
            }
            return result;
        }

    } // namespace

    std::vector<std::string_view> supported_model_extensions() { return {".gltf", ".glb", ".fbx", ".obj"}; }
    std::vector<std::string_view> supported_animation_extensions() { return {".bvh", ".gltf", ".glb", ".fbx"}; }

    AssetExpected<ImportedScene> import_model(AssetManager &assets, const std::filesystem::path &source, Asset shader) {
        const std::string ext = extension_of(source);
        if (ext == ".gltf" || ext == ".glb") return import_gltf(assets, source, shader);
        if (ext == ".fbx" || ext == ".obj") return import_fbx(assets, source, shader);
        if (ext == ".bvh") {
            return std::unexpected(import_error(AssetErrorCode::Unsupported,
                                                "BVH files hold motion only; load them with import_animations.", source));
        }
        return std::unexpected(import_error(AssetErrorCode::Unsupported,
                                            "Unsupported model format '" + ext + "'. Supported: " + supported_list(), source));
    }

    AssetExpected<ImportedAnimations> import_animations(AssetManager &assets, const std::filesystem::path &source) {
        const std::string ext = extension_of(source);
        if (ext == ".bvh") {
            auto text = Foundation::Io::read_text_file(source);
            if (!text) return std::unexpected(import_error(AssetErrorCode::IoFailure, text.error(), source));
            auto parsed = Animation::parse_bvh(*text);
            if (!parsed) return std::unexpected(import_error(AssetErrorCode::DecodeFailure, parsed.error().cpp_string(), source));
            auto clip = std::make_shared<Animation::Clip>(std::move(parsed->clip));
            clip->name = UString{source.stem().string()};
            ImportedAnimations result;
            result.skeleton = std::make_shared<const Animation::Skeleton>(std::move(parsed->skeleton));
            result.clips.push_back(std::move(clip));
            return result;
        }
        AssetExpected<ImportedScene> scene = std::unexpected(import_error(AssetErrorCode::Unsupported, "", source));
        if (ext == ".gltf" || ext == ".glb") scene = import_gltf(assets, source, Asset{}, /*animations_only=*/true);
        else if (ext == ".fbx") scene = import_fbx(assets, source, Asset{}, /*animations_only=*/true);
        else {
            return std::unexpected(import_error(AssetErrorCode::Unsupported,
                                                "Unsupported animation format '" + ext + "'. Supported: " + supported_list(), source));
        }
        if (!scene) return std::unexpected(scene.error());
        ImportedAnimations result = animations_of(*scene);
        if (!result.skeleton || result.clips.empty()) {
            return std::unexpected(import_error(AssetErrorCode::InvalidDescription, "The file contains no animation.", source));
        }
        return result;
    }

    std::vector<std::shared_ptr<const Animation::Clip>> adapt_clips(const ImportedAnimations &animations,
                                                                    const Animation::Skeleton &target,
                                                                    const Animation::RetargetOptions &options) {
        std::vector<std::shared_ptr<const Animation::Clip>> out;
        if (!animations.skeleton) return out;
        for (const auto &clip : animations.clips) {
            out.push_back(std::make_shared<const Animation::Clip>(Animation::adapt_clip(*clip, *animations.skeleton, target, options)));
        }
        return out;
    }

    AssetExpected<std::shared_ptr<const Animation::GraphDef>> parse_animation_graph(
        std::string_view json, const Animation::Skeleton &skeleton,
        std::span<const std::shared_ptr<const Animation::Clip>> clips) {
        auto def = Animation::load_graph_json(json, clips, skeleton);
        if (!def) {
            return std::unexpected(import_error(AssetErrorCode::InvalidDescription, def.error().cpp_string(), {}));
        }
        return std::make_shared<const Animation::GraphDef>(std::move(*def));
    }

    AssetExpected<std::shared_ptr<const Animation::GraphDef>> load_animation_graph(
        const std::filesystem::path &source, const Animation::Skeleton &skeleton,
        std::span<const std::shared_ptr<const Animation::Clip>> clips) {
        auto text = Foundation::Io::read_text_file(source);
        if (!text) return std::unexpected(import_error(AssetErrorCode::IoFailure, text.error(), source));
        auto graph = parse_animation_graph(*text, skeleton, clips);
        if (!graph) {
            AssetError error = graph.error();
            error.source = source;
            return std::unexpected(std::move(error));
        }
        return graph;
    }

    usize animation_adopt_clips(Ecs::World &world, Ecs::Entity entity, const ImportedAnimations &animations,
                                const Animation::RetargetOptions &options) {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) skeleton = animator->skeleton;
        if (!skeleton) return 0;
        const auto adapted = adapt_clips(animations, *skeleton, options);
        return animation_add_clips(world, entity, adapted) ? adapted.size() : 0;
    }

    AssetExpected<bool> animation_attach_graph_json(Ecs::World &world, Ecs::Entity entity, std::string_view json) {
        std::shared_ptr<const Animation::Skeleton> skeleton;
        std::vector<std::shared_ptr<const Animation::Clip>> clips;
        if (auto animator = world.get_component<SkeletonAnimator>(entity)) {
            skeleton = animator->skeleton;
            clips = animator->clips;
        }
        if (!skeleton) {
            return std::unexpected(import_error(AssetErrorCode::InvalidAsset, "The entity has no skeletal animator to build a graph for.", {}));
        }
        auto graph = parse_animation_graph(json, *skeleton, clips);
        if (!graph) return std::unexpected(graph.error());
        if (!animation_attach_graph(world, entity, *graph)) {
            return std::unexpected(import_error(AssetErrorCode::InvalidAsset, "Could not attach the graph to the entity.", {}));
        }
        return true;
    }

} // namespace SFT::Engine
