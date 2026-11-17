// Link's body as each player animates it (pose.h): this game's Link's skeleton pose, read at the camera's lens, goes to the
// others; each other player's Link shows the pose its player sent, made into the world bones the game draws.
//
// A pose carries the body's bones (the face, the hair and the cloth/physics bones stay the puppet's own): each one's
// rotation (the three smallest of its quaternion, 15 bits each), and the translation and scale of those that differ from
// the skeleton's rest. Both games pick the same bones from the same skeleton (by name), checked by a hash of their names.
#include "pose.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "coop.h"
#include "fellow.h"
#include "game.h"
#include "session.h"

namespace coop {

namespace {
constexpr uint64_t kGameActorVt = 0x84b45e18ULL;
constexpr uint64_t kUnitVt = 0x84c28cc8ULL;      // (the model unit)
constexpr uint64_t kModelGetter = 0x809f7160ULL; // (the model component's plain getter of its model: *(+0x18))
constexpr uint32_t kFskl = 0x4c4b5346u;          // "FSKL"
constexpr int kMaxBones = 512, kMaxSent = 250, kMaxTrans = 64, kMaxScale = 48;
constexpr uint8_t kBlobVersion = 1;

std::atomic<bool> g_mirror{false};

// ---- the fall: the pose the game's own death ends in (AS Dead, then DeadRagdoll), each body bone's rotation as child
// Link's local bones held it, recorded from the game; the hips' height (Skl_Root 0.12888 m, 0.583875 at rest) scaled to
// each skeleton's own (the adult's bones bear the same names)
struct DownBone {
    const char* name;
    float q[4];
};
const DownBone kDownPose[] = {
    {"Root", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Pred_Ankle_L", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Ph_Pred_Ankle_R", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Skl_Root", {0.70710f, -0.02468f, 0.00000f, 0.70668f}},
    {"Spine_1", {0.53495f, 0.47039f, 0.50967f, 0.48249f}}, {"Spine_2", {0.01525f, 0.00839f, 0.01005f, 0.99980f}},
    {"Clavicle_L", {-0.07006f, -0.70629f, 0.02618f, 0.70396f}}, {"Arm_1_L", {-0.70025f, -0.06116f, -0.01850f, 0.71103f}},
    {"Arm_2_L", {-0.00000f, -0.00001f, 0.44041f, 0.89780f}}, {"Ph_Elbow_L", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Ph_Wrist_Assist_A_L", {0.01140f, 0.00000f, 0.00000f, 0.99993f}}, {"Ph_Wrist_Assist_B_L", {0.00784f, 0.00000f, 0.00000f, 0.99997f}},
    {"Wrist_L", {0.01583f, 0.06401f, -0.00331f, 0.99782f}}, {"Finger_A_1_L", {0.93218f, 0.25157f, 0.16421f, 0.20194f}},
    {"Finger_A_2_L", {-0.01347f, 0.00897f, 0.07144f, 0.99731f}}, {"Finger_A_3_L", {0.00000f, 0.00000f, -0.03803f, 0.99928f}},
    {"Weapon_L", {0.00000f, 0.70711f, 0.70711f, 0.00000f}}, {"Wrist_Finger_B_L", {0.70183f, 0.08622f, 0.11362f, 0.69792f}},
    {"Finger_B_1_L", {-0.02773f, -0.12362f, 0.12919f, 0.98349f}}, {"Finger_B_2_L", {0.00000f, -0.00010f, 0.17146f, 0.98519f}},
    {"Finger_B_3_L", {0.00002f, 0.00001f, 0.10750f, 0.99420f}}, {"Ph_Finger_B_Support_L", {-0.00000f, 0.00000f, -0.02844f, 0.99960f}},
    {"Wrist_Finger_C_L", {0.70559f, 0.04628f, 0.02922f, 0.70650f}}, {"Finger_C_1_L", {-0.00521f, -0.05311f, 0.12259f, 0.99102f}},
    {"Finger_C_2_L", {0.00000f, 0.00000f, 0.28810f, 0.95760f}}, {"Finger_C_3_L", {0.00000f, 0.00000f, 0.01955f, 0.99981f}},
    {"Ph_Finger_C_Support_L", {-0.00000f, 0.00000f, -0.02094f, 0.99978f}}, {"Wrist_Finger_D_L", {0.67402f, -0.06603f, -0.00652f, 0.73573f}},
    {"Finger_D_1_L", {-0.01329f, -0.00564f, 0.02873f, 0.99948f}}, {"Finger_D_2_L", {0.00000f, 0.00000f, 0.33548f, 0.94205f}},
    {"Finger_D_3_L", {0.00000f, 0.00000f, 0.10268f, 0.99471f}}, {"Ph_Finger_D_Support_L", {-0.00000f, 0.00000f, -0.00450f, 0.99999f}},
    {"Wrist_Finger_E_L", {0.64880f, -0.16647f, -0.01514f, 0.74237f}}, {"Finger_E_1_L", {-0.04438f, 0.05104f, -0.06370f, 0.99567f}},
    {"Finger_E_2_L", {0.00000f, 0.00000f, 0.34019f, 0.94036f}}, {"Finger_E_3_L", {0.00000f, 0.00000f, 0.10127f, 0.99486f}},
    {"Ph_Finger_E_Support_L", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Arm_1_Assist_B_L", {0.19044f, 0.00000f, 0.00000f, 0.98170f}},
    {"Ph_Arm_1_Assist_L", {0.54120f, -0.06921f, 0.03389f, 0.83735f}}, {"Ph_Clavicle_Assist_L", {0.03116f, 0.00000f, 0.00000f, 0.99951f}},
    {"Ph_Sleeve_Assist_Rotate_L", {0.05165f, 0.33070f, -0.05433f, 0.94076f}}, {"Clavicle_R", {0.66101f, -0.03392f, 0.74902f, -0.02976f}},
    {"Arm_1_R", {-0.68692f, -0.25310f, -0.17422f, 0.65858f}}, {"Arm_2_R", {0.00000f, 0.00000f, 0.55679f, 0.83065f}},
    {"Ph_Elbow_R", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Wrist_Assist_A_R", {0.00987f, 0.00000f, 0.00000f, 0.99995f}},
    {"Ph_Wrist_Assist_B_R", {0.00719f, 0.00000f, 0.00000f, 0.99997f}}, {"Wrist_R", {0.01337f, 0.05818f, 0.02230f, 0.99797f}},
    {"Finger_A_1_R", {0.93218f, 0.25163f, 0.16405f, 0.20202f}}, {"Finger_A_2_R", {-0.01348f, 0.00897f, 0.07144f, 0.99731f}},
    {"Finger_A_3_R", {0.00000f, 0.00000f, -0.03803f, 0.99928f}}, {"Weapon_R", {0.70711f, 0.00000f, 0.00000f, 0.70711f}},
    {"Wrist_Finger_B_R", {0.70183f, 0.08622f, 0.11362f, 0.69792f}}, {"Finger_B_1_R", {-0.02778f, -0.12368f, 0.12858f, 0.98356f}},
    {"Finger_B_2_R", {0.00000f, 0.00000f, 0.17191f, 0.98511f}}, {"Finger_B_3_R", {0.00000f, 0.00000f, 0.10750f, 0.99420f}},
    {"Ph_Finger_B_Support_R", {-0.00000f, 0.00000f, -0.02842f, 0.99960f}}, {"Wrist_Finger_C_R", {0.70559f, 0.04629f, 0.02922f, 0.70650f}},
    {"Finger_C_1_R", {-0.00521f, -0.05311f, 0.12259f, 0.99102f}}, {"Finger_C_2_R", {0.00000f, 0.00000f, 0.28810f, 0.95760f}},
    {"Finger_C_3_R", {0.00000f, 0.00000f, 0.02004f, 0.99980f}}, {"Ph_Finger_C_Support_R", {-0.00000f, 0.00000f, -0.02094f, 0.99978f}},
    {"Wrist_Finger_D_R", {0.67402f, -0.06603f, -0.00652f, 0.73573f}}, {"Finger_D_1_R", {-0.01324f, -0.00564f, 0.02882f, 0.99948f}},
    {"Finger_D_2_R", {0.00000f, 0.00000f, 0.33548f, 0.94205f}}, {"Finger_D_3_R", {0.00000f, 0.00000f, 0.10268f, 0.99471f}},
    {"Ph_Finger_D_Support_R", {-0.00000f, 0.00000f, -0.00452f, 0.99999f}}, {"Wrist_Finger_E_R", {0.64880f, -0.16647f, -0.01514f, 0.74237f}},
    {"Finger_E_1_R", {-0.04441f, 0.05102f, -0.06383f, 0.99567f}}, {"Finger_E_2_R", {0.00000f, 0.00000f, 0.34019f, 0.94036f}},
    {"Finger_E_3_R", {0.00000f, 0.00000f, 0.10127f, 0.99486f}}, {"Ph_Finger_E_Support_R", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Ph_Arm_1_Assist_B_R", {0.19391f, 0.00000f, 0.00000f, 0.98102f}}, {"Ph_Arm_1_Assist_R", {0.69248f, -0.01464f, -0.00881f, 0.72123f}},
    {"Ph_Clavicle_Assist_R", {0.00284f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Sleeve_Assist_Rotate_R", {0.00955f, 0.08538f, -0.04408f, 0.99533f}},
    {"Neck", {-0.12154f, -0.00604f, -0.03942f, 0.99179f}}, {"Head", {-0.28547f, 0.00280f, -0.08709f, 0.95442f}},
    {"Head_2", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ear_L", {-0.33953f, -0.27831f, -0.44812f, 0.77875f}},
    {"Ear_R", {0.27831f, -0.33953f, 0.77875f, 0.44812f}}, {"Pod_A", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Waist", {0.46881f, -0.45321f, -0.50698f, 0.56374f}}, {"Leg_1_L", {-0.44451f, 0.15472f, 0.34242f, 0.81316f}},
    {"Leg_2_L", {-0.00000f, 0.00000f, -0.55597f, 0.83120f}}, {"Ankle_L", {0.00000f, 0.00000f, 0.73728f, 0.67559f}},
    {"Toe_L", {-0.00000f, -0.00000f, 0.00000f, 1.00000f}}, {"Ph_Ankle_Assist_L", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Ph_Calf_L", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Knee_L", {0.00000f, 0.00000f, 0.58612f, 0.81022f}},
    {"Ph_Leg_Support_L", {-0.00000f, 0.00000f, -0.13261f, 0.99117f}}, {"Ph_Leg_Support_Roll_L", {0.24783f, 0.00000f, 0.00000f, 0.96880f}},
    {"Leg_1_R", {-0.20001f, -0.36947f, 0.87563f, -0.23825f}}, {"Leg_2_R", {-0.00000f, 0.00000f, -0.40316f, 0.91513f}},
    {"Ankle_R", {0.00000f, 0.00000f, 0.73728f, 0.67559f}}, {"Toe_R", {0.00000f, 0.00000f, -0.00000f, 1.00000f}},
    {"Ph_Ankle_Assist_R", {0.00000f, 0.00000f, 0.00000f, 1.00000f}}, {"Ph_Calf_R", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Ph_Knee_R", {0.00000f, 0.00000f, 0.42620f, 0.90463f}}, {"Ph_Leg_Support_R", {-0.00000f, 0.00000f, -0.08199f, 0.99663f}},
    {"Ph_Leg_Support_Roll_R", {0.19844f, 0.00000f, 0.00000f, 0.98011f}}, {"Ph_Leg_Support_Waist_L", {0.00000f, 0.15566f, 0.00000f, 0.98781f}},
    {"Ph_Leg_Support_Waist_R", {-0.14148f, 0.00000f, 0.98994f, 0.00000f}}, {"Pod_B", {0.00000f, 0.00000f, 0.00000f, 1.00000f}},
    {"Weapon_Root", {-0.70697f, 0.01122f, 0.01338f, 0.70703f}},
};
constexpr float kDownHips = 0.12888f, kChildHipsRest = 0.583875f;

bool heap(uint64_t a) { return a >= kHeapBase && a < kHeapBase + 0x100000000ULL; }

// ---- the skeleton (one for each model: the child's, the adult's)
struct Skel {
    uint64_t fskl = 0;
    bool body = true;               // (Link's: his body's bones; else every bone, as many as a pose carries)
    bool ok = false;
    int count = 0;
    std::vector<int16_t> parent;
    std::vector<int16_t> sent;      // the bones a pose carries
    std::vector<int16_t> sent_of;   // a bone's place among them (-1: not carried)
    std::vector<float> rest_t;      // (3 a bone)
    uint32_t hash = 0;
    std::vector<int16_t> down_of;   // (a carried bone's place in kDownPose, -1 none)
    int hips = -1;                  // (Skl_Root's place among the carried bones)
};
std::vector<Skel*> g_skels;

const Skel* skel_of(uint64_t fskl, bool body) {
    static std::mutex m;   // (Link's update and the lens both ask)
    std::lock_guard<std::mutex> lock(m);
    for (const Skel* s : g_skels)
        if (s->fskl == fskl && s->body == body) return s->ok ? s : nullptr;
    Skel* s = new Skel;
    s->fskl = fskl;
    s->body = body;
    g_skels.push_back(s);
    if (r32(fskl) != kFskl) return nullptr;
    const int n = r16(fskl + 0x38);
    const uint64_t arr = r64(fskl + 0x10);
    if (n <= 0 || n > kMaxBones || !guest_ok(arr, (uint64_t)n * 0x58)) return nullptr;
    s->count = n;
    s->parent.assign(n, -1);
    s->sent_of.assign(n, -1);
    s->rest_t.assign(3 * (size_t)n, 0.0f);
    std::vector<bool> skip(n, false);
    uint32_t h = 2166136261u;
    auto mix = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
    static const char* const kKeep[] = {"Assist", "Support", "Elbow", "Knee", "Calf", "Pred_", "Wrist", "Clavicle"};
    for (int i = 0; i < n; i++) {
        const uint64_t rec = arr + 0x58 * (uint64_t)i;
        const std::string name = cstr_at(r64(rec) + 2, 64);
        const int p = (int16_t)r16(rec + 0x22);
        if (p >= i || (p < 0 && i > 0 && p != -1)) return nullptr;   // (parents come first: the order the world bones are made in)
        s->parent[i] = (int16_t)p;
        for (int k = 0; k < 3; k++) s->rest_t[3 * (size_t)i + k] = rf(rec + 0x4c + 4 * (uint64_t)k);
        bool out = body && ((p >= 0 && skip[p]) || name == "Face_Root" || name == "Hair_Root");
        if (body && !out && name.rfind("Ph_", 0) == 0) {
            out = true;
            for (const char* w : kKeep)
                if (name.find(w) != std::string::npos) out = false;
        }
        skip[i] = out;
        if (!out && (int)s->sent.size() < kMaxSent) {
            int down = -1;
            for (int d = 0; d < (int)(sizeof kDownPose / sizeof kDownPose[0]); d++)
                if (name == kDownPose[d].name) down = d;
            if (body && name == "Skl_Root") s->hips = (int)s->sent.size();
            if (!body) down = -1;
            s->down_of.push_back((int16_t)down);
            s->sent_of[i] = (int16_t)s->sent.size();
            s->sent.push_back((int16_t)i);
            for (char ch : name) mix((uint8_t)ch);
            mix(0);
        }
    }
    mix((uint8_t)n), mix((uint8_t)(n >> 8));
    s->hash = h;
    s->ok = true;
    log("pose: skeleton %#llx: %d bones, %zu carried in a pose (%s, hash %08x)", (unsigned long long)fskl, n, s->sent.size(),
        body ? "Link's body" : "every bone", h);
    return s;
}

// ---- an actor's model unit
struct Unit {
    uint64_t unit = 0, local = 0, world = 0;
    const Skel* skel = nullptr;
};

bool unit_of(uint64_t actor, Unit* u, bool body = true) {
    if (!heap(actor) || r64(actor) != kGameActorVt) return false;
    const uint64_t comps = r64(actor + 0x270);
    const uint64_t comp = heap(comps) ? r64(comps + 8) : 0;
    if (!heap(comp) || r64(r64(comp) + 0xf8) != kModelGetter) return false;
    const uint64_t model = r64(comp + 0x18);
    const uint64_t units = heap(model) ? r64(model + 0x28) : 0;
    const uint64_t holder = heap(units) ? r64(units) : 0;
    const uint64_t unit = heap(holder) ? r64(holder) : 0;
    if (!heap(unit) || r64(unit) != kUnitVt) return false;
    const uint64_t st = r64(unit + 0xf8);
    if (!heap(st)) return false;
    u->unit = unit;
    u->local = r64(st + 0x18);
    u->world = r64(st + 0x20);
    u->skel = skel_of(r64(st), body);
    return u->skel && heap(u->local) && heap(u->world) && guest_ok(u->local, (uint64_t)u->skel->count * 0x60) &&
           guest_ok(u->world, (uint64_t)u->skel->count * 0x40);
}

// ---- rotations
void mat_quat(const float* col0, const float* col1, const float* col2, float q[4]) {   // (columns of R; q: x y z w)
    const float m00 = col0[0], m10 = col0[1], m20 = col0[2];
    const float m01 = col1[0], m11 = col1[1], m21 = col1[2];
    const float m02 = col2[0], m12 = col2[1], m22 = col2[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = sqrtf(std::max(1e-12f, 1.0f + m00 - m11 - m22)) * 2.0f;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = sqrtf(std::max(1e-12f, 1.0f + m11 - m00 - m22)) * 2.0f;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        const float s = sqrtf(std::max(1e-12f, 1.0f + m22 - m00 - m11)) * 2.0f;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
    const float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] = len > 1e-8f ? q[i] / len : (i == 3 ? 1.0f : 0.0f);
}

void quat_mat(const float q[4], float r[3][3]) {   // (r[row][col])
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - z * w), r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w), r[2][1] = 2 * (y * z + x * w), r[2][2] = 1 - 2 * (x * x + y * y);
}

constexpr float kHalfRoot2 = 0.70710678f;

uint64_t quat_bits(const float q[4]) {   // (the three smallest: 2 bits which is largest, then 15 bits each)
    int k = 0;
    for (int i = 1; i < 4; i++)
        if (fabsf(q[i]) > fabsf(q[k])) k = i;
    const float sign = q[k] < 0 ? -1.0f : 1.0f;
    uint64_t bits = (uint64_t)k;
    int shift = 2;
    for (int i = 0; i < 4; i++) {
        if (i == k) continue;
        const float v = std::clamp(q[i] * sign / kHalfRoot2, -1.0f, 1.0f);
        const uint64_t u = (uint64_t)std::clamp((int)lroundf((v * 0.5f + 0.5f) * 32767.0f), 0, 32767);
        bits |= u << shift;
        shift += 15;
    }
    return bits;
}

void bits_quat(uint64_t bits, float q[4]) {
    const int k = (int)(bits & 3);
    int shift = 2;
    float sum = 0;
    for (int i = 0; i < 4; i++) {
        if (i == k) continue;
        const float u = (float)((bits >> shift) & 0x7fff) / 32767.0f;
        q[i] = (u * 2.0f - 1.0f) * kHalfRoot2;
        sum += q[i] * q[i];
        shift += 15;
    }
    q[k] = sqrtf(std::max(0.0f, 1.0f - sum));
}

// ---- what a Link carries: the actors of his equipment (the sword, the sheath, the shield, a mask, the slingshot, a Deku
// Stick...), each hanging from one of his equipment's slots (*(equipment + 0x28 + 0x18 x slot) + 0x40; the slot's own
// vtable 0x84994a90 at +0x20 + 0x18 x slot). The game places each one's model on Link's body (a drawn sword in his hand, a
// sheathed one on his back); the items of his hands are there all the time but taken out of the game's actors (their
// link at +0x190 empty) while put away, so only the ones in the actors are sent. Each one's model root is sent against the
// nearest body bone a pose carries (its anchor), so on the other Links it sits on the same bone as their pose moves it,
// the fall included. The other Link (the pause menu's) wears only the gear; the items of the hands (which the game never
// gives it: it would fail on them) are shown by stand-alone ones of the same name (puppet.cpp)
constexpr int kMaxCarried = 8, kEquipSlots = 16;
constexpr uint64_t kEquipSlotVt = 0x84994a90ULL;
constexpr uint8_t kCarriedMark = 0xc7;
struct Carried {
    uint8_t slot = 0, anchor = 0;   // (anchor: the body bone's place among the bones a pose carries)
    uint32_t name = 0;              // (FNV-1a of the actor's name)
    float q[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0}, s[3] = {1, 1, 1};
    char text[40] = {0};            // (the actor's name)
};
struct CarriedSet {
    int n = 0;
    Carried c[kMaxCarried];
};

// ---- a pose, as shown
struct PoseData {
    uint32_t hash = 0;
    int n = 0;
    float q[kMaxSent][4];
    float t[kMaxSent][3];
    float s[kMaxSent][3];
    CarriedSet carried;
};

// ---- 3 x 4 matrices (rows); a world bone holds four columns of four floats
void bone_rows(const float* w, float m[12]) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m[4 * r + c] = w[4 * c + r];
}
void rows_bone(const float m[12], float* w) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) w[4 * c + r] = m[4 * r + c];
}
void affine_mul(const float a[12], const float b[12], float out[12]) {
    float o[12];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) o[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c];
        o[4 * r + 3] += a[4 * r + 3];
    }
    memcpy(out, o, sizeof o);
}
bool affine_inverse(const float m[12], float out[12]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (fabsf(det) < 1e-12f || !std::isfinite(det)) return false;
    const float k = 1.0f / det;
    float o[12] = {A * k, -(b * i - c * h) * k, (b * f - c * e) * k, 0,
                   B * k, (a * i - c * g) * k, -(a * f - c * d) * k, 0,
                   C * k, -(a * h - b * g) * k, (a * e - b * d) * k, 0};
    for (int r = 0; r < 3; r++) o[4 * r + 3] = -(o[4 * r] * m[3] + o[4 * r + 1] * m[7] + o[4 * r + 2] * m[11]);
    memcpy(out, o, sizeof o);
    return true;
}

void slerp(const float a[4], const float b[4], float k, float out[4]);

// this Link's pose: its body's bones as the game animated them this frame; fall (0..1): that far into the death's pose
// (down: Link lies there for everyone, his own game too). pd: the same pose, as shown
std::vector<uint8_t> capture(const Unit& u, float fall, PoseData* pd, const CarriedSet* carried = nullptr) {
    const Skel& sk = *u.skel;
    std::vector<uint8_t> b;
    b.reserve(16 + sk.sent.size() * 6 + 8 * 32);
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    const uint16_t count = (uint16_t)sk.count;
    b.push_back(kBlobVersion);
    b.push_back(0);
    put(&count, 2);
    put(&sk.hash, 4);
    b.push_back((uint8_t)sk.sent.size());
    struct Extra {
        uint8_t at;
        int16_t v[3];
    };
    Extra trans[kMaxTrans], scales[kMaxScale];
    int nt = 0, ns = 0;
    for (size_t k = 0; k < sk.sent.size(); k++) {
        const int i = sk.sent[k];
        float bone[24];
        peek(u.local + 0x60 * (uint64_t)i, bone, sizeof bone);
        // (bone: flags and padding [0..3], scale [4..6], the columns [8..11] [12..15] [16..19], the translation [20..22])
        float q[4];
        mat_quat(&bone[8], &bone[12], &bone[16], q);
        if (fall > 0 && sk.down_of[k] >= 0) {
            float d[4];
            slerp(q, kDownPose[sk.down_of[k]].q, fall, d);
            memcpy(q, d, sizeof d);
        }
        if (fall > 0 && (int)k == sk.hips) {
            const float y = kDownHips * sk.rest_t[3 * (size_t)i + 1] / kChildHipsRest;
            bone[20] *= 1.0f - fall, bone[22] *= 1.0f - fall;
            bone[21] += (y - bone[21]) * fall;
        }
        if (pd) {
            memcpy(pd->q[k], q, sizeof q);
            for (int j = 0; j < 3; j++) pd->t[k][j] = bone[20 + j], pd->s[k][j] = bone[4 + j];
        }
        const uint64_t bits = quat_bits(q);
        put(&bits, 6);
        const float* rest = &sk.rest_t[3 * (size_t)i];
        if (nt < kMaxTrans && (fabsf(bone[20] - rest[0]) > 0.0004f || fabsf(bone[21] - rest[1]) > 0.0004f || fabsf(bone[22] - rest[2]) > 0.0004f)) {
            Extra& e = trans[nt++];
            e.at = (uint8_t)k;
            for (int j = 0; j < 3; j++) e.v[j] = (int16_t)std::clamp((int)lroundf(bone[20 + j] * 2048.0f), -32767, 32767);
        }
        if (ns < kMaxScale && (fabsf(bone[4] - 1) > 0.002f || fabsf(bone[5] - 1) > 0.002f || fabsf(bone[6] - 1) > 0.002f)) {
            Extra& e = scales[ns++];
            e.at = (uint8_t)k;
            for (int j = 0; j < 3; j++) e.v[j] = (int16_t)std::clamp((int)lroundf(bone[4 + j] * 8192.0f), -32767, 32767);
        }
    }
    b.push_back((uint8_t)nt);
    for (int k = 0; k < nt; k++) b.push_back(trans[k].at), put(trans[k].v, 6);
    b.push_back((uint8_t)ns);
    for (int k = 0; k < ns; k++) b.push_back(scales[k].at), put(scales[k].v, 6);
    if (pd) pd->hash = sk.hash, pd->n = (int)sk.sent.size(), pd->carried.n = 0;
    if (carried && carried->n > 0) {   // (what Link carries, after the bones: older games stop reading before it)
        b.push_back(kCarriedMark);
        b.push_back((uint8_t)carried->n);
        for (int k = 0; k < carried->n; k++) {
            const Carried& c = carried->c[k];
            b.push_back(c.slot), b.push_back(c.anchor);
            put(&c.name, 4);
            const uint64_t bits = quat_bits(c.q);
            put(&bits, 6);
            put(c.t, 12);
            for (int j = 0; j < 3; j++) {
                const int16_t v = (int16_t)std::clamp((int)lroundf(c.s[j] * 8192.0f), -32767, 32767);
                put(&v, 2);
            }
            const uint8_t len = (uint8_t)strnlen(c.text, sizeof c.text - 1);
            b.push_back(len);
            put(c.text, len);
        }
        if (pd) pd->carried = *carried;
    }
    return b;
}

bool decode(const std::vector<uint8_t>& b, const Skel& sk, PoseData* out) {
    size_t at = 0;
    auto get = [&](void* p, size_t n) {
        if (at + n > b.size()) return false;
        memcpy(p, b.data() + at, n);
        at += n;
        return true;
    };
    uint8_t ver = 0, flags = 0, n = 0;
    uint16_t count = 0;
    uint32_t hash = 0;
    if (!get(&ver, 1) || ver != kBlobVersion || !get(&flags, 1) || !get(&count, 2) || !get(&hash, 4) || !get(&n, 1)) return false;
    if (hash != sk.hash || count != sk.count || n != sk.sent.size()) return false;
    out->hash = hash;
    out->n = n;
    for (int k = 0; k < n; k++) {
        uint64_t bits = 0;
        if (!get(&bits, 6)) return false;
        bits_quat(bits, out->q[k]);
        const float* rest = &sk.rest_t[3 * (size_t)sk.sent[k]];
        for (int j = 0; j < 3; j++) out->t[k][j] = rest[j], out->s[k][j] = 1.0f;
    }
    for (int pass = 0; pass < 2; pass++) {
        uint8_t m = 0;
        if (!get(&m, 1)) return false;
        for (int k = 0; k < m; k++) {
            uint8_t idx = 0;
            int16_t v[3];
            if (!get(&idx, 1) || !get(v, 6)) return false;
            if (idx >= n) continue;
            for (int j = 0; j < 3; j++) {
                if (pass == 0) out->t[idx][j] = v[j] / 2048.0f;
                else out->s[idx][j] = v[j] / 8192.0f;
            }
        }
    }
    out->carried.n = 0;
    uint8_t mark = 0, m = 0;
    if (at < b.size() && get(&mark, 1) && mark == kCarriedMark && get(&m, 1)) {
        for (int k = 0; k < m && k < kMaxCarried; k++) {
            Carried c;
            uint64_t bits = 0;
            int16_t sc[3];
            uint8_t len = 0;
            if (!get(&c.slot, 1) || !get(&c.anchor, 1) || !get(&c.name, 4) || !get(&bits, 6) || !get(c.t, 12) || !get(sc, 6) ||
                !get(&len, 1) || len >= sizeof c.text || !get(c.text, len))
                break;
            c.text[len] = 0;
            if (c.anchor >= n) continue;
            bits_quat(bits, c.q);
            for (int j = 0; j < 3; j++) c.s[j] = sc[j] / 8192.0f;
            out->carried.c[out->carried.n++] = c;
        }
    }
    return true;
}

void slerp(const float a[4], const float b[4], float k, float out[4]) {
    float dot = 0;
    for (int j = 0; j < 4; j++) dot += a[j] * b[j];
    const float sb = dot < 0 ? -1.0f : 1.0f;
    dot = fabsf(dot);
    float wa = 1.0f - k, wb = k;
    if (dot < 0.9995f) {
        const float th = acosf(std::min(dot, 1.0f)), st = sinf(th);
        wa = sinf((1.0f - k) * th) / st, wb = sinf(k * th) / st;
    }
    float len = 0;
    for (int j = 0; j < 4; j++) {
        out[j] = wa * a[j] + wb * sb * b[j];
        len += out[j] * out[j];
    }
    len = sqrtf(len);
    for (int j = 0; j < 4; j++) out[j] = len > 1e-8f ? out[j] / len : b[j];
}

void blend(const PoseData& a, const PoseData& b, float k, PoseData* out) {
    out->hash = b.hash;
    out->n = b.n;
    out->carried = b.carried;
    for (int i = 0; i < b.carried.n; i++) {   // (each carried thing eased as the bones are, when it hangs on the same bone)
        const Carried& cb = b.carried.c[i];
        for (int j = 0; j < a.carried.n; j++) {
            const Carried& ca = a.carried.c[j];
            if (ca.slot != cb.slot || ca.name != cb.name || ca.anchor != cb.anchor) continue;
            Carried& co = out->carried.c[i];
            slerp(ca.q, cb.q, k, co.q);
            for (int m = 0; m < 3; m++) co.t[m] = ca.t[m] + (cb.t[m] - ca.t[m]) * k, co.s[m] = ca.s[m] + (cb.s[m] - ca.s[m]) * k;
            break;
        }
    }
    for (int i = 0; i < b.n; i++) {
        slerp(a.q[i], b.q[i], k, out->q[i]);
        for (int j = 0; j < 3; j++) {
            out->t[i][j] = a.t[i][j] + (b.t[i][j] - a.t[i][j]) * k;
            out->s[i][j] = a.s[i][j] + (b.s[i][j] - a.s[i][j]) * k;
        }
    }
}

// the puppet's world bones made from the pose (the bones a pose does not carry: the puppet's own, as its game animated
// them); its root's world, where the puppet stands, kept as the game made it
void apply(const Unit& u, const PoseData& p) {
    const Skel& sk = *u.skel;
    const int n = sk.count;
    static float local[kMaxBones][24];
    static float world[kMaxBones][16];
    static float plain[kMaxBones][12];   // (world without the bone's own scale: 3 rows of 4)
    static float scale[kMaxBones][3];
    if (!peek(u.local, local, (size_t)n * 0x60) || !peek(u.world, world, (size_t)n * 0x40)) return;
    for (int i = 0; i < n; i++) {
        const int par = sk.parent[i];
        const int k = sk.sent_of[i];
        float r[3][3], t[3], s[3];
        if (k >= 0) {
            quat_mat(p.q[k], r);
            for (int j = 0; j < 3; j++) t[j] = p.t[k][j], s[j] = p.s[k][j];
        } else {
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 3; col++) r[row][col] = local[i][8 + 4 * col + row];
            for (int j = 0; j < 3; j++) t[j] = local[i][20 + j], s[j] = local[i][4 + j];
        }
        for (int j = 0; j < 3; j++) scale[i][j] = s[j];
        if (par < 0) {
            // (the root: where the game put the puppet this frame)
            for (int row = 0; row < 3; row++)
                for (int col = 0; col < 4; col++) plain[i][4 * row + col] = world[i][4 * col + row];
            continue;
        }
        float tt[3];
        for (int j = 0; j < 3; j++) tt[j] = t[j] * scale[par][j];
        const float* m = plain[par];
        float out[12];
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++)
                out[4 * row + col] = m[4 * row] * r[0][col] + m[4 * row + 1] * r[1][col] + m[4 * row + 2] * r[2][col];
            out[4 * row + 3] = m[4 * row] * tt[0] + m[4 * row + 1] * tt[1] + m[4 * row + 2] * tt[2] + m[4 * row + 3];
        }
        memcpy(plain[i], out, sizeof out);
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 3; row++) world[i][4 * col + row] = out[4 * row + col] * (col < 3 ? s[col] : 1.0f);
        world[i][3] = world[i][7] = world[i][11] = world[i][15] = 0;
    }
    poke(u.world, world, (size_t)n * 0x40);
}

// ---- what Link carries
uint32_t name_hash(uint64_t actor) {
    uint32_t h = 2166136261u;
    for (char ch : cstr_at(r64(actor + 0x260), 64)) h = (h ^ (uint8_t)ch) * 16777619u;
    return h;
}

uint64_t carried_actor(uint64_t equipment, int slot) {
    if (r64(equipment + 0x20 + 0x18 * (uint64_t)slot) != kEquipSlotVt) return 0;
    const uint64_t item = r64(equipment + 0x28 + 0x18 * (uint64_t)slot);
    const uint64_t a = heap(item) ? r64(item + 0x40) : 0;
    return heap(a) && r64(a) == kGameActorVt ? a : 0;
}

uint64_t equipment_of(uint64_t actor) {
    const uint64_t comps = r64(actor + 0x270);
    const uint64_t eq = heap(comps) ? r64(comps + 0x200) : 0;
    return heap(eq) ? eq : 0;
}

// (each thing this Link carries: its model root against the nearest body bone, as the game placed them)
void carried_read(uint64_t link, const Unit& lu, CarriedSet* out) {
    const uint64_t eq = equipment_of(link);
    const Skel& sk = *lu.skel;
    static float body[kMaxBones][16];
    if (!eq || !peek(lu.world, body, (size_t)sk.count * 0x40)) return;
    CarriedSet cs;
    for (int slot = 0; slot < kEquipSlots && cs.n < kMaxCarried; slot++) {
        const uint64_t a = carried_actor(eq, slot);
        if (!a || a == link || !heap(r64(a + 0x190))) continue;   // (put away: out of the game's actors, not drawn)
        Unit wu;
        float w[16], m[12];
        if (!unit_of(a, &wu, false) || !peek(wu.world, w, sizeof w)) continue;
        bone_rows(w, m);
        if (!std::isfinite(m[3]) || !std::isfinite(m[7]) || !std::isfinite(m[11])) continue;
        int best = -1;
        float best_d = 1e9f;
        for (size_t k = 0; k < sk.sent.size(); k++) {
            const float* bw = body[sk.sent[k]];
            const float dx = bw[12] - m[3], dy = bw[13] - m[7], dz = bw[14] - m[11];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best_d) best_d = d, best = (int)k;
        }
        float anchor[12], inv[12], rel[12];
        if (best < 0) continue;
        bone_rows(body[sk.sent[best]], anchor);
        if (!affine_inverse(anchor, inv)) continue;
        affine_mul(inv, m, rel);
        Carried& c = cs.c[cs.n++];
        c.slot = (uint8_t)slot, c.anchor = (uint8_t)best, c.name = name_hash(a);
        snprintf(c.text, sizeof c.text, "%s", cstr_at(r64(a + 0x260), 39).c_str());
        float col[3][3];
        for (int j = 0; j < 3; j++) {
            float len = 0;
            for (int r = 0; r < 3; r++) len += rel[4 * r + j] * rel[4 * r + j];
            c.s[j] = sqrtf(len);
            for (int r = 0; r < 3; r++) col[j][r] = c.s[j] > 1e-6f ? rel[4 * r + j] / c.s[j] : (r == j ? 1.0f : 0.0f);
        }
        mat_quat(col[0], col[1], col[2], c.q);
        for (int r = 0; r < 3; r++) c.t[r] = rel[4 * r + 3];
    }
    *out = cs;
}

// (another Link's things put where its player's are: each moved whole, with its own bones, so that its model root sits on
// the same body bone in the same way)
void carried_apply(uint64_t link, const Unit& lu, const CarriedSet& cs, int player = -1) {
    if (!cs.n) return;
    const uint64_t eq = equipment_of(link);
    const Skel& sk = *lu.skel;
    if (!eq) return;
    for (int i = 0; i < cs.n; i++) {
        const Carried& c = cs.c[i];
        if (c.anchor >= sk.sent.size()) continue;
        uint64_t a = carried_actor(eq, c.slot);
        if (a && (a == link || name_hash(a) != c.name)) a = 0;
        if (!a && player >= 0) a = puppet_held(player, c.name);   // (one this Link cannot wear: its stand-alone one)
        if (!a) continue;
        Unit wu;
        if (!unit_of(a, &wu, false) || wu.skel->count > 64) continue;
        const int n = wu.skel->count;
        float w[64][16], anchor_w[16];
        if (!peek(wu.world, w, (size_t)n * 0x40) || !peek(lu.world + 0x40 * (uint64_t)sk.sent[c.anchor], anchor_w, sizeof anchor_w)) continue;
        float r[3][3], rel[12], anchor[12], target[12], cur[12], cur_inv[12], d[12];
        quat_mat(c.q, r);
        for (int row = 0; row < 3; row++) {
            for (int col = 0; col < 3; col++) rel[4 * row + col] = r[row][col] * c.s[col];
            rel[4 * row + 3] = c.t[row];
        }
        bone_rows(anchor_w, anchor);
        affine_mul(anchor, rel, target);
        bone_rows(w[0], cur);
        if (!affine_inverse(cur, cur_inv)) continue;
        affine_mul(target, cur_inv, d);
        for (int b = 0; b < n; b++) {
            float m[12], o[12];
            bone_rows(w[b], m);
            affine_mul(d, m, o);
            rows_bone(o, w[b]);
        }
        poke(wu.world, w, (size_t)n * 0x40);
    }
}

// ---- the players' poses as shown
struct Shown {
    bool has = false, live = false;
    uint32_t seq = 0;
    PoseData from, to, cur;
    int64_t at = 0, last_got = 0;
    float interval = 0.03f, dur = 0;
};
Shown g_shown[kMaxPlayers];
uint64_t g_shown_actor[kMaxPlayers];

// ---- the shared enemies' poses (enemies.cpp): every bone of an enemy, as the game that runs it sent them
struct ActorShown {
    uint64_t actor = 0;
    std::vector<uint8_t> blob;   // (the newest, handed over from Link's update)
    bool fresh = false;
    int64_t got = 0;
    Shown sh;
};
std::mutex g_actor_mutex;
ActorShown g_actor_poses[16];

// the way to a new pose, as for the players (a pose's interval long, begun a frame ago)
void take(Shown& sh, const PoseData& in, int64_t now, int64_t frame_ns) {
    if (!sh.has) {
        sh.from = sh.to = sh.cur = in;
        sh.dur = 0;
        sh.has = true;
    } else {
        const float gap = (float)(now - sh.last_got) / 1e9f;
        if (gap > 0.004f && gap < 0.25f) sh.interval = sh.interval * 0.8f + gap * 0.2f;
        sh.from = sh.cur;
        sh.to = in;
        sh.dur = std::clamp(sh.interval, 0.01f, 0.1f);
    }
    sh.last_got = now;
    sh.at = now - frame_ns;
}
}  // namespace

bool pose_actor_tip(uint64_t actor, float length, float out[3]) {
    Unit u;
    if (!unit_of(actor, &u, false) || u.skel->count < 1) return false;
    float w[16], m0[12];
    if (!peek(u.world, w, sizeof w)) return false;
    bone_rows(w, m0);
    float best = 0;
    for (int i = 1; i < u.skel->count && i < 64; i++) {
        float b[16];
        if (!peek(u.world + 0x40 * (uint64_t)i, b, sizeof b)) break;
        const float dx = b[12] - m0[3], dy = b[13] - m0[7], dz = b[14] - m0[11];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d > best && std::isfinite(d)) best = d, out[0] = b[12], out[1] = b[13], out[2] = b[14];
    }
    if (best > 0.2f * 0.2f) return true;
    for (int r = 0; r < 3; r++) out[r] = m0[4 * r + 3] + m0[4 * r + 1] * length;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

bool pose_actor_root(uint64_t actor, float out[3]) {
    Unit u;
    float w[16];
    if (!unit_of(actor, &u, false) || !peek(u.world, w, sizeof w)) return false;
    out[0] = w[12], out[1] = w[13], out[2] = w[14];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

bool pose_capture_actor(uint64_t actor, std::vector<uint8_t>* blob) {
    Unit u;
    if (!unit_of(actor, &u, false)) return false;
    *blob = capture(u, 0, nullptr);
    return !blob->empty();
}

void pose_show_actor(uint64_t actor, const std::vector<uint8_t>& blob, int64_t now) {
    std::lock_guard<std::mutex> lock(g_actor_mutex);
    ActorShown* slot = nullptr;
    for (auto& a : g_actor_poses)
        if (a.actor == actor) slot = &a;
    if (!slot)
        for (auto& a : g_actor_poses)
            if (!a.actor || now - a.got > 1000000000LL) {
                slot = &a;
                break;
            }
    if (!slot) return;
    if (slot->actor != actor) *slot = ActorShown{}, slot->actor = actor;
    slot->blob = blob;
    slot->fresh = true;
    slot->got = now;
}

void pose_mirror(bool on) {
    g_mirror = on;
    log("pose: mirror %s", on ? "on" : "off");
}

void pose_frame(Ctx*, int64_t now) {
    // (one at a time: the lens may be built on more than one of the game's threads; the work uses shared buffers)
    static std::mutex busy;
    std::lock_guard<std::mutex> lock(busy);
    // (more than one camera builds its lens in a frame, some before the game makes the world bones: the poses are worked
    // out once a frame and put on at each lens; the last one, after the game's world bones, is what is drawn)
    static int64_t last = 0, frame_ns = 25000000LL;
    const bool frame = now - last >= 4000000LL;
    if (frame) {
        if (now - last < 200000000LL) frame_ns = now - last;
        last = now;
    }
    Session& ses = Session::get();
    if (!ses.active()) return;
    // ---- this Link's pose, as the game animated it; down, he falls into the death's pose over 0.7 s (faster as he
    // goes, as a fall does) and lies there, in his game and in the others'; up again, he rises out of it
    static float fall = 0;
    static PoseData mine_pd;
    static bool mine_live = false;
    static CarriedSet mine_carried;   // (as the newest lens found it: at a frame's start, the last frame's last lens)
    const bool down = fellow_downed();
    if (frame) fall = std::clamp(fall + (down ? 1.0f / 0.7f : -1.0f / 0.6f) * std::min((float)frame_ns / 1e9f, 0.1f), 0.0f, 1.0f);
    const float shown_fall = down ? fall * fall : fall * fall * (3 - 2 * fall);
    std::vector<uint8_t> mine;
    const LocalSnapshot snap = local_snapshot();
    Unit me;
    const bool mine_ok = snap.valid && now - snap.at_ns < 500000000LL && unit_of(snap.mover, &me);
    if (frame) {
        mine_live = false;
        if (mine_ok && (ses.others_present() || g_mirror || fall > 0)) {
            mine = capture(me, shown_fall, &mine_pd, &mine_carried);
            mine_live = fall > 0;
            if (ses.others_present()) ses.push_pose(mine);
        }
    }
    if (mine_ok) carried_read(snap.mover, me, &mine_carried);   // (before this Link's own fall is put on his bones)
    if (mine_live && mine_ok) apply(me, mine_pd), carried_apply(snap.mover, me, mine_pd.carried);
    // ---- the shared enemies this game copies: the pose the game that runs each one sent
    {
        std::lock_guard<std::mutex> lock(g_actor_mutex);
        for (auto& a : g_actor_poses) {
            if (!a.actor || now - a.got > 300000000LL) {
                a.sh.has = a.sh.live = false;
                continue;
            }
            Unit u;
            if (!unit_of(a.actor, &u, false)) continue;
            if (frame && a.fresh) {
                a.fresh = false;
                static PoseData in;
                if (decode(a.blob, *u.skel, &in)) take(a.sh, in, now, frame_ns);
            }
            if (!a.sh.has) continue;
            if (frame) {
                const float k = a.sh.dur > 0 ? std::clamp((float)(now - a.sh.at) / 1e9f / a.sh.dur, 0.0f, 1.0f) : 1.0f;
                blend(a.sh.from, a.sh.to, k, &a.sh.cur);
            }
            apply(u, a.sh.cur);
        }
    }
    // ---- the others' Links: the pose each one's player sent
    for (int slot = 0; slot < kMaxPlayers; slot++) {
        Shown& sh = g_shown[slot];
        const uint64_t actor = puppet_actor(slot);
        Unit u;
        if (!actor || !unit_of(actor, &u)) {
            sh.has = sh.live = false;
            continue;
        }
        if (g_shown_actor[slot] != actor) sh.has = sh.live = false, g_shown_actor[slot] = actor;
        if (!frame) {
            if (sh.live) apply(u, sh.cur), carried_apply(actor, u, sh.cur.carried, slot);
            continue;
        }
        std::vector<uint8_t> blob;
        uint32_t seq = 0;
        bool fresh = false;
        if (g_mirror && !mine.empty()) blob = mine, seq = sh.seq + 1, fresh = true;
        else fresh = ses.take_pose(slot, sh.seq, &blob, &seq);
        if (fresh) {
            sh.seq = seq;
            static PoseData in;
            if (decode(blob, *u.skel, &in)) {
                if (!sh.has) {
                    sh.from = sh.to = sh.cur = in;
                    sh.dur = 0;
                    sh.has = true;
                } else {
                    const float gap = (float)(now - sh.last_got) / 1e9f;
                    if (gap > 0.004f && gap < 0.25f) sh.interval = sh.interval * 0.8f + gap * 0.2f;
                    sh.from = sh.cur;
                    sh.to = in;
                    sh.dur = std::clamp(sh.interval, 0.01f, 0.1f);
                }
                // (the way from the shown pose to the new one: a pose's interval long, begun a frame ago, so a pose that
                // comes every frame is shown at once and slower ones are eased between)
                sh.last_got = now;
                sh.at = now - frame_ns;
            } else {
                static int64_t told = 0;
                if (now - told > 10000000000LL) {
                    told = now;
                    log("pose: player %d's pose does not fit this Link's skeleton (%zu bytes)", slot, blob.size());
                }
            }
        }
        sh.live = sh.has && now - sh.last_got < 1500000000LL;   // (none for a while: the puppet's own pose)
        if (!sh.live) continue;
        const float k = sh.dur > 0 ? std::clamp((float)(now - sh.at) / 1e9f / sh.dur, 0.0f, 1.0f) : 1.0f;
        blend(sh.from, sh.to, k, &sh.cur);
        apply(u, sh.cur);
        carried_apply(actor, u, sh.cur.carried, slot);
        // (what the puppet cannot wear and the pose carries: asked of puppet.cpp, which makes stand-alone ones)
        std::vector<std::string> held;
        const uint64_t eq = equipment_of(actor);
        for (int i = 0; i < sh.to.carried.n; i++) {
            const Carried& c = sh.to.carried.c[i];
            const uint64_t a = eq ? carried_actor(eq, c.slot) : 0;
            if (c.text[0] && !(a && name_hash(a) == c.name)) held.push_back(c.text);
        }
        puppet_want_held(slot, held);
    }
}

}  // namespace coop
