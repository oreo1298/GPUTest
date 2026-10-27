#pragma once
// Direct3D 12 backend: platform headers and small helpers shared by its sources. The API
// comes from Microsoft's DirectX-Headers (the Agility SDK's d3d12.h); d3d12.dll and dxgi.dll
// are loaded at run time so the runner still starts where Direct3D 12 is missing.

#include "gt/rhi/rhi.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <directx/d3d12.h>
#include <directx/d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxguids/dxguids.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace gt::rhi::d3d12 {

/// Minimal COM smart pointer (no WRL dependency).
template <typename T>
class Com {
public:
    Com() = default;
    Com(std::nullptr_t) {}
    /// Adopts one reference.
    explicit Com(T* p) : m_p(p) {}
    Com(const Com& o) : m_p(o.m_p) {
        if (m_p) m_p->AddRef();
    }
    Com(Com&& o) noexcept : m_p(std::exchange(o.m_p, nullptr)) {}
    ~Com() { reset(); }
    Com& operator=(Com o) noexcept {
        std::swap(m_p, o.m_p);
        return *this;
    }
    void reset() {
        if (m_p) std::exchange(m_p, nullptr)->Release();
    }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    /// Releases the current object and returns the slot for an out-parameter.
    T** put() {
        reset();
        return &m_p;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    /// QueryInterface; empty when the object does not implement U.
    template <typename U>
    Com<U> as() const {
        Com<U> out;
        if (m_p) m_p->QueryInterface(__uuidof(U), out.putVoid());
        return out;
    }

private:
    T* m_p = nullptr;
};

// Methods returning structs use an out-parameter in the non-MSVC Windows ABI of DirectX-Headers.
inline D3D12_CPU_DESCRIPTOR_HANDLE cpuStart(ID3D12DescriptorHeap* heap) {
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetCPUDescriptorHandleForHeapStart();
#else
    D3D12_CPU_DESCRIPTOR_HANDLE h{};
    heap->GetCPUDescriptorHandleForHeapStart(&h);
    return h;
#endif
}
inline D3D12_GPU_DESCRIPTOR_HANDLE gpuStart(ID3D12DescriptorHeap* heap) {
#if defined(_MSC_VER) || !defined(_WIN32)
    return heap->GetGPUDescriptorHandleForHeapStart();
#else
    D3D12_GPU_DESCRIPTOR_HANDLE h{};
    heap->GetGPUDescriptorHandleForHeapStart(&h);
    return h;
#endif
}

std::string hresultName(HRESULT hr);
/// Throws DeviceLostError / OutOfMemoryError / Error for a failed HRESULT.
[[noreturn]] void throwHr(HRESULT hr, const char* what);
inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throwHr(hr, what);
}

std::string narrow(const wchar_t* s);
std::wstring widen(std::string_view s);

/// Resource format: typeless for depth formats so they can also be sampled.
DXGI_FORMAT toDxgiResource(Format f);
/// Format of render-target, depth-stencil and unordered-access views and of copies.
DXGI_FORMAT toDxgi(Format f);
/// Format of shader-resource views (depth formats map to their readable colour format).
DXGI_FORMAT toDxgiSrv(Format f);
D3D12_COMPARISON_FUNC toD3d(CompareOp op);
inline std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return a ? (v + a - 1) / a * a : v; }

/// Entry points of d3d12.dll / dxgi.dll, loaded on first use.
struct Runtime {
    HMODULE d3d12 = nullptr;
    HMODULE dxgi = nullptr;
    PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
    PFN_D3D12_GET_DEBUG_INTERFACE getDebugInterface = nullptr;
    PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE serializeRootSignature = nullptr;
    PFN_D3D12_GET_INTERFACE getInterface = nullptr; ///< SDK configuration (side-by-side Agility SDK)
    HRESULT(WINAPI* createDxgiFactory2)(UINT, REFIID, void**) = nullptr;
    std::string error; ///< why Direct3D 12 is unavailable (empty when loaded)
};
const Runtime& runtime();

/// Creates devices and debug interfaces from the Direct3D 12 Agility SDK runtime in
/// "<runner folder>\D3D12\" when one is there (D3D12SDKConfiguration1), otherwise from the
/// operating system's runtime.
HRESULT createRawDevice(IUnknown* adapter, REFIID iid, void** device);
HRESULT getDebugInterface(REFCLSID clsid, REFIID iid, void** object);
/// "Agility SDK 619 (path)" or "Windows built-in runtime".
std::string runtimeDescription();

/// Validation (debug layer) message counters shared by every device.
ValidationCounts globalValidationCounts();
void countValidationMessage(D3D12_MESSAGE_SEVERITY severity, const char* text);

} // namespace gt::rhi::d3d12
