#include "xr_system.h"

#include "../log.h"

#include <dxgi.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

XrSystem& Xr()
{
    static XrSystem system;
    return system;
}

namespace {

// Names for the errors that can come before there's an instance to ask
// (xrResultToString needs one): those of finding and starting the runtime.
const char* EarlyResultName(XrResult result)
{
    switch (result) {
    case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_FILE_ACCESS_ERROR: return "XR_ERROR_FILE_ACCESS_ERROR";
    case XR_ERROR_FILE_CONTENTS_INVALID: return "XR_ERROR_FILE_CONTENTS_INVALID";
    case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE: return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    default: return "?";
    }
}

bool FileExists(const std::string& path)
{
    DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// A string value from the runtime manifest (JSON), by key, unescaped. Enough
// for the flat "name" / "library_path" fields; "" if not there.
std::string ManifestString(const std::string& json, const char* key)
{
    size_t at = json.find(std::string("\"") + key + "\"");
    if (at == std::string::npos)
        return "";
    at = json.find(':', at);
    at = at == std::string::npos ? at : json.find('"', at);
    if (at == std::string::npos)
        return "";
    std::string value;
    for (size_t i = at + 1; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size())
            ++i;
        value += json[i];
    }
    return value;
}

// Which OpenXR runtime the loader will use. This is a 32-bit process, so it's
// the 32-bit setting (HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1, which 64-bit
// apps don't use), unless XR_RUNTIME_JSON overrides it. Logged with whether its
// manifest and library exist, so a failure to start can be told apart.
void LogRuntime()
{
    std::string manifest, source;
    char env[MAX_PATH * 2];
    DWORD env_len = GetEnvironmentVariableA("XR_RUNTIME_JSON", env, sizeof(env));
    if (env_len > 0 && env_len < sizeof(env)) {
        manifest = env;
        source = "XR_RUNTIME_JSON";
    } else {
        char value[MAX_PATH * 2];
        DWORD size = sizeof(value);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                         RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, value, &size) == ERROR_SUCCESS) {
            manifest = value;
            source = "32-bit setting (WOW6432Node ActiveRuntime)";
        }
    }
    if (manifest.empty()) {
        Log("OpenXR: no 32-bit runtime is set (HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1 ActiveRuntime)");
        return;
    }
    bool exists = FileExists(manifest);
    Log("OpenXR: runtime from the %s: %s%s", source.c_str(), manifest.c_str(), exists ? "" : " (MISSING)");
    if (!exists)
        return;
    std::ifstream file(manifest, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    std::string name = ManifestString(text.str(), "name"), library = ManifestString(text.str(), "library_path");
    if (!library.empty() && library.find(':') == std::string::npos && library.rfind("\\\\", 0) != 0) {
        size_t slash = manifest.find_last_of("\\/");
        library = (slash == std::string::npos ? "" : manifest.substr(0, slash + 1)) + library;
    }
    Log("OpenXR: runtime \"%s\", library %s%s", name.empty() ? "?" : name.c_str(),
        library.empty() ? "?" : library.c_str(), library.empty() || FileExists(library) ? "" : " (MISSING)");
}

// What to check when the runtime can't be started.
void LogRuntimeHint()
{
    Log("OpenXR: Thief 2 is a 32-bit game, so it uses the 32-bit runtime setting, not the one 64-bit apps use. "
        "Check that it points to your headset's 32-bit runtime (e.g. SteamVR: steamxr_win32.json; Virtual "
        "Desktop: virtualdesktop-openxr-32.json), and that the runtime (e.g. SteamVR) is running before the game.");
}

}  // namespace

bool XrCheck(XrResult result, const char* what)
{
    if (XR_SUCCEEDED(result))
        return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (Xr().instance() != XR_NULL_HANDLE)
        xrResultToString(Xr().instance(), result, name);
    else
        strcpy_s(name, EarlyResultName(result));
    Log("OpenXR: %s failed: %s (%d)", what, name, result);
    return false;
}

// ---------------------------------------------------------------------------
// XrSwapchainD3D11

bool XrSwapchainD3D11::Create(XrSession session, int64_t format, uint32_t width, uint32_t height,
                              uint32_t array_size)
{
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    if (!XrCheck(xrCreateSwapchain(session, &info, &handle_), "xrCreateSwapchain"))
        return false;

    uint32_t count = 0;
    xrEnumerateSwapchainImages(handle_, 0, &count, nullptr);
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (!XrCheck(xrEnumerateSwapchainImages(handle_, count, &count,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(images_.data())),
                 "xrEnumerateSwapchainImages")) {
        Destroy();
        return false;
    }
    width_ = width;
    height_ = height;
    Log("Swapchain %ux%u format %lld, %u images", width, height, (long long)format, count);
    return true;
}

void XrSwapchainD3D11::Destroy()
{
    if (handle_ != XR_NULL_HANDLE)
        xrDestroySwapchain(handle_);
    handle_ = XR_NULL_HANDLE;
    images_.clear();
    width_ = height_ = 0;
}

ID3D11Texture2D* XrSwapchainD3D11::Acquire()
{
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!XrCheck(xrAcquireSwapchainImage(handle_, &acquire, &index), "xrAcquireSwapchainImage"))
        return nullptr;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!XrCheck(xrWaitSwapchainImage(handle_, &wait), "xrWaitSwapchainImage"))
        return nullptr;
    return images_[index].texture;
}

void XrSwapchainD3D11::Release()
{
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XrCheck(xrReleaseSwapchainImage(handle_, &release), "xrReleaseSwapchainImage");
}

// ---------------------------------------------------------------------------
// XrSystem

bool XrSystem::Init()
{
    // openxr_loader.dll is delay-loaded; make sure it exists before the first call.
    if (!LoadLibraryA("openxr_loader.dll")) {
        Log("OpenXR: openxr_loader.dll not found next to Thief2.exe");
        return false;
    }

    LogRuntime();
    uint32_t ext_count = 0;
    if (!XrCheck(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr),
                 "xrEnumerateInstanceExtensionProperties")) {
        LogRuntimeHint();
        return false;
    }
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    bool has_d3d11 = false;
    for (const auto& e : exts) {
        has_d3d11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
        has_refresh_rate_ |= strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0;
    }
    Log("OpenXR: runtime offers %u extensions (D3D11: %s)", ext_count, has_d3d11 ? "yes" : "no");
    if (!has_d3d11) {
        Log("OpenXR: runtime has no %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ici.applicationInfo.applicationName, "Thief2VR");
    ici.applicationInfo.applicationVersion = 1;
    strcpy_s(ici.applicationInfo.engineName, "NewDark");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = has_refresh_rate_ ? 2 : 1;
    ici.enabledExtensionNames = enabled;
    if (!XrCheck(xrCreateInstance(&ici, &instance_), "xrCreateInstance")) {
        LogRuntimeHint();
        return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &props);
    Log("OpenXR runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
        XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrCheck(xrGetSystem(instance_, &sgi, &system_), "xrGetSystem (is the headset connected?)"))
        return false;
    XrSystemProperties sys{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, system_, &sys);
    Log("OpenXR system: %s", sys.systemName);

    uint32_t view_count = 2;
    if (!XrCheck(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   2, &view_count, view_configs_),
                 "xrEnumerateViewConfigurationViews"))
        return false;
    Log("Recommended eye size: %ux%u", view_configs_[0].recommendedImageRectWidth,
        view_configs_[0].recommendedImageRectHeight);

    if (!CreateD3D11Device())
        return false;

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device_;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system_;
    if (!XrCheck(xrCreateSession(instance_, &sci, &session_), "xrCreateSession"))
        return false;

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XrCheck(xrCreateReferenceSpace(session_, &rsci, &local_space_), "xrCreateReferenceSpace(LOCAL)");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XrCheck(xrCreateReferenceSpace(session_, &rsci, &view_space_), "xrCreateReferenceSpace(VIEW)");

    // Refresh rates the headset offers (for the Performance tab).
    if (has_refresh_rate_) {
        PFN_xrEnumerateDisplayRefreshRatesFB enumerate = nullptr;
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&enumerate));
        xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_refresh_rate_));
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&request_refresh_rate_));
        uint32_t n = 0;
        if (enumerate && XR_SUCCEEDED(enumerate(session_, 0, &n, nullptr)) && n) {
            refresh_rates_.resize(n);
            enumerate(session_, n, &n, refresh_rates_.data());
        }
        char list[128] = "";
        for (float r : refresh_rates_)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), " %.0f", r);
        Log("OpenXR: refresh rates%s (now %.0f Hz)", list, current_refresh_rate());
    } else {
        Log("OpenXR: no %s; the refresh rate is set in the streaming app", XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    }

    // The game renders gamma-encoded 8-bit BGRA. Use an sRGB swapchain so the
    // compositor decodes it correctly; copies go through the matching UNORM type.
    uint32_t fmt_count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &fmt_count, nullptr);
    std::vector<int64_t> formats(fmt_count);
    xrEnumerateSwapchainFormats(session_, fmt_count, &fmt_count, formats.data());
    const int64_t preferred[] = {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    for (int64_t want : preferred) {
        for (int64_t f : formats) {
            if (f == want) {
                color_format_ = f;
                break;
            }
        }
        if (color_format_)
            break;
    }
    if (!color_format_) {
        Log("OpenXR: runtime offers no 8-bit sRGB swapchain format");
        return false;
    }
    color_format_rgba_ = color_format_ == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    Log("Swapchain color format: %lld", (long long)color_format_);
    return true;
}

bool XrSystem::CreateD3D11Device()
{
    auto get_reqs = PFN_xrGetD3D11GraphicsRequirementsKHR(nullptr);
    xrGetInstanceProcAddr(instance_, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));
    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!get_reqs || !XrCheck(get_reqs(instance_, system_, &reqs), "xrGetD3D11GraphicsRequirementsKHR"))
        return false;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0) {
            Log("D3D11 adapter: %ls", desc.Description);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    if (!adapter) {
        Log("OpenXR: headset adapter not found");
        return false;
    }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device_, nullptr, &context_);
    adapter->Release();
    if (FAILED(hr)) {
        Log("D3D11CreateDevice failed: 0x%08x", hr);
        return false;
    }
    return true;
}

void XrSystem::PollEvents()
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* changed = reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            state_ = changed->state;
            Log("OpenXR session state -> %d", state_);
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = XrCheck(xrBeginSession(session_, &begin), "xrBeginSession");
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                XrCheck(xrEndSession(session_), "xrEndSession");
                running_ = false;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            Log("OpenXR instance loss pending");
            running_ = false;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool XrSystem::BeginFrame(XrFrame& frame)
{
    if (!session_)
        return false;
    PollEvents();
    if (!running_)
        return false;

    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState state{XR_TYPE_FRAME_STATE};
    if (!XrCheck(xrWaitFrame(session_, &wait, &state), "xrWaitFrame"))
        return false;
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    if (!XrCheck(xrBeginFrame(session_, &begin), "xrBeginFrame"))
        return false;

    frame.display_time = state.predictedDisplayTime;
    frame.should_render = state.shouldRender == XR_TRUE;

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame.display_time;
    locate.space = local_space_;
    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t count = 2;
    frame.views[0] = {XR_TYPE_VIEW};
    frame.views[1] = {XR_TYPE_VIEW};
    if (!XrCheck(xrLocateViews(session_, &locate, &view_state, 2, &count, frame.views), "xrLocateViews") ||
        !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        frame.should_render = false;
    return true;
}

void XrSystem::EndFrame(const XrFrame& frame, const XrCompositionLayerBaseHeader* const* layers, uint32_t count)
{
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frame.display_time;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = frame.should_render ? count : 0;
    end.layers = layers;
    XrCheck(xrEndFrame(session_, &end), "xrEndFrame");
}

float XrSystem::current_refresh_rate() const
{
    float hz = 0;
    if (get_refresh_rate_ && session_)
        get_refresh_rate_(session_, &hz);
    return hz;
}

bool XrSystem::RequestRefreshRate(float hz)
{
    if (!request_refresh_rate_ || !session_)
        return false;
    bool ok = XrCheck(request_refresh_rate_(session_, hz), "xrRequestDisplayRefreshRateFB");
    Log("OpenXR: refresh rate %.0f Hz requested (%s)", hz, ok ? "ok" : "failed");
    return ok;
}
