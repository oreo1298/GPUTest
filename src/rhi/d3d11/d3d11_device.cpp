#include "d3d11_device.h"

#include "gt/log.h"
#include "gt/shaders.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

namespace gt::rhi::d3d11 {

// --- helpers ------------------------------------------------------------------------------------------------
std::string narrow(const wchar_t* s) {
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

std::string hresultName(HRESULT hr) {
    switch (hr) {
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_FAIL: return "E_FAIL";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    case DXGI_ERROR_SDK_COMPONENT_MISSING: return "DXGI_ERROR_SDK_COMPONENT_MISSING";
    default: return std::format("HRESULT {:#010x}", static_cast<std::uint32_t>(hr));
    }
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

D3D11_COMPARISON_FUNC toD3d(CompareOp op) {
    switch (op) {
    case CompareOp::Never: return D3D11_COMPARISON_NEVER;
    case CompareOp::Less: return D3D11_COMPARISON_LESS;
    case CompareOp::Equal: return D3D11_COMPARISON_EQUAL;
    case CompareOp::LessEqual: return D3D11_COMPARISON_LESS_EQUAL;
    case CompareOp::Greater: return D3D11_COMPARISON_GREATER;
    case CompareOp::NotEqual: return D3D11_COMPARISON_NOT_EQUAL;
    case CompareOp::GreaterEqual: return D3D11_COMPARISON_GREATER_EQUAL;
    case CompareOp::Always: return D3D11_COMPARISON_ALWAYS;
    }
    return D3D11_COMPARISON_ALWAYS;
}

namespace {

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "?";
    }
}

/// "a.b.c.d" user-mode driver version; NVIDIA's also in its usual "560.94" form.
std::string driverVersion(IDXGIAdapter1* adapter, std::uint32_t vendorId) {
    LARGE_INTEGER v{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return "unknown";
    const auto q = static_cast<std::uint64_t>(v.QuadPart);
    const unsigned a = static_cast<unsigned>((q >> 48) & 0xFFFF), b = static_cast<unsigned>((q >> 32) & 0xFFFF),
                   c = static_cast<unsigned>((q >> 16) & 0xFFFF), d = static_cast<unsigned>(q & 0xFFFF);
    const std::string raw = std::format("{}.{}.{}.{}", a, b, c, d);
    if (vendorId == vendor::kNvidia) {
        const unsigned nv = (c % 10) * 10000 + d;
        return std::format("{}.{:02} ({})", nv / 100, nv % 100, raw);
    }
    return raw;
}

// Shader model 5 (every embedded D3D11 shader) needs feature level 11_0.
const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

template <typename F>
F proc(HMODULE module, const char* name) {
    // FARPROC -> void(*)() -> F keeps -Wcast-function-type quiet.
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

/// d3d11.dll and dxgi.dll are loaded at run time, like the Direct3D 12 backend's runtime.
struct Runtime {
    PFN_D3D11_CREATE_DEVICE createDevice = nullptr;
    HRESULT(WINAPI* createDxgiFactory1)(REFIID, void**) = nullptr;
    std::string error;
};

const Runtime& runtime() {
    static const Runtime rt = [] {
        Runtime r;
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!d3d11 || !dxgi) {
            r.error = "Direct3D 11 is not available on this system (d3d11.dll / dxgi.dll not found)";
            return r;
        }
        r.createDevice = proc<PFN_D3D11_CREATE_DEVICE>(d3d11, "D3D11CreateDevice");
        r.createDxgiFactory1 = proc<HRESULT(WINAPI*)(REFIID, void**)>(dxgi, "CreateDXGIFactory1");
        if (!r.createDevice || !r.createDxgiFactory1) r.error = "the Direct3D 11 runtime is missing entry points";
        return r;
    }();
    return rt;
}

HRESULT createRaw(IDXGIAdapter1* adapter, UINT flags, ID3D11Device** dev, ID3D11DeviceContext** ctx,
                  D3D_FEATURE_LEVEL* level) {
    const auto create = runtime().createDevice;
    HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels, static_cast<UINT>(std::size(kLevels)),
                        D3D11_SDK_VERSION, dev, level, ctx);
    if (hr == E_INVALIDARG) // runtimes without 11_1 reject the list
        hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, kLevels + 1, 1, D3D11_SDK_VERSION, dev, level,
                    ctx);
    return hr;
}

struct FoundAdapter {
    Com<IDXGIAdapter1> adapter;
    AdapterInfo info;
};

std::vector<FoundAdapter> findAdapters(IDXGIFactory2* factory) {
    std::vector<FoundAdapter> out;
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        Com<ID3D11Device> dev;
        Com<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL level{};
        if (FAILED(createRaw(a.get(), 0, dev.put(), ctx.put(), &level))) continue;
        FoundAdapter f;
        f.adapter = a;
        AdapterInfo& ai = f.info;
        ai.api = Api::D3D11;
        ai.index = static_cast<std::uint32_t>(out.size());
        ai.name = narrow(d.Description);
        ai.vendorId = d.VendorId;
        ai.deviceId = d.DeviceId;
        ai.subsysId = d.SubSysId;
        ai.revision = d.Revision;
        ai.type = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == vendor::kMicrosoft ? AdapterType::Cpu
                  : d.DedicatedVideoMemory < (512ull << 20)                                   ? AdapterType::Integrated
                                                                                               : AdapterType::Discrete;
        ai.driverName = std::string(vendorName(d.VendorId));
        ai.driverVersion = driverVersion(a.get(), d.VendorId);
        ai.driverInfo = std::format("WDDM user-mode driver {}", ai.driverVersion);
        ai.apiVersion = std::format("FL {}", featureLevelName(level));
        ai.dedicatedMemory = d.DedicatedVideoMemory;
        ai.sharedMemory = d.SharedSystemMemory;
        std::array<std::uint8_t, 8> luid{};
        std::memcpy(luid.data(), &d.AdapterLuid, sizeof(luid));
        ai.luid = luid;
        out.push_back(std::move(f));
    }
    return out;
}

Com<IDXGIFactory2> createFactory() {
    if (!runtime().error.empty()) throw Error(runtime().error);
    Com<IDXGIFactory2> f;
    const HRESULT hr = runtime().createDxgiFactory1(__uuidof(IDXGIFactory2), f.putVoid());
    if (FAILED(hr)) throw Error("CreateDXGIFactory1 failed: " + hresultName(hr));
    return f;
}

} // namespace

std::vector<AdapterInfo> enumerateAdapters(std::string* error) {
    std::vector<AdapterInfo> out;
    try {
        for (auto& f : findAdapters(createFactory().get())) out.push_back(f.info);
        if (out.empty() && error) *error = "no adapter supports Direct3D 11 feature level 11_0";
    } catch (const std::exception& e) {
        if (error) *error = e.what();
    }
    return out;
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) { return std::make_unique<D3D11Device>(info); }

// --- device -----------------------------------------------------------------------------------------------------
D3D11Device::D3D11Device(const DeviceCreateInfo& info) {
    m_api = Api::D3D11;
    m_factory = createFactory();
    auto adapters = findAdapters(m_factory.get());
    if (info.adapterIndex >= adapters.size()) throw Error("Direct3D 11 adapter index out of range");
    m_adapterDxgi = adapters[info.adapterIndex].adapter;
    m_adapter = adapters[info.adapterIndex].info;

    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    HRESULT hr = E_FAIL;
    if (info.validation) {
        // The debug layer needs Windows' "Graphics Tools" optional feature.
        hr = createRaw(m_adapterDxgi.get(), D3D11_CREATE_DEVICE_DEBUG, dev.put(), ctx.put(), &m_featureLevel);
        if (FAILED(hr)) log::warn("Direct3D 11 debug layer unavailable ({}): install Graphics Tools", hresultName(hr));
    }
    if (FAILED(hr)) hr = createRaw(m_adapterDxgi.get(), 0, dev.put(), ctx.put(), &m_featureLevel);
    if (FAILED(hr)) throw Error("D3D11CreateDevice failed: " + hresultName(hr));
    m_device = dev.as<ID3D11Device1>();
    m_context = ctx.as<ID3D11DeviceContext1>();
    if (!m_device || !m_context) throw Error("Direct3D 11.1 runtime required (Windows 8 or later)");
    m_infoQueue = dev.as<ID3D11InfoQueue>();
    if (m_infoQueue) {
        m_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
        D3D11_MESSAGE_ID deny[] = {D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET};
        D3D11_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(deny));
        filter.DenyList.pIDList = deny;
        m_infoQueue->AddStorageFilterEntries(&filter);
    }
    m_annotation = ctx.as<ID3DUserDefinedAnnotation>();
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (SUCCEEDED(m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))))
        m_cbOffsets = options.ConstantBufferOffsetting != FALSE;
    if (auto f5 = m_factory.as<IDXGIFactory5>()) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
            m_tearing = allow != FALSE;
    }
    // Timestamp frequency (constant for the device in practice).
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    Com<ID3D11Query> disjoint;
    if (SUCCEEDED(m_device->CreateQuery(&qd, disjoint.put()))) {
        m_context->Begin(disjoint.get());
        m_context->End(disjoint.get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        for (int i = 0; i < 1000 && m_context->GetData(disjoint.get(), &data, sizeof(data), 0) == S_FALSE; ++i)
            std::this_thread::yield();
        if (data.Frequency) m_timestampFrequency = static_cast<double>(data.Frequency);
    }
    m_exec = std::make_unique<Executor>(*this, m_context.get());
    fillFeatures();
    log::info("Direct3D 11 device: {} (FL {}){}", m_adapter.name, featureLevelName(m_featureLevel),
              m_infoQueue ? ", debug layer on" : "");
}

D3D11Device::~D3D11Device() {
    try {
        waitIdle();
        m_context->ClearState();
        m_context->Flush();
    } catch (const std::exception& e) {
        log::warn("Direct3D 11 shutdown: {}", e.what());
    }
}

void D3D11Device::fillFeatures() {
    DeviceFeatures& f = m_features;
    f.timestamps = true;
    f.timestampsOnComputeQueue = true;
    f.timestampPeriodNs = 1e9 / m_timestampFrequency;
    f.asyncComputeQueue = false;
    f.copyQueue = false;
    f.geometryShader = true;
    f.tessellation = true;
    f.wireframe = true;
    f.anisotropicFiltering = true;
    f.textureCompressionBC = true;
    D3D11_FEATURE_DATA_DOUBLES doubles{};
    m_device->CheckFeatureSupport(D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles));
    f.shaderFloat64 = doubles.DoublePrecisionFloatShaderOps != FALSE;
    f.multiDrawIndirect = false;
    f.drawIndirectCount = false;
    f.pipelineStatistics = false;
    f.memoryBudget = static_cast<bool>(m_adapterDxgi.as<IDXGIAdapter3>());
    f.maxTexture2D = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    f.maxMsaaSamples = 1;
    for (UINT s = 2; s <= 8; s *= 2) {
        UINT q = 0;
        if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, s, &q)) && q > 0)
            f.maxMsaaSamples = s;
    }
    f.maxComputeSharedMemory = D3D11_CS_TGSM_REGISTER_COUNT * 4;
    f.maxComputeWorkgroupSize = {D3D11_CS_THREAD_GROUP_MAX_X, D3D11_CS_THREAD_GROUP_MAX_Y, D3D11_CS_THREAD_GROUP_MAX_Z};
    f.maxComputeWorkgroupInvocations = D3D11_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    // Resource size limit: max(128 MiB, min(VRAM / 4, 2 GiB)).
    const std::uint64_t limit =
        std::max<std::uint64_t>(128ull << 20, std::min<std::uint64_t>(m_adapter.dedicatedMemory / 4, 2048ull << 20));
    f.maxStorageBufferRange = limit;
    f.maxBufferSize = limit;
}

std::vector<Capability> D3D11Device::capabilities() const {
    std::vector<Capability> c;
    auto add = [&](const std::string& g, const std::string& n, const std::string& v, bool ok = true) {
        c.push_back({g, n, v, ok});
    };
    auto yes = [&](const std::string& g, const std::string& n, bool v) { add(g, n, v ? "yes" : "no", v); };
    add("Device", "Adapter", m_adapter.name);
    add("Device", "Driver", m_adapter.driverVersion);
    add("Device", "Feature level", featureLevelName(m_featureLevel));
    D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
    m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
    D3D11_FEATURE_DATA_THREADING th{};
    m_device->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof(th));
    const std::string g = "Features";
    yes(g, "UAVs in every shader stage (FL 11_1)", m_featureLevel >= D3D_FEATURE_LEVEL_11_1);
    yes(g, "Tessellation", m_features.tessellation);
    yes(g, "Double-precision shader ops", m_features.shaderFloat64);
    yes(g, "Constant buffer offsetting (11.1)", o.ConstantBufferOffsetting != FALSE);
    yes(g, "Output-merger logic ops", o.OutputMergerLogicOp != FALSE);
    yes(g, "UAV-only rendering (forced sample count)", o.UAVOnlyRenderingForcedSampleCount != FALSE);
    yes(g, "Concurrent resource creation", th.DriverConcurrentCreates != FALSE);
    yes(g, "Driver command lists", th.DriverCommandLists != FALSE);
    yes(g, "Tearing (variable refresh) presentation", m_tearing);
    add("Limits", "Max MSAA samples (RGBA8)", std::to_string(m_features.maxMsaaSamples));
    add("Limits", "Max 2D texture size", std::to_string(m_features.maxTexture2D));
    add("Limits", "Timestamp frequency", std::format("{:.0f} Hz", m_timestampFrequency));
    return c;
}

std::unique_ptr<Buffer> D3D11Device::createBuffer(const BufferDesc& desc, const void* initialData) {
    return std::make_unique<D3D11Buffer>(*this, desc, initialData);
}
std::unique_ptr<Texture> D3D11Device::createTexture(const TextureDesc& desc) {
    return std::make_unique<D3D11Texture>(*this, desc);
}
std::unique_ptr<Sampler> D3D11Device::createSampler(const SamplerDesc& desc) {
    return std::make_unique<D3D11Sampler>(*this, desc);
}
std::unique_ptr<Pipeline> D3D11Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    return std::make_unique<D3D11Pipeline>(*this, desc);
}
std::unique_ptr<Pipeline> D3D11Device::createComputePipeline(const ComputePipelineDesc& desc) {
    return std::make_unique<D3D11Pipeline>(*this, desc);
}
std::unique_ptr<QueryPool> D3D11Device::createQueryPool(QueryType type, std::uint32_t count) {
    return std::make_unique<D3D11QueryPool>(*this, type, count);
}
std::unique_ptr<CommandList> D3D11Device::createCommandList(QueueType queue) {
    return std::make_unique<D3D11CommandList>(*this, queue);
}
std::unique_ptr<Swapchain> D3D11Device::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<D3D11Swapchain>(*this, desc);
}

std::span<const std::uint8_t> D3D11Device::shaderBytecode(const ShaderRef& ref) const {
    if (!ref.bytecode.empty()) return ref.bytecode;
    auto blob = shaders::find(ref.module, ref.entry, shaders::Format::Dxbc);
    if (!blob) throw Error(std::format("DXBC for shader {}:{} is not embedded in this build", ref.module, ref.entry));
    return blob.bytes();
}

void D3D11Device::setName(ID3D11DeviceChild* object, const char* name) const {
    if (!object || !name || !*name) return;
    // WKPDID_D3DDebugObjectName, spelled out so no GUID library is needed.
    static constexpr GUID kDebugObjectName = {0x429b8c22, 0x9188, 0x4b0c, {0x87, 0x42, 0xac, 0xb0, 0xbf, 0x85, 0xc2, 0x00}};
    object->SetPrivateData(kDebugObjectName, static_cast<UINT>(std::strlen(name)), name);
}

void D3D11Device::setDebugName(Buffer* b, const char* name) {
    auto* buffer = static_cast<D3D11Buffer*>(b);
    if (buffer->created()) setName(buffer->gpu(), name);
}

void D3D11Device::setDebugName(Texture* t, const char* name) {
    setName(static_cast<D3D11Texture*>(t)->resource(), name);
}

void D3D11Device::check(HRESULT hr, const char* what) const {
    if (SUCCEEDED(hr)) return;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        throw DeviceLostError(std::format("{}: device removed ({})", what, hresultName(reason)));
    }
    throw Error(std::format("{} failed: {}", what, hresultName(hr)));
}

// --- submission ------------------------------------------------------------------------------------------------
std::uint64_t D3D11Device::submit(QueueType /*queue*/, const SubmitInfo& info) {
    // One immediate context: every queue is the same ordered stream, so queue waits hold already.
    const std::uint64_t value = m_submitted + 1;
    Pending p;
    p.value = value;
    D3D11_QUERY_DESC dq{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    check(m_device->CreateQuery(&dq, p.disjoint.put()), "CreateQuery(timestamp disjoint)");
    D3D11_QUERY_DESC eq{D3D11_QUERY_EVENT, 0};
    check(m_device->CreateQuery(&eq, p.event.put()), "CreateQuery(event)");
    m_context->Begin(p.disjoint.get()); // timestamps are only defined inside a disjoint query
    m_exec->reset();
    m_replayAfter.clear();
    try {
        for (CommandList* l : info.lists) {
            auto* list = static_cast<D3D11CommandList*>(l);
            list->uploadTransient(value);
            list->commands().execute(*m_exec);
        }
    } catch (...) {
        m_context->End(p.disjoint.get());
        m_replayAfter.clear();
        drainMessages();
        throw;
    }
    m_context->End(p.disjoint.get());
    m_context->End(p.event.get());
    m_context->Flush();
    p.after = std::move(m_replayAfter); // readback copies recorded during this replay
    m_replayAfter.clear();
    m_pending.push_back(std::move(p));
    m_submitted = value;
    for (CommandList* l : info.lists) static_cast<D3D11CommandList*>(l)->setLastSubmit(value);
    drainMessages();
    return value;
}

void D3D11Device::afterReplay(std::function<void()> fn) { m_replayAfter.push_back(std::move(fn)); }

void D3D11Device::retire(std::uint64_t upTo) {
    while (!m_pending.empty() && m_pending.front().value <= upTo) {
        Pending p = std::move(m_pending.front());
        m_pending.erase(m_pending.begin());
        for (auto& fn : p.after) fn();
        m_completed = p.value;
    }
}

void D3D11Device::present(Swapchain* swapchain) { static_cast<D3D11Swapchain*>(swapchain)->present(); }

void D3D11Device::wait(QueueType /*queue*/, std::uint64_t value) {
    for (Pending& p : m_pending) {
        if (p.value > value) break;
        for (;;) {
            const HRESULT hr = m_context->GetData(p.event.get(), nullptr, 0, 0);
            if (hr == S_OK) break;
            if (FAILED(hr)) check(hr, "ID3D11DeviceContext::GetData(event)");
            std::this_thread::yield();
        }
    }
    retire(value);
}

bool D3D11Device::isComplete(QueueType /*queue*/, std::uint64_t value) {
    std::uint64_t done = m_completed;
    for (Pending& p : m_pending) {
        if (m_context->GetData(p.event.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        done = p.value;
    }
    retire(done);
    return value <= m_completed || m_pending.empty();
}

void D3D11Device::waitIdle() { wait(QueueType::Graphics, m_submitted); }

void D3D11Device::drainMessages() {
    if (!m_infoQueue) return;
    const UINT64 n = m_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        m_infoQueue->GetMessage(i, nullptr, &len);
        std::vector<std::byte> buf(len);
        auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
        if (FAILED(m_infoQueue->GetMessage(i, msg, &len))) continue;
        if (msg->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            ++m_validationCounts.errors;
            log::warn("[d3d11-debug] error: {}", std::string(msg->pDescription, msg->DescriptionByteLength));
        } else if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            ++m_validationCounts.warnings;
            log::debug("[d3d11-debug] {}", std::string(msg->pDescription, msg->DescriptionByteLength));
        }
    }
    m_infoQueue->ClearStoredMessages();
}

MemoryBudget D3D11Device::memoryBudget() const {
    MemoryBudget b;
    if (auto a3 = m_adapterDxgi.as<IDXGIAdapter3>()) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal))) {
            b.localUsage = local.CurrentUsage;
            b.localBudget = local.Budget;
            b.nonLocalUsage = nonLocal.CurrentUsage;
            b.nonLocalBudget = nonLocal.Budget;
            b.valid = true;
        }
    }
    return b;
}

std::uint32_t D3D11Device::renderTargetSampleCounts(Format format) const {
    std::uint32_t mask = 1;
    for (UINT s = 2; s <= 16; s *= 2) {
        UINT q = 0;
        if (SUCCEEDED(m_device->CheckMultisampleQualityLevels(toDxgi(format), s, &q)) && q > 0) mask |= s;
    }
    return mask;
}

// --- uploads and readbacks ------------------------------------------------------------------------------------------
void D3D11Device::uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) {
    auto* b = static_cast<D3D11Buffer*>(dst);
    if (b->mapped()) {
        std::memcpy(b->shadow() + dstOffset, data, size);
        return;
    }
    if (b->writePending(dstOffset, data, size)) return;
    if (any(b->desc().usage & BufferUsage::Uniform)) { // constant buffers update as a whole before 11.1
        if (dstOffset != 0 || size < b->desc().size)
            throw Error("uploadBuffer: Direct3D 11 constant buffers are uploaded whole");
        m_context->UpdateSubresource(b->gpu(), 0, nullptr, data, 0, 0);
        return;
    }
    const D3D11_BOX box{static_cast<UINT>(dstOffset), 0, 0, static_cast<UINT>(dstOffset + size), 1, 1};
    m_context->UpdateSubresource(b->gpu(), 0, &box, data, 0, 0);
}

void D3D11Device::uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                                std::uint32_t rowPitch) {
    auto* t = static_cast<D3D11Texture*>(dst);
    const FormatInfo& fi = formatInfo(t->desc().format);
    const std::uint32_t h = std::max(1u, t->desc().height >> mip);
    const UINT rows = (h + fi.blockDim - 1) / fi.blockDim;
    if (t->desc().type == TextureType::Tex3D) { // one depth slice
        const D3D11_BOX box{0, 0, layer, std::max(1u, t->desc().width >> mip), h, layer + 1};
        m_context->UpdateSubresource(t->resource(), t->subresource(mip, 0), &box, data, rowPitch, rowPitch * rows);
        return;
    }
    m_context->UpdateSubresource(t->resource(), t->subresource(mip, layer), nullptr, data, rowPitch, rowPitch * rows);
}

std::vector<std::uint8_t> D3D11Device::readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) {
    auto* t = static_cast<D3D11Texture*>(src);
    const TextureDesc& d = t->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const std::uint32_t w = std::max(1u, d.width >> mip), h = std::max(1u, d.height >> mip);
    if (d.sampleCount > 1) throw Error("readbackTexture: multisampled textures must be resolved first");
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = w;
    sd.Height = h;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = toDxgiResource(d.format);
    sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> stagingTex;
    check(m_device->CreateTexture2D(&sd, nullptr, stagingTex.put()), "CreateTexture2D(staging)");
    ID3D11Texture2D* staging = stagingTex.get();
    if (d.type == TextureType::Tex3D) {
        const D3D11_BOX box{0, 0, layer, w, h, layer + 1};
        m_context->CopySubresourceRegion(staging, 0, 0, 0, 0, t->resource(), t->subresource(mip, 0), &box);
    } else {
        m_context->CopySubresourceRegion(staging, 0, 0, 0, 0, t->resource(), t->subresource(mip, layer), nullptr);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    check(m_context->Map(staging, 0, D3D11_MAP_READ, 0, &m), "Map(staging texture)");
    const std::uint32_t tight = ((w + fi.blockDim - 1) / fi.blockDim) * fi.bytesPerBlock;
    const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(tight) * rows);
    for (std::uint32_t r = 0; r < rows; ++r)
        std::memcpy(out.data() + static_cast<std::size_t>(r) * tight,
                    static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(r) * m.RowPitch, tight);
    m_context->Unmap(staging, 0);
    return out;
}

} // namespace gt::rhi::d3d11
