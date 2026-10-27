#include "d3d12_device.h"

#include "gt/log.h"
#include "gt/shaders.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace gt::rhi::d3d12 {
namespace {

constexpr std::uint32_t kViewHeapCapacity = 262144;
constexpr std::uint32_t kViewBlock = 1024;
constexpr std::uint32_t kSamplerHeapCapacity = 2048;
constexpr std::uint32_t kSamplerBlock = 32;

std::uint64_t alignUp(std::uint64_t v, std::uint64_t a) { return a ? (v + a - 1) / a * a : v; }

template <typename T>
bool feature(ID3D12Device* dev, D3D12_FEATURE f, T& data) {
    return SUCCEEDED(dev->CheckFeatureSupport(f, &data, sizeof(T)));
}

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    default: return "11_0";
    }
}

std::string shaderModelName(D3D_SHADER_MODEL sm) {
    const int v = static_cast<int>(sm);
    return std::format("{}.{}", v >> 4, v & 0xF);
}

D3D_FEATURE_LEVEL maxFeatureLevel(ID3D12Device* dev) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    fl.NumFeatureLevels = static_cast<UINT>(std::size(levels));
    fl.pFeatureLevelsRequested = levels;
    return feature(dev, D3D12_FEATURE_FEATURE_LEVELS, fl) ? fl.MaxSupportedFeatureLevel : D3D_FEATURE_LEVEL_11_0;
}

D3D_SHADER_MODEL highestShaderModel(ID3D12Device* dev) {
    // Older runtimes reject shader models they do not know: ask from the newest down.
    for (D3D_SHADER_MODEL m : {D3D_SHADER_MODEL_6_9, D3D_SHADER_MODEL_6_8, D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6,
                               D3D_SHADER_MODEL_6_5, D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_2,
                               D3D_SHADER_MODEL_6_1, D3D_SHADER_MODEL_6_0}) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{m};
        if (feature(dev, D3D12_FEATURE_SHADER_MODEL, sm)) return sm.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_5_1;
}

/// "a.b.c.d" user-mode driver version; NVIDIA's is also shown in its usual "560.94" form.
std::string driverVersion(IDXGIAdapter1* adapter, std::uint32_t vendor) {
    LARGE_INTEGER v{};
    if (FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return "unknown";
    const auto q = static_cast<std::uint64_t>(v.QuadPart);
    const unsigned a = static_cast<unsigned>((q >> 48) & 0xFFFF), b = static_cast<unsigned>((q >> 32) & 0xFFFF),
                   c = static_cast<unsigned>((q >> 16) & 0xFFFF), d = static_cast<unsigned>(q & 0xFFFF);
    const std::string raw = std::format("{}.{}.{}.{}", a, b, c, d);
    if (vendor == vendor::kNvidia) {
        const unsigned nv = (c % 10) * 10000 + d;
        return std::format("{}.{:02} ({})", nv / 100, nv % 100, raw);
    }
    return raw;
}

struct FoundAdapter {
    Com<IDXGIAdapter1> adapter;
    AdapterInfo info;
};

/// Adapters that can create a Direct3D 12 device, in DXGI order.
std::vector<FoundAdapter> findAdapters(IDXGIFactory4* factory) {
    const Runtime& rt = runtime();
    std::vector<FoundAdapter> out;
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d))) continue;
        Com<ID3D12Device> dev;
        if (FAILED(rt.createDevice(a.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), dev.putVoid()))) continue;
        FoundAdapter f;
        f.adapter = a;
        AdapterInfo& ai = f.info;
        ai.api = Api::D3D12;
        ai.index = static_cast<std::uint32_t>(out.size());
        ai.name = narrow(d.Description);
        ai.vendorId = d.VendorId;
        ai.deviceId = d.DeviceId;
        ai.subsysId = d.SubSysId;
        ai.revision = d.Revision;
        D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
        feature(dev.get(), D3D12_FEATURE_ARCHITECTURE1, arch);
        ai.type = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == vendor::kMicrosoft ? AdapterType::Cpu
                  : arch.UMA                                                                   ? AdapterType::Integrated
                                                                                               : AdapterType::Discrete;
        ai.driverName = std::string(vendorName(d.VendorId));
        ai.driverVersion = driverVersion(a.get(), d.VendorId);
        ai.driverInfo = std::format("WDDM user-mode driver {}", ai.driverVersion);
        ai.apiVersion = std::format("FL {}, SM {}", featureLevelName(maxFeatureLevel(dev.get())),
                                    shaderModelName(highestShaderModel(dev.get())));
        ai.dedicatedMemory = d.DedicatedVideoMemory;
        ai.sharedMemory = d.SharedSystemMemory;
        std::array<std::uint8_t, 8> luid{};
        std::memcpy(luid.data(), &d.AdapterLuid, sizeof(luid));
        ai.luid = luid;
        out.push_back(std::move(f));
    }
    return out;
}

Com<IDXGIFactory4> createFactory(bool debug) {
    const Runtime& rt = runtime();
    Com<IDXGIFactory4> f;
    // The DXGI debug factory needs the "Graphics Tools" optional feature; fall back silently.
    if (debug && SUCCEEDED(rt.createDxgiFactory2(DXGI_CREATE_FACTORY_DEBUG, __uuidof(IDXGIFactory4), f.putVoid())))
        return f;
    check(rt.createDxgiFactory2(0, __uuidof(IDXGIFactory4), f.putVoid()), "CreateDXGIFactory2");
    return f;
}

bool ignoredMessage(D3D12_MESSAGE_ID id) {
    switch (id) {
    // Clears with a value other than the resource's optimized clear value are only slower.
    case D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE:
    case D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE: return true;
    default: return false;
    }
}

void __stdcall messageCallback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
                               LPCSTR description, void*) {
    if (ignoredMessage(id)) return;
    countValidationMessage(severity, description ? description : "");
}

} // namespace

// --- descriptor heaps ------------------------------------------------------------------------------
CpuDescriptorHeap::CpuDescriptorHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t perHeap)
    : m_dev(dev), m_type(type), m_perHeap(perHeap), m_increment(dev->GetDescriptorHandleIncrementSize(type)) {}

D3D12_CPU_DESCRIPTOR_HANDLE CpuDescriptorHeap::alloc() {
    std::lock_guard lock(m_mutex);
    if (!m_free.empty()) {
        const D3D12_CPU_DESCRIPTOR_HANDLE h = m_free.back();
        m_free.pop_back();
        return h;
    }
    if (m_heaps.empty() || m_used == m_perHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = m_type;
        d.NumDescriptors = m_perHeap;
        Com<ID3D12DescriptorHeap> heap;
        check(m_dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), heap.putVoid()), "CreateDescriptorHeap");
        m_heaps.push_back(std::move(heap));
        m_used = 0;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpuStart(m_heaps.back().get());
    h.ptr += static_cast<SIZE_T>(m_used++) * m_increment;
    return h;
}

void CpuDescriptorHeap::free(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr) return;
    std::lock_guard lock(m_mutex);
    m_free.push_back(handle);
}

GpuDescriptorHeap::GpuDescriptorHeap(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t capacity,
                                     std::uint32_t blockSize)
    : m_capacity(capacity), m_blockSize(blockSize), m_rangeBase(capacity) {
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = type;
    d.NumDescriptors = capacity;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(dev->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), m_heap.putVoid()), "CreateDescriptorHeap");
    m_cpuStart = cpuStart(m_heap.get());
    m_gpuStart = gpuStart(m_heap.get());
    m_increment = dev->GetDescriptorHandleIncrementSize(type);
}

std::uint32_t GpuDescriptorHeap::allocBlock() {
    std::lock_guard lock(m_mutex);
    if (!m_freeBlocks.empty()) {
        const std::uint32_t b = m_freeBlocks.back();
        m_freeBlocks.pop_back();
        return b;
    }
    if (m_blockTop + m_blockSize > m_rangeBase)
        throw Error(std::format("shader-visible descriptor heap exhausted ({} descriptors)", m_capacity));
    const std::uint32_t b = m_blockTop;
    m_blockTop += m_blockSize;
    return b;
}

void GpuDescriptorHeap::freeBlock(std::uint32_t first) {
    std::lock_guard lock(m_mutex);
    m_freeBlocks.push_back(first);
}

std::uint32_t GpuDescriptorHeap::allocRange(std::uint32_t count) {
    std::lock_guard lock(m_mutex);
    for (auto it = m_freeRanges.begin(); it != m_freeRanges.end(); ++it) {
        if (it->second < count) continue;
        const std::uint32_t first = it->first;
        it->first += count;
        it->second -= count;
        if (it->second == 0) m_freeRanges.erase(it);
        return first;
    }
    if (m_rangeBase < m_blockTop + count)
        throw Error(std::format("shader-visible descriptor heap exhausted ({} descriptors)", m_capacity));
    m_rangeBase -= count;
    return m_rangeBase;
}

void GpuDescriptorHeap::freeRange(std::uint32_t first, std::uint32_t count) {
    std::lock_guard lock(m_mutex);
    m_freeRanges.emplace_back(first, count);
}

D3D12_CPU_DESCRIPTOR_HANDLE GpuDescriptorHeap::cpu(std::uint32_t index) const {
    return {m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_increment};
}

D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptorHeap::gpu(std::uint32_t index) const {
    return {m_gpuStart.ptr + static_cast<UINT64>(index) * m_increment};
}

// --- adapters ------------------------------------------------------------------------------------
std::vector<AdapterInfo> enumerateAdapters(std::string* error) {
    try {
        const Runtime& rt = runtime();
        if (!rt.error.empty()) {
            if (error) *error = rt.error;
            return {};
        }
        std::vector<AdapterInfo> out;
        for (auto& f : findAdapters(createFactory(false).get())) out.push_back(f.info);
        if (out.empty() && error) *error = "no adapter supports Direct3D 12";
        return out;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return {};
    }
}

// --- device ----------------------------------------------------------------------------------------
D3D12Device::D3D12Device(const DeviceCreateInfo& info) {
    m_api = Api::D3D12;
    const Runtime& rt = runtime();
    if (!rt.error.empty()) throw Error(rt.error);

    if (info.validation) {
        Com<ID3D12Debug> debug;
        if (rt.getDebugInterface && SUCCEEDED(rt.getDebugInterface(__uuidof(ID3D12Debug), debug.putVoid()))) {
            debug->EnableDebugLayer();
            if (info.gpuAssisted)
                if (auto d1 = debug.as<ID3D12Debug1>()) d1->SetEnableGPUBasedValidation(TRUE);
            m_validation = true;
        } else {
            log::warn("Direct3D 12 debug layer unavailable (install the Graphics Tools optional feature)");
        }
    }
    // Device-removed extended data: breadcrumbs and page-fault details for device-loss reports.
    if (rt.getDebugInterface) {
        Com<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(rt.getDebugInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), dred.putVoid()))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    m_factory = createFactory(m_validation);
    auto adapters = findAdapters(m_factory.get());
    if (info.adapterIndex >= adapters.size())
        throw Error(std::format("Direct3D 12 adapter index {} out of range ({} adapters)", info.adapterIndex,
                                adapters.size()));
    m_adapterDxgi = adapters[info.adapterIndex].adapter;
    m_adapter = adapters[info.adapterIndex].info;
    check(rt.createDevice(m_adapterDxgi.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), m_device.putVoid()),
          "D3D12CreateDevice");
    m_device5 = m_device.as<ID3D12Device5>();
    setName(m_device.get(), "kiln-device");
    if (m_validation) installMessageCallback();

    m_cpuHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] =
        std::make_unique<CpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024);
    m_cpuHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] =
        std::make_unique<CpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256);
    m_cpuHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV] =
        std::make_unique<CpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256);
    m_cpuHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV] =
        std::make_unique<CpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64);
    m_viewHeap = std::make_unique<GpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                     kViewHeapCapacity, kViewBlock);
    m_samplerHeap = std::make_unique<GpuDescriptorHeap>(m_device.get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                                        kSamplerHeapCapacity, kSamplerBlock);
    createQueues();
    fillFeatures();
    log::info("Direct3D 12 device: {} ({}, driver {}, {}){}", m_adapter.name, m_adapter.driverName,
              m_adapter.driverVersion, m_adapter.apiVersion, m_validation ? ", debug layer on" : "");
}

D3D12Device::~D3D12Device() {
    try {
        waitIdle();
    } catch (const std::exception& e) {
        log::warn("Direct3D 12 device shutdown: {}", e.what());
    }
    m_uploadList.reset();
    for (auto& g : m_garbage) g.fn();
    m_garbage.clear();
    if (m_callbackCookie)
        if (auto q1 = m_infoQueue.as<ID3D12InfoQueue1>()) q1->UnregisterMessageCallback(m_callbackCookie);
    drainMessages();
    for (auto& q : m_queues)
        if (q.event) CloseHandle(q.event);
}

void D3D12Device::installMessageCallback() {
    m_infoQueue = m_device.as<ID3D12InfoQueue>();
    if (!m_infoQueue) return;
    D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
                               D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE};
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumIDs = static_cast<UINT>(std::size(deny));
    filter.DenyList.pIDList = deny;
    m_infoQueue->PushStorageFilter(&filter);
    if (auto q1 = m_infoQueue.as<ID3D12InfoQueue1>())
        q1->RegisterMessageCallback(messageCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &m_callbackCookie);
}

void D3D12Device::drainMessages() {
    if (!m_infoQueue || m_callbackCookie) return; // the callback already saw every message
    const UINT64 n = m_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T size = 0;
        if (FAILED(m_infoQueue->GetMessage(i, nullptr, &size)) || size == 0) continue;
        std::vector<std::uint8_t> buf(size);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (SUCCEEDED(m_infoQueue->GetMessage(i, msg, &size)) && !ignoredMessage(msg->ID))
            countValidationMessage(msg->Severity, msg->pDescription ? msg->pDescription : "");
    }
    m_infoQueue->ClearStoredMessages();
}

void D3D12Device::createQueues() {
    const D3D12_COMMAND_LIST_TYPE types[3] = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              D3D12_COMMAND_LIST_TYPE_COPY};
    const char* names[3] = {"kiln-graphics-queue", "kiln-compute-queue", "kiln-copy-queue"};
    for (std::size_t i = 0; i < 3; ++i) {
        Queue& q = m_queues[i];
        D3D12_COMMAND_QUEUE_DESC d{};
        d.Type = types[i];
        check(m_device->CreateCommandQueue(&d, __uuidof(ID3D12CommandQueue), q.queue.putVoid()), "CreateCommandQueue");
        check(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), q.fence.putVoid()), "CreateFence");
        q.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!q.event) throw Error("CreateEvent failed");
        UINT64 freq = 0;
        if (SUCCEEDED(q.queue->GetTimestampFrequency(&freq))) q.frequency = freq;
        setName(q.queue.get(), names[i]);
    }
}

void D3D12Device::fillFeatures() {
    ID3D12Device* d = m_device.get();
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    feature(d, D3D12_FEATURE_D3D12_OPTIONS, o);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS1, o1);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS3, o3);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS4, o4);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS5, o5);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS6, o6);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS7, o7);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS16, o16);
    m_featureLevel = maxFeatureLevel(d);
    m_shaderModel = highestShaderModel(d);
    m_rtTier = o5.RaytracingTier;
    m_bindingTier = o.ResourceBindingTier;
    m_gpuUploadHeap = o16.GPUUploadHeapSupported;
    m_copyTimestamps = o3.CopyQueueTimestampQueriesSupported && m_queues[2].frequency > 0;
    BOOL tearing = FALSE;
    if (auto f5 = m_factory.as<IDXGIFactory5>())
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            m_tearing = tearing;

    const int sm = static_cast<int>(m_shaderModel);
    DeviceFeatures& f = m_features;
    f.timestamps = m_queues[0].frequency > 0;
    f.timestampsOnComputeQueue = m_queues[1].frequency > 0;
    f.asyncComputeQueue = true;
    f.copyQueue = true;
    f.geometryShader = true;
    f.tessellation = true;
    f.wireframe = true;
    f.anisotropicFiltering = true;
    f.textureCompressionBC = true;
    f.shaderFloat64 = o.DoublePrecisionFloatShaderOps;
    f.shaderFloat16 = o4.Native16BitShaderOpsSupported;
    f.shaderInt16 = o4.Native16BitShaderOpsSupported;
    f.storage16Bit = o4.Native16BitShaderOpsSupported;
    f.shaderInt64 = o1.Int64ShaderOps;
    f.shaderInt8 = sm >= 0x64;
    f.integerDotProduct = sm >= 0x64;
    f.subgroupOps = o1.WaveOps;
    f.subgroupSize = o1.WaveLaneCountMin;
    f.minSubgroupSize = o1.WaveLaneCountMin;
    f.maxSubgroupSize = o1.WaveLaneCountMax;
    f.atomicInt64 = sm >= 0x66 && o1.Int64ShaderOps;
    f.descriptorIndexing = m_bindingTier >= D3D12_RESOURCE_BINDING_TIER_2;
    f.maxBindlessTextures = f.descriptorIndexing ? 65536u : 0u;
    f.rayQuery = m_rtTier >= D3D12_RAYTRACING_TIER_1_1 && m_device5;
    f.meshShader = o7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 && sm >= 0x65;
    f.taskShader = f.meshShader;
    f.variableRateShading = o6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1;
    f.maxShadingRate = f.variableRateShading ? (o6.AdditionalShadingRatesSupported ? 4u : 2u) : 1u;
    f.multiDrawIndirect = true;
    f.drawIndirectCount = true;
    f.pipelineStatistics = true;
    f.memoryBudget = static_cast<bool>(m_adapterDxgi.as<IDXGIAdapter3>());
    f.resizableBar = m_gpuUploadHeap;
    f.maxTexture2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    f.maxMsaaSamples = 1;
    for (std::uint32_t s : {2u, 4u, 8u, 16u})
        if (renderTargetSampleCounts(Format::RGBA8_UNORM) & s) f.maxMsaaSamples = s;
    f.maxComputeSharedMemory = D3D12_CS_TGSM_REGISTER_COUNT * 4;
    f.maxComputeWorkgroupSize = {D3D12_CS_THREAD_GROUP_MAX_X, D3D12_CS_THREAD_GROUP_MAX_Y, D3D12_CS_THREAD_GROUP_MAX_Z};
    f.maxComputeWorkgroupInvocations = D3D12_CS_THREAD_GROUP_MAX_THREADS_PER_GROUP;
    f.maxBufferSize = 1ull << std::min<UINT>(40, std::max<UINT>(31, o.MaxGPUVirtualAddressBitsPerResource));
    f.maxStorageBufferRange = 1ull << 31;
    f.timestampPeriodNs = m_queues[0].frequency ? 1e9 / static_cast<double>(m_queues[0].frequency) : 0.0;
}

// --- queues and submission ------------------------------------------------------------------------
void D3D12Device::check(HRESULT hr, const char* what) const {
    if (SUCCEEDED(hr)) return;
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
        const HRESULT reason = m_device->GetDeviceRemovedReason();
        throw DeviceLostError(std::format("{} failed: {} (reason {})", what, hresultName(hr), hresultName(reason)));
    }
    d3d12::check(hr, what);
}

std::uint64_t D3D12Device::poll(Queue& q) {
    const UINT64 v = q.fence->GetCompletedValue();
    if (v == UINT64_MAX) check(DXGI_ERROR_DEVICE_REMOVED, "ID3D12Fence::GetCompletedValue");
    q.completed = std::max<std::uint64_t>(q.completed, v);
    return q.completed;
}

std::uint64_t D3D12Device::submit(QueueType type, const SubmitInfo& info) {
    Queue& q = m_queues[static_cast<std::size_t>(type)];
    for (const QueueWait& w : info.waits) {
        if (w.queue == type) continue; // same queue: ordered already
        Queue& other = m_queues[static_cast<std::size_t>(w.queue)];
        check(q.queue->Wait(other.fence.get(), w.value), "ID3D12CommandQueue::Wait");
    }
    // One ExecuteCommandLists per list: buffers and simultaneous-access textures decay to COMMON
    // between calls, which is what the command lists' state tracking assumes.
    for (CommandList* l : info.lists) {
        ID3D12CommandList* cl = static_cast<D3D12CommandList*>(l)->handle();
        q.queue->ExecuteCommandLists(1, &cl);
    }
    const std::uint64_t value = q.submitted + 1;
    check(q.queue->Signal(q.fence.get(), value), "ID3D12CommandQueue::Signal");
    q.submitted = value;
    for (CommandList* l : info.lists) static_cast<D3D12CommandList*>(l)->setLastSubmit(value);
    collectGarbage();
    drainMessages();
    return value;
}

void D3D12Device::present(Swapchain* swapchain) {
    static_cast<D3D12Swapchain*>(swapchain)->present();
    drainMessages();
}

void D3D12Device::wait(QueueType type, std::uint64_t value) {
    Queue& q = m_queues[static_cast<std::size_t>(type)];
    if (value <= q.completed || value <= poll(q)) return;
    check(q.fence->SetEventOnCompletion(value, q.event), "ID3D12Fence::SetEventOnCompletion");
    WaitForSingleObject(q.event, INFINITE);
    poll(q);
}

bool D3D12Device::isComplete(QueueType type, std::uint64_t value) {
    Queue& q = m_queues[static_cast<std::size_t>(type)];
    return value <= q.completed || value <= poll(q);
}

void D3D12Device::waitIdle() {
    for (std::size_t i = 0; i < 3; ++i) wait(static_cast<QueueType>(i), m_queues[i].submitted);
    collectGarbage();
    drainMessages();
}

void D3D12Device::deferDestroy(std::function<void()> fn) {
    std::lock_guard lock(m_garbageMutex);
    Garbage g;
    for (std::size_t i = 0; i < 3; ++i) g.values[i] = m_queues[i].submitted;
    g.fn = std::move(fn);
    m_garbage.push_back(std::move(g));
}

void D3D12Device::collectGarbage() {
    std::array<std::uint64_t, 3> done{};
    for (std::size_t i = 0; i < 3; ++i) done[i] = poll(m_queues[i]);
    std::deque<Garbage> ready;
    {
        std::lock_guard lock(m_garbageMutex);
        while (!m_garbage.empty()) {
            const auto& g = m_garbage.front();
            if (g.values[0] > done[0] || g.values[1] > done[1] || g.values[2] > done[2]) break;
            ready.push_back(std::move(m_garbage.front()));
            m_garbage.pop_front();
        }
    }
    for (auto& g : ready) g.fn();
}

// --- factories ---------------------------------------------------------------------------------------
std::unique_ptr<Buffer> D3D12Device::createBuffer(const BufferDesc& desc, const void* initialData) {
    auto b = std::make_unique<D3D12Buffer>(*this, desc);
    if (initialData) {
        if (b->mapped()) std::memcpy(b->mapped(), initialData, desc.size);
        else uploadBuffer(b.get(), initialData, desc.size, 0);
    }
    return b;
}
std::unique_ptr<Texture> D3D12Device::createTexture(const TextureDesc& desc) {
    return std::make_unique<D3D12Texture>(*this, desc);
}
std::unique_ptr<Sampler> D3D12Device::createSampler(const SamplerDesc& desc) {
    return std::make_unique<D3D12Sampler>(*this, desc);
}
std::unique_ptr<Pipeline> D3D12Device::createGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    return std::make_unique<D3D12Pipeline>(*this, desc);
}
std::unique_ptr<Pipeline> D3D12Device::createComputePipeline(const ComputePipelineDesc& desc) {
    return std::make_unique<D3D12Pipeline>(*this, desc);
}
std::unique_ptr<TextureTable> D3D12Device::createTextureTable(std::uint32_t capacity) {
    if (!m_features.descriptorIndexing) throw Error("texture tables need resource binding tier 2");
    return std::make_unique<D3D12TextureTable>(*this, capacity);
}
std::unique_ptr<QueryPool> D3D12Device::createQueryPool(QueryType type, std::uint32_t count) {
    return std::make_unique<D3D12QueryPool>(*this, type, count);
}
std::unique_ptr<CommandList> D3D12Device::createCommandList(QueueType queue) {
    return std::make_unique<D3D12CommandList>(*this, queue);
}
std::unique_ptr<Swapchain> D3D12Device::createSwapchain(const SwapchainDesc& desc) {
    return std::make_unique<D3D12Swapchain>(*this, desc);
}

std::span<const std::uint8_t> D3D12Device::shaderBytecode(const ShaderRef& ref) const {
    if (!ref.bytecode.empty()) return ref.bytecode;
    auto blob = shaders::find(ref.module, ref.entry, shaders::Format::Dxil);
    if (!blob) throw Error(std::format("DXIL for shader {}:{} is not embedded in this build", ref.module, ref.entry));
    return blob.bytes();
}

void D3D12Device::setName(ID3D12Object* object, const char* name) const {
    if (object && name && *name) object->SetName(widen(name).c_str());
}

void D3D12Device::setDebugName(Buffer* b, const char* name) {
    setName(static_cast<D3D12Buffer*>(b)->resource(), name);
}

void D3D12Device::setDebugName(Texture* t, const char* name) {
    setName(static_cast<D3D12Texture*>(t)->resource(), name);
}

// --- root signatures ---------------------------------------------------------------------------------
const RootSignature& D3D12Device::rootSignature(const LayoutDesc& layout, bool inputAssembler) {
    std::string key = std::format("{}|{}|{}|", inputAssembler, layout.pushConstantBytes, layout.textureTable);
    for (const Binding& b : layout.bindings) key += std::format("{}:{},", static_cast<int>(b.type), b.slot);
    std::lock_guard lock(m_rootMutex);
    if (auto it = m_rootSignatures.find(key); it != m_rootSignatures.end()) return *it->second;

    auto rs = std::make_unique<RootSignature>();
    RootLayout& L = rs->layout;
    std::vector<Binding> rootDescs, views, samplers;
    for (const Binding& b : layout.bindings) {
        switch (b.type) {
        case BindingType::Sampler: samplers.push_back(b); break;
        case BindingType::SampledTexture:
        case BindingType::StorageTexture: views.push_back(b); break;
        default: rootDescs.push_back(b); break;
        }
    }
    L.pushConstantDwords = (layout.pushConstantBytes + 3) / 4;
    // Root signatures hold 64 DWORDs: constants 1 each, root descriptors 2, tables 1.
    auto cost = [&] {
        return L.pushConstantDwords + 2 * static_cast<std::uint32_t>(rootDescs.size()) + (views.empty() ? 0u : 1u) +
               (samplers.empty() ? 0u : 1u) + (layout.textureTable ? 1u : 0u);
    };
    while (cost() > D3D12_MAX_ROOT_COST && !rootDescs.empty()) {
        views.push_back(rootDescs.back());
        rootDescs.pop_back();
    }
    if (cost() > D3D12_MAX_ROOT_COST) throw Error("pipeline layout does not fit a Direct3D 12 root signature");

    std::vector<D3D12_ROOT_PARAMETER1> params;
    std::vector<D3D12_DESCRIPTOR_RANGE1> viewRanges, samplerRanges, tableRange;
    if (L.pushConstantDwords) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants = {13, 0, L.pushConstantDwords};
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        L.pushParam = static_cast<int>(params.size());
        params.push_back(p);
    }
    for (const Binding& b : rootDescs) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = b.type == BindingType::UniformBuffer     ? D3D12_ROOT_PARAMETER_TYPE_CBV
                          : b.type == BindingType::StorageBufferRW ? D3D12_ROOT_PARAMETER_TYPE_UAV
                                                                   : D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor = {b.slot, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        L.rootDescriptors.push_back({b.type, b.slot, static_cast<std::uint32_t>(params.size())});
        params.push_back(p);
    }
    for (const Binding& b : views) {
        D3D12_DESCRIPTOR_RANGE1 r{};
        r.RangeType = b.type == BindingType::UniformBuffer ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                      : b.type == BindingType::StorageTexture || b.type == BindingType::StorageBufferRW
                          ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV
                          : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = b.slot;
        r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        r.OffsetInDescriptorsFromTableStart = static_cast<UINT>(viewRanges.size());
        L.viewTable.push_back({b.type, b.slot, static_cast<std::uint32_t>(viewRanges.size())});
        viewRanges.push_back(r);
    }
    for (const Binding& b : samplers) {
        D3D12_DESCRIPTOR_RANGE1 r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = b.slot;
        r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        r.OffsetInDescriptorsFromTableStart = static_cast<UINT>(samplerRanges.size());
        L.samplerTable.push_back({b.type, b.slot, static_cast<std::uint32_t>(samplerRanges.size())});
        samplerRanges.push_back(r);
    }
    auto addTable = [&](std::vector<D3D12_DESCRIPTOR_RANGE1>& ranges) {
        D3D12_ROOT_PARAMETER1 p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
        return static_cast<int>(params.size()) - 1;
    };
    if (!viewRanges.empty()) L.viewTableParam = addTable(viewRanges);
    if (!samplerRanges.empty()) L.samplerTableParam = addTable(samplerRanges);
    if (layout.textureTable) {
        D3D12_DESCRIPTOR_RANGE1 r{};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = UINT_MAX;
        r.BaseShaderRegister = 0;
        r.RegisterSpace = 1;
        r.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        tableRange.push_back(r);
        L.textureTableParam = addTable(tableRange);
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
    desc.Desc_1_1.pParameters = params.data();
    desc.Desc_1_1.Flags =
        inputAssembler ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    Com<ID3DBlob> blob, errors;
    const HRESULT hr = runtime().serializeRootSignature(&desc, blob.put(), errors.put());
    if (FAILED(hr)) {
        std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                                 : hresultName(hr);
        throw Error("D3D12SerializeVersionedRootSignature failed: " + msg);
    }
    check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
                                        rs->signature.putVoid()),
          "CreateRootSignature");
    return *m_rootSignatures.emplace(key, std::move(rs)).first->second;
}

ID3D12CommandSignature* D3D12Device::commandSignature(D3D12_INDIRECT_ARGUMENT_TYPE type, std::uint32_t stride) {
    std::uint32_t argSize = 0;
    switch (type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: argSize = sizeof(D3D12_DRAW_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: argSize = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS); break;
    case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: argSize = sizeof(D3D12_DISPATCH_ARGUMENTS); break;
    default: argSize = sizeof(D3D12_DISPATCH_MESH_ARGUMENTS); break;
    }
    stride = std::max(stride, argSize);
    std::lock_guard lock(m_rootMutex);
    auto& sig = m_commandSignatures[{static_cast<int>(type), stride}];
    if (!sig) {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = type;
        D3D12_COMMAND_SIGNATURE_DESC d{};
        d.ByteStride = stride;
        d.NumArgumentDescs = 1;
        d.pArgumentDescs = &arg;
        check(m_device->CreateCommandSignature(&d, nullptr, __uuidof(ID3D12CommandSignature), sig.putVoid()),
              "CreateCommandSignature");
    }
    return sig.get();
}

// --- uploads / readback ------------------------------------------------------------------------------
void D3D12Device::uploadBuffer(Buffer* dst, const void* data, std::uint64_t size, std::uint64_t dstOffset) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    D3D12Buffer staging(*this, BufferDesc{size, BufferUsage::CopySrc, MemoryType::Upload, 0, "staging"});
    std::memcpy(staging.mapped(), data, size);
    m_uploadList->begin();
    m_uploadList->copyBuffer(dst, dstOffset, &staging, 0, size);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    wait(QueueType::Graphics, submit(QueueType::Graphics, SubmitInfo{lists}));
}

void D3D12Device::uploadTexture(Texture* dst, std::uint32_t mip, std::uint32_t layer, const void* data,
                                std::uint32_t rowPitch) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    const auto& d = dst->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const std::uint32_t h = std::max(1u, d.height >> mip);
    const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
    const std::uint32_t pitch = static_cast<std::uint32_t>(alignUp(rowPitch, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    D3D12Buffer staging(*this, BufferDesc{static_cast<std::uint64_t>(pitch) * rows, BufferUsage::CopySrc,
                                          MemoryType::Upload, 0, "staging"});
    for (std::uint32_t r = 0; r < rows; ++r)
        std::memcpy(static_cast<std::uint8_t*>(staging.mapped()) + static_cast<std::size_t>(r) * pitch,
                    static_cast<const std::uint8_t*>(data) + static_cast<std::size_t>(r) * rowPitch, rowPitch);
    m_uploadList->begin();
    m_uploadList->copyBufferToTexture(dst, mip, layer, &staging, 0, pitch);
    m_uploadList->transition(dst, ResourceState::ShaderRead);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    wait(QueueType::Graphics, submit(QueueType::Graphics, SubmitInfo{lists}));
}

std::vector<std::uint8_t> D3D12Device::readbackTexture(Texture* src, std::uint32_t mip, std::uint32_t layer) {
    if (!m_uploadList) m_uploadList = createCommandList(QueueType::Graphics);
    const auto& d = src->desc();
    const FormatInfo& fi = formatInfo(d.format);
    const std::uint32_t w = std::max(1u, d.width >> mip);
    const std::uint32_t h = std::max(1u, d.height >> mip);
    const std::uint32_t tight = ((w + fi.blockDim - 1) / fi.blockDim) * fi.bytesPerBlock;
    const std::uint32_t pitch = static_cast<std::uint32_t>(alignUp(tight, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const std::uint32_t rows = (h + fi.blockDim - 1) / fi.blockDim;
    D3D12Buffer rb(*this, BufferDesc{static_cast<std::uint64_t>(pitch) * rows, BufferUsage::CopyDst,
                                     MemoryType::Readback, 0, "readback"});
    auto* tex = static_cast<D3D12Texture*>(src);
    const ResourceState prev = tex->state;
    m_uploadList->begin();
    m_uploadList->copyTextureToBuffer(&rb, 0, pitch, src, mip, layer);
    if (prev != ResourceState::Undefined) m_uploadList->transition(src, prev);
    m_uploadList->end();
    CommandList* lists[] = {m_uploadList.get()};
    wait(QueueType::Graphics, submit(QueueType::Graphics, SubmitInfo{lists}));
    std::vector<std::uint8_t> out(static_cast<std::size_t>(tight) * rows);
    for (std::uint32_t r = 0; r < rows; ++r)
        std::memcpy(out.data() + static_cast<std::size_t>(r) * tight,
                    static_cast<const std::uint8_t*>(rb.mapped()) + static_cast<std::size_t>(r) * pitch, tight);
    return out;
}

// --- queries about the device --------------------------------------------------------------------------
std::uint32_t D3D12Device::renderTargetSampleCounts(Format format) const {
    std::uint32_t mask = 1;
    for (std::uint32_t s = 2; s <= 16; s *= 2) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
        q.Format = toDxgi(format);
        q.SampleCount = s;
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
            q.NumQualityLevels > 0)
            mask |= s;
    }
    return mask;
}

MemoryBudget D3D12Device::memoryBudget() const {
    MemoryBudget b;
    auto a3 = m_adapterDxgi.as<IDXGIAdapter3>();
    if (!a3) return b;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
    if (FAILED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) return b;
    a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    b.localUsage = local.CurrentUsage;
    b.localBudget = local.Budget;
    b.nonLocalUsage = nonLocal.CurrentUsage;
    b.nonLocalBudget = nonLocal.Budget;
    b.valid = true;
    return b;
}

DeviceFaultReport D3D12Device::deviceFaultReport() {
    DeviceFaultReport r;
    r.supported = true;
    r.description = std::format("device removed: {}", hresultName(m_device->GetDeviceRemovedReason()));
    auto dred = m_device.as<ID3D12DeviceRemovedExtendedData>();
    if (!dred) return r;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        int listed = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && listed < 16; n = n->pNext) {
            if (!n->pLastBreadcrumbValue || *n->pLastBreadcrumbValue >= n->BreadcrumbCount) continue; // finished
            const UINT last = *n->pLastBreadcrumbValue;
            r.details.push_back(std::format("command list '{}' on '{}' stopped after operation {} of {} (op code {})",
                                            n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?",
                                            n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "?", last,
                                            n->BreadcrumbCount, static_cast<int>(n->pCommandHistory[last])));
            ++listed;
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA) {
        r.details.push_back(std::format("page fault at GPU address 0x{:x}", pf.PageFaultVA));
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
            r.details.push_back(std::format("  allocation at that address: {}", a->ObjectNameA ? a->ObjectNameA : "?"));
    }
    return r;
}

std::vector<Capability> D3D12Device::capabilities() const {
    std::vector<Capability> c;
    auto add = [&](const char* group, std::string name, std::string value, bool supported = true) {
        c.push_back({group, std::move(name), std::move(value), supported});
    };
    auto yes = [&](const char* group, const char* name, bool v) { add(group, name, v ? "yes" : "no", v); };
    const AdapterInfo& a = m_adapter;
    add("Adapter", "Name", a.name);
    add("Adapter", "Vendor ID", std::format("0x{:04X} ({})", a.vendorId, vendorName(a.vendorId)));
    add("Adapter", "Device ID", std::format("0x{:04X}", a.deviceId));
    add("Adapter", "Subsystem ID", std::format("0x{:08X}", a.subsysId));
    add("Adapter", "Revision", std::to_string(a.revision));
    add("Adapter", "Type", std::string(adapterTypeName(a.type)));
    add("Adapter", "Driver version", a.driverVersion);
    add("Adapter", "Dedicated video memory", std::format("{} MiB", a.dedicatedMemory >> 20));
    add("Adapter", "Shared system memory", std::format("{} MiB", a.sharedMemory >> 20));

    ID3D12Device* d = m_device.get();
    const char* g = "Direct3D 12";
    add(g, "Feature level", featureLevelName(m_featureLevel));
    add(g, "Shader model", shaderModelName(m_shaderModel));
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 o22{};
    D3D12_FEATURE_DATA_ARCHITECTURE1 arch{};
    feature(d, D3D12_FEATURE_D3D12_OPTIONS, o);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS1, o1);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS3, o3);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS4, o4);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS5, o5);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS6, o6);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS7, o7);
    feature(d, D3D12_FEATURE_D3D12_OPTIONS9, o9);
    const bool has12 = feature(d, D3D12_FEATURE_D3D12_OPTIONS12, o12);
    const bool has16 = feature(d, D3D12_FEATURE_D3D12_OPTIONS16, o16);
    const bool has21 = feature(d, D3D12_FEATURE_D3D12_OPTIONS21, o21);
    const bool has22 = feature(d, D3D12_FEATURE_D3D12_OPTIONS22, o22);
    feature(d, D3D12_FEATURE_ARCHITECTURE1, arch);
    auto tier = [](int t) { return t ? std::format("tier {}", t) : std::string("not supported"); };
    auto tier10 = [](int t) { return t ? std::format("tier {}.{}", t / 10, t % 10) : std::string("not supported"); };
    add(g, "Resource binding tier", tier(o.ResourceBindingTier));
    add(g, "Resource heap tier", tier(o.ResourceHeapTier));
    add(g, "Tiled resources tier", tier(o.TiledResourcesTier));
    add(g, "Conservative rasterization tier", tier(o.ConservativeRasterizationTier));
    yes(g, "Rasterizer ordered views", o.ROVsSupported);
    yes(g, "Typed UAV load (additional formats)", o.TypedUAVLoadAdditionalFormats);
    yes(g, "Double-precision shader ops", o.DoublePrecisionFloatShaderOps);
    yes(g, "Wave operations", o1.WaveOps);
    add(g, "Wave lane count", std::format("{}-{}", o1.WaveLaneCountMin, o1.WaveLaneCountMax));
    yes(g, "64-bit integer shader ops", o1.Int64ShaderOps);
    yes(g, "Native 16-bit shader ops", o4.Native16BitShaderOpsSupported);
    yes(g, "Barycentrics", o3.BarycentricsSupported);
    yes(g, "Copy queue timestamps", o3.CopyQueueTimestampQueriesSupported);
    add(g, "View instancing tier", tier(o3.ViewInstancingTier));
    add(g, "Raytracing tier", tier10(o5.RaytracingTier));
    add(g, "Variable shading rate tier", tier(o6.VariableShadingRateTier));
    yes(g, "Additional shading rates (4x4)", o6.AdditionalShadingRatesSupported);
    add(g, "Mesh shader tier", tier(o7.MeshShaderTier));
    add(g, "Sampler feedback tier", tier10(o7.SamplerFeedbackTier));
    yes(g, "64-bit atomics on typed resources", o9.AtomicInt64OnTypedResourceSupported);
    yes(g, "64-bit atomics on group-shared memory", o9.AtomicInt64OnGroupSharedSupported);
    add(g, "Wave matrix tier", tier10(o9.WaveMMATier));
    if (has12) yes(g, "Enhanced barriers", o12.EnhancedBarriersSupported);
    if (has16) yes(g, "GPU upload heaps (ReBAR)", o16.GPUUploadHeapSupported);
    if (has21) add(g, "Work graphs tier", tier10(o21.WorkGraphsTier));
    if (has21) add(g, "ExecuteIndirect tier", tier10(o21.ExecuteIndirectTier));
    if (has22) yes(g, "Shader execution reordering actually reorders", o22.ShaderExecutionReorderingActuallyReorders);
    yes(g, "Unified memory architecture", arch.UMA);
    yes(g, "Cache-coherent UMA", arch.CacheCoherentUMA);
    yes(g, "Tearing (variable refresh) presentation", m_tearing);
    add("Limits", "Max 2D texture size", std::to_string(m_features.maxTexture2D));
    add("Limits", "Max MSAA samples (RGBA8)", std::to_string(m_features.maxMsaaSamples));
    add("Limits", "GPU virtual address bits per resource", std::to_string(o.MaxGPUVirtualAddressBitsPerResource));
    add("Limits", "Timestamp frequency (graphics queue)", std::format("{} Hz", m_queues[0].frequency));
    return c;
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) { return std::make_unique<D3D12Device>(info); }

} // namespace gt::rhi::d3d12
