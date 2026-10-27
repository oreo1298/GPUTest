// Intel XeSS super resolution as an upscaler plugin (Direct3D 12). Builds from XeSS's MIT public headers
// (third_party/xess); the runtime, libxess.dll, is the user's to install next to this plugin or on the DLL
// search path (Intel Simplified Software License; not distributed with this project).

#define GT_UPSCALER_PLUGIN
#include "gt/bench/upscaler_plugin.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <xess/xess_d3d12.h>

#include <cstdio>
#include <new>
#include <string>

namespace {

struct Runtime {
    HMODULE module = nullptr;
    decltype(&xessGetVersion) getVersion = nullptr;
    decltype(&xessD3D12CreateContext) createContext = nullptr;
    decltype(&xessD3D12Init) init = nullptr;
    decltype(&xessD3D12Execute) execute = nullptr;
    decltype(&xessDestroyContext) destroyContext = nullptr;
    std::string error;
    char version[64] = "plugin 1.0";
};

template <typename F>
F proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(m, name)));
}

/// Folder of this plugin DLL.
std::wstring pluginDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&pluginDir), &self);
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path, n);
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

Runtime& runtime() {
    static Runtime rt = [] {
        Runtime r;
        const std::wstring local = pluginDir() + L"libxess.dll";
        r.module = LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!r.module) r.module = LoadLibraryW(L"libxess.dll");
        if (!r.module) {
            r.error = "libxess.dll was not found: copy it from the Intel XeSS SDK next to this plugin";
            return r;
        }
        r.getVersion = proc<decltype(r.getVersion)>(r.module, "xessGetVersion");
        r.createContext = proc<decltype(r.createContext)>(r.module, "xessD3D12CreateContext");
        r.init = proc<decltype(r.init)>(r.module, "xessD3D12Init");
        r.execute = proc<decltype(r.execute)>(r.module, "xessD3D12Execute");
        r.destroyContext = proc<decltype(r.destroyContext)>(r.module, "xessDestroyContext");
        if (!r.getVersion || !r.createContext || !r.init || !r.execute || !r.destroyContext) {
            r.error = "libxess.dll lacks the Direct3D 12 entry points";
            return r;
        }
        xess_version_t v{};
        if (r.getVersion(&v) == XESS_RESULT_SUCCESS)
            std::snprintf(r.version, sizeof(r.version), "plugin 1.0, XeSS %u.%u.%u", v.major, v.minor, v.patch);
        return r;
    }();
    return rt;
}

void say(char* message, size_t size, const char* text) {
    if (message && size) std::snprintf(message, size, "%s", text);
}

void sayResult(char* message, size_t size, const char* what, xess_result_t r) {
    if (message && size) std::snprintf(message, size, "%s failed (XeSS result %d)", what, static_cast<int>(r));
}

struct Context {
    xess_context_handle_t xess = nullptr;
};

int query(const gt_upscaler_device* device, char* message, size_t size) {
    if (device->api != GT_UPSCALER_API_D3D12) {
        say(message, size, "the XeSS plugin supports Direct3D 12 only");
        return GT_UPSCALER_UNSUPPORTED;
    }
    Runtime& rt = runtime();
    if (!rt.error.empty()) {
        say(message, size, rt.error.c_str());
        return GT_UPSCALER_UNSUPPORTED;
    }
    xess_context_handle_t h = nullptr;
    const xess_result_t r = rt.createContext(static_cast<ID3D12Device*>(device->device), &h);
    if (r != XESS_RESULT_SUCCESS) {
        sayResult(message, size, "xessD3D12CreateContext", r);
        return GT_UPSCALER_UNSUPPORTED;
    }
    rt.destroyContext(h);
    return GT_UPSCALER_OK;
}

xess_quality_settings_t quality(uint32_t q) {
    switch (q) {
    case GT_UPSCALER_QUALITY_ULTRA_QUALITY: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    case GT_UPSCALER_QUALITY_BALANCED: return XESS_QUALITY_SETTING_BALANCED;
    case GT_UPSCALER_QUALITY_PERFORMANCE: return XESS_QUALITY_SETTING_PERFORMANCE;
    default: return XESS_QUALITY_SETTING_QUALITY;
    }
}

int create(const gt_upscaler_device* device, const gt_upscaler_create_info* info, void** context, char* message,
           size_t size) {
    if (int q = query(device, message, size); q != GT_UPSCALER_OK) return q;
    Runtime& rt = runtime();
    auto* c = new (std::nothrow) Context;
    if (!c) return GT_UPSCALER_ERROR;
    xess_result_t r = rt.createContext(static_cast<ID3D12Device*>(device->device), &c->xess);
    if (r != XESS_RESULT_SUCCESS) {
        sayResult(message, size, "xessD3D12CreateContext", r);
        delete c;
        return GT_UPSCALER_ERROR;
    }
    xess_d3d12_init_params_t p{};
    p.outputResolution = {info->output_width, info->output_height};
    p.qualitySetting = quality(info->quality);
    p.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR; // RGBA8 display-referred colour
    r = rt.init(c->xess, &p);
    if (r != XESS_RESULT_SUCCESS) {
        sayResult(message, size, "xessD3D12Init", r);
        rt.destroyContext(c->xess);
        delete c;
        return GT_UPSCALER_ERROR;
    }
    *context = c;
    return GT_UPSCALER_OK;
}

int evaluate(void* context, const gt_upscaler_frame* f, char* message, size_t size) {
    auto* c = static_cast<Context*>(context);
    xess_d3d12_execute_params_t p{};
    p.pColorTexture = static_cast<ID3D12Resource*>(f->color.resource);
    p.pVelocityTexture = static_cast<ID3D12Resource*>(f->motion.resource);
    p.pDepthTexture = static_cast<ID3D12Resource*>(f->depth.resource);
    p.pOutputTexture = static_cast<ID3D12Resource*>(f->output.resource);
    p.jitterOffsetX = f->jitter_x;
    p.jitterOffsetY = f->jitter_y;
    p.exposureScale = 1.0f;
    p.resetHistory = f->reset;
    p.inputWidth = f->color.width;
    p.inputHeight = f->color.height;
    const xess_result_t r =
        runtime().execute(c->xess, static_cast<ID3D12GraphicsCommandList*>(f->command_buffer), &p);
    if (r != XESS_RESULT_SUCCESS) {
        sayResult(message, size, "xessD3D12Execute", r);
        return GT_UPSCALER_ERROR;
    }
    return GT_UPSCALER_OK;
}

void destroy(void* context) {
    auto* c = static_cast<Context*>(context);
    if (c->xess) runtime().destroyContext(c->xess);
    delete c;
}

gt_upscaler_plugin g_plugin = {GT_UPSCALER_ABI_VERSION, "xess", "Intel XeSS", "plugin 1.0", GT_UPSCALER_API_D3D12, 1,
                               query, create, evaluate, destroy};

} // namespace

extern "C" GT_UPSCALER_EXPORT const gt_upscaler_plugin* gt_upscaler_plugin_get(void) {
    g_plugin.version = runtime().version; // names the XeSS version once the runtime is found
    return &g_plugin;
}
