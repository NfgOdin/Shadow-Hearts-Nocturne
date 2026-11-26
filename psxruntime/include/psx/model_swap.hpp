#pragma once

// Shadow Hearts: Nocturne — High-Poly .glb (glTF 2.0) Skinned Model Swap Loader
// Supports Meshy.ai, Blender, Mixamo, and Rigify .glb exports (5k..150k+ triangles)
// with embedded Albedo + Normal textures, automatic height normalization, humanoid
// bone classification, and diagnostic rig reporting.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace psx::model_swap {

constexpr int kMaxSkinBones = 96;

enum class HumanoidBone : std::int8_t {
    None = -1,
    Hips = 0,
    Spine,
    Chest,
    Neck,
    Head,
    ShoulderL,
    UpperArmL,
    LowerArmL,
    HandL,
    ShoulderR,
    UpperArmR,
    LowerArmR,
    HandR,
    UpperLegL,
    LowerLegL,
    FootL,
    UpperLegR,
    LowerLegR,
    FootR,
    Count
};

struct GlbVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    float tx = 1.0f, ty = 0.0f, tz = 0.0f, tw = 1.0f;  // tangent + handedness
    float u = 0.0f, v = 0.0f;
    std::uint8_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};

struct GlbTexture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4
    bool valid() const noexcept { return width > 0 && height > 0 && !rgba.empty(); }
};

struct GlbJoint {
    std::string name;
    int parent = -1;
    HumanoidBone humanoid = HumanoidBone::None;
    std::array<float, 16> inv_bind{};    // column-major 4x4
    std::array<float, 16> rest_local{};  // column-major 4x4
    std::array<float, 3> bind_pos{0.0f, 0.0f, 0.0f};  // bind-pose world pivot in model space
};

struct GlbModel {
    std::string source_path;
    std::vector<GlbVertex> vertices;
    std::vector<std::uint32_t> indices;
    GlbTexture albedo;
    GlbTexture normal_map;
    std::vector<GlbJoint> joints;

    // Normalized bounding box (meters/model units before normalization)
    float min_x = 0.0f, max_x = 0.0f;
    float min_y = 0.0f, max_y = 0.0f;
    float min_z = 0.0f, max_z = 0.0f;
    float norm_scale = 1.0f;  // scales model height to 1.0 unit with feet at y = 0
    float center_x = 0.0f;
    float center_z = 0.0f;
    float front_sign = 1.0f;  // +1 if model faces +Z, -1 if model faces -Z
    float left_sign = 1.0f;   // +1 if character's Left is +X, -1 if -X

    bool has_skin = false;
    int mapped_humanoid_count = 0;
    std::string rig_preset_detected;  // "meshy", "mixamo", "rigify", "koudelka", "generic", "unrigged"
    std::string diagnostic_summary;

    bool valid() const noexcept { return !vertices.empty() && !indices.empty(); }
    std::size_t triangle_count() const noexcept { return indices.size() / 3u; }
};

// Per-frame draw parameters passed from Koudelka host/GPU into the D3D11 .glb renderer
struct ModelDrawParams {
    // Screen-space anchor in PS1 VRAM coords (0..1024 x 0..512) inside active clip rect
    float screen_cx = 160.0f;
    float screen_feet_y = 180.0f;
    float screen_height_px = 96.0f;
    float depth_z = 512.0f;

    // Clip rect (active PS1 draw buffer)
    int clip_x1 = 0, clip_y1 = 0, clip_x2 = 320, clip_y2 = 240;

    // Orientation (radians) & user tuning
    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    float user_scale = 1.0f;
    float y_offset_px = 0.0f;

    // Live Koudelka GTE 1898 room lighting (0..1 normalized)
    float ambient_rgb[3] = {0.32f, 0.28f, 0.26f};
    float light_dir[3] = {0.35f, 0.72f, 0.60f};
    float light_rgb[3] = {0.95f, 0.86f, 0.74f};
    float rim_strength = 0.22f;
    float normal_strength = 1.0f;

    // Procedural walk/idle/turn animation phase driven from Koudelka live movement when rigged
    float anim_time_sec = 0.0f;
    float stride_phase = 0.0f;  // continuous walk/run cycle phase (radians)
    float move_speed = 0.0f;    // 0 = idle, 0.5..0.7 = walk, 1.0 = run
    bool in_battle = false;
    bool pad_moving = false;
};

// Load a .glb / .gltf file from disk, decode embedded textures, classify humanoid skeleton,
// and fill `out.diagnostic_summary` (also useful for verifying Meshy.ai exports).
bool load_glb_file(const std::filesystem::path &path, GlbModel &out, std::string *err_out = nullptr);

// Compute column-major skinning matrices (up to kMaxSkinBones) for the current frame pose.
void evaluate_skin_matrices(const GlbModel &model, const ModelDrawParams &p,
                            std::array<std::array<float, 16>, kMaxSkinBones> &out_mats);

}  // namespace psx::model_swap
