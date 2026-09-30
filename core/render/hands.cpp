#define NOMINMAX
#include "hands.h"

#include "../config/settings.h"
#include "../log.h"
#include "game_models.h"
#include "hand_model_data.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace hands {
namespace {

// --- Lighting ----------------------------------------------------------------------
double g_light_sum;
int g_light_count;
float g_light = 0.1f;  // the arm's vertex light when a weapon was last out (about 0.1 when lit)
bool g_light_logged;

// --- Depth mapping z = A + B * rhw ------------------------------------------------
std::vector<float> g_depth_z, g_depth_rhw;
bool g_have_depth;
float g_depth_a = 0, g_depth_b = 0;

IDirect3DStateBlock9* g_state;

// Which weapon the hidden arm holds (see ArmWeapon): each frame, the arm's
// vertices are matched by texture coordinates against the points only the
// sword arm's mesh has and those only the blackjack arm's has (the engine culls
// back faces, so which triangles are there changes with the view). Each
// frame adds (blackjack points - sword points) to a running score; the score
// decides once it's clearly one way.
struct UvPoint {
    float u, v;
};
std::vector<UvPoint> g_sword_uvs, g_blackjack_uvs;
constexpr float kUvTolerance = 0.002f;
constexpr int kScoreLimit = 30, kScoreDecide = 10;
int g_sword_hits, g_blackjack_hits, g_arm_vertices;
int g_weapon_score;  // > 0 blackjack, < 0 sword
Weapon g_arm_weapon = Weapon::None;
ULONGLONG g_arm_reset_ms;  // the arm / weapon last changed
// The first frames after a draw can still show the previous arm.
constexpr ULONGLONG kSettleMs = 600;
// Which weapon each weapon object is (learned from clear frames).
int g_weapon_obj;
struct WeaponObject {
    int obj;
    Weapon kind;
};
std::vector<WeaponObject> g_weapon_objects;

Weapon KindOf(int obj)
{
    for (const WeaponObject& w : g_weapon_objects)
        if (w.obj == obj)
            return w.kind;
    return Weapon::None;
}

void Learn(int obj, Weapon kind)
{
    if (obj <= 0)
        return;
    for (WeaponObject& w : g_weapon_objects)
        if (w.obj == obj) {
            if (w.kind != kind)
                Log("Hands: weapon object %d is the %s", obj, kind == Weapon::Sword ? "sword" : "blackjack");
            w.kind = kind;
            return;
        }
    if (g_weapon_objects.size() >= 32)
        g_weapon_objects.erase(g_weapon_objects.begin());
    g_weapon_objects.push_back({obj, kind});
    Log("Hands: weapon object %d is the %s", obj, kind == Weapon::Sword ? "sword" : "blackjack");
}
int g_undecided_frames;
bool g_undecided_logged;
UvPoint g_uv_sample[6];
int g_uv_samples;

bool HasUv(const std::vector<UvPoint>& points, float u, float v, float tolerance = kUvTolerance)
{
    for (const UvPoint& p : points)
        if (std::fabs(p.u - u) < tolerance && std::fabs(p.v - v) < tolerance)
            return true;
    return false;
}

void __cdecl OnArmLight(const CapturedVertex v[3], IDirect3DBaseTexture9*, void*)
{
    for (int k = 0; k < 3; ++k) {
        ++g_arm_vertices;
        g_sword_hits += HasUv(g_sword_uvs, v[k].u, v[k].v) ? 1 : 0;
        g_blackjack_hits += HasUv(g_blackjack_uvs, v[k].u, v[k].v) ? 1 : 0;
        if (g_uv_samples < 6 && (g_arm_vertices % 37) == 0)
            g_uv_sample[g_uv_samples++] = {v[k].u, v[k].v};
    }
    for (int k = 0; k < 3; ++k) {
        if (v[k].diffuse == 0xffffffffu)
            continue;
        unsigned c = v[k].diffuse;
        g_light_sum += ((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)) / (3.0 * 255.0);
        ++g_light_count;
    }
}

void __cdecl OnDepthSample(const CapturedVertex v[3], IDirect3DBaseTexture9*, void*)
{
    for (int k = 0; k < 3 && g_depth_z.size() < 4000; ++k) {
        if (v[k].rhw > 0.0005f && v[k].rhw < 2.0f && v[k].z > 0.0f && v[k].z < 1.0f) {
            g_depth_z.push_back(v[k].z);
            g_depth_rhw.push_back(v[k].rhw);
        }
    }
}

bool FitDepth(float& a, float& b, float& rms)
{
    size_t n = g_depth_z.size();
    std::vector<bool> use(n, true);
    for (int pass = 0; pass < 2; ++pass) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, m = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!use[i])
                continue;
            double x = g_depth_rhw[i], y = g_depth_z[i];
            sx += x, sy += y, sxx += x * x, sxy += x * y, m += 1;
        }
        double den = m * sxx - sx * sx;
        if (m < 20 || std::fabs(den) < 1e-12)
            return false;
        b = (float)((m * sxy - sx * sy) / den);
        a = (float)((sy - b * sx) / m);
        double e2 = 0;
        for (size_t i = 0; i < n; ++i)
            if (use[i]) {
                double e = g_depth_z[i] - (a + b * g_depth_rhw[i]);
                e2 += e * e;
            }
        rms = (float)std::sqrt(e2 / m);
        for (size_t i = 0; i < n; ++i)
            if (std::fabs(g_depth_z[i] - (a + b * g_depth_rhw[i])) > 3 * rms + 1e-5)
                use[i] = false;
    }
    return true;
}

// --- Small affine helpers ------------------------------------------------------------
struct Affine {
    Mat3 r;
    Vec3 t;
};
Affine operator*(const Affine& a, const Affine& b)
{
    return {a.r * b.r, a.r * b.t + a.t};
}
Vec3 operator*(const Affine& a, Vec3 v)
{
    return a.r * v + a.t;
}
Affine Inverse(const Affine& a)  // rigid
{
    Mat3 rt = Transpose(a.r);
    return {rt, rt * (a.t * -1.0f)};
}
Affine FromRows(const float m[12])
{
    Affine a;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            a.r.m[r][c] = m[r * 4 + c];
        (&a.t.x)[r] = m[r * 4 + 3];
    }
    return a;
}
Mat3 AxisRotation(const float axis[3], float angle)
{
    float x = axis[0], y = axis[1], z = axis[2];
    float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    Mat3 r;
    r.m[0][0] = c + x * x * t;     r.m[0][1] = x * y * t - z * s; r.m[0][2] = x * z * t + y * s;
    r.m[1][0] = y * x * t + z * s; r.m[1][1] = c + y * y * t;     r.m[1][2] = y * z * t - x * s;
    r.m[2][0] = z * x * t - y * s; r.m[2][1] = z * y * t + x * s; r.m[2][2] = c + z * z * t;
    return r;
}
Mat3 FromColumns(Vec3 x, Vec3 y, Vec3 z)
{
    Mat3 m;
    for (int r = 0; r < 3; ++r) {
        m.m[r][0] = (&x.x)[r];
        m.m[r][1] = (&y.x)[r];
        m.m[r][2] = (&z.x)[r];
    }
    return m;
}

// --- Textures -------------------------------------------------------------------------
IDirect3DTexture9* MakeTexture(IDirect3DDevice9* dev, unsigned w, unsigned h, const unsigned* pixels)
{
    IDirect3DTexture9* t = nullptr;
    // MANAGED is turned into a lockable DYNAMIC texture on a D3D9Ex device by our hooks.
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(t->LockRect(0, &lr, nullptr, 0))) {
        for (unsigned y = 0; y < h; ++y)
            memcpy(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, pixels + y * w, w * 4);
        t->UnlockRect(0);
    }
    return t;
}

IDirect3DTexture9* g_grain;  // leather grain (hands)

// --- Hand models ------------------------------------------------------------------------
struct HandModel {
    const float (*rest)[12];
    const float (*inv_bind)[12];
    const float (*axis)[3];
    const float (*vertices)[17];
    int vertex_count;
    const unsigned short (*triangles)[3];
    int triangle_count;
};
const HandModel kModels[2] = {
    {hand_model::kLeftRest, hand_model::kLeftInvBind, hand_model::kLeftAxis, hand_model::kLeftVertices,
     hand_model::kLeftVertexCount, hand_model::kLeftTriangles, hand_model::kLeftTriangleCount},
    {hand_model::kRightRest, hand_model::kRightInvBind, hand_model::kRightAxis, hand_model::kRightVertices,
     hand_model::kRightVertexCount, hand_model::kRightTriangles, hand_model::kRightTriangleCount},
};

// The hand model's size (the WebXR hands are a small adult hand).
constexpr float kHandScale = 1.2f;

// Joint curl angles (radians) when fully curled.
const float kFingerCurl[5] = {0.0f, 1.35f, 1.7f, 1.2f, 0.0f};  // metacarpal, proximal, intermediate, distal, tip
const float kThumbCurl[4] = {0.3f, 0.55f, 0.9f, 0.0f};         // metacarpal, proximal, distal, tip

// Model space -> the controller's grip frame. OpenXR grip axes: +X out of the
// back of the right hand / the palm of the left, -Z along the handle towards
// the index finger, fingers wrapping towards -Y. The model's wrist joint: -Z to
// the fingers, +Y the back of the hand, +X the little finger (right) / thumb
// (left). Plus the Hands tab's adjustments.
Affine WristInGrip(int side)
{
    const Settings& s = Config();
    const float deg = kPi / 180.0f, cm = 0.01f;
    const float mirror = side == 0 ? -1.0f : 1.0f;
    Mat3 wrist = side == 1 ? FromColumns({0, 0, 1}, {1, 0, 0}, {0, 1, 0})
                           : FromColumns({0, 0, -1}, {-1, 0, 0}, {0, 1, 0});
    Mat3 adjust = RotX(s.hand_pitch_deg * deg) * RotY(mirror * s.hand_yaw_deg * deg) *
                  RotZ(mirror * s.hand_roll_deg * deg);
    // The wrist sits behind the knuckles (+Y) and on the back-of-hand side of the handle.
    Vec3 t = Vec3{mirror * 3.5f * cm, 6.0f * cm, 0.0f} +
             Vec3{mirror * s.hand_side_cm * cm, s.hand_up_cm * cm, -s.hand_forward_cm * cm};
    return {adjust * wrist, t};
}

struct SkinnedVertex {
    Vec3 p, n;
};

void SkinHand(int side, const Hand& h, std::vector<SkinnedVertex>& out)
{
    const HandModel& m = kModels[side];
    float curl[5] = {h.thumb, h.index, h.middle, h.ring, h.pinky};
    Affine pose[hand_model::kJoints], skin[hand_model::kJoints];
    for (int j = 0; j < hand_model::kJoints; ++j) {
        Affine rest = FromRows(m.rest[j]);
        int parent = hand_model::kParent[j];
        if (parent < 0) {
            pose[j] = rest;
        } else {
            Affine local = Inverse(FromRows(m.rest[parent])) * rest;
            int finger = j <= 4 ? 0 : 1 + (j - 5) / 5;
            int bone = j <= 4 ? j - 1 : (j - 5) % 5;
            float angle = finger == 0 ? kThumbCurl[bone] * curl[0] : kFingerCurl[bone] * curl[finger];
            local.r = local.r * AxisRotation(m.axis[j], angle);
            pose[j] = pose[parent] * local;
        }
        skin[j] = pose[j] * FromRows(m.inv_bind[j]);
    }
    // Scaled about the controller's grip point, so the fist stays around the handle.
    Affine scale{Mat3{}, Vec3{}};
    for (int i = 0; i < 3; ++i)
        scale.r.m[i][i] = kHandScale;
    Affine model_to_world = Affine{h.a, h.b} * Affine{h.grip_rot, h.grip_pos} * scale * WristInGrip(side) *
                            Inverse(FromRows(m.rest[0]));
    out.resize(m.vertex_count);
    for (int i = 0; i < m.vertex_count; ++i) {
        const float* v = m.vertices[i];
        Vec3 p{v[0], v[1], v[2]}, n{v[3], v[4], v[5]}, ps{}, ns{};
        for (int k = 0; k < 4; ++k) {
            float w = v[12 + k];
            if (w > 0) {
                const Affine& s = skin[(int)v[8 + k]];
                ps = ps + (s * p) * w;
                ns = ns + (s.r * n) * w;
            }
        }
        out[i].p = model_to_world * ps;
        out[i].n = Normalize(model_to_world.r * ns);
    }
}

struct DrawVertex {
    float x, y, z, rhw;
    D3DCOLOR color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

bool Project(const Eye& eye, const Mat3& to_eye, Vec3 w, DrawVertex& out)
{
    Vec3 d = to_eye * (w - eye.pos);
    if (d.x < 0.02f)
        return false;
    float rhw = 1.0f / d.x;
    float z = g_depth_a + g_depth_b * rhw;
    out.x = eye.cx - d.y * eye.f * rhw;
    out.y = eye.cy - d.z * eye.f * rhw;
    out.z = std::min(std::max(z, 0.0f), 0.99999f);
    out.rhw = rhw;
    return true;
}

D3DCOLOR Shade(const float base[3], float intensity)
{
    return D3DCOLOR_ARGB(255, (int)std::min(255.0f, base[0] * intensity), (int)std::min(255.0f, base[1] * intensity),
                         (int)std::min(255.0f, base[2] * intensity));
}

// A hand as a black fingerless leather glove, smooth shaded, with the grain texture.
void DrawHand(IDirect3DDevice9* dev, int side, const Hand& h, const Eye& eye, float light)
{
    static std::vector<SkinnedVertex> world;
    SkinHand(side, h, world);
    const HandModel& m = kModels[side];
    Mat3 to_eye = Transpose(eye.rot);
    Vec3 light_dir = Normalize(eye.rot * Vec3{0.3f, 0.3f, 1.0f});  // from above, a little in front
    const float leather[3] = {48, 44, 42}, skin[3] = {200, 150, 120};
    const float kGrainTiling = 3.0f;
    std::vector<DrawVertex> verts;
    verts.reserve(m.triangle_count * 3);
    for (int t = 0; t < m.triangle_count; ++t) {
        DrawVertex out[3];
        bool visible = true;
        for (int k = 0; k < 3 && visible; ++k) {
            int i = m.triangles[t][k];
            const float* v = m.vertices[i];
            visible = Project(eye, to_eye, world[i].p, out[k]);
            float diffuse = std::max(0.0f, Dot(world[i].n, light_dir));
            float intensity = (0.45f + 0.8f * diffuse) * light;
            out[k].color = Shade(v[16] > 0.5f ? skin : leather, intensity);
            out[k].u = v[6] * kGrainTiling;
            out[k].v = v[7] * kGrainTiling;
        }
        if (visible)
            verts.insert(verts.end(), out, out + 3);
    }
    if (verts.empty())
        return;
    dev->SetTexture(0, g_grain);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, g_grain ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)verts.size() / 3, verts.data(), sizeof(DrawVertex));
}

// --- Weapon models ------------------------------------------------------------------------
struct WeaponModel {
    game_models::Model model;
    std::vector<IDirect3DTexture9*> textures;
    Mat3 to_frame;   // model axes -> the arm's weapon frame (x towards the tip)
    Vec3 grip;       // the model point held in the hand (at the frame's origin)
    bool loaded = false;
};
WeaponModel g_sword, g_blackjack;
bool g_models_tried;

void LoadWeaponModels()
{
    g_models_tried = true;
    // Sword: the tip at -X, the handle from +0.91 to +1.69 (grip centre +1.3);
    // the blade is wide along Z, thin along Y.
    if (game_models::Load("sword.bin", g_sword.model)) {
        g_sword.to_frame = FromColumns({-1, 0, 0}, {0, 0, 1}, {0, 1, 0});  // columns: model X, Y, Z in the frame
        g_sword.grip = {1.3f, 0, 0};
        g_sword.loaded = true;
    }
    // Blackjack: the head at +Z, the handle from -0.98 to -0.23 (grip centre -0.6);
    // the handle is off the model's axis, so the grip is centred on it.
    if (game_models::Load("BLACJACK.BIN", g_blackjack.model)) {
        g_blackjack.to_frame = FromColumns({0, 1, 0}, {0, 0, 1}, {1, 0, 0});
        Vec3 lo{1e9f, 1e9f, 0}, hi{-1e9f, -1e9f, 0};
        for (const game_models::Triangle& t : g_blackjack.model.triangles)
            for (const Vec3& p : t.p)
                if (p.z < -0.2f) {
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), 0};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), 0};
                }
        g_blackjack.grip = {lo.x <= hi.x ? (lo.x + hi.x) * 0.5f : 0.0f, lo.y <= hi.y ? (lo.y + hi.y) * 0.5f : 0.0f, -0.6f};
        g_blackjack.loaded = true;
        Log("Hands: blackjack grip at (%.3f %.3f %.3f)", g_blackjack.grip.x, g_blackjack.grip.y, g_blackjack.grip.z);
    }

    // The first-person arms are meshes with the weapon built in (one texture
    // each): the texture coordinates each arm has that the other doesn't.
    auto points = [](const char* mesh) {
        std::vector<game_models::Uv> uvs;
        std::vector<UvPoint> out;
        if (game_models::LoadMeshUvs(mesh, uvs))
            for (const game_models::Uv& p : uvs)
                if (!HasUv(out, p.u, p.v))
                    out.push_back({p.u, p.v});
        return out;
    };
    std::vector<UvPoint> sword = points("ARMSW2.BIN"), blackjack = points("BJACHAND.BIN");
    for (const UvPoint& p : sword)
        if (!HasUv(blackjack, p.u, p.v, 3 * kUvTolerance))
            g_sword_uvs.push_back(p);
    for (const UvPoint& p : blackjack)
        if (!HasUv(sword, p.u, p.v, 3 * kUvTolerance))
            g_blackjack_uvs.push_back(p);
    Log("Hands: weapon texture points: sword %d, blackjack %d", (int)g_sword_uvs.size(), (int)g_blackjack_uvs.size());
}

void EnsureWeaponTextures(IDirect3DDevice9* dev, WeaponModel& w)
{
    if (!w.loaded || !w.textures.empty())
        return;
    for (const game_models::Texture& t : w.model.textures)
        w.textures.push_back(MakeTexture(dev, t.w, t.h, t.pixels.data()));
}

void DrawWeapon(IDirect3DDevice9* dev, WeaponModel& w, const WeaponPose& pose, const Eye& eye, float light)
{
    EnsureWeaponTextures(dev, w);
    Affine model_to_world = Affine{pose.r, pose.p} * Affine{w.to_frame, w.to_frame * (w.grip * -1.0f)};
    Mat3 to_eye = Transpose(eye.rot);
    DWORD level = (DWORD)(std::min(1.0f, light) * 255.0f);
    D3DCOLOR c = D3DCOLOR_ARGB(255, level, level, level);
    std::vector<std::vector<DrawVertex>> by_texture(w.textures.size());
    for (const game_models::Triangle& t : w.model.triangles) {
        DrawVertex out[3];
        bool visible = true;
        for (int k = 0; k < 3 && visible; ++k) {
            visible = Project(eye, to_eye, model_to_world * t.p[k], out[k]);
            out[k].color = c;
            out[k].u = t.uv[k][0];
            out[k].v = t.uv[k][1];
        }
        if (visible && t.material < (int)by_texture.size())
            by_texture[t.material].insert(by_texture[t.material].end(), out, out + 3);
    }
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
    for (size_t i = 0; i < by_texture.size(); ++i) {
        if (by_texture[i].empty())
            continue;
        dev->SetTexture(0, w.textures[i]);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)by_texture[i].size() / 3, by_texture[i].data(),
                             sizeof(DrawVertex));
    }
}

void ReleaseTextures()
{
    for (WeaponModel* w : {&g_sword, &g_blackjack}) {
        for (IDirect3DTexture9* t : w->textures)
            if (t)
                t->Release();
        w->textures.clear();
    }
    if (g_grain)
        g_grain->Release();
    g_grain = nullptr;
}

}  // namespace

DrawCapture ArmLightCapture()
{
    // Last frame's arm.
    // Only one arm's points in a frame decide it; mixed frames add to the
    // score. A known weapon object decides until then.
    if (g_arm_vertices > 0) {
        Weapon clear = Weapon::None;
        if (GetTickCount64() - g_arm_reset_ms >= kSettleMs) {
            if (g_sword_hits >= 3 && g_blackjack_hits == 0)
                clear = Weapon::Sword;
            else if (g_blackjack_hits >= 3 && g_sword_hits == 0)
                clear = Weapon::Blackjack;
            g_weapon_score = clear == Weapon::Sword       ? -kScoreLimit
                             : clear == Weapon::Blackjack ? kScoreLimit
                                                          : std::max(-kScoreLimit, std::min(kScoreLimit, g_weapon_score + g_blackjack_hits - g_sword_hits));
        }
        if (clear != Weapon::None)
            Learn(g_weapon_obj, clear);
        Weapon known = KindOf(g_weapon_obj);
        Weapon now = clear != Weapon::None             ? clear
                     : known != Weapon::None           ? known
                     : g_weapon_score >= kScoreDecide  ? Weapon::Blackjack
                     : g_weapon_score <= -kScoreDecide ? Weapon::Sword
                                                       : g_arm_weapon;
        if (now != g_arm_weapon) {
            Log("Hands: the arm holds the %s (texture points: sword %d, blackjack %d of %d vertices, score %d)",
                now == Weapon::Sword ? "sword" : "blackjack", g_sword_hits, g_blackjack_hits, g_arm_vertices,
                g_weapon_score);
            g_arm_weapon = now;
        }
        if (g_arm_weapon == Weapon::None && ++g_undecided_frames == 120 && !g_undecided_logged) {
            g_undecided_logged = true;
            Log("Hands: can't tell the weapon (texture points: sword %d, blackjack %d of %d vertices; uv sample "
                "%.3f,%.3f %.3f,%.3f %.3f,%.3f %.3f,%.3f)",
                g_sword_hits, g_blackjack_hits, g_arm_vertices, g_uv_sample[0].u, g_uv_sample[0].v,
                g_uv_sample[1].u, g_uv_sample[1].v, g_uv_sample[2].u, g_uv_sample[2].v, g_uv_sample[3].u,
                g_uv_sample[3].v);
        }
    }
    g_sword_hits = g_blackjack_hits = g_arm_vertices = 0;
    g_uv_samples = 0;
    if (g_light_count > 0) {
        float l = (float)(g_light_sum / g_light_count);
        g_light = g_light * 0.8f + l * 0.2f;
        if (!g_light_logged) {
            g_light_logged = true;
            Log("Hands: arm light %.2f", l);
        }
    }
    g_light_sum = 0;
    g_light_count = 0;
    DrawCapture c;
    c.callback = &OnArmLight;
    c.skip_draw = true;
    return c;
}

bool NeedDepthSample()
{
    return !g_have_depth;
}

DrawCapture DepthSampleCapture()
{
    DrawCapture c;
    c.callback = &OnDepthSample;
    return c;
}

void EndDepthSample()
{
    if (g_have_depth || g_depth_z.size() < 200)
        return;
    float a, b, rms;
    if (FitDepth(a, b, rms) && b != 0) {
        g_depth_a = a;
        g_depth_b = b;
        g_have_depth = true;
        Log("Hands: engine depth z = %.6f + %.6f * rhw (rms %.6f, %d samples, near %.3f ft)", a, b, rms,
            (int)g_depth_z.size(), a != 0 ? -b / a : 0.0f);
    }
    g_depth_z.clear();
    g_depth_rhw.clear();
}

bool Ready()
{
    return g_have_depth;
}

Weapon ArmWeapon()
{
    return g_arm_weapon;
}

void ResetArmWeapon()
{
    g_arm_weapon = KindOf(g_weapon_obj);
    g_weapon_score = 0;
    g_arm_reset_ms = GetTickCount64();
    g_undecided_frames = 0;
}

void SetWeaponObject(int obj)
{
    if (obj != g_weapon_obj) {
        g_weapon_obj = obj;
        g_arm_weapon = KindOf(obj);
        g_weapon_score = 0;
        g_arm_reset_ms = GetTickCount64();
    }
}

void WeaponFrame(const Hand& right, float world_scale, Mat3& r, Vec3& p)
{
    // Grip axes: -Z along the handle (index finger end), +Y towards the back of
    // the fist, +X out of the back of the right hand. Frame: x towards the tip
    // (out of the index end of the fist), y the blade's width, z its thickness.
    Mat3 in_grip = FromColumns({0, 0, -1}, {0, 1, 0}, {1, 0, 0});
    Mat3 rot = right.a;
    float inv = world_scale > 0 ? 1.0f / world_scale : 1.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot.m[i][j] *= inv;  // the tracking -> world mapping without its scale
    r = rot * right.grip_rot * in_grip;
    p = right.b + right.a * right.grip_pos;
}

bool WeaponModelsLoaded()
{
    return g_sword.loaded || g_blackjack.loaded;
}

void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* color, IDirect3DSurface9* depth, const Eye& eye,
          const Hand& left, const Hand& right, const WeaponPose& weapon, float brightness)
{
    if (!g_models_tried)
        LoadWeaponModels();
    if (!Ready() || (!left.visible && !right.visible && weapon.kind == Weapon::None))
        return;
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state)))
        return;
    if (!g_grain) {
        std::vector<unsigned> px(hand_model::kGrainSize * hand_model::kGrainSize);
        for (size_t i = 0; i < px.size(); ++i) {
            unsigned g = hand_model::kGrain[i];
            px[i] = 0xff000000 | g << 16 | g << 8 | g;
        }
        g_grain = MakeTexture(dev, hand_model::kGrainSize, hand_model::kGrainSize, px.data());
    }
    g_state->Capture();
    IDirect3DSurface9 *old_color = nullptr, *old_depth = nullptr;
    dev->GetRenderTarget(0, &old_color);
    dev->GetDepthStencilSurface(&old_depth);
    dev->SetRenderTarget(0, color);
    dev->SetDepthStencilSurface(depth);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHAREF, 0x40);
    dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    // The engine's arm vertex light is about 0.1 in a lit room (it scales it up
    // when drawing); about 0.25 counts as fully lit here.
    float light = std::min(1.5f, std::max(0.1f, g_light / 0.25f * brightness));
    if (weapon.kind == Weapon::Sword && g_sword.loaded)
        DrawWeapon(dev, g_sword, weapon, eye, light);
    else if (weapon.kind == Weapon::Blackjack && g_blackjack.loaded)
        DrawWeapon(dev, g_blackjack, weapon, eye, light);
    if (left.visible)
        DrawHand(dev, 0, left, eye, light);
    if (right.visible)
        DrawHand(dev, 1, right, eye, light);

    dev->SetRenderTarget(0, old_color);
    dev->SetDepthStencilSurface(old_depth);
    if (old_color)
        old_color->Release();
    if (old_depth)
        old_depth->Release();
    g_state->Apply();
}

void OnDeviceReset()
{
    ReleaseTextures();
    if (g_state)
        g_state->Release();
    g_state = nullptr;
}

}  // namespace hands
