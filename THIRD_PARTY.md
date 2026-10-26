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
| Qt 6 (qtbase, qtsvg, qtwayland) | 6.8 | LGPL-3.0 (dynamically linked) | Linked (GUI) | https://www.qt.io |
| SQLite (via Qt's QSQLITE driver) | bundled with Qt | Public domain | Linked (GUI) | https://sqlite.org |
| stb_image_write | 1.16 (commit 003501f) | Public domain / MIT | Linked (vendored in third_party/stb) | https://github.com/nothings/stb |
| 8×8 VGA bitmap font | — | Public domain (Marcel Sondaar / IBM VGA), via SDL3 | Linked (HUD font, src/bench/hud_font.h) | https://github.com/libsdl-org/SDL |
| linuxdeploy + plugin-qt | continuous | MIT | Build (AppImage packaging) | https://github.com/linuxdeploy |

Qt is used under the LGPLv3: it is dynamically linked, its source is available from qt.io, and users can replace
the shipped Qt libraries. No GPL-only Qt module (Charts, Graphs, Data Visualization) is used.
