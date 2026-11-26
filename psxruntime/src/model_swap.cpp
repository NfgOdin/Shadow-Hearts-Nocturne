#include "psx/model_swap.hpp"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace psx::model_swap {
namespace {

std::string to_lower_clean(const char *s) {
    if (!s) return {};
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        if (std::isalnum(*p)) out.push_back(static_cast<char>(std::tolower(*p)));
    }
    return out;
}

bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

HumanoidBone classify_bone_name(const char *raw_name, bool &saw_mixamo, bool &saw_meshy) {
    const std::string n = to_lower_clean(raw_name);
    if (n.empty()) return HumanoidBone::None;
    if (contains(n, "mixamo")) saw_mixamo = true;
    if (contains(n, "meshy") || n == "hips" || n == "spine02" || n == "leftarm" || n == "rightarm") {
        saw_meshy = true;
    }

    const bool is_l = contains(n, "left") || (n.size() >= 2 && n.back() == 'l' && (contains(n, "arm") || contains(n, "leg") || contains(n, "hand") || contains(n, "foot") || contains(n, "shoulder") || contains(n, "thigh") || contains(n, "calf") || contains(n, "shin") || contains(n, "clavicle")));
    const bool is_r = !is_l && (contains(n, "right") || (n.size() >= 2 && n.back() == 'r' && (contains(n, "arm") || contains(n, "leg") || contains(n, "hand") || contains(n, "foot") || contains(n, "shoulder") || contains(n, "thigh") || contains(n, "calf") || contains(n, "shin") || contains(n, "clavicle"))));

    if (contains(n, "hips") || contains(n, "pelvis") || n == "root" || contains(n, "bip01pelvis")) return HumanoidBone::Hips;
    if (contains(n, "head") && !contains(n, "top") && !contains(n, "end")) return HumanoidBone::Head;
    if (contains(n, "neck")) return HumanoidBone::Neck;
    if (contains(n, "chest") || contains(n, "spine2") || contains(n, "spine02") || contains(n, "upperchest")) return HumanoidBone::Chest;
    if (contains(n, "spine")) return HumanoidBone::Spine;

    if (is_l) {
        if (contains(n, "shoulder") || contains(n, "clavicle")) return HumanoidBone::ShoulderL;
        if (contains(n, "forearm") || contains(n, "lowerarm") || contains(n, "elbow")) return HumanoidBone::LowerArmL;
        if (contains(n, "hand") || contains(n, "wrist")) return HumanoidBone::HandL;
        if (contains(n, "upperarm") || contains(n, "arm")) return HumanoidBone::UpperArmL;
        if (contains(n, "upleg") || contains(n, "upperleg") || contains(n, "thigh")) return HumanoidBone::UpperLegL;
        if (contains(n, "leg") || contains(n, "calf") || contains(n, "shin") || contains(n, "knee")) return HumanoidBone::LowerLegL;
        if (contains(n, "foot") || contains(n, "ankle")) return HumanoidBone::FootL;
    }
    if (is_r) {
        if (contains(n, "shoulder") || contains(n, "clavicle")) return HumanoidBone::ShoulderR;
        if (contains(n, "forearm") || contains(n, "lowerarm") || contains(n, "elbow")) return HumanoidBone::LowerArmR;
        if (contains(n, "hand") || contains(n, "wrist")) return HumanoidBone::HandR;
        if (contains(n, "upperarm") || contains(n, "arm")) return HumanoidBone::UpperArmR;
        if (contains(n, "upleg") || contains(n, "upperleg") || contains(n, "thigh")) return HumanoidBone::UpperLegR;
        if (contains(n, "leg") || contains(n, "calf") || contains(n, "shin") || contains(n, "knee")) return HumanoidBone::LowerLegR;
        if (contains(n, "foot") || contains(n, "ankle")) return HumanoidBone::FootR;
    }
    return HumanoidBone::None;
}

std::array<float, 16> mat4_identity() {
    return {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
}

std::array<float, 16> mat4_mul(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> r{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    return r;
}

std::array<float, 16> mat4_rot_x(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {1, 0, 0, 0,  0, c, s, 0,  0, -s, c, 0,  0, 0, 0, 1};
}

std::array<float, 16> mat4_rot_y(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, 0, -s, 0,  0, 1, 0, 0,  s, 0, c, 0,  0, 0, 0, 1};
}

std::array<float, 16> mat4_rot_z(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return {c, s, 0, 0,  -s, c, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
}

std::array<float, 16> mat4_translate(float tx, float ty, float tz) {
    return {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  tx, ty, tz, 1};
}

bool decode_gltf_image(const cgltf_image *img, GlbTexture &out) {
    if (!img || !img->buffer_view || !img->buffer_view->buffer || !img->buffer_view->buffer->data) {
        return false;
    }
    const auto *bytes = static_cast<const unsigned char *>(img->buffer_view->buffer->data) + img->buffer_view->offset;
    const int len = static_cast<int>(img->buffer_view->size);
    if (len <= 0) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char *pix = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
    if (!pix || w <= 0 || h <= 0) return false;
    out.width = w;
    out.height = h;
    out.rgba.assign(pix, pix + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(pix);
    return true;
}

}  // namespace

bool load_glb_file(const std::filesystem::path &path, GlbModel &out, std::string *err_out) {
    out = GlbModel{};
    out.source_path = path.string();

    cgltf_options opts{};
    cgltf_data *data = nullptr;
    const std::string pstr = path.string();
    if (cgltf_parse_file(&opts, pstr.c_str(), &data) != cgltf_result_success || !data) {
        if (err_out) *err_out = "Failed to parse glTF/GLB file: " + pstr;
        return false;
    }
    if (cgltf_load_buffers(&opts, data, pstr.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        if (err_out) *err_out = "Failed to load glTF/GLB buffers: " + pstr;
        return false;
    }

    // 1. Extract primary skin / skeleton if present
    bool saw_mixamo = false, saw_meshy = false;
    if (data->skins_count > 0) {
        const cgltf_skin &sk = data->skins[0];
        const std::size_t jcount = std::min<std::size_t>(sk.joints_count, static_cast<std::size_t>(kMaxSkinBones));
        out.joints.resize(jcount);
        for (std::size_t j = 0; j < jcount; ++j) {
            const cgltf_node *jn = sk.joints[j];
            GlbJoint &gj = out.joints[j];
            gj.name = (jn && jn->name) ? std::string(jn->name) : ("joint_" + std::to_string(j));
            gj.humanoid = classify_bone_name(gj.name.c_str(), saw_mixamo, saw_meshy);
            if (gj.humanoid != HumanoidBone::None) ++out.mapped_humanoid_count;
            gj.inv_bind = mat4_identity();
            if (sk.inverse_bind_matrices && j < sk.inverse_bind_matrices->count) {
                cgltf_accessor_read_float(sk.inverse_bind_matrices, j, gj.inv_bind.data(), 16);
            }
            gj.rest_local = mat4_identity();
            if (jn) {
                // Find parent index within skin joints
                for (std::size_t p = 0; p < jcount; ++p) {
                    if (sk.joints[p] == jn->parent) {
                        gj.parent = static_cast<int>(p);
                        break;
                    }
                }
                // Root skin joints need their full world transform so non-joint ancestor
                // nodes (e.g. Blender/Mixamo Armature scale or rotation) match inv_bind.
                if (gj.parent < 0) {
                    cgltf_node_transform_world(jn, gj.rest_local.data());
                } else {
                    cgltf_node_transform_local(jn, gj.rest_local.data());
                }
            }
        }
        out.has_skin = !out.joints.empty();
    }

    // 2. Extract all mesh primitives + first available embedded Albedo & Normal textures
    bool first_vert = true;
    for (std::size_t mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh &mesh = data->meshes[mi];
        for (std::size_t pi = 0; pi < mesh.primitives_count; ++pi) {
            const cgltf_primitive &prim = mesh.primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            bool has_uv_xform = false;
            float uv_off[2] = {0.0f, 0.0f};
            float uv_scale[2] = {1.0f, 1.0f};
            float uv_rot = 0.0f;
            if (prim.material) {
                if (prim.material->has_pbr_metallic_roughness) {
                    const auto &bct = prim.material->pbr_metallic_roughness.base_color_texture;
                    if (!out.albedo.valid() && bct.texture && bct.texture->image) {
                        decode_gltf_image(bct.texture->image, out.albedo);
                    }
                    if (bct.has_transform) {
                        has_uv_xform = true;
                        uv_off[0] = bct.transform.offset[0];
                        uv_off[1] = bct.transform.offset[1];
                        uv_scale[0] = bct.transform.scale[0];
                        uv_scale[1] = bct.transform.scale[1];
                        uv_rot = bct.transform.rotation;
                    }
                }
                if (!out.normal_map.valid() && prim.material->normal_texture.texture &&
                    prim.material->normal_texture.texture->image) {
                    decode_gltf_image(prim.material->normal_texture.texture->image, out.normal_map);
                }
            }

            const cgltf_accessor *pos_acc = nullptr;
            const cgltf_accessor *nrm_acc = nullptr;
            const cgltf_accessor *tan_acc = nullptr;
            const cgltf_accessor *uv_acc  = nullptr;
            const cgltf_accessor *jnt_acc = nullptr;
            const cgltf_accessor *wgt_acc = nullptr;

            for (std::size_t ai = 0; ai < prim.attributes_count; ++ai) {
                const cgltf_attribute &a = prim.attributes[ai];
                if (a.type == cgltf_attribute_type_position) pos_acc = a.data;
                else if (a.type == cgltf_attribute_type_normal) nrm_acc = a.data;
                else if (a.type == cgltf_attribute_type_tangent) tan_acc = a.data;
                else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv_acc = a.data;
                else if (a.type == cgltf_attribute_type_joints && a.index == 0) jnt_acc = a.data;
                else if (a.type == cgltf_attribute_type_weights && a.index == 0) wgt_acc = a.data;
            }
            if (!pos_acc || pos_acc->count == 0) continue;

            const std::uint32_t base_v = static_cast<std::uint32_t>(out.vertices.size());
            out.vertices.resize(base_v + pos_acc->count);

            for (std::size_t vi = 0; vi < pos_acc->count; ++vi) {
                GlbVertex &v = out.vertices[base_v + vi];
                float p[3] = {0, 0, 0};
                cgltf_accessor_read_float(pos_acc, vi, p, 3);
                v.px = p[0]; v.py = p[1]; v.pz = p[2];

                if (first_vert) {
                    out.min_x = out.max_x = p[0];
                    out.min_y = out.max_y = p[1];
                    out.min_z = out.max_z = p[2];
                    first_vert = false;
                } else {
                    out.min_x = std::min(out.min_x, p[0]); out.max_x = std::max(out.max_x, p[0]);
                    out.min_y = std::min(out.min_y, p[1]); out.max_y = std::max(out.max_y, p[1]);
                    out.min_z = std::min(out.min_z, p[2]); out.max_z = std::max(out.max_z, p[2]);
                }

                if (nrm_acc && vi < nrm_acc->count) {
                    float n[3] = {0, 1, 0};
                    cgltf_accessor_read_float(nrm_acc, vi, n, 3);
                    v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
                }
                if (tan_acc && vi < tan_acc->count) {
                    float t[4] = {1, 0, 0, 1};
                    cgltf_accessor_read_float(tan_acc, vi, t, 4);
                    v.tx = t[0]; v.ty = t[1]; v.tz = t[2]; v.tw = t[3];
                }
                if (uv_acc && vi < uv_acc->count) {
                    float uv[2] = {0, 0};
                    cgltf_accessor_read_float(uv_acc, vi, uv, 2);
                    if (has_uv_xform) {
                        const float c = std::cos(uv_rot);
                        const float s = std::sin(uv_rot);
                        const float su = uv[0] * uv_scale[0];
                        const float sv = uv[1] * uv_scale[1];
                        v.u = c * su + s * sv + uv_off[0];
                        v.v = -s * su + c * sv + uv_off[1];
                    } else {
                        v.u = uv[0];
                        v.v = uv[1];
                    }
                }
                if (out.has_skin && jnt_acc && wgt_acc && vi < jnt_acc->count && vi < wgt_acc->count) {
                    cgltf_uint jv[4] = {0, 0, 0, 0};
                    float wv[4] = {1, 0, 0, 0};
                    cgltf_accessor_read_uint(jnt_acc, vi, jv, 4);
                    cgltf_accessor_read_float(wgt_acc, vi, wv, 4);
                    const float wsum = wv[0] + wv[1] + wv[2] + wv[3];
                    const float inv_w = (wsum > 1e-5f) ? (1.0f / wsum) : 1.0f;
                    for (int k = 0; k < 4; ++k) {
                        v.joints[k] = static_cast<std::uint8_t>(std::min<cgltf_uint>(jv[k], static_cast<cgltf_uint>(kMaxSkinBones - 1)));
                        v.weights[k] = (wsum > 1e-5f) ? (wv[k] * inv_w) : (k == 0 ? 1.0f : 0.0f);
                    }
                }
            }

            if (prim.indices && prim.indices->count >= 3) {
                const std::size_t ic = prim.indices->count;
                out.indices.reserve(out.indices.size() + ic);
                for (std::size_t ii = 0; ii < ic; ++ii) {
                    out.indices.push_back(base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, ii)));
                }
            } else {
                for (std::size_t ii = 0; ii < pos_acc->count; ++ii) {
                    out.indices.push_back(base_v + static_cast<std::uint32_t>(ii));
                }
            }
        }
    }

    cgltf_free(data);

    if (!out.valid()) {
        if (err_out) *err_out = "GLB contains no triangle geometry: " + pstr;
        return false;
    }

    const float height = std::max(1e-4f, out.max_y - out.min_y);
    out.norm_scale = 1.0f / height;
    out.center_x = 0.5f * (out.min_x + out.max_x);
    out.center_z = 0.5f * (out.min_z + out.max_z);

    // Precompute bind-pose world pivot for every joint (arbitrary parent order)
    // and auto-detect model left/front axes so character-space procedural animation
    // works identically across Blender, Meshy, Mixamo, and Rigify exports.
    if (out.has_skin && !out.joints.empty()) {
        const std::size_t jn = out.joints.size();
        std::vector<std::array<float, 16>> gbind(jn, mat4_identity());
        std::vector<std::uint8_t> st(jn, 0);
        auto eval_bind = [&](auto &&self, std::size_t idx) -> const std::array<float, 16> & {
            if (st[idx] == 2) return gbind[idx];
            if (st[idx] == 1) return out.joints[idx].rest_local;
            st[idx] = 1;
            const int p = out.joints[idx].parent;
            if (p >= 0 && static_cast<std::size_t>(p) < jn && static_cast<std::size_t>(p) != idx) {
                gbind[idx] = mat4_mul(self(self, static_cast<std::size_t>(p)), out.joints[idx].rest_local);
            } else {
                gbind[idx] = out.joints[idx].rest_local;
            }
            st[idx] = 2;
            return gbind[idx];
        };
        float leg_l_x = 0.0f, leg_r_x = 0.0f;
        bool have_ll = false, have_lr = false;
        for (std::size_t i = 0; i < jn; ++i) {
            const auto &m = eval_bind(eval_bind, i);
            out.joints[i].bind_pos = {m[12], m[13], m[14]};
            if (out.joints[i].humanoid == HumanoidBone::UpperLegL) { leg_l_x = m[12]; have_ll = true; }
            if (out.joints[i].humanoid == HumanoidBone::UpperLegR) { leg_r_x = m[12]; have_lr = true; }
        }
        if (have_ll && have_lr && std::fabs(leg_l_x - leg_r_x) > 1e-4f) {
            out.left_sign = (leg_l_x > leg_r_x) ? 1.0f : -1.0f;
        }
        out.front_sign = 1.0f;  // glTF standard +Z forward
    }

    out.rig_preset_detected = !out.has_skin ? "unrigged"
                            : saw_mixamo ? "mixamo"
                            : saw_meshy ? "meshy"
                            : (out.mapped_humanoid_count >= 6 ? "humanoid" : "generic");

    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "%s: %zu tris | %zu verts | Albedo %dx%d | Normal %dx%d | %s (%zu joints, %d humanoid mapped, preset=%s)",
                  path.filename().string().c_str(),
                  out.triangle_count(), out.vertices.size(),
                  out.albedo.width, out.albedo.height,
                  out.normal_map.width, out.normal_map.height,
                  out.has_skin ? "RIGGED" : "UNRIGGED (rigid fallback)",
                  out.joints.size(), out.mapped_humanoid_count,
                  out.rig_preset_detected.c_str());
    out.diagnostic_summary = buf;
    return true;
}

void evaluate_skin_matrices(const GlbModel &model, const ModelDrawParams &p,
                            std::array<std::array<float, 16>, kMaxSkinBones> &out_mats) {
    const auto id = mat4_identity();
    for (int i = 0; i < kMaxSkinBones; ++i) out_mats[static_cast<std::size_t>(i)] = id;
    if (!model.has_skin || model.joints.empty()) return;

    const std::size_t n = std::min<std::size_t>(model.joints.size(), static_cast<std::size_t>(kMaxSkinBones));
    const float model_h = std::max(0.1f, model.max_y - model.min_y);
    const float fsign = model.front_sign;  // +1 for +Z forward
    const float lsign = model.left_sign;   // +1 for +X left

    const float spd = std::clamp(p.move_speed, 0.0f, 1.0f);
    const float idle_w = 1.0f - spd;
    const float t = p.anim_time_sec;
    const float ph = p.stride_phase;
    const float sin_p = std::sin(ph);
    const float cos_p = std::cos(ph);
    const float breath = std::sin(t * 2.1f);
    const float head_sway = std::sin(t * 0.73f);

    // Character-space bind-pivot delta matrix per joint:
    //   delta_i = T(pivot_i + trans_i) * Ry(yaw) * Rx(pitch) * Rz(roll) * T(-pivot_i)
    //   M_skin[i] = M_skin[parent] * delta_i
    // Completely independent of exporter bone roll / local axis conventions!
    std::vector<std::array<float, 16>> delta_mats(n, id);
    std::vector<std::array<float, 16>> skin_mats(n, id);
    std::vector<std::uint8_t> state(n, 0);

    const bool battle_idle = p.in_battle && (spd < 0.25f);

    for (std::size_t i = 0; i < n; ++i) {
        const GlbJoint &j = model.joints[i];
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        float tx = 0.0f, ty = 0.0f, tz = 0.0f;

        // Walk/run stride amplitudes (in radians; right-hand rule around +X:
        // for downward limbs (0,-1,0), -rx swings FORWARD (+Z) and +rx bends BACKWARD (-Z))
        const float leg_amp = 0.56f * spd;      // ~32 deg walk -> ~45+ deg run
        const float arm_amp = 0.48f * spd;      // natural opposite arm swing
        const float knee_l = (0.10f + 0.88f * std::max(0.0f, cos_p)) * spd;
        const float knee_r = (0.10f + 0.88f * std::max(0.0f, -cos_p)) * spd;
        const float elbow_l = 0.26f * idle_w + (0.34f + 0.42f * std::max(0.0f, -sin_p)) * spd;
        const float elbow_r = 0.26f * idle_w + (0.34f + 0.42f * std::max(0.0f, sin_p)) * spd;

        switch (j.humanoid) {
        case HumanoidBone::Hips:
            ty = model_h * (0.020f * (std::fabs(sin_p) - 0.35f) * spd +
                            0.003f * breath * idle_w -
                            (battle_idle ? 0.022f : 0.0f));
            ry = sin_p * 0.14f * spd * fsign + (battle_idle ? -0.22f * lsign : 0.0f);
            rz = sin_p * 0.055f * spd * lsign + 0.015f * head_sway * idle_w * lsign;
            break;
        case HumanoidBone::Spine:
            rx = (0.07f * spd + 0.025f * breath * idle_w + (battle_idle ? 0.08f : 0.0f)) * fsign;
            ry = -sin_p * 0.09f * spd * fsign;
            break;
        case HumanoidBone::Chest:
            rx = (0.04f * spd + 0.032f * breath * idle_w + (battle_idle ? 0.05f : 0.0f)) * fsign;
            ry = -sin_p * 0.11f * spd * fsign + (battle_idle ? 0.12f * lsign : 0.0f);
            break;
        case HumanoidBone::Neck:
            rx = (-0.04f * spd - 0.015f * breath * idle_w) * fsign;
            break;
        case HumanoidBone::Head:
            ry = (0.07f * head_sway * idle_w + (battle_idle ? 0.10f * lsign : 0.0f));
            rx = (-0.03f * spd + 0.02f * std::sin(t * 1.1f) * idle_w) * fsign;
            break;
        case HumanoidBone::UpperLegL:
            rx = (-sin_p * leg_amp - (battle_idle ? 0.18f : 0.04f * idle_w)) * fsign;
            rz = (battle_idle ? 0.06f : 0.0f) * lsign;
            break;
        case HumanoidBone::UpperLegR:
            rx = (sin_p * leg_amp - (battle_idle ? 0.14f : 0.04f * idle_w)) * fsign;
            rz = (battle_idle ? -0.06f : 0.0f) * lsign;
            break;
        case HumanoidBone::LowerLegL:
            rx = (knee_l + (battle_idle ? 0.24f : 0.06f * idle_w)) * fsign;
            break;
        case HumanoidBone::LowerLegR:
            rx = (knee_r + (battle_idle ? 0.20f : 0.06f * idle_w)) * fsign;
            break;
        case HumanoidBone::FootL:
            rx = (-0.22f * std::max(0.0f, cos_p) * spd + 0.14f * std::max(0.0f, -sin_p) * spd) * fsign;
            break;
        case HumanoidBone::FootR:
            rx = (-0.22f * std::max(0.0f, -cos_p) * spd + 0.14f * std::max(0.0f, sin_p) * spd) * fsign;
            break;
        case HumanoidBone::ShoulderL:
            rz = (0.02f * breath * idle_w) * lsign;
            break;
        case HumanoidBone::ShoulderR:
            rz = (-0.02f * breath * idle_w) * lsign;
            break;
        case HumanoidBone::UpperArmL:
            if (battle_idle) {
                rx = (-0.42f + 0.04f * breath) * fsign;
                rz = 0.12f * lsign;
            } else {
                rx = (sin_p * arm_amp - 0.06f * idle_w) * fsign;
                rz = (0.04f + 0.02f * breath * idle_w) * lsign;  // tiny clearance from skirt
            }
            break;
        case HumanoidBone::UpperArmR:
            if (battle_idle) {
                rx = (-0.52f - 0.04f * breath) * fsign;
                rz = -0.14f * lsign;
            } else {
                rx = (-sin_p * arm_amp - 0.06f * idle_w) * fsign;
                rz = (-0.04f - 0.02f * breath * idle_w) * lsign;
            }
            break;
        case HumanoidBone::LowerArmL:
            rx = -(battle_idle ? (0.82f + 0.04f * breath) : elbow_l) * fsign;
            break;
        case HumanoidBone::LowerArmR:
            rx = -(battle_idle ? (0.95f - 0.04f * breath) : elbow_r) * fsign;
            break;
        default:
            break;
        }

        if (rx != 0.0f || ry != 0.0f || rz != 0.0f || tx != 0.0f || ty != 0.0f || tz != 0.0f) {
            const float px = j.bind_pos[0];
            const float py = j.bind_pos[1];
            const float pz = j.bind_pos[2];
            std::array<float, 16> rot = id;
            if (ry != 0.0f) rot = mat4_mul(rot, mat4_rot_y(ry));
            if (rx != 0.0f) rot = mat4_mul(rot, mat4_rot_x(rx));
            if (rz != 0.0f) rot = mat4_mul(rot, mat4_rot_z(rz));
            delta_mats[i] = mat4_mul(mat4_translate(px + tx, py + ty, pz + tz),
                                     mat4_mul(rot, mat4_translate(-px, -py, -pz)));
        }
    }

    auto eval_skin = [&](auto &&self, std::size_t idx) -> const std::array<float, 16> & {
        if (state[idx] == 2) return skin_mats[idx];
        if (state[idx] == 1) return delta_mats[idx];  // cycle guard
        state[idx] = 1;
        const int parent = model.joints[idx].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < n && static_cast<std::size_t>(parent) != idx) {
            skin_mats[idx] = mat4_mul(self(self, static_cast<std::size_t>(parent)), delta_mats[idx]);
        } else {
            skin_mats[idx] = delta_mats[idx];
        }
        state[idx] = 2;
        return skin_mats[idx];
    };

    for (std::size_t i = 0; i < n; ++i) {
        out_mats[i] = eval_skin(eval_skin, i);
    }
}

}  // namespace psx::model_swap
