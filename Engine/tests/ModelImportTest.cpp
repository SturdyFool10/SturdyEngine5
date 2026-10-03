// Importers without a GPU: animation-only loads touch neither meshes nor textures, so a default-constructed
// Renderer (no device) is enough. A tiny skinned, morphing, animated glTF is generated in memory; an FBX is checked
// too when STURDY_FBX_TEST_FILE points at one.
#include <Engine/AssetManager.hpp>
#include <Engine/ModelImport.hpp>

#include <Renderer/RendererModule.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    }

    std::string base64(const std::vector<unsigned char> &bytes) {
        static const char *table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const uint32_t n = (bytes[i] << 16) | (i + 1 < bytes.size() ? bytes[i + 1] << 8 : 0) | (i + 2 < bytes.size() ? bytes[i + 2] : 0);
            out += table[(n >> 18) & 63];
            out += table[(n >> 12) & 63];
            out += i + 1 < bytes.size() ? table[(n >> 6) & 63] : '=';
            out += i + 2 < bytes.size() ? table[n & 63] : '=';
        }
        return out;
    }

    template <class T>
    size_t push(std::vector<unsigned char> &buffer, const std::vector<T> &values) {
        const size_t offset = buffer.size();
        const auto *bytes = reinterpret_cast<const unsigned char *>(values.data());
        buffer.insert(buffer.end(), bytes, bytes + values.size() * sizeof(T));
        while (buffer.size() % 4 != 0) buffer.push_back(0);
        return offset;
    }

    std::string make_gltf() {
        std::vector<unsigned char> b;
        const size_t positions = push(b, std::vector<float>{0, 0, 0, 1, 0, 0, 0, 2, 0, 1, 2, 0});
        const size_t joints = push(b, std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
        const size_t weights = push(b, std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0});
        const size_t indices = push(b, std::vector<uint16_t>{0, 1, 2, 2, 1, 3});
        const size_t ibm = push(b, std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                                                      1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});
        const size_t times = push(b, std::vector<float>{0.0f, 1.0f});
        const float s = std::sin(3.14159265f / 4.0f), c = std::cos(3.14159265f / 4.0f);
        const size_t rotations = push(b, std::vector<float>{0, 0, 0, 1, 0, 0, s, c});
        const size_t morph = push(b, std::vector<float>{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
        const size_t weight_values = push(b, std::vector<float>{0.0f, 1.0f});

        std::string j = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2]}],)";
        j += R"("nodes":[{"name":"Root","children":[1]},{"name":"Bone","translation":[0,1,0]},{"name":"Body","mesh":0,"skin":0}],)";
        j += R"("meshes":[{"name":"Quad","weights":[0],"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2},"indices":3,)";
        j += R"("targets":[{"POSITION":7}]}]}],)";
        j += R"("skins":[{"name":"Rig","joints":[0,1],"inverseBindMatrices":4}],)";
        j += R"("animations":[{"name":"Bend","samplers":[{"input":5,"output":6},{"input":5,"output":8}],)";
        j += R"("channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}},{"sampler":1,"target":{"node":2,"path":"weights"}}]}],)";
        j += R"("buffers":[{"byteLength":)" + std::to_string(b.size()) + R"(,"uri":"data:application/octet-stream;base64,)" + base64(b) + R"("}],)";
        const auto view = [](size_t offset, size_t length) {
            return R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" + std::to_string(length) + "}";
        };
        j += R"("bufferViews":[)" + view(positions, 48) + "," + view(joints, 16) + "," + view(weights, 64) + "," + view(indices, 12) + "," +
             view(ibm, 128) + "," + view(times, 8) + "," + view(rotations, 32) + "," + view(morph, 48) + "," + view(weight_values, 8) + "],";
        j += R"("accessors":[
            {"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,2,0]},
            {"bufferView":1,"componentType":5121,"count":4,"type":"VEC4"},
            {"bufferView":2,"componentType":5126,"count":4,"type":"VEC4"},
            {"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"},
            {"bufferView":4,"componentType":5126,"count":2,"type":"MAT4"},
            {"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
            {"bufferView":6,"componentType":5126,"count":2,"type":"VEC4"},
            {"bufferView":7,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,1]},
            {"bufferView":8,"componentType":5126,"count":2,"type":"SCALAR"}]
        )";
        return j + "}";
    }

} // namespace

int main() {
    namespace Engine = SFT::Engine;
    namespace Animation = SFT::Animation;
    SFT::Renderer::Renderer renderer;
    Engine::AssetManager assets(renderer);
    const auto dir = std::filesystem::temp_directory_path() / "sturdy_model_import_test";
    std::filesystem::create_directories(dir);

    // ---- glTF skin + animation + blend-shape weights ------------------------------------------------
    {
        const auto path = dir / "skinned.gltf";
        std::ofstream(path) << make_gltf();
        auto scene = Engine::import_gltf(assets, path, Engine::Asset{}, /*animations_only=*/true);
        check(scene.has_value(), "glTF animations-only import succeeds");
        if (!scene) std::cerr << scene.error().message.cpp_string() << '\n';
        if (scene) {
            check(scene->skins.size() == 1, "one skin");
            const auto &skin = scene->skins[0];
            check(skin.skeleton && skin.skeleton->joint_count() >= 2 && skin.skeleton->valid(), "skeleton is valid and parent-first");
            check(skin.skeleton->find_joint("Bone") != Animation::no_joint, "joint names kept");
            check(skin.clips.size() == 1 && skin.clips[0]->name == "Bend"_ustr, "clip imported under its name");
            if (!skin.clips.empty()) {
                const Animation::Clip &clip = *skin.clips[0];
                const u32 bone = skin.skeleton->find_joint("Bone");
                check(!clip.channels[bone].rotation.empty() && std::fabs(clip.duration - 1.0f) < 1e-4f, "rotation track and duration");
                check(clip.morph_tracks.size() == 1 && clip.morph_tracks[0].target == "Body"_ustr && clip.morph_tracks[0].weight_count == 1,
                      "weight channel becomes a morph track keyed by the mesh node");
                Animation::Pose pose;
                Animation::sample_clip(*skin.skeleton, clip, 1.0f, false, pose);
                check(std::fabs(glm::degrees(glm::angle(pose[bone].rotation)) - 90.0f) < 0.5f, "sampled rotation");
            }
            check(scene->scene_skeleton && scene->scene_clips.size() == 1, "scene rig for node animation");
        }
        auto animations = Engine::import_animations(assets, path);
        check(animations.has_value() && animations->clips.size() == 1, "import_animations on glTF");
    }

    // ---- BVH through import_animations, then retarget by name onto a Mixamo-style skeleton ------------
    {
        const auto path = dir / "walk.bvh";
        std::ofstream(path) << "HIERARCHY\nROOT Hips\n{\nOFFSET 0 1 0\nCHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n"
                               "JOINT Spine\n{\nOFFSET 0 0.5 0\nCHANNELS 3 Zrotation Xrotation Yrotation\nEnd Site\n{\nOFFSET 0 0.5 0\n}\n}\n}\n"
                               "MOTION\nFrames: 2\nFrame Time: 0.5\n0 0 0 0 0 0 0 0 0\n1 0 0 0 0 0 0 0 0\n";
        auto animations = Engine::import_animations(assets, path);
        check(animations.has_value() && animations->clips.size() == 1 && animations->clips[0]->name == "walk"_ustr, "BVH import");
        if (animations) {
            Animation::Skeleton target;
            target.names = {"mixamorig:Hips", "mixamorig:Spine"};
            target.parents = {Animation::no_joint, 0};
            target.rest_pose.assign(2, Animation::JointTransform{});
            target.inverse_bind.assign(2, glm::mat4(1.0f));
            const auto adapted = Engine::adapt_clips(*animations, target);
            check(adapted.size() == 1 && adapted[0]->channels.size() == 2 && !adapted[0]->channels[0].translation.empty(),
                  "clips adapt onto a differently named skeleton");
        }
    }

    // ---- errors ---------------------------------------------------------------------------------------
    check(!Engine::import_model(assets, dir / "x.xyz", Engine::Asset{}).has_value(), "unknown extension is rejected");
    check(!Engine::import_model(assets, dir / "walk.bvh", Engine::Asset{}).has_value(), "BVH is not a model");
    check(!Engine::import_animations(assets, dir / "missing.fbx").has_value(), "missing file is an error");

    // ---- optional FBX ---------------------------------------------------------------------------------
    if (const char *fbx = std::getenv("STURDY_FBX_TEST_FILE")) {
        auto animations = Engine::import_animations(assets, fbx);
        std::cout << "FBX " << fbx << ": " << (animations ? "ok" : animations.error().message.cpp_string()) << '\n';
        if (animations) {
            std::cout << "  joints=" << animations->skeleton->joint_count() << " clips=" << animations->clips.size() << '\n';
            for (const auto &clip : animations->clips) std::cout << "  clip '" << clip->name << "' duration=" << clip->duration << '\n';
            check(animations->skeleton->valid(), "FBX skeleton is valid and parent-first");
        } else {
            ++failures;
        }
    }

    std::filesystem::remove_all(dir);
    return failures == 0 ? 0 : 1;
}
