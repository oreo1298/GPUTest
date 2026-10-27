# Results file format (schema version 1)

Results are JSON written by the runner (`src/common/results.cpp` is the reference implementation; the GUI reads
the same code). Units are in the key names. Unknown keys must be ignored by readers; incompatible changes bump
`schema_version`, and readers keep accepting older versions.

```jsonc
{
  "schema": "gpu-benchmark-results",       // fixed identifier (product-name independent)
  "schema_version": 1,
  "app": {"name": "...", "version": "0.1.0", "revision": "git sha"},
  "created_utc": "2026-10-26T20:00:00Z",
  "command_line": "...",
  "config": {"suite", "api", "gpu_index", "width", "height", "quality", "upscale", "duration", "loop",
             "offscreen", "fullscreen", "vsync", "seed", "validation", "safety": {...}, "tests": ["id", ...]},
  "system": {
    "host": {"cpu", "cpu_cores", "cpu_threads", "ram_bytes", "os", "kernel", "display_server", "desktop", "hostname"},
    "gpu": {"name", "vendor", "vendor_id", "device_id", "subsys_vendor_id", "subsys_id", "revision", "type",
            "driver_name", "driver_version", "driver_info", "vbios", "pci_address",
            "pcie_current", "pcie_max", "pcie_current_gen", "pcie_current_width", "pcie_max_gen", "pcie_max_width",
            "vram_bytes", "shared_bytes", "api", "api_version", "resizable_bar"},
    "extensions": ["VK_KHR_swapchain", ...],
    "capabilities": [{"group", "name", "value", "supported"}, ...]
  },
  "status": "completed | stopped | aborted | safety_abort | device_lost | crashed | running",
  "status_message": "...",
  "duration_s": 123.4,
  "tests": [{
    "id": "scene.lattice", "name": "Lattice", "category": "scene|synthetic|feature|diagnostic", "group": "...",
    "api": "vulkan", "loop": 0,
    "status": "completed | skipped | error | aborted | device_lost", "message": "skip reason / error",
    "verdict": "pass | warn | fail", "verdict_message": "plain-language explanation",   // diagnostics, golden checks
    "score": 1234.5,                                                                       // scored tests only
    "primary_metric": "avg_fps",                                                           // headline metric (optional)
    "metrics": [{"name", "label", "value", "unit", "higher_is_better"}],
    "frame_stats": {"frames", "total_ms", "avg_fps", "min_fps", "max_fps", "low_1pct_fps", "low_01pct_fps",
                    "mean_ms", "min_ms", "max_ms", "stddev_ms", "p50_ms", "p90_ms", "p95_ms", "p99_ms", "p999_ms",
                    "stutter_count"},
    "frame_times_ms": [...], "gpu_times_ms": [...], "gpu_busy_percent": 97.5,
    "start_s": 1.2, "duration_s": 30.1, "frames": 1200, "time_limited": false,
    "details": {...}   // test-specific (e.g. "golden": {frame: {hash, signature}}, failing VRAM ranges)
  }],
  "loops": [{"index", "start_s", "duration_s", "score", "complete"}],
  "scores": {"overall", "subscores": {"scene": ..., ...}, "comparable_key", "per_watt"},
  "stability": {"ratio", "threshold", "passed", "trend_percent_per_hour"},   // loop runs only
  "telemetry": {
    "provider": "nvml | amdgpu-sysfs | ... | none", "interval_s": 0.1,
    "samples": [{"t", "gpu_clock_mhz", "mem_clock_mhz", "temp_edge_c", "temp_hotspot_c", "temp_mem_c",
                 "power_w", "power_limit_w", "fan_percent", "fan_rpm", "gpu_util_percent", "mem_util_percent",
                 "vram_used_mb", "vram_total_mb", "throttle": ["power", ...], "throttle_raw"}],  // absent = unavailable
    "summary": {"<channel>": {"min", "avg", "max", "count"}},
    "throttle_events": [{"start_s", "end_s", "reasons": [...]}]
  },
  "validation": {"errors", "warnings"},
  "warnings": ["..."],
  "device_losses": [{"test", "loop", "t", "message", "recovered",
                     "fault": {"supported", "description", "details": ["..."]},   // VK_EXT_device_fault / DRED
                     "last_sample": {...telemetry sample...}}]                    // optional; [] when none
}
```

Throttle reason names: `power`, `thermal`, `memory_thermal`, `current`, `voltage`, `hw_slowdown`, `sw_power`,
`application`, `idle`, `other` (vendor reasons are mapped onto these; `throttle_raw` keeps the vendor bits).

`scripts/check-results.py` validates a file against this description.
