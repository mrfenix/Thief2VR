#pragma once

#include <d3d9.h>
#include <d3d11.h>

// Moves a D3D9 render target into a D3D11 texture through system memory
// (frame path v1). Capture() queues the GPU->CPU copy; Upload() pushes the
// previous capture to D3D11. Call Upload() before Capture() each frame: the
// image is then one frame behind but the CPU doesn't wait on the copy.
// A piece cut out of an uploaded image (e.g. a HUD element moved onto a wrist):
// the box of what's visible (alpha) within the zone is copied into a cell of a
// second texture, and the zone is cleared in the image itself.
struct TransferCut {
    int zone[4] = {};        // x0 y0 x1 y1 in the image (in)
    int clear[4] = {};       // x0 y0 x1 y1 cleared in the image (in; all 0 = the zone)
    int cell[4] = {};        // x y w h in the cut texture (in); the box is clipped to w x h
    bool found = false;      // something visible was there (out)
    int box[4] = {};         // x0 y0 x1 y1 of it in the image (out)
};

class FrameTransfer {
public:
    // Queue a copy of src (any 32-bit BGRA render target). Returns false on error.
    bool Capture(IDirect3DDevice9* device, IDirect3DSurface9* src);
    // Copy the previous capture into dst (a swapchain image of the same size).
    // swizzle_rgba: dst is RGBA rather than BGRA. alpha_from_color: write
    // alpha = max(r,g,b) so black becomes transparent (premultiplied), used for
    // the HUD, which the game draws over a black clear. Returns false if nothing
    // to upload.
    // With cuts (alpha_from_color only): each cut's box goes to cut_dst first,
    // then its zone is cleared in dst.
    bool Upload(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, bool swizzle_rgba,
                bool alpha_from_color = false, TransferCut* cuts = nullptr, int cut_count = 0,
                ID3D11Texture2D* cut_dst = nullptr);

    // Diagnostics: the next upload with alpha_from_color also writes its image
    // (with the alpha) to a 32-bit BMP at path.
    void RequestDump(const char* path) { dump_path_ = path; }

    // Release D3D9 DEFAULT-pool resources (call before IDirect3DDevice9::Reset).
    void OnDeviceLost();
    void ReleaseAll();

    UINT width() const { return width_; }
    UINT height() const { return height_; }

private:
    bool EnsureResources(IDirect3DDevice9* device, const D3DSURFACE_DESC& desc);

    IDirect3DSurface9* resolve_ = nullptr;     // non-MSAA copy when the source is multisampled
    IDirect3DSurface9* sysmem_[2] = {};        // readback ring
    int write_ = 0;                            // ring slot the next Capture writes
    bool pending_[2] = {};                     // slot holds an un-uploaded capture
    ID3D11Texture2D* staging_ = nullptr;       // UNORM texture in the swapchain's format family
    bool staging_rgba_ = false;
    UINT width_ = 0, height_ = 0;
    D3DFORMAT format_ = D3DFMT_UNKNOWN;
    const char* dump_path_ = nullptr;
};
