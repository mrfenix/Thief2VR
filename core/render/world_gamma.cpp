#include "world_gamma.h"

#include "../config/settings.h"
#include "../log.h"

#include <d3dcompiler.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace world_gamma {

namespace {

// out = (in * brightness) ^ exponent, per channel.
const char kShader[] = R"(
sampler2D scene : register(s0);
float4 k : register(c0);  // x: brightness, y: exponent
float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float3 c = tex2D(scene, uv).rgb;
    return float4(pow(saturate(c * k.x), k.y), 1);
}
)";

IDirect3DPixelShader9* g_shader;
bool g_shader_failed;
IDirect3DTexture9* g_scratch;  // a copy of the eye image, sampled by the pass
UINT g_scratch_w, g_scratch_h;
IDirect3DStateBlock9* g_state;

// The game's gamma as an exponent (out = in ^ e; below 1 brightens), from its
// ramp; 0 until it sets one.
float g_ramp_exponent;
bool g_ramp_logged;

// The fallback, while the game hasn't set a ramp: "gamma" in cam.cfg (the
// option's value; higher is brighter), re-read every few seconds.
float CamCfgExponent()
{
    static float exponent = 1.0f;
    static ULONGLONG next_read;
    ULONGLONG now = GetTickCount64();
    if (now < next_read)
        return exponent;
    next_read = now + 5000;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path, n);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    FILE* f = nullptr;
    if (fopen_s(&f, (dir + "cam.cfg").c_str(), "r") != 0 || !f)
        return exponent;
    char line[256];
    float gamma = 0;
    while (fgets(line, sizeof(line), f)) {
        float value;
        if (sscanf_s(line, " gamma %f", &value) == 1 && value > 0.1f)
            gamma = value;
    }
    fclose(f);
    float e = gamma > 0 ? 1.0f / gamma : 1.0f;
    static bool logged;
    if (!logged || std::fabs(e - exponent) > 1e-3f)
        Log("Gamma: the game's gamma %.2f (from cam.cfg; the game hasn't set a gamma ramp)", gamma > 0 ? gamma : 1.0f);
    logged = true;
    exponent = e;
    return exponent;
}

bool EnsureShader(IDirect3DDevice9* dev)
{
    if (g_shader || g_shader_failed)
        return g_shader != nullptr;
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "world_gamma", nullptr, nullptr, "main", "ps_2_0", 0, 0,
                            &code, &errors);
    if (SUCCEEDED(hr))
        hr = dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &g_shader);
    if (FAILED(hr) || !g_shader) {
        Log("Gamma: shader failed (%#lx) %s", hr, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        g_shader_failed = true;
    }
    if (code)
        code->Release();
    if (errors)
        errors->Release();
    return g_shader != nullptr;
}

bool EnsureScratch(IDirect3DDevice9* dev, UINT w, UINT h)
{
    if (g_scratch && g_scratch_w == w && g_scratch_h == h)
        return true;
    if (g_scratch)
        g_scratch->Release();
    g_scratch = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_scratch,
                                  nullptr)))
        return false;
    g_scratch_w = w, g_scratch_h = h;
    return true;
}

}  // namespace

void OnGameGammaRamp(const D3DGAMMARAMP* ramp)
{
    if (!ramp)
        return;
    // The middle of the ramp: in = 128/255 gives out = v, so e = ln v / ln in.
    float v = ramp->green[128] / 65535.0f, in = 128.0f / 255.0f;
    if (v <= 0.001f || v >= 0.999f)
        return;
    float e = std::log(v) / std::log(in);
    e = e < 0.2f ? 0.2f : e > 5.0f ? 5.0f : e;
    if (!g_ramp_logged || std::fabs(e - g_ramp_exponent) > 0.01f)
        Log("Gamma: the game's gamma ramp: exponent %.2f", e);
    g_ramp_logged = true;
    g_ramp_exponent = e;
}

void Apply(IDirect3DDevice9* dev, IDirect3DSurface9* eye, UINT w, UINT h)
{
    const Settings& s = Config();
    float game = !s.follow_game_gamma ? 1.0f : g_ramp_exponent > 0 ? g_ramp_exponent : CamCfgExponent();
    float exponent = game / (s.world_gamma > 0.1f ? s.world_gamma : 1.0f);
    float brightness = s.world_brightness;
    if (std::fabs(exponent - 1.0f) < 0.01f && std::fabs(brightness - 1.0f) < 0.01f)
        return;
    if (!eye || !w || !h || !EnsureShader(dev) || !EnsureScratch(dev, w, h))
        return;
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state)))
        return;

    IDirect3DSurface9* scratch = nullptr;
    if (FAILED(g_scratch->GetSurfaceLevel(0, &scratch)))
        return;
    dev->StretchRect(eye, nullptr, scratch, nullptr, D3DTEXF_NONE);
    scratch->Release();

    g_state->Capture();
    IDirect3DSurface9 *old_color = nullptr, *old_depth = nullptr;
    dev->GetRenderTarget(0, &old_color);
    dev->GetDepthStencilSurface(&old_depth);
    dev->SetRenderTarget(0, eye);  // (also sets the viewport to all of it)
    dev->SetDepthStencilSurface(nullptr);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(g_shader);
    const float k[4] = {brightness, exponent, 0, 0};
    dev->SetPixelShaderConstantF(0, k, 1);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetTexture(0, g_scratch);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    // A full-screen quad (pixel centres: the half-pixel offset of D3D9).
    struct Vertex {
        float x, y, z, rhw, u, v;
    };
    const float x0 = -0.5f, y0 = -0.5f, x1 = w - 0.5f, y1 = h - 0.5f;
    const Vertex quad[4] = {
        {x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));

    dev->SetTexture(0, nullptr);
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
    if (g_scratch)
        g_scratch->Release();
    g_scratch = nullptr;
    g_scratch_w = g_scratch_h = 0;
    if (g_shader)
        g_shader->Release();
    g_shader = nullptr;
    g_shader_failed = false;
    if (g_state)
        g_state->Release();
    g_state = nullptr;
}

}  // namespace world_gamma
