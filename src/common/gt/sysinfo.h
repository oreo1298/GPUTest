#pragma once

#include "gt/results.h"

#include <optional>
#include <string>

namespace gt::sysinfo {

/// CPU, RAM, OS, kernel, display server. Never throws; unknown fields stay empty.
results::HostInfo collectHost();

/// PCI identity and link state read from the OS (Linux sysfs; Windows via SetupAPI where available).
struct PciDeviceInfo {
    bool valid = false;
    std::uint32_t vendorId = 0, deviceId = 0, subsysVendorId = 0, subsysId = 0, revision = 0;
    int currentGen = 0, currentWidth = 0, maxGen = 0, maxWidth = 0;
    std::string vbios;
    std::string driver; ///< kernel driver ("amdgpu", "nvidia", "i915", "xe", ...)
    std::string sysfsPath;
};

/// `address` in "dddd:bb:dd.f" form. For the link state, walks up past upstream
/// switch ports that are part of the GPU package (AMD) so the slot link is reported.
PciDeviceInfo readPciDevice(const std::string& address);

/// "Gen4 x16" style string (empty when unknown).
std::string formatLink(int gen, int width);
/// PCIe generation from a transfer rate in GT/s.
int pcieGenFromGts(double gts);

} // namespace gt::sysinfo
