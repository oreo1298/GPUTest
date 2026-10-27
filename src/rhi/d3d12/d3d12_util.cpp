#include "d3d12_common.h"

#include "gt/log.h"

#include <atomic>
#include <format>
#include <mutex>

namespace gt::rhi::d3d12 {
namespace {

std::atomic<std::uint64_t> g_validationErrors{0};
std::atomic<std::uint64_t> g_validationWarnings{0};

template <typename F>
F proc(HMODULE module, const char* name) {
    // FARPROC -> void(*)() -> F keeps -Wcast-function-type quiet.
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

} // namespace

std::string hresultName(HRESULT hr) {
    switch (hr) {
    case S_OK: return "S_OK";
    case S_FALSE: return "S_FALSE";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
    case D3D12_ERROR_ADAPTER_NOT_FOUND: return "D3D12_ERROR_ADAPTER_NOT_FOUND";
    case D3D12_ERROR_DRIVER_VERSION_MISMATCH: return "D3D12_ERROR_DRIVER_VERSION_MISMATCH";
    default: return std::format("HRESULT 0x{:08X}", static_cast<std::uint32_t>(hr));
    }
}

void throwHr(HRESULT hr, const char* what) {
    const std::string msg = std::format("{} failed: {}", what, hresultName(hr));
    switch (hr) {
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: throw DeviceLostError(msg);
    case E_OUTOFMEMORY: throw OutOfMemoryError(msg);
    default: throw Error(msg);
    }
}

std::string narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

DXGI_FORMAT toDxgi(Format f) {
    switch (f) {
    case Format::Unknown: return DXGI_FORMAT_UNKNOWN;
    case Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case Format::R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case Format::RG16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case Format::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case Format::RG32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case Format::RGB32_FLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
    case Format::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
    case Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
    case Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
    case Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
    case Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
    case Format::R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case Format::RGB10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case Format::D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case Format::BC1_UNORM: return DXGI_FORMAT_BC1_UNORM;
    case Format::BC3_UNORM: return DXGI_FORMAT_BC3_UNORM;
    case Format::BC5_UNORM: return DXGI_FORMAT_BC5_UNORM;
    case Format::BC7_UNORM: return DXGI_FORMAT_BC7_UNORM;
    case Format::Count: break;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT toDxgiResource(Format f) {
    switch (f) {
    case Format::D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
    case Format::D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
    case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return toDxgi(f);
    }
}

DXGI_FORMAT toDxgiSrv(Format f) {
    switch (f) {
    case Format::D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case Format::D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case Format::D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case Format::D32_FLOAT_S8_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return toDxgi(f);
    }
}

D3D12_COMPARISON_FUNC toD3d(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
    case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
    case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
    case CompareOp::LessEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
    case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case CompareOp::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

const Runtime& runtime() {
    static Runtime rt = [] {
        Runtime r;
        r.d3d12 = LoadLibraryW(L"d3d12.dll");
        r.dxgi = LoadLibraryW(L"dxgi.dll");
        if (!r.d3d12 || !r.dxgi) {
            r.error = "Direct3D 12 is not available on this system (d3d12.dll / dxgi.dll not found)";
            return r;
        }
        r.createDevice = proc<PFN_D3D12_CREATE_DEVICE>(r.d3d12, "D3D12CreateDevice");
        r.getDebugInterface = proc<PFN_D3D12_GET_DEBUG_INTERFACE>(r.d3d12, "D3D12GetDebugInterface");
        r.serializeRootSignature =
            proc<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE>(r.d3d12, "D3D12SerializeVersionedRootSignature");
        r.getInterface = proc<PFN_D3D12_GET_INTERFACE>(r.d3d12, "D3D12GetInterface");
        r.createDxgiFactory2 = proc<HRESULT(WINAPI*)(UINT, REFIID, void**)>(r.dxgi, "CreateDXGIFactory2");
        if (!r.createDevice || !r.serializeRootSignature || !r.createDxgiFactory2)
            r.error = "this Windows version's Direct3D 12 runtime is too old (missing entry points)";
        return r;
    }();
    return rt;
}

namespace {

struct Agility {
    Com<ID3D12DeviceFactory> factory;
    std::string path;
};

const Agility& agility() {
    static const Agility a = [] {
        Agility out;
        const Runtime& rt = runtime();
        if (!rt.getInterface) return out;
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return out;
        std::wstring dir(exe, n);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"D3D12\\";
        if (GetFileAttributesW((dir + L"D3D12Core.dll").c_str()) == INVALID_FILE_ATTRIBUTES) return out;
        Com<ID3D12SDKConfiguration1> config;
        if (FAILED(rt.getInterface(CLSID_D3D12SDKConfiguration, __uuidof(ID3D12SDKConfiguration1), config.putVoid()))) {
            log::info("Direct3D 12: this Windows version cannot load the Agility SDK side by side; using its own runtime");
            return out;
        }
        const std::string path = narrow(dir.c_str());
        const HRESULT hr = config->CreateDeviceFactory(D3D12_SDK_VERSION, path.c_str(), __uuidof(ID3D12DeviceFactory),
                                                       out.factory.putVoid());
        if (FAILED(hr)) {
            log::warn("Direct3D 12: Agility SDK in {} not usable ({}); using the Windows runtime", path, hresultName(hr));
            out.factory.reset();
            return out;
        }
        out.path = path;
        return out;
    }();
    return a;
}

} // namespace

HRESULT createRawDevice(IUnknown* adapter, REFIID iid, void** device) {
    if (const Agility& a = agility(); a.factory)
        return a.factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, iid, device);
    return runtime().createDevice(adapter, D3D_FEATURE_LEVEL_11_0, iid, device);
}

HRESULT getDebugInterface(REFCLSID clsid, REFIID iid, void** object) {
    if (const Agility& a = agility(); a.factory) return a.factory->GetConfigurationInterface(clsid, iid, object);
    if (!runtime().getDebugInterface) return E_NOINTERFACE;
    return runtime().getDebugInterface(iid, object);
}

std::string runtimeDescription() {
    const Agility& a = agility();
    return a.factory ? std::format("Agility SDK {} ({})", D3D12_SDK_VERSION, a.path) : "Windows built-in runtime";
}

ValidationCounts globalValidationCounts() {
    return {g_validationErrors.load(std::memory_order_relaxed), g_validationWarnings.load(std::memory_order_relaxed)};
}

void countValidationMessage(D3D12_MESSAGE_SEVERITY severity, const char* text) {
    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
    case D3D12_MESSAGE_SEVERITY_ERROR:
        g_validationErrors.fetch_add(1, std::memory_order_relaxed);
        log::error("[d3d12-debug] {}", text);
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        g_validationWarnings.fetch_add(1, std::memory_order_relaxed);
        log::warn("[d3d12-debug] {}", text);
        break;
    default: log::trace("[d3d12] {}", text); break;
    }
}

} // namespace gt::rhi::d3d12
