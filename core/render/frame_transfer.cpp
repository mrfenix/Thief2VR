#include "frame_transfer.h"

#include "../log.h"

#include <emmintrin.h>
#include <vector>

template <typename T>
static void SafeRelease(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

bool FrameTransfer::EnsureResources(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc)
{
    if (sysmem_[0] && desc.Width == width_ && desc.Height == height_ && desc.Format == format_)
        return true;
    ReleaseAll();
    width_ = desc.Width;
    height_ = desc.Height;
    format_ = desc.Format;
    for (auto& s : sysmem_) {
        HRESULT hr = device->CreateOffscreenPlainSurface(width_, height_, format_, D3DPOOL_SYSTEMMEM, &s, nullptr);
        if (FAILED(hr)) {
            Log("FrameTransfer: CreateOffscreenPlainSurface %ux%u fmt %d failed 0x%08x", width_, height_, format_, hr);
            ReleaseAll();
            return false;
        }
    }
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        HRESULT hr = device->CreateRenderTarget(width_, height_, format_, D3DMULTISAMPLE_NONE, 0, FALSE, &resolve_,
                                                nullptr);
        if (FAILED(hr)) {
            Log("FrameTransfer: resolve target failed 0x%08x", hr);
            ReleaseAll();
            return false;
        }
    }
    // At most once a second: dragging the HUD resolution slider resizes it every frame.
    static ULONGLONG last_log;
    if (GetTickCount64() - last_log > 1000) {
        last_log = GetTickCount64();
        Log("FrameTransfer: %ux%u fmt %d msaa %d", width_, height_, format_, desc.MultiSampleType);
    }
    return true;
}

bool FrameTransfer::Capture(IDirect3DDevice9* device, IDirect3DSurface9* src)
{
    D3DSURFACE_DESC desc;
    src->GetDesc(&desc);
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8)
        return false;
    if (!EnsureResources(device, desc))
        return false;

    IDirect3DSurface9* copy_from = src;
    if (resolve_) {
        if (FAILED(device->StretchRect(src, nullptr, resolve_, nullptr, D3DTEXF_NONE)))
            return false;
        copy_from = resolve_;
    }
    if (FAILED(device->GetRenderTargetData(copy_from, sysmem_[write_])))
        return false;
    pending_[write_] = true;
    write_ ^= 1;
    return true;
}

bool FrameTransfer::Upload(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, bool swizzle_rgba,
                           bool alpha_from_color)
{
    // The slot that Capture wrote last time is the one Capture will write next;
    // upload the older one if it has data, else the newest.
    int read = write_;
    if (!pending_[read])
        read ^= 1;
    if (!pending_[read])
        return false;

    if (!staging_ || staging_rgba_ != swizzle_rgba) {
        SafeRelease(staging_);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = swizzle_rgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &staging_)))
            return false;
        staging_rgba_ = swizzle_rgba;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(sysmem_[read]->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
        return false;
    if (swizzle_rgba || alpha_from_color) {
        static std::vector<unsigned> pixels;
        pixels.resize(width_ * height_);
        for (UINT y = 0; y < height_; ++y) {
            const unsigned* s = reinterpret_cast<const unsigned*>(static_cast<const char*>(lr.pBits) + y * lr.Pitch);
            unsigned* d = pixels.data() + y * width_;
            UINT x = 0;
            if (alpha_from_color && !swizzle_rgba) {
                // BGRA: alpha = max(b, g, r), 4 pixels at a time.
                const __m128i rgb_mask = _mm_set1_epi32(0x00ffffff);
                for (; x + 4 <= width_; x += 4) {
                    __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + x));
                    __m128i m = _mm_max_epu8(p, _mm_max_epu8(_mm_srli_epi32(p, 8), _mm_srli_epi32(p, 16)));
                    __m128i out = _mm_or_si128(_mm_and_si128(p, rgb_mask), _mm_slli_epi32(m, 24));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(d + x), out);
                }
            }
            for (; x < width_; ++x) {
                unsigned p = s[x];
                unsigned r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
                unsigned a = 0xff;
                if (alpha_from_color) {
                    a = r > g ? r : g;
                    a = a > b ? a : b;
                }
                d[x] = swizzle_rgba ? (a << 24) | (b << 16) | (g << 8) | r : (a << 24) | (p & 0xffffff);
            }
        }
        ctx->UpdateSubresource(staging_, 0, nullptr, pixels.data(), width_ * 4, 0);
    } else {
        ctx->UpdateSubresource(staging_, 0, nullptr, lr.pBits, lr.Pitch, 0);
    }
    sysmem_[read]->UnlockRect();
    pending_[read] = false;

    D3D11_TEXTURE2D_DESC dd;
    dst->GetDesc(&dd);
    if (dd.Width == width_ && dd.Height == height_) {
        ctx->CopyResource(dst, staging_);
    } else {
        D3D11_BOX box{0, 0, 0, min(width_, dd.Width), min(height_, dd.Height), 1};
        ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, staging_, 0, &box);
    }
    return true;
}

void FrameTransfer::OnDeviceLost()
{
    SafeRelease(resolve_);
}

void FrameTransfer::ReleaseAll()
{
    SafeRelease(resolve_);
    for (auto& s : sysmem_)
        SafeRelease(s);
    SafeRelease(staging_);
    pending_[0] = pending_[1] = false;
    write_ = 0;
    width_ = height_ = 0;
    format_ = D3DFMT_UNKNOWN;
}
