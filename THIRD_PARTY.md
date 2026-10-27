# Third-party components

All components are MIT-compatible. "Build" = used only to build; "Linked" = compiled into or shipped with the app.

| Component | Version | License | Use | Source |
|---|---|---|---|---|
| nlohmann/json | 3.12.0 | MIT | Linked (header-only) | https://github.com/nlohmann/json |
| CLI11 | 2.7.2 | BSD-3-Clause | Linked (header-only) | https://github.com/CLIUtils/CLI11 |
| Catch2 | 3.16.1 | BSL-1.0 | Build (unit tests) | https://github.com/catchorg/Catch2 |
| Vulkan-Headers | 1.4.363 | Apache-2.0 OR MIT | Build (headers) | https://github.com/KhronosGroup/Vulkan-Headers |
| volk | 1.4.363 | MIT | Linked | https://github.com/zeux/volk |
| Vulkan Memory Allocator | 3.5.0 | MIT | Linked | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator |
| SDL3 | 3.4.18 | Zlib | Linked (static) | https://github.com/libsdl-org/SDL |
| Slang | 2026.20 | Apache-2.0 WITH LLVM-exception | Build (shader compiler; not shipped) | https://github.com/shader-slang/slang |
| SPIRV-Cross | 1.4.363 | Apache-2.0 | Build (GLSL generator `gt_glslgen`; not shipped) | https://github.com/KhronosGroup/SPIRV-Cross |
| AMD FidelityFX Super Resolution 1 | 1.0 (FSR 1) | MIT | Ported (EASU/RCAS shaders in `shaders/src/upscale/fsr1.slang`, notice kept there) | https://github.com/GPUOpen-Effects/FidelityFX-FSR |
| Intel XeSS SDK public headers | commit de0fb9c (2026-09-09) | MIT | Build (XeSS upscaler plugin, `third_party/xess`); the XeSS runtime is not distributed (docs/PLUGINS.md) | https://github.com/intel/xess |
| DirectX-Headers | 1.619.5 | MIT | Build (D3D12 headers) | https://github.com/microsoft/DirectX-Headers |
| DirectX Shader Compiler (DXC) | 1.9.2609.5 (NuGet) | LLVM Release License + Microsoft license | Build (Windows CI: DXIL for slangc, HLSL work graphs; not shipped) | https://github.com/microsoft/DirectXShaderCompiler |
| D3D12 Agility SDK, WARP | 1.619.6, 1.0.21 (NuGet) | Microsoft (redistribution terms; WARP test-only) | CI testing only (not shipped, see docs/DECISIONS.md) | https://devblogs.microsoft.com/directx/directx12agility/ |
| Qt 6 (qtbase, qtsvg, qtwayland) | 6.12 LTS (CI) | LGPL-3.0 (dynamically linked) | Linked (GUI) | https://www.qt.io |
| SQLite (via Qt's QSQLITE driver) | bundled with Qt | Public domain | Linked (GUI) | https://sqlite.org |
| stb_image_write | 1.16 (commit 003501f) | Public domain / MIT | Linked (vendored in third_party/stb) | https://github.com/nothings/stb |
| 8×8 VGA bitmap font | — | Public domain (Marcel Sondaar / IBM VGA), via SDL3 | Linked (HUD font, src/bench/hud_font.h) | https://github.com/libsdl-org/SDL |
| Lucide icons (subset) | main (2026) | ISC | Linked (GUI icons, src/gui/icons) | https://github.com/lucide-icons/lucide |
| linuxdeploy + plugin-qt | continuous | MIT | Build (AppImage packaging) | https://github.com/linuxdeploy |
| Inno Setup | 6 | Inno Setup License (free, redistribution of installers allowed) | Build (Windows installer; its setup runtime is part of the installer) | https://jrsoftware.org/isinfo.php |
| KDE / Freedesktop Flatpak runtime | 6.11 | LGPL / various (runtime, not bundled) | Runtime of the Flatpak build | https://flathub.org |

Qt is used under the LGPLv3: it is dynamically linked, its source is available from qt.io, and users can replace
the shipped Qt libraries. No GPL-only Qt module (Charts, Graphs, Data Visualization) is used.
