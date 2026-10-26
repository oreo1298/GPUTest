#include "sysreport.h"

#include "gt/sysinfo.h"

#include <format>

namespace gt::runner {

int defaultAdapter(const std::vector<rhi::AdapterInfo>& adapters) {
    for (auto t : {rhi::AdapterType::Discrete, rhi::AdapterType::Integrated}) {
        for (const auto& a : adapters)
            if (a.type == t && !a.isSoftware()) return static_cast<int>(a.index);
    }
    for (const auto& a : adapters)
        if (!a.isSoftware()) return static_cast<int>(a.index);
    return adapters.empty() ? -1 : static_cast<int>(adapters.front().index);
}

rhi::Api defaultApi() {
    for (rhi::Api api : {rhi::Api::Vulkan, rhi::Api::D3D12, rhi::Api::D3D11, rhi::Api::OpenGL}) {
        const auto compiled = rhi::compiledApis();
        if (std::find(compiled.begin(), compiled.end(), api) == compiled.end()) continue;
        if (!rhi::enumerateAdapters(api).empty()) return api;
    }
    return rhi::Api::Vulkan;
}

nlohmann::json adapterJson(const rhi::AdapterInfo& a) {
    nlohmann::json j{{"index", a.index},
                     {"api", rhi::apiName(a.api)},
                     {"name", a.name},
                     {"vendor", rhi::vendorName(a.vendorId)},
                     {"vendor_id", std::format("0x{:04x}", a.vendorId)},
                     {"device_id", std::format("0x{:04x}", a.deviceId)},
                     {"type", rhi::adapterTypeName(a.type)},
                     {"software", a.isSoftware()},
                     {"driver_name", a.driverName},
                     {"driver_version", a.driverVersion},
                     {"driver_info", a.driverInfo},
                     {"api_version", a.apiVersion},
                     {"dedicated_memory", a.dedicatedMemory},
                     {"shared_memory", a.sharedMemory}};
    if (a.pci) j["pci_address"] = a.pci->toString();
    if (a.luid) {
        std::string s;
        for (auto b : *a.luid) s += std::format("{:02x}", b);
        j["luid"] = s;
    }
    if (a.uuid) {
        std::string s;
        for (auto b : *a.uuid) s += std::format("{:02x}", b);
        j["uuid"] = s;
    }
    return j;
}

nlohmann::json devicesJson() {
    nlohmann::json apis = nlohmann::json::array();
    for (rhi::Api api : rhi::compiledApis()) {
        std::string error;
        const auto adapters = rhi::enumerateAdapters(api, &error);
        nlohmann::json list = nlohmann::json::array();
        for (const auto& a : adapters) list.push_back(adapterJson(a));
        apis.push_back({{"api", rhi::apiName(api)},
                        {"display_name", rhi::apiDisplayName(api)},
                        {"available", !adapters.empty()},
                        {"error", error},
                        {"default", defaultAdapter(adapters)},
                        {"adapters", list}});
    }
    return {{"apis", apis}, {"default_api", rhi::apiName(defaultApi())}};
}

telemetry::DeviceMatch deviceMatch(const rhi::AdapterInfo& a) {
    telemetry::DeviceMatch m;
    m.vendorId = a.vendorId;
    m.deviceId = a.deviceId;
    m.name = a.name;
    if (a.pci) m.pciAddress = a.pci->toString();
    m.luid = a.luid;
    return m;
}

results::SystemInfo buildSystemInfo(const rhi::Device& device, telemetry::Provider* provider) {
    results::SystemInfo s;
    s.host = sysinfo::collectHost();
    const rhi::AdapterInfo& a = device.adapter();
    auto& g = s.gpu;
    g.name = a.name;
    g.vendor = std::string(rhi::vendorName(a.vendorId));
    g.vendorId = a.vendorId;
    g.deviceId = a.deviceId;
    g.revision = a.revision;
    g.type = std::string(rhi::adapterTypeName(a.type));
    g.driverName = a.driverName;
    g.driverVersion = a.driverVersion;
    g.driverInfo = a.driverInfo;
    g.vramBytes = a.dedicatedMemory;
    g.sharedBytes = a.sharedMemory;
    g.api = std::string(rhi::apiName(a.api));
    g.apiVersion = a.apiVersion;
    g.resizableBar = device.features().resizableBar;
    if (a.pci) {
        g.pciAddress = a.pci->toString();
        const auto pci = sysinfo::readPciDevice(g.pciAddress);
        if (pci.valid) {
            g.subsysVendorId = pci.subsysVendorId;
            g.subsysId = pci.subsysId;
            g.revision = pci.revision;
            g.vbios = pci.vbios;
            g.pcieCurrentGen = pci.currentGen;
            g.pcieCurrentWidth = pci.currentWidth;
            g.pcieMaxGen = pci.maxGen;
            g.pcieMaxWidth = pci.maxWidth;
            g.pcieCurrent = sysinfo::formatLink(pci.currentGen, pci.currentWidth);
            g.pcieMax = sysinfo::formatLink(pci.maxGen, pci.maxWidth);
        }
    }
    if (provider) provider->fillGpuInfo(g);
    for (const auto& c : device.capabilities()) {
        if (c.group == "Extensions") s.extensions.push_back(c.name);
        s.capabilities.push_back({{"group", c.group}, {"name", c.name}, {"value", c.value}, {"supported", c.supported}});
    }
    return s;
}

} // namespace gt::runner
