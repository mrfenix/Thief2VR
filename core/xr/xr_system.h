#pragma once

#define XR_USE_GRAPHICS_API_D3D11
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

// One OpenXR swapchain whose images are D3D11 textures.
class XrSwapchainD3D11 {
public:
    bool Create(XrSession session, int64_t format, uint32_t width, uint32_t height, uint32_t array_size = 1);
    void Destroy();
    bool IsValid() const { return handle_ != XR_NULL_HANDLE; }

    // Acquire + wait; returns the image to render into, or nullptr on failure.
    ID3D11Texture2D* Acquire();
    void Release();

    XrSwapchain handle() const { return handle_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    XrSwapchain handle_ = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

struct XrFrame {
    bool should_render = false;
    XrTime display_time = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};  // in local space
};

// Owns the OpenXR instance/session and the D3D11 device frames are submitted with.
// All calls must come from the game's render thread.
class XrSystem {
public:
    bool Init();
    bool IsInitialized() const { return session_ != XR_NULL_HANDLE; }

    // Polls events, then xrWaitFrame + xrBeginFrame. Returns false when no frame
    // was begun (session not running); EndFrame must be called only after true.
    bool BeginFrame(XrFrame& frame);
    void EndFrame(const XrFrame& frame, const XrCompositionLayerBaseHeader* const* layers, uint32_t count);

    XrInstance instance() const { return instance_; }
    ID3D11Device* device() const { return device_; }
    ID3D11DeviceContext* context() const { return context_; }
    XrSession session() const { return session_; }
    XrSpace local_space() const { return local_space_; }
    XrSpace view_space() const { return view_space_; }
    int64_t color_format() const { return color_format_; }
    // True when color_format is RGBA rather than BGRA (the game's buffers are BGRA).
    bool color_format_is_rgba() const { return color_format_rgba_; }
    const XrViewConfigurationView& view_config(int eye) const { return view_configs_[eye]; }

    // Display refresh rates (XR_FB_display_refresh_rate). Empty when the runtime
    // doesn't offer the extension (then the rate is set in the streaming app).
    const std::vector<float>& refresh_rates() const { return refresh_rates_; }
    float current_refresh_rate() const;
    bool RequestRefreshRate(float hz);  // 0 = the runtime's default

private:
    bool CreateD3D11Device();
    void PollEvents();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace local_space_ = XR_NULL_HANDLE;
    XrSpace view_space_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    int64_t color_format_ = 0;
    bool color_format_rgba_ = false;
    XrViewConfigurationView view_configs_[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    bool has_refresh_rate_ = false;
    std::vector<float> refresh_rates_;
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_refresh_rate_ = nullptr;
};

XrSystem& Xr();

// Logs a failed XrResult with context; returns true when the call succeeded.
bool XrCheck(XrResult result, const char* what);
