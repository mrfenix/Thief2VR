#define NOMINMAX
#include "hands.h"

#include "../config/settings.h"
#include "../log.h"
#include "game_models.h"
#include "hand_model_data.h"
#include "hand_resources.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
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

// A texture with its mip levels (box filtered), so it doesn't shimmer small.
IDirect3DTexture9* MakeMippedTexture(IDirect3DDevice9* dev, unsigned w, unsigned h, const unsigned* pixels)
{
    UINT levels = 1;
    for (unsigned s = std::max(w, h); s > 1; s >>= 1)
        ++levels;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)))
        return MakeTexture(dev, w, h, pixels);
    std::vector<unsigned> level(pixels, pixels + (size_t)w * h), next;
    for (UINT l = 0; l < levels; ++l) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0))) {
            for (unsigned y = 0; y < h; ++y)
                memcpy(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, level.data() + (size_t)y * w, w * 4);
            t->UnlockRect(l);
        }
        unsigned nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        next.assign((size_t)nw * nh, 0);
        for (unsigned y = 0; y < nh; ++y)
            for (unsigned x = 0; x < nw; ++x) {
                unsigned sum[4] = {};
                for (unsigned k = 0; k < 4; ++k) {
                    unsigned sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                    unsigned p = level[(size_t)sy * w + sx];
                    for (int c = 0; c < 4; ++c)
                        sum[c] += (p >> (c * 8)) & 255;
                }
                unsigned out = 0;
                for (int c = 0; c < 4; ++c)
                    out |= ((sum[c] + 2) / 4) << (c * 8);
                next[(size_t)y * nw + x] = out;
            }
        level.swap(next);
        w = nw;
        h = nh;
    }
    return t;
}

// The hands' textures (skin, and the glove as black leather), from the DLL's resources.
IDirect3DTexture9* g_hand_texture[2];
bool g_hand_texture_tried[2];

IDirect3DTexture9* HandTexture(IDirect3DDevice9* dev, int side)
{
    if (g_hand_texture[side] || g_hand_texture_tried[side])
        return g_hand_texture[side];
    g_hand_texture_tried[side] = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HandTexture), &self);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(side == 0 ? IDR_HAND_LEFT : IDR_HAND_RIGHT), MAKEINTRESOURCEW(10));
    HGLOBAL data = res ? LoadResource(self, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    game_models::Texture image;
    if (!bytes || !game_models::DecodeImage(bytes, SizeofResource(self, res), image)) {
        Log("Hands: the %s hand's texture couldn't be loaded", side == 0 ? "left" : "right");
        return nullptr;
    }
    g_hand_texture[side] = MakeMippedTexture(dev, image.w, image.h, image.pixels.data());
    return g_hand_texture[side];
}

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

// A hand as a black fingerless leather glove, smooth shaded, with its texture
// (the glove and the bare fingers are in it).
void DrawHand(IDirect3DDevice9* dev, int side, const Hand& h, const Eye& eye, float light)
{
    static std::vector<SkinnedVertex> world;
    SkinHand(side, h, world);
    const HandModel& m = kModels[side];
    Mat3 to_eye = Transpose(eye.rot);
    Vec3 light_dir = Normalize(eye.rot * Vec3{0.3f, 0.3f, 1.0f});  // from above, a little in front
    IDirect3DTexture9* texture = HandTexture(dev, side);
    // Without the texture: plain colours (leather, and skin for the bare fingers).
    const float leather[3] = {48, 44, 42}, skin[3] = {200, 150, 120}, white[3] = {255, 255, 255};
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
            out[k].color = Shade(texture ? white : v[16] > 0.5f ? skin : leather, intensity);
            out[k].u = v[6];
            out[k].v = v[7];
        }
        if (visible)
            verts.insert(verts.end(), out, out + 3);
    }
    if (verts.empty())
        return;
    dev->SetTexture(0, texture);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
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

// A game model, each vertex placed by to_world(model point).
template <class ToWorld>
void DrawModel(IDirect3DDevice9* dev, WeaponModel& w, const Eye& eye, float light, ToWorld to_world)
{
    EnsureWeaponTextures(dev, w);
    Mat3 to_eye = Transpose(eye.rot);
    DWORD level = (DWORD)(std::min(1.0f, light) * 255.0f);
    D3DCOLOR c = D3DCOLOR_ARGB(255, level, level, level);
    std::vector<std::vector<DrawVertex>> by_texture(w.textures.size());
    for (const game_models::Triangle& t : w.model.triangles) {
        DrawVertex out[3];
        bool visible = true;
        for (int k = 0; k < 3 && visible; ++k) {
            visible = Project(eye, to_eye, to_world(t.p[k]), out[k]);
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

void DrawWeapon(IDirect3DDevice9* dev, WeaponModel& w, const WeaponPose& pose, const Eye& eye, float light)
{
    Affine model_to_world = Affine{pose.r, pose.p} * Affine{w.to_frame, w.to_frame * (w.grip * -1.0f)};
    DrawModel(dev, w, eye, light, [&](Vec3 p) { return model_to_world * p; });
}

// --- The bow ------------------------------------------------------------------------------
// Models held by name (the bow, the arrows), loaded from the game files on first use.
struct HeldModel {
    std::string file;
    WeaponModel w;
};
std::vector<std::unique_ptr<HeldModel>> g_held;

WeaponModel* Held(const char* file)
{
    for (const auto& h : g_held)
        if (_stricmp(h->file.c_str(), file) == 0)
            return h->w.loaded ? &h->w : nullptr;
    auto h = std::make_unique<HeldModel>();
    h->file = file;
    h->w.loaded = game_models::Load(file, h->w.model);
    if (h->w.loaded)
        Log("Hands: %s loaded (%d triangles)", file, (int)h->w.model.triangles.size());
    WeaponModel* w = h->w.loaded ? &h->w : nullptr;
    g_held.push_back(std::move(h));
    return w;
}

// bow2.bin: up the bow along model Z (tips at +-1.89 ft), the handle around
// Z = 0 on the -Y side; the limbs sweep towards +Y (the target) and the tips
// back to -Y (the archer, where the string runs). Bow frame (x towards the
// target, y left, z up) = model (Y, -X, Z) about the handle's centre.
struct BowShape {
    bool ready = false;
    Mat3 to_frame;
    Vec3 handle;     // model
    float height;    // handle to tip (ft)
    float tip_x;     // the tips' frame x (unbent)
};
BowShape g_bow_shape;
const char* const kBowModel = "bow2.bin";
// The string at rest is kBraceBend behind the unbent tips (the limbs bend back
// that far when strung); drawn fully, the limbs bend a further kDrawBend and
// the tips come in by kDrawInward. The arrow lies kRestUp above the handle's
// centre, its nock at most kMaxDraw behind that.
constexpr float kBraceBend = 0.45f, kDrawBend = 0.55f, kDrawInward = 0.15f, kLimbStart = 0.25f;
constexpr float kRestUp = 0.13f, kMaxDraw = 2.3f;
constexpr float kArrowSide = 0.1f;  // the arrow passes beside the bow (the limbs are ~0.08 ft either side)
constexpr float kPinchAhead = 0.12f;  // the drawing fingers, ahead of the hand's grip point

const BowShape* Bow()
{
    if (g_bow_shape.ready)
        return &g_bow_shape;
    WeaponModel* w = Held(kBowModel);
    if (!w)
        return nullptr;
    BowShape& s = g_bow_shape;
    s.to_frame = FromColumns({0, -1, 0}, {1, 0, 0}, {0, 0, 1});  // columns: model X, Y, Z in the frame
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    float top = 0;
    for (const game_models::Triangle& t : w->model.triangles)
        for (const Vec3& p : t.p) {
            top = std::max(top, std::fabs(p.z));
            if (std::fabs(p.z) < 0.25f) {
                lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
            }
        }
    s.handle = lo.x <= hi.x ? (lo + hi) * 0.5f : Vec3{};
    s.height = top - std::fabs(s.handle.z);
    float sum = 0;
    int n = 0;
    for (const game_models::Triangle& t : w->model.triangles)
        for (const Vec3& p : t.p)
            if (std::fabs(p.z) > top - 0.06f) {
                sum += (s.to_frame * (p - s.handle)).x;
                ++n;
            }
    s.tip_x = n ? sum / n : 0.0f;
    s.ready = true;
    Log("Hands: bow handle (%.2f %.2f %.2f), handle to tip %.2f ft, tips %.2f ft behind the handle", s.handle.x,
        s.handle.y, s.handle.z, s.height, -s.tip_x);
    return &s;
}

// A bow point (frame) with the limbs bent for the draw.
Vec3 Bend(Vec3 f, float draw, const BowShape& s)
{
    float h = std::fabs(f.z);
    if (h <= kLimbStart)
        return f;
    float t = (h - kLimbStart) / std::max(0.1f, s.height - kLimbStart);
    t *= t;
    f.x -= (kBraceBend + draw * kDrawBend) * t;
    f.z -= (f.z > 0 ? 1.0f : -1.0f) * draw * kDrawInward * t;
    return f;
}

Vec3 BowTip(const BowShape& s, float draw, float sign)
{
    return {s.tip_x - kBraceBend - draw * kDrawBend, 0.0f, sign * (s.height - draw * kDrawInward)};
}

// A thin strip facing the eye from a to b (the string).
void DrawStrip(IDirect3DDevice9* dev, const Eye& eye, Vec3 a, Vec3 b, float width, D3DCOLOR c)
{
    Vec3 side = Cross(b - a, eye.pos - (a + b) * 0.5f);
    float len = Length(side);
    if (len < 1e-6f)
        return;
    side = side * (width * 0.5f / len);
    Mat3 to_eye = Transpose(eye.rot);
    Vec3 corner[4] = {a - side, a + side, b + side, b - side};
    DrawVertex v[4];
    for (int k = 0; k < 4; ++k) {
        if (!Project(eye, to_eye, corner[k], v[k]))
            return;
        v[k].color = c;
        v[k].u = v[k].v = 0;
    }
    DrawVertex tris[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tris, sizeof(DrawVertex));
}

void DrawBow(IDirect3DDevice9* dev, const BowPose& b, const Eye& eye, float light)
{
    const BowShape* s = Bow();
    WeaponModel* w = Held(kBowModel);
    if (!s || !w)
        return;
    DrawModel(dev, *w, eye, light,
              [&](Vec3 p) { return b.grip + b.r * Bend(s->to_frame * (p - s->handle), b.draw, *s); });
    // The string: tip, nock, tip.
    float shade = std::min(1.0f, light) * 70.0f;
    D3DCOLOR string_color = D3DCOLOR_ARGB(255, (int)shade, (int)(shade * 0.9f), (int)(shade * 0.75f));
    const float kStringWidth = 0.008f;
    Vec3 top = b.grip + b.r * BowTip(*s, b.draw, 1.0f), bottom = b.grip + b.r * BowTip(*s, b.draw, -1.0f);
    DrawStrip(dev, eye, top, b.nock, kStringWidth, string_color);
    DrawStrip(dev, eye, b.nock, bottom, kStringWidth, string_color);
    // The nocked arrow: its nock (the model's -X end) on the string, along the
    // bow; or still in the drawing hand.
    if (b.arrow) {
        if (WeaponModel* a = Held(b.arrow)) {
            const Mat3& r = b.arrow_in_hand ? b.hand_r : b.r;
            Vec3 nock = b.arrow_in_hand ? b.hand_nock : b.nock;
            Vec3 x = r * Vec3{1, 0, 0}, y = r * Vec3{0, 1, 0}, z = r * Vec3{0, 0, 1};
            float nock_x = a->model.bbox_min.x;
            DrawModel(dev, *a, eye, light, [&](Vec3 p) { return nock + x * (p.x - nock_x) + y * p.y + z * p.z; });
        }
    }
}

void ReleaseTextures()
{
    std::vector<WeaponModel*> models{&g_sword, &g_blackjack};
    for (const auto& h : g_held)
        models.push_back(&h->w);
    for (WeaponModel* w : models) {
        for (IDirect3DTexture9* t : w->textures)
            if (t)
                t->Release();
        w->textures.clear();
    }
    for (int side = 0; side < 2; ++side) {
        if (g_hand_texture[side])
            g_hand_texture[side]->Release();
        g_hand_texture[side] = nullptr;
        g_hand_texture_tried[side] = false;
    }
}

}  // namespace

DrawCapture ArmLightCapture()
{
    // Last frame's arm.
    // Only one arm's points in a frame decide it; mixed frames add to the
    // score. A known weapon object decides until then. (Only with a sword /
    // blackjack out: the bow's arm is captured too.)
    if (g_arm_vertices > 0 && g_weapon_obj) {
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
        // A weak clear frame (a few points) doesn't overturn a known weapon object.
        Weapon known = KindOf(g_weapon_obj);
        if (clear != Weapon::None && known != Weapon::None && clear != known &&
            std::max(g_sword_hits, g_blackjack_hits) < 8)
            clear = Weapon::None;
        if (clear != Weapon::None)
            Learn(g_weapon_obj, clear);
        known = KindOf(g_weapon_obj);
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
        if (g_weapon_obj && g_arm_weapon == Weapon::None && ++g_undecided_frames == 120 && !g_undecided_logged) {
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

bool MakeBow(const BowInput& in, BowPose& out, Vec3& shot_centre, Vec3& shot_dir)
{
    const BowShape* s = Bow();
    if (!s)
        return false;
    Mat3 r = in.r;
    Vec3 forward = r * Vec3{1, 0, 0};
    const Vec3 rest_in_bow{0, in.left_hand ? kArrowSide : -kArrowSide, kRestUp};
    Vec3 rest = in.grip + r * rest_in_bow;
    const float brace = kBraceBend - s->tip_x;
    float draw = 0, pulled_to = brace;
    if (in.pulled) {
        // Drawing: the bow turns in the hand to lie along the arrow, from the
        // drawing fingers to the rest.
        Vec3 fingers = in.string_hand;
        Vec3 line = BowArrowLine(forward, rest, fingers);
        r = RotationBetween(forward, line) * r;
        forward = line;
        rest = in.grip + r * rest_in_bow;
        pulled_to = std::max(brace, std::min(kMaxDraw, Dot(rest - fingers, forward) - kPinchAhead));
        draw = (pulled_to - brace) / (kMaxDraw - brace);
    } else if (in.auto_draw >= 0) {
        draw = std::min(1.0f, in.auto_draw);
        pulled_to = brace + draw * (kMaxDraw - brace);
    }
    out.visible = true;
    out.r = r;
    out.grip = in.grip;
    out.nock = rest - forward * pulled_to;
    out.draw = draw;
    out.arrow = in.arrow;
    // The arrow's centre: its model origin, this far from the nock.
    float nock_to_centre = 1.63f;
    if (in.arrow)
        if (WeaponModel* a = Held(in.arrow))
            nock_to_centre = -a->model.bbox_min.x;
    shot_centre = out.nock + forward * nock_to_centre;
    shot_dir = forward;
    return true;
}

Mat3 BowFrameInHand(const Hand& hand, float world_scale)
{
    // Grip axes: -Z out of the index-finger end of the fist, -Y towards the
    // fingers, +X out of the back of the right hand / the palm of the left.
    // Bow: x (towards the target) = -Y, y = -X, z (up the bow) = -Z.
    Mat3 in_grip = FromColumns({0, -1, 0}, {-1, 0, 0}, {0, 0, -1});
    Mat3 rot = hand.a;
    float inv = world_scale > 0 ? 1.0f / world_scale : 1.0f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            rot.m[i][j] *= inv;
    return rot * hand.grip_rot * in_grip;
}

bool WeaponModelsLoaded()
{
    return g_sword.loaded || g_blackjack.loaded;
}

void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* color, IDirect3DSurface9* depth, const Eye& eye,
          const Hand& left, const Hand& right, const WeaponPose& weapon, const BowPose& bow, float brightness)
{
    if (!g_models_tried)
        LoadWeaponModels();
    if (!Ready() || (!left.visible && !right.visible && weapon.kind == Weapon::None && !bow.visible))
        return;
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state)))
        return;
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
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    // The engine's arm vertex light is about 0.1 in a lit room (it scales it up
    // when drawing); about 0.25 counts as fully lit here.
    float light = std::min(1.5f, std::max(0.1f, g_light / 0.25f * brightness));
    if (weapon.kind == Weapon::Sword && g_sword.loaded)
        DrawWeapon(dev, g_sword, weapon, eye, light);
    else if (weapon.kind == Weapon::Blackjack && g_blackjack.loaded)
        DrawWeapon(dev, g_blackjack, weapon, eye, light);
    if (bow.visible)
        DrawBow(dev, bow, eye, light);
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
