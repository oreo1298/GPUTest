#pragma once

#include "gt/results.h"
#include "gt/rhi/rhi.h"
#include "gt/telemetry/telemetry.h"

#include <nlohmann/json.hpp>

namespace gt::runner {

/// Index of the adapter to use by default: first discrete, then integrated, then anything non-CPU, then the first.
int defaultAdapter(const std::vector<rhi::AdapterInfo>& adapters);
/// Preferred API when none is given: Vulkan if it has a usable adapter, else D3D12, D3D11, OpenGL.
rhi::Api defaultApi();
nlohmann::json adapterJson(const rhi::AdapterInfo& a);
/// {"apis":[{"api":"vulkan","available":true,"error":"","default":0,"adapters":[...]}, ...]}
nlohmann::json devicesJson();
telemetry::DeviceMatch deviceMatch(const rhi::AdapterInfo& a);
results::SystemInfo buildSystemInfo(const rhi::Device& device, telemetry::Provider* provider);

} // namespace gt::runner
