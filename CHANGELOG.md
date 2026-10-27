# Changelog

Each release's section becomes its GitHub release notes (`.github/workflows/ci.yml`, job `release`). The
version must match `APP_VERSION` in `cmake/AppInfo.cmake` and the `v<version>` tag.

## 0.1.0

First release.

- Scene benchmarks from OpenGL 3.3 / Direct3D 11 up to ray-traced and path-traced Vulkan / Direct3D 12 scenes,
  with frame-time percentiles, scores and run history.
- Feature tests: ray tracing (DXR / VK ray query and pipelines, BVH builds, SER, OMM), mesh shaders, variable-rate
  shading, sampler feedback, work graphs, cooperative matrix, subgroups, bindless, async compute, FSR 1 and
  upscaler plugins, HDR output, and the video engines (Vulkan Video and Direct3D 12 Video, H.264 / H.265 / AV1).
- Synthetic tests and diagnostics: compute throughput, memory bandwidth and latency, fill rates, VRAM tester,
  artifact detection, compute correctness, thermal soak, PCIe link check.
- Packages: AppImage, Flatpak bundle, Arch package, Windows installer and portable zip.
