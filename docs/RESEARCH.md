# Research notes (2026-10-26)

Research pass required by SPEC §0, done by two research subagents with web access. Each claim carries a source
URL and a confidence tag. Hosts unreachable from the cloud VM (docs.nvidia.com, gpuopen.com, qt.io,
learn.microsoft.com, sdk.lunarg.com) mean some facts are from search snippets; those are marked.

## Summary of what changed our plans
- **Vulkan 1.4** (headers 1.4.366). Cross-vendor SER is `VK_EXT_ray_tracing_invocation_reorder` (1.4.333);
  opacity micromaps now also exist as `VK_KHR_opacity_micromap` (1.4.351, needs `VK_KHR_device_address_commands`);
  `VK_EXT_descriptor_heap` (1.4.340) deprecates descriptor buffers. Clusters/PTLAS, linear swept spheres and
  cooperative vectors remain NVIDIA-only. Work graphs = `VK_AMDX_shader_enqueue` (provisional, no KHR).
- **D3D12**: Agility SDK 1.619.6 retail (SM 6.9: DXR 1.2 SER + OMM, long vectors); 1.721.3-preview has SM 6.10
  (LinAlg) which replaced cooperative vectors / wave matrix. Work graphs 1.0 retail, mesh nodes unreleased.
  WARP NuGet 1.0.21 supports DXR 1.2, mesh shaders, work graphs — license is test-only (CI yes, ship no).
- **Slang** 2026.20, Apache-2.0 WITH LLVM-exception, Khronos-hosted; DXIL needs dxcompiler/dxil DLLs next to it,
  DXBC needs d3dcompiler_47 (Windows only).
- **lavapipe** (Mesa 25.2.8 here): RT pipeline + ray query + position fetch, mesh shaders, descriptor buffer,
  shader objects, DGC. No SER, OMM, VRS, cooperative matrix (needs Mesa ≥ 26.x), video.
- **Telemetry licensing**: NVML — declare our own prototypes (nvml.h may not be redistributed).
  ADLX — proprietary, header redistribution banned, incompatible with an MIT project → **not used**;
  AMD on Windows uses ADL (atiadlxx, MIT-noticed headers re-declared by us) + D3DKMT.
  IGCL — Intel-only license → **not used**; Intel uses Level Zero Sysman (MIT) + D3DKMT on Windows.
- **Upscalers**: FSR 1 / FSR 2.2 / FSR 3.1 (FidelityFX SDK 1.1.x) are MIT with Vulkan + DX12 backends; FSR 4 is
  binary-only. DLSS SDK license forbids open-source exposure → no DLSS code in this repo. XeSS headers are MIT
  (since 2026-09-09) and DLLs redistributable unmodified → optional runtime plugin is possible.
- **Qt**: Charts / Graphs / Data Visualization are GPL-3.0-only (or commercial) → custom charts. Newest LTS is
  **6.12**; `setColorScheme` (6.8), `QPalette::Accent` (6.6), `windows11` style (6.7); Qt reads the portal
  color-scheme but **not** the portal accent color → we read `org.freedesktop.appearance accent-color` ourselves.

---

# GPU API research pass: Vulkan, D3D12, Slang, Mesa/lavapipe, LunarG SDK (as of 2026-10-26)

How this was gathered: most facts come from primary artifacts I downloaded and parsed directly:
- `registry/vk.xml` and `vulkan_core.h` at Vulkan-Headers `v1.4.366`
- Vulkan-Docs `ChangeLog.adoc`
- Mesa source at tags `mesa-26.2.5` and `mesa-26.3.0-rc2`, read from the GitHub mirror `chaotic-cx/mesa-mirror` because gitlab.freedesktop.org was not reachable from here
- The NuGet API and the packages themselves (Agility SDK `1.619.6`, WARP `1.0.21`): READMEs, headers and licenses
- The microsoft/DirectX-Specs repo and the DXC `docs/ReleaseNotes.md`
- The Slang `v2026.20` source and its release tarballs
- Launchpad PPA pages

Where a fact comes only from web-search snippets, it is marked **(secondary)**. Where I could not check a fact, it is marked **UNVERIFIED**.

---

## 1. Vulkan

### Core version and headers
- **The latest core version is Vulkan 1.4.** `vk.xml` defines `VK_VERSION_1_0` through `VK_VERSION_1_4` and nothing later. Header `VK_HEADER_VERSION 366`, so `VK_HEADER_VERSION_COMPLETE = VK_MAKE_API_VERSION(0,1,4,366)`. Source: https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.4.366/include/vulkan/vulkan_core.h
- **The latest Vulkan-Headers tag is `v1.4.366`**, committed 2026-10-23 as "Update for Vulkan-Docs 1.4.366". The latest SDK tag in that repo is `vulkan-sdk-1.4.363.0` (2026-09-18). Source: https://github.com/KhronosGroup/Vulkan-Headers/tags
- **Extensions promoted to 1.4 core** (from `promotedto="VK_VERSION_1_4"`):
  - `VK_KHR_maintenance5` and `VK_KHR_maintenance6`
  - `VK_KHR_push_descriptor`, `VK_KHR_global_priority`, `VK_KHR_dynamic_rendering_local_read`, `VK_EXT_host_image_copy`
  - `VK_KHR_map_memory2`, `VK_KHR_shader_subgroup_rotate`, `VK_EXT_pipeline_robustness`, `VK_EXT_pipeline_protected_access`
  - `VK_KHR_vertex_attribute_divisor`, `VK_KHR_load_store_op_none`, `VK_KHR_shader_float_controls2`
  - `VK_KHR_index_type_uint8`, `VK_KHR_line_rasterization`, `VK_KHR_shader_expect_assume`

  Maintenance7 through maintenance11 are still extensions.
- **Roadmap 2026 milestone.** It requires Vulkan 1.4 plus these extensions:
  - `VK_KHR_fragment_shading_rate`, `VK_KHR_cooperative_matrix`, `VK_KHR_pipeline_binary`, `VK_KHR_robustness2`
  - `VK_KHR_maintenance7`, `VK_KHR_maintenance8`, `VK_KHR_maintenance9`
  - `VK_KHR_shader_untyped_pointers`, `VK_KHR_copy_memory_indirect`, `VK_KHR_shader_clock`, `VK_KHR_compute_shader_derivatives`
  - `VK_KHR_present_id2`, `VK_KHR_present_wait2`, and more

  The profile is in `xml/profiles/VP_KHR_roadmap.json`. Source: https://github.com/KhronosGroup/Vulkan-Docs/blob/main/appendices/roadmap/Roadmap-2026.adoc

### Extension status
Columns: extension (number, spec version); status in `vk.xml`; feature struct and its members; notes. "Ratified" means `ratified="vulkan"` in `vk.xml`.

| Extension | Status | Feature struct and members | Notes |
|---|---|---|---|
| `VK_KHR_acceleration_structure` (151, v13) | KHR, ratified, not core | `VkPhysicalDeviceAccelerationStructureFeaturesKHR {accelerationStructure, accelerationStructureCaptureReplay, accelerationStructureIndirectBuild, accelerationStructureHostCommands, descriptorBindingAccelerationStructureUpdateAfterBind}` | Requires `VK_KHR_deferred_host_operations`, plus BDA and descriptor indexing (or 1.2) |
| `VK_KHR_ray_tracing_pipeline` (348) | KHR, ratified | `VkPhysicalDeviceRayTracingPipelineFeaturesKHR {rayTracingPipeline, ...TraceRaysIndirect, rayTraversalPrimitiveCulling}` | `VK_NV_ray_tracing` is deprecated by this extension |
| `VK_KHR_ray_query` (349) | KHR, ratified | `VkPhysicalDeviceRayQueryFeaturesKHR {rayQuery}` | |
| `VK_KHR_ray_tracing_maintenance1` (387) | KHR, ratified | `{rayTracingMaintenance1, rayTracingPipelineTraceRaysIndirect2}` | |
| `VK_KHR_ray_tracing_position_fetch` (482) | KHR, ratified | `{rayTracingPositionFetch}` | |
| `VK_KHR_pipeline_library_group_handles` | KHR, ratified | | New in 1.4.362 (2026-09-04) |
| `VK_NV_ray_tracing_linear_swept_spheres` (430) | NV vendor extension | `VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV {spheres, linearSweptSpheres}` | Added 1.4.307 (2025-01-30). No EXT/KHR successor exists |
| `VK_NV_cluster_acceleration_structure` (570, v4) | NV vendor extension | `{clusterAccelerationStructure}` | "RTX Mega Geometry" clusters (CLAS). Added 1.4.307 |
| `VK_NV_partitioned_acceleration_structure` (571) | NV vendor extension | `{partitionedAccelerationStructure}` | Partitioned TLAS (PTLAS). Added 1.4.307. **No EXT/KHR cluster or PTLAS extension exists yet** |
| `VK_NV_ray_tracing_invocation_reorder` (491) | NV, `promotedto="VK_EXT_ray_tracing_invocation_reorder"` | `...InvocationReorderFeaturesNV {rayTracingInvocationReorder}` | |
| **`VK_EXT_ray_tracing_invocation_reorder`** (582, v2) | **Cross-vendor EXT, ratified.** Added 1.4.333 (2025-11-14) | `VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT {rayTracingInvocationReorder}` | SPIR-V `SPV_EXT_shader_invocation_reorder`. AMD added it in Adrenalin 26.2.1 (2026-02-11) **(secondary)**. NVIDIA and Intel driver versions **UNVERIFIED**. No Mesa driver exposes it as of 26.3.0-rc2 |
| `VK_EXT_opacity_micromap` (397, v2) | EXT, ratified, `promotedto="VK_KHR_opacity_micromap"` | `VkPhysicalDeviceOpacityMicromapFeaturesEXT {micromap, micromapCaptureReplay, micromapHostCommands}` | No Mesa driver exposes it. Proprietary vendor list **UNVERIFIED** |
| **`VK_KHR_opacity_micromap`** (624) | **KHR, ratified.** Added 1.4.351 (2026-05-08) | `VkPhysicalDeviceOpacityMicromapFeaturesKHR {micromap}` | Depends on `VK_KHR_acceleration_structure` + **`VK_KHR_device_address_commands`** (1.4.346, 2026-03-13). Shipping drivers **UNVERIFIED** |
| `VK_NV_displacement_micromap` (398) | NV, **provisional**, `deprecatedby="VK_NV_cluster_acceleration_structure"` | `{displacementMicromap}` | Do not build on it |
| `VK_EXT_mesh_shader` (329) | EXT, ratified, not promoted. No KHR version | `VkPhysicalDeviceMeshShaderFeaturesEXT {taskShader, meshShader, multiviewMeshShader, primitiveFragmentShadingRateMeshShader, meshShaderQueries}` | Mesa: anv/gfx12.5+, lvp, nvk, radv, vn. `VK_NV_mesh_shader` is legacy |
| `VK_KHR_cooperative_matrix` (507, v2) | KHR, ratified, not core | `{cooperativeMatrix, cooperativeMatrixRobustBufferAccess}` | Required by Roadmap 2026. Mesa: anv, lvp, nvk/Turing+, panvk/v11+, radv/gfx11+, vn |
| `VK_EXT_cooperative_matrix_maintenance1` (660) | EXT, ratified. Added 1.4.359 (2026-08-07) | `{cooperativeMatrixProperties2, cooperativeMatrixReductions, cooperativeMatrixConversions, cooperativeMatrixPerElementOperations, cooperativeMatrixGetCoordinate}` | Mesa 26.3: lvp, radv/gfx11+ |
| `VK_NV_cooperative_matrix2` (594) | NV vendor extension | `{cooperativeMatrixWorkgroupScope, ...FlexibleDimensions, ...Reductions, ...Conversions, ...PerElementOperations, ...TensorAddressing, ...BlockLoads}` | lavapipe also exposes it (Mesa ≥26.1) |
| `VK_NV_cooperative_vector` (492, v4) | **NV only.** Added 1.4.307. **No EXT/KHR cooperative-vector extension exists** | `VkPhysicalDeviceCooperativeVectorFeaturesNV {cooperativeVector, cooperativeVectorTraining}` | Not in Mesa |
| `VK_KHR_shader_bfloat16` (142) | KHR, ratified. Added 1.4.311 (2025-03-21) | `VkPhysicalDeviceShaderBfloat16FeaturesKHR {shaderBFloat16Type, shaderBFloat16DotProduct, shaderBFloat16CooperativeMatrix}` | Mesa: anv/gfx12.5+, radv/gfx12+, vn. Not lvp |
| `VK_EXT_shader_float8` (568) | EXT, ratified. Added 1.4.317 (2025-06-06) | `VkPhysicalDeviceShaderFloat8FeaturesEXT {shaderFloat8, shaderFloat8CooperativeMatrix}` | Mesa: radv/gfx12+, vn |
| `VK_EXT_shader_ocp_microscaling_types` (673) | EXT, ratified (1.4.356, 2026-07-03) | `VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT` | OCP MX (FP4/FP6) types |
| `VK_EXT_shader_long_vector` (636) | EXT, ratified (1.4.337, 2025-12-19) | `{longVector}` | Counterpart of HLSL long vectors |
| `VK_AMDX_shader_enqueue` (135, v2) | **AMDX, still `provisional="true"`**. Needs `VK_ENABLE_BETA_EXTENSIONS` / `vulkan_beta.h` | `VkPhysicalDeviceShaderEnqueueFeaturesAMDX {shaderEnqueue, shaderMeshEnqueue}` | Work graphs. **No KHR/EXT successor in the registry.** V2 API landed in 1.3.298 |
| `VK_EXT_descriptor_buffer` (317) | EXT, ratified, **`deprecatedby="VK_EXT_descriptor_heap"`** | `{descriptorBuffer, descriptorBufferCaptureReplay, descriptorBufferImageLayoutIgnored, descriptorBufferPushDescriptors}` | Mesa: anv, lvp, nvk, radv, tu |
| **`VK_EXT_descriptor_heap`** (136) | EXT, ratified. Added **1.4.340 (2026-01-23)** | `VkPhysicalDeviceDescriptorHeapFeaturesEXT {descriptorHeap, descriptorHeapCaptureReplay}` | Depends on maintenance5/`VK_KHR_extended_flags` + BDA, or 1.4. SPIR-V `SPV_EXT_descriptor_heap`. Mesa 26.3: anv, lvp, radv, vn. NVIDIA: beta drivers, then "610 and later" **(secondary)**. Khronos says a `VK_KHR_descriptor_heap` is targeted for end of 2026 **(secondary)** |
| `VK_KHR_fragment_shading_rate` (227, v2) | KHR, ratified, not core (Roadmap 2026) | `{pipelineFragmentShadingRate, primitiveFragmentShadingRate, attachmentFragmentShadingRate}` | Mesa: anv/gen11+, nvk/Turing+, radv/gfx10.3+, tu/a7xx+, vn. **Not lvp** |
| `VK_KHR_shader_atomic_int64` (181) | **Core in 1.2.** There is no `VK_EXT_shader_atomic_int64` | `VkPhysicalDeviceShaderAtomicInt64Features {shaderBufferInt64Atomics, shaderSharedInt64Atomics}`, also in `VkPhysicalDeviceVulkan12Features` | |
| `VK_EXT_shader_image_atomic_int64` (235) | EXT, ratified | `{shaderImageInt64Atomics, sparseImageInt64Atomics}` | |
| `VK_EXT_swapchain_colorspace` (105, v5) | Instance extension, ratified | n/a | |
| `VK_EXT_hdr_metadata` (106, v3) | Device extension, ratified | n/a (`vkSetHdrMetadataEXT`) | Also new: `VK_HUAWEI_hdr_vivid` and `VK_GOOGLE_hdr_smpte_st_209450_metadata` (1.4.366) |
| `VK_EXT_memory_budget` (238) | EXT, ratified | Properties only: `VkPhysicalDeviceMemoryBudgetPropertiesEXT` | |
| `VK_EXT_pci_bus_info` (213, v2) | EXT, ratified | Properties only: `VkPhysicalDevicePCIBusInfoPropertiesEXT` | **Not lvp** |
| `VK_KHR_pipeline_binary` (484) | KHR, ratified (Roadmap 2026) | `{pipelineBinaries}` | Mesa: anv, hk, nvk, panvk, radv. Not lvp |
| `VK_EXT_shader_object` (483) | EXT, ratified, not promoted | `{shaderObject}` | Mesa: anv, lvp, hk, nvk, radv |
| `VK_KHR_maintenance7`…`11` | KHR, ratified, all still extensions | `VkPhysicalDeviceMaintenanceNFeaturesKHR {maintenanceN}` | m7 is #563, m8 #575, m9 #585, m10 #631 (1.4.330), m11 #658 (1.4.350). lvp exposes all of them |
| `VK_EXT_device_generated_commands` (573) | EXT, ratified | | Mesa: anv, lvp, nvk/Turing+, radv/gfx8+ |

### Vulkan Video
None of the video extensions are promoted to core. All are KHR and ratified (`vk.xml`). Support is reported through video capabilities and queue-family queries, not feature structs.

| Extension | Spec version | Introduced (Vulkan-Docs ChangeLog) |
|---|---|---|
| `VK_KHR_video_queue` | v8 | Core of Vulkan Video |
| `VK_KHR_video_decode_queue` | v8 | |
| `VK_KHR_video_encode_queue` | v12 | |
| `VK_KHR_video_decode_h264` | v9 | |
| `VK_KHR_video_decode_h265` | v8 | |
| `VK_KHR_video_encode_h264` | v14 | |
| `VK_KHR_video_encode_h265` | v14 | |
| `VK_KHR_video_decode_av1` | v1 | |
| `VK_KHR_video_encode_av1` | v1 | 1.3.302 (2024-11-21) |
| `VK_KHR_video_decode_vp9` | v1 | 1.4.317 (2025-06-06). VP9 is decode only; no VP9 encode extension exists |

Other video extensions:
- `VK_KHR_video_maintenance1` and `VK_KHR_video_maintenance2`
- `VK_KHR_video_encode_quantization_map` and `VK_KHR_video_encode_intra_refresh`
- `VK_KHR_video_encode_feedback2` (1.4.353)

Mesa RADV and ANV implement the AV1, VP9, H.264 and H.265 decode/encode extensions behind codec and hardware conditions. **lavapipe implements no video extensions.**

Sources:
- vk.xml at v1.4.366: https://github.com/KhronosGroup/Vulkan-Headers/blob/v1.4.366/registry/vk.xml
- Vulkan-Docs ChangeLog: https://github.com/KhronosGroup/Vulkan-Docs/blob/main/ChangeLog.adoc
- Khronos SER blog: https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder
- AMD Vulkan release notes **(secondary)**: https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-VULKAN.html
- Khronos Roadmap 2026 / descriptor heap blog: https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension
- NVIDIA descriptor heap driver coverage **(secondary)**: https://phoronix.com/news/NVIDIA-595.44.06-Vulkan-Beta
- Mesa `docs/features.txt` at mesa-26.3.0-rc2: https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.3.0-rc2/docs/features.txt

---

## 2. Mesa / lavapipe

### Versions
- **The newest stable Mesa is 26.2.5, released 2026-10-14.** 26.3.0-rc2 was tagged 2026-10-23.
- Release dates in this series: 26.2.0 on 2026-08-05, 26.1.0 on 2026-05-06, 26.0.0 on 2026-02-11.
- lavapipe reports Vulkan **1.4** (`LVP_API_VERSION = VK_MAKE_VERSION(1,4,VK_HEADER_VERSION)` when LLVM ≥10).
- Source: `docs/relnotes/26.2.5.rst` and the tags, read via the mirror https://github.com/chaotic-cx/mesa-mirror. Canonical: https://docs.mesa3d.org/relnotes/26.2.5.html

### What lavapipe 26.2.5 exposes
Taken from `lvp_device_extensions_supported` in `src/gallium/frontends/lavapipe/lvp_device.c`.

**Supported:**

| Area | Extensions / features |
|---|---|
| Ray tracing | **YES**: `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_maintenance1`, `VK_KHR_ray_tracing_position_fetch` (CPU-side BVH). **NO**: SER (`VK_EXT/NV_ray_tracing_invocation_reorder`), opacity micromap (EXT/KHR), NV clusters/PTLAS/LSS |
| Mesh shaders | **YES**: `VK_EXT_mesh_shader` (`taskShader`, `meshShader`, `meshShaderQueries`). `multiviewMeshShader` and `primitiveFragmentShadingRateMeshShader` are false |
| Cooperative matrix | **YES**: `VK_KHR_cooperative_matrix` (since 26.0), `VK_NV_cooperative_matrix2` (since 26.1), `VK_EXT_cooperative_matrix_maintenance1` (26.3). The `cooperativeMatrix` feature is only true when `lp_native_vector_width/32 >= 8`, i.e. the host has 256-bit SIMD (AVX/AVX2) |
| Descriptor models | **YES**: `VK_EXT_descriptor_buffer` and `VK_EXT_descriptor_heap` (heap since 26.2) |
| Other | **YES**: `VK_EXT_shader_object`, `VK_EXT_device_generated_commands`, `VK_KHR_shader_atomic_int64`, `VK_EXT_shader_image_atomic_int64`, `VK_EXT_memory_budget`, `VK_EXT_hdr_metadata`, `VK_EXT_swapchain_colorspace`, `VK_KHR_maintenance1`…`11`, `VK_KHR_device_address_commands`, `VK_KHR_shader_untyped_pointers` (26.2), `VK_KHR_robustness2`, `VK_KHR_shader_float16_int8` |

**Not supported:**
- `VK_KHR_fragment_shading_rate`
- `VK_KHR_pipeline_binary`
- `VK_EXT_pci_bus_info`
- All Vulkan Video extensions
- `VK_KHR_shader_bfloat16` and `VK_EXT_shader_float8`
- `VK_NV_cooperative_vector`
- `VK_EXT_shader_long_vector`

**Work graphs:** `VK_AMDX_shader_enqueue` is in lavapipe's table and `.shaderEnqueue = true` is set under `#ifdef VK_ENABLE_BETA_EXTENSIONS`. Mesa only advertises provisional extensions when it is built with `-Dvulkan-beta=true`, and that option defaults to `false` in `meson.options`. So **stock and distro builds do not expose it.** You would need a custom Mesa build to exercise AMDX work graphs on lavapipe.

**Version differences:** Ubuntu 24.04's stock Mesa is `25.2.8-0ubuntu0.24.04.4` in noble-updates (2026-10-02). Its lavapipe has ray tracing, mesh shaders, descriptor buffer, shader objects and DGC. It **lacks** `VK_KHR_cooperative_matrix`, `VK_NV_cooperative_matrix2`, `VK_EXT_descriptor_heap`, maintenance9 through 11, `VK_KHR_robustness2` and `VK_KHR_shader_untyped_pointers`. I checked this by diffing the extension tables at `mesa-25.2.8` and `mesa-26.2.5`.

**Proprietary-relevant gaps in Mesa:** as of 26.3.0-rc2, no Mesa driver (radv, anv, nvk, lvp) exposes `VK_EXT_ray_tracing_invocation_reorder`, `VK_EXT_opacity_micromap` or `VK_KHR_opacity_micromap`.

### Newest Mesa on Ubuntu 24.04
- **Use `ppa:kisak/kisak-mesa`.** It currently publishes **`mesa 26.2.5~kisak1~n`** for noble. The PPA description says it follows the "latest point release". Noble (24.04) is "Supported"; 26.04 and 26.10 have preliminary support. `ppa:kisak/turtle` is the more conservative alternative. Source: https://launchpad.net/~kisak/+archive/ubuntu/kisak-mesa
- **Do not use oibaf (`ppa:oibaf/graphics-drivers`).** Its description says "2026 UPDATE: ppa is currently suspended" and that it tracked the 25.0 branch since 2025. The +packages page currently shows no mesa package. Source: https://launchpad.net/~oibaf/+archive/ubuntu/graphics-drivers
- **Selecting lavapipe:** install `mesa-vulkan-drivers`, then select lavapipe with `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`. `VK_ICD_FILENAMES` is the legacy name for the same variable.
- Ubuntu stock versions: https://launchpad.net/ubuntu/+source/mesa

---

## 3. Direct3D 12

### Agility SDK (NuGet `Microsoft.Direct3D.D3D12`, publish dates from the NuGet API)

| Track | Version | Published | Notes |
|---|---|---|---|
| **Retail** | **1.619.6** | 2026-09-14 | 1.619.0 was published 2026-02-24. The header sets `D3D12_SDK_VERSION 619` |
| **Preview** | **1.721.3-preview** | 2026-07-30 | 1.720.0-preview was published 2026-04-24, 1.721.0-preview 2026-05-27 |
| Not published | **1.620** | — | Not on NuGet as of 2026-10-26. WARP 1.0.21 notes say "Support for features in D3D12 Agility SDK 620", and DXC's "Upcoming Release" notes say "SM 6.10 and DXIL 1.10 are now available in retail". So an SM 6.10 retail release looks imminent but is **not released yet** |

- Redistributable files (from `distributable files.txt`): `D3D12Core.dll`, `d3d12SDKLayers.dll`, `d3dconfig.exe`.
- Sources: https://www.nuget.org/packages/Microsoft.Direct3D.D3D12 and the package README

### Shader model
- **SM 6.9 is retail** in Agility 1.619 with **DXC v1.9.2602**. Contents:
  - SER and opacity micromaps (DXR 1.2)
  - Long vectors (5–1024 elements), DXIL 1.9 native vectors
  - 16-bit `isinf`/`isnan`/`isfinite` and a new `isnormal`
  - `Native16BitShaderOps`, `WaveOps` and `Int64ShaderOps` become **required**
- Sources:
  - https://github.com/microsoft/DirectX-Specs/blob/master/d3d/HLSL_ShaderModel6_9.md
  - https://devblogs.microsoft.com/directx/shader-model-6-9-dxr-1-2-and-agilitysdk-1-619-release
- **SM 6.10 is in preview** (Agility 1.720/1.721-preview with DXC 1.10.2605.x). Contents:
  - LinAlg `linalg::Matrix` (with `VectorAccumulate` added in 1.721)
  - `GetGroupWaveIndex`/`GetGroupWaveCount`, `DebugBreak`
  - `TriangleObjectPositions`, ClusterID, variable groupshared memory
- The retail 1.619 header already defines `D3D_SHADER_MODEL_6_10 = 0x6a` (`D3D_HIGHEST_SHADER_MODEL`), but runtime support needs a 6.10-capable SDK and driver. Sources:
  - https://devblogs.microsoft.com/directx/d3d12-linalg-preview/ (2026-04-27)
  - https://devblogs.microsoft.com/directx/announcing-agilitysdk-721-preview-and-more-shader-model-6-10-features/ (2026-05-28)
- An experimental **SM 6.11** target exists only in DXC "Upcoming Preview Release" notes.

### Feature status

**DXR 1.2 (`D3D12_RAYTRACING_TIER_1_2` = OMM + SER) — retail.**
- Query it with `D3D12_FEATURE_DATA_D3D12_OPTIONS5::RaytracingTier`.
- New cap: `D3D12_FEATURE_DATA_D3D12_OPTIONS22::ShaderExecutionReorderingActuallyReorders`. That struct also has `CreateByteOffsetViewsSupported`, `Max1DDispatchSize` and `Max1DDispatchMeshSize`.
- Every RT tier ≥1.0 device that supports SM 6.9 must accept SER, though it may be a no-op.
- The OMM API was retail in 1.616; the HLSL part arrived with SM 6.9.
- `D3D12_RAYTRACING_TIER_2_0` is **not shipped**. It covers clusters/CLAS, partitioned TLAS, indirect AS operations and SM 6.10.

**Work graphs — 1.0 retail.**
- `D3D12_WORK_GRAPHS_TIER_1_0` in `OPTIONS21::WorkGraphsTier`; SM 6.8; Agility 1.613+.
- **Mesh nodes (`D3D12_WORK_GRAPHS_TIER_1_1`) are unreleased** and preview only (1.715-preview with DXC `v1.8.2405-mesh-nodes-preview`). The retail 1.619 header only defines `TIER_1_0`.
- The DXC upcoming notes say: "Removed work graph support from Shader Model 6.10, and DXIL 1.10". I did not research what this means in detail.
- A new "Work Lists" spec (v0.855, 2026-10-21) is draft only.

**Cooperative vectors (SM 6.9 preview only) — removed.**
- They were previewed in the SM 6.9 preview, 1.717-preview (2025) **(secondary)**. DXC 1.10.2605 says: "Removed experimental Cooperative Vector, this has been replaced by LinAlg matrix".
- **Wave matrix** (`D3D12_WAVE_MMA_TIER` in `OPTIONS9`) never shipped retail. LinAlg `MatrixScope::Wave` replaces WaveMMA, and `MatrixScope::Thread` replaces cooperative vectors.

**Long vectors — retail** (SM 6.9).

**Other features:**

| Feature | Status | Query |
|---|---|---|
| Sampler feedback | Retail | `OPTIONS7::SamplerFeedbackTier`, `D3D12_SAMPLER_FEEDBACK_TIER_0_9` or `_1_0` |
| Mesh shaders | Retail | `OPTIONS7::MeshShaderTier = D3D12_MESH_SHADER_TIER_1` |
| VRS | Retail | `OPTIONS6::VariableShadingRateTier`, `TIER_1` or `TIER_2` |
| Enhanced barriers | Retail | `OPTIONS12::EnhancedBarriersSupported`. The exact first SDK version was not checked |

**Feature level 12_2** = SM 6.5 + DXR tier 1.1 + VRS tier 2 + mesh shader tier 1 + sampler feedback tier 0.9 + binding tier 3, among others. Source: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12_FeatureLevel12_2.md

**Video encode AV1 — present in the retail headers.**
- `d3d12video.h` in 1.619.6 has `D3D12_VIDEO_ENCODER_CODEC_AV1` and AV1 decode profiles `D3D12_VIDEO_DECODE_PROFILE_AV1_PROFILE0/1/2`.
- Spec: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12_Video_Encoding_AV1.md
- The first SDK/OS version that shipped it is **UNVERIFIED**.

Sources for this subsection: https://github.com/microsoft/DirectX-Specs (`d3d/Raytracing.md`, `Raytracing2.md`, `WorkGraphs.md`, `WorkLists.md`) and the retail `d3d12.h` from NuGet 1.619.6.

### DXC (GitHub tags and NuGet `Microsoft.Direct3D.DXC`)
- **SM 6.9** needs **v1.9.2602** or later.
- **Latest retail** is **v1.9.2609**. NuGet `1.9.2609.5` was published 2026-09-29.
- **SM 6.10 preview** needs **v1.10.2605.x**. The latest is `v1.10.2605.37`; NuGet `1.10.2605.37-preview` was published 2026-08-12.
- License: `LICENSE-LLVM.txt` covers everything except `d3d12shader.h`, which is MIT.
- Sources:
  - https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/ReleaseNotes.md
  - https://www.nuget.org/packages/Microsoft.Direct3D.DXC

### WARP
**Redistributable package.** NuGet **`Microsoft.Direct3D.WARP` 1.0.21** was published 2026-09-22. It contains `d3d10warp.dll` for x64, win32 and arm64. Its `.targets` file copies the DLL next to the built executable.

**Feature timeline** (from the package README changelog):

| WARP version | What it added |
|---|---|
| 1.0.3 | Mesh shaders |
| 1.0.8 | DXR 1.1 |
| 1.0.9 | Sampler feedback |
| 1.0.11 | **SM 6.8 and work graphs**, ExecuteIndirect tier 1.1 |
| 1.0.13 | VRS tier 2; **claims FL 12_2**; H.264/H.265 video through Media Foundation |
| 1.0.18 | Agility 1.619 features: **SM 6.9, SER (no actual reorder), OMM, long vectors** |
| 1.0.21 | **SER "now actually re-orders"**; "Support for features in D3D12 Agility SDK 620" |

Mesh nodes (Work Graphs tier 1.1) are not listed in the WARP changelog.

**LICENSE — important.** The license says "Purpose – for testing purposes only". It forbids you to "share, publish, distribute, or lease the software". **You cannot ship the NuGet WARP DLL in your app.** Users must obtain it themselves, or you fall back to the in-box Windows WARP, whose feature level depends on the OS build and was **UNVERIFIED**.

Source: https://www.nuget.org/packages/Microsoft.Direct3D.WARP (README.md and LICENSE.TXT inside the 1.0.21 nupkg)

---

## 4. Slang

**Release and license**
- **Latest release: `v2026.20`**, published 2026-10-17 and marked "Latest". Recent cadence: v2026.17 (09-04), v2026.18 (09-15), v2026.19, v2026.20. Source: https://github.com/shader-slang/slang/releases/tag/v2026.20
- **License: `Apache-2.0 WITH LLVM-exception`** (SPDX header in `LICENSE`). It is not MIT. Bundled dependencies carry MIT, BSD and Apache licenses, and `LICENSES/` ships in the archive. Source: https://github.com/shader-slang/slang/blob/v2026.20/LICENSE
- **Khronos project:** yes. Khronos launched the Slang Initiative on 2024-11-21 and the README carries a Khronos copyright. Source: https://www.khronos.org/news/press/khronos-group-launches-slang-initiative-hosting-open-source-compiler-contributed-by-nvidia

**Targets** (README table):

| Target | Status |
|---|---|
| SPIR-V | Supported; emitted directly, with GLSL as an alternative |
| D3D12 | HLSL, compiled to DXIL |
| D3D11 | HLSL, compiled to DXBC |
| Metal (MSL) | Experimental; vertex, fragment, compute, task and mesh stages |
| WGSL | Experimental |
| CUDA | Supported |
| OptiX | Experimental |
| CPU C++ | Experimental |

Source: https://github.com/shader-slang/slang/blob/v2026.20/README.md

**Downstream compilers**
- **DXBC** goes through fxc by dynamically loading **`d3dcompiler_47`** (`slang-fxc-compiler.cpp`, which includes `d3dcompiler.h` under `#ifdef _WIN32`). It is effectively Windows-only and uses the in-box `d3dcompiler_47.dll`.
- **DXIL** dynamically loads **`dxcompiler`**, with `dxil` as a dependent library (`slang-dxc-compiler.cpp`).
- **The release archives do NOT bundle DXC.** I listed both archives:
  - `slang-2026.20-windows-x86_64.zip` (63.7 MB) contains `slangc.exe`, `slang-compiler.dll`, `slang-glslang.dll`, `slang-llvm.dll`, `slang-rt.dll` and `gfx.dll`. It has no `dxcompiler.dll` or `dxil.dll`.
  - `slang-2026.20-linux-x86_64.tar.gz` (83.4 MB) has `bin/slangc` and `lib/libslang-compiler.so…`. It has no `libdxcompiler.so`.
  - So you must put `dxcompiler.dll` + `dxil.dll` (or `libdxcompiler.so` + `libdxil.so` from the DXC release) on the library search path, or next to `slangc` or the app.

**Feature coverage** (from the capability atoms in `docs/user-guide/a4-02-reference-capability-atoms.md`):
- Ray tracing: `raytracing`, `SPV_KHR_ray_tracing`, `raytracing_position`, `raytracing_motionblur`
- SER / HitObject:
  - Vulkan: `ser`, `SPV_EXT_shader_invocation_reorder`, `SPV_NV_shader_invocation_reorder`
  - D3D: `ser_hlsl_native` = "SM 6.9, no NVAPI", plus `ser_nvapi`
- Mesh shaders: `meshshading`, `SPV_EXT_mesh_shader`. Note that `09-targets.md` still says "does not currently support mesh shaders"; that doc is stale
- Cooperative matrix: `cooperative_matrix`, `cooperative_matrix_2` (KHR and NV2)
- Cooperative vectors: `cooperative_vector`, `SPV_NV_cooperative_vector`
- Numeric types: `SPV_KHR_bfloat16`, `SPV_EXT_float8`
- Descriptor heap: `SPV_EXT_descriptor_heap`
- NV cluster AS and LSS: `raytracing_lss`, `SPV_NV_cluster_acceleration_structure`
- **Work graphs: experimental.** `import experimental.workgraph;` gives the `node` stage, SM 6.8, for HLSL/DXIL.

**Prebuilt binaries** (GitHub release asset names):
- **`slang-2026.20-linux-x86_64.tar.gz`** or `.zip`. glibc-pinned variants also exist: `slang-2026.20-linux-x86_64-glibc-2.27.tar.gz` and `-glibc-2.28`.
- **`slang-2026.20-windows-x86_64.zip`** or `.tar.gz`.
- aarch64 and macOS variants, `-debug-info` archives and wasm archives are also published.

**In the LunarG Vulkan SDK:** yes, "since version 1.3.296.0" (README). SDK 1.4.363.0 pins Slang commit `c7954eb` (2026-09-10, tag `vulkan-sdk-1.4.363.0` in shader-slang/slang). That commit is first contained in `v2026.17.1`, so the SDK's Slang is older than v2026.20.

---

## 5. LunarG Vulkan SDK

- **Latest: 1.4.363.0**, released 2026-09-29 for Windows, Linux and macOS. Headline: KosmicKrisp is a 1.4-conformant ICD, and there is a new `vkprofiles` tool. The previous release was 1.4.357.0 (July 2026). Sources: https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-363-0/ and https://vulkan.lunarg.com/doc/view/latest/windows/release_notes.html **(secondary: fetched via search; vulkan.lunarg.com was unreachable from here)**
- **Linux tarball URL pattern:** `https://sdk.lunarg.com/sdk/download/<ver>/linux/vulkansdk-linux-x86_64-<ver>.tar.xz`. Releases after 1.3.250.1 use `.tar.xz`; older ones use `.tar.gz`.
  - Latest alias: `https://sdk.lunarg.com/sdk/download/latest/linux/vulkan_sdk.tar.xz`
  - Version query: `https://vulkan.lunarg.com/sdk/latest/linux.txt`
  - Sources: https://vulkan.lunarg.com/content/view/latest-sdk-version-api and the jakoch/install-vulkan-sdk-action `src/downloader.ts`
  - **Size: UNVERIFIED.** A third-party page lists about 314 MB for a 1.4.357.x tarball **(secondary)**.
- **Linux requirements:** GLIBC ≥ 2.35 (Ubuntu 24.04 is fine) and x86_64 only. There are no official ARM Linux builds. **Ubuntu .deb packages were discontinued** around 1.4.321.0 (2025), and the tarball is the supported path **(secondary: LunarG 1.4.309.0 notes and Vulkanised 2026 deck)**.
- **Contents:**
  - Validation layers (`Vulkan-ValidationLayers`, tag `vulkan-sdk-1.4.363.0`)
  - glslang (`glslangValidator`, tag `vulkan-sdk-1.4.363.0`) and shaderc `glslc`
  - SPIRV-Tools
  - SPIRV-Cross (tag `vulkan-sdk-1.4.363.0`) and SPIRV-Reflect
  - **DXC** (`dxc`; LunarG docs list "slang, DXC, SPIRV Reflect, and SPIRV-cross")
  - **Slang** (`slangc`)
  - vkconfig, vulkaninfo, the Profiles library, volk, VMA, GFXReconstruct
  - The exact Linux file list was **UNVERIFIED** because the tarball couldn't be downloaded here (sdk.lunarg.com was blocked).
  - Source: https://vulkan.lunarg.com/doc/view/latest/linux/getting_started.html

---

## Takeaways for implementation

**1. Vulkan gating** (check the extension *and* its feature bit via `vkGetPhysicalDeviceFeatures2`):

| Area | Extension | Feature bit(s) |
|---|---|---|
| RT core | `VK_KHR_acceleration_structure` | `accelerationStructure` |
| | `VK_KHR_ray_tracing_pipeline` | `rayTracingPipeline` |
| | `VK_KHR_ray_query` | `rayQuery` |
| | `VK_KHR_ray_tracing_maintenance1` | |
| | `VK_KHR_ray_tracing_position_fetch` | |
| SER | `VK_EXT_ray_tracing_invocation_reorder` (preferred) | `rayTracingInvocationReorder` |
| | Fallback: `VK_NV_ray_tracing_invocation_reorder` | |
| OMM | `VK_KHR_opacity_micromap` (new; needs `VK_KHR_device_address_commands`) | `micromap` |
| | Fallback: `VK_EXT_opacity_micromap` | `micromap` |
| NVIDIA-only RT | `VK_NV_cluster_acceleration_structure` | |
| | `VK_NV_partitioned_acceleration_structure` | |
| | `VK_NV_ray_tracing_linear_swept_spheres` | |
| Mesh | `VK_EXT_mesh_shader` | `meshShader`, `taskShader` |
| Matrix / ML | `VK_KHR_cooperative_matrix` (+ `vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR`) | `cooperativeMatrix` |
| | `VK_EXT_cooperative_matrix_maintenance1` | |
| | `VK_NV_cooperative_matrix2` | |
| | `VK_NV_cooperative_vector` (NV only; no cross-vendor version) | |
| | `VK_KHR_shader_bfloat16` | `shaderBFloat16Type` |
| | `VK_EXT_shader_float8` | `shaderFloat8` |
| | `VK_EXT_shader_long_vector` | |
| Work graphs | `VK_AMDX_shader_enqueue` (provisional; compile with `VK_ENABLE_BETA_EXTENSIONS`; treat as experimental) | `shaderEnqueue`, `shaderMeshEnqueue` |
| Descriptors | `VK_EXT_descriptor_heap` (new; preferred) | |
| | `VK_EXT_descriptor_buffer` (deprecated by heap but more widely shipped) | |
| VRS | `VK_KHR_fragment_shading_rate` | |
| 64-bit atomics | Core 1.2 | `VkPhysicalDeviceVulkan12Features::shaderBufferInt64Atomics` / `shaderSharedInt64Atomics` |
| | `VK_EXT_shader_image_atomic_int64` | `shaderImageInt64Atomics` |
| Video | `VK_KHR_video_decode_{h264,h265,av1,vp9}`, `VK_KHR_video_encode_{h264,h265,av1}` (+ `vkGetPhysicalDeviceVideoCapabilitiesKHR` per profile) | |
| HDR / info | `VK_EXT_swapchain_colorspace` (instance), `VK_EXT_hdr_metadata`, `VK_EXT_memory_budget`, `VK_EXT_pci_bus_info` | |
| Misc | `VK_KHR_pipeline_binary`, `VK_EXT_shader_object`, `VK_KHR_maintenance5` (core 1.4), `VK_KHR_maintenance6` (core 1.4), `VK_KHR_maintenance7`…`11` | |

**2. What lavapipe can actually exercise.** This assumes Mesa ≥26.2 from kisak-mesa (26.2.5) on an AVX2 host.

| Category | Features |
|---|---|
| **CAN exercise** | Vulkan 1.4 core; ray tracing pipeline and ray query (+ maintenance1 and position fetch); mesh and task shaders; `VK_KHR_cooperative_matrix` (AVX/256-bit SIMD only); `VK_NV_cooperative_matrix2`; descriptor buffer **and** descriptor heap; shader objects; EXT DGC; int64 buffer/shared/image atomics; memory budget; HDR metadata and colorspace (with a WSI surface); maintenance5 through 11 |
| **CANNOT exercise** (gate these off; they need real GPUs) | SER, OMM, NV clusters/PTLAS/LSS, fragment shading rate, pipeline binary, all video, bf16/fp8, cooperative vector, long vector, pci_bus_info. AMDX work graphs only work with a custom `-Dvulkan-beta=true` Mesa build |
| Stock Ubuntu 24.04 Mesa 25.2.8 | No cooperative matrix and no descriptor heap on lavapipe. Recommend `ppa:kisak/kisak-mesa` for CI |

**3. D3D12.**
- Ship the **Agility SDK 1.619.x** (`D3D12SDKVersion=619`, redistributable `D3D12Core.dll` + `d3d12SDKLayers.dll`).
- Gate on these queries:

  | Feature | Query |
  |---|---|
  | Highest shader model | `D3D12_FEATURE_SHADER_MODEL` (request `D3D_SHADER_MODEL_6_9`, which needs DXC ≥ v1.9.2602) |
  | Raytracing tier | `OPTIONS5.RaytracingTier` (1.0, 1.1, 1.2) |
  | SER actually reorders | `OPTIONS22.ShaderExecutionReorderingActuallyReorders` |
  | Mesh shaders, sampler feedback | `OPTIONS7` (`MeshShaderTier`, `SamplerFeedbackTier`) |
  | VRS | `OPTIONS6.VariableShadingRateTier` |
  | Work graphs | `OPTIONS21.WorkGraphsTier` (only 1.0 is retail) |
  | Enhanced barriers | `OPTIONS12.EnhancedBarriersSupported` |
  | Video encode | Video encode feature support with `D3D12_VIDEO_ENCODER_CODEC_AV1` |

- Keep cooperative vectors and WaveMMA out. The replacement, LinAlg (SM 6.10), is preview only and needs the 1.72x-preview SDK plus developer mode.

**4. WARP as a CI adapter.**
- WARP 1.0.21 from NuGet covers FL 12_2, DXR 1.1/1.2 (SER and OMM), mesh shaders, VRS tier 2, sampler feedback, work graphs 1.0 and SM 6.9.
- Its **license forbids redistribution**: use it on CI and developer machines only, and do not bundle it.

**5. Slang.**
- License: Apache-2.0 WITH LLVM-exception, which is compatible with MIT as a dependency, but keep its LICENSE files.
- Pin `v2026.20` and use the assets `slang-2026.20-linux-x86_64.tar.gz` and `slang-2026.20-windows-x86_64.zip`.
- SPIR-V, GLSL and MSL work out of the box.
- DXIL needs `dxcompiler` + `dxil` from a DXC ≥ v1.9.2602 release placed next to it. DXBC needs `d3dcompiler_47.dll` and is Windows-only.
- SER (`HitObject`), mesh shaders, cooperative matrix and cooperative vector, bf16/fp8 and descriptor heap are supported. Work graphs are experimental (`experimental.workgraph`).

**6. Vulkan SDK.**
- 1.4.363.0 is the latest SDK release; `v1.4.366` is the latest header tag.
- On Linux use the tarball (`vulkansdk-linux-x86_64-1.4.363.0.tar.xz`). It bundles validation layers, glslang, shaderc, SPIRV-Tools, SPIRV-Cross, DXC and Slang. Its Slang is about v2026.17.1, so pull standalone Slang for the newest features.


---

# Vendor GPU telemetry, upscalers and UI licensing: research notes

Research date: 2026-10-26. Scope: an MIT-licensed, cross-platform (Linux + Windows) GPU benchmark/stress/diagnostics tool that loads every vendor library at runtime (dlopen/LoadLibrary) and never needs a vendor SDK to build.

## How this was researched (read first)

- **Primary sources read directly.** Most facts below come from vendor repositories cloned or fetched from GitHub at the commit or tag shown in each link: NVIDIA, AMD GPUOpen, Intel, Khronos, Qt mirrors, the Linux kernel `master` branch (7.3 development cycle), Microsoft `libdxg` and System Informer. Those claims are tagged **[verified]**.
- **Sites this environment could not reach.** `docs.nvidia.com`, `gpuopen.com`, `qt.io`/`doc.qt.io` and `learn.microsoft.com` were blocked (proxy 403 or DNS failure). Claims that rely only on search-engine summaries of those pages are tagged **[snippet]**. Re-check them in a browser before relying on them.
- **[unverified]** marks an item I could not confirm.

---

## 1. NVIDIA NVML

### 1.1 Libraries and locations

| OS | Library to load | Notes | Source |
|---|---|---|---|
| Linux | `libnvidia-ml.so.1` (SONAME). Fall back to `libnvidia-ml.so` only as a last resort, because it is a dev symlink. | NVIDIA's own Go binding uses `defaultNvmlLibraryName = "libnvidia-ml.so.1"`. nvtop tries `libnvidia-ml.so.1` first and notes that under WSL only the WSL-provided `.so.1` works. | [verified] [go-nvml lib.go](https://github.com/NVIDIA/go-nvml/blob/main/pkg/nvml/lib.go), [nvtop extract_gpuinfo_nvidia.c](https://github.com/Syllo/nvtop/blob/master/src/extract_gpuinfo_nvidia.c) |
| Windows | `nvml.dll` | DCH driver: `\Windows\System32`. Standard (legacy) driver: `%ProgramW6432%\NVIDIA Corporation\NVSMI\`. Neither location is on PATH, so call `LoadLibrary` with a full path: try System32 first, then NVSMI. | [snippet] [NVML API Reference](https://docs.nvidia.com/deploy/nvml-api/nvml-api-reference.html), [NVIDIA forum](https://forums.developer.nvidia.com/t/nvml-dll-location/327758) |

### 1.2 Latest API version
- `#define NVML_API_VERSION 13` / `NVML_API_VERSION_STR "13"` in the `nvml.h` shipped with nvidia-settings **615.78.08** (2026-10-07), the newest tag at research time. **[verified]** [nvml.h @615.78.08](https://github.com/NVIDIA/nvidia-settings/blob/615.78.08/src/nvml.h)
- API 13 begins with the R580 headers (580.65.06). R535 through R575 headers are API 12. **[verified]** by comparing historical `src/nvml.h` across nvidia-settings tags.
- The header `#define`s unversioned names to versioned symbols, for example `nvmlInit → nvmlInit_v2`, `nvmlDeviceGetPciInfo → nvmlDeviceGetPciInfo_v3`, `nvmlDeviceGetCount → _v2`, `nvmlDeviceGetHandleByIndex → _v2` and `nvmlDeviceGetHandleByPciBusId → _v2`. **With dlsym/GetProcAddress you must resolve the versioned export names.** **[verified]** same file.

### 1.3 Functions needed (exact signatures from nvml.h @615.78.08) **[verified]**
Types: `typedef struct nvmlDevice_st* nvmlDevice_t;`, `nvmlReturn_t` is an int enum (`NVML_SUCCESS = 0`). In the "First seen" column, "≤R535" means the function is already present in the R535 headers, the oldest checked.

| Purpose | Signature | First seen |
|---|---|---|
| init (preferred) | `nvmlReturn_t nvmlInit_v2(void);` | ≤R535 |
| init with flags | `nvmlReturn_t nvmlInitWithFlags(unsigned int flags);` (`NVML_INIT_FLAG_NO_GPUS = 1<<0`, `NVML_INIT_FLAG_NO_ATTACH = 1<<1`) | ≤R535 |
| shutdown | `nvmlReturn_t nvmlShutdown(void);` (ref-counted) | ≤R535 |
| error text | `const char* nvmlErrorString(nvmlReturn_t result);` | ≤R535 |
| count / index | `nvmlDeviceGetCount_v2(unsigned int *deviceCount)`, `nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device)` | ≤R535 |
| match by PCI | `nvmlDeviceGetHandleByPciBusId_v2(const char *pciBusId, nvmlDevice_t *device)`. Bus ID format `NVML_DEVICE_PCI_BUS_ID_FMT "%08X:%02X:%02X.0"`, buffer `NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE 32` | ≤R535 |
| PCI info | `nvmlDeviceGetPciInfo_v3(nvmlDevice_t, nvmlPciInfo_t *pci)` | ≤R535 |
| name | `nvmlDeviceGetName(nvmlDevice_t, char *name, unsigned int length)` (`NVML_DEVICE_NAME_V2_BUFFER_SIZE 96`) | ≤R535 |
| UUID | `nvmlDeviceGetUUID(nvmlDevice_t, char *uuid, unsigned int length)` (`NVML_DEVICE_UUID_V2_BUFFER_SIZE 96`) | ≤R535 |
| VBIOS | `nvmlDeviceGetVbiosVersion(nvmlDevice_t, char *version, unsigned int length)` (`NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE 32`) | ≤R535 |
| driver version | `nvmlSystemGetDriverVersion(char *version, unsigned int length)` (`NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE 80`); also `nvmlSystemGetNVMLVersion`, `nvmlSystemGetCudaDriverVersion_v2(int*)` | ≤R535 |
| PCIe current | `nvmlDeviceGetCurrPcieLinkGeneration(nvmlDevice_t, unsigned int *currLinkGen)`, `nvmlDeviceGetCurrPcieLinkWidth(nvmlDevice_t, unsigned int *currLinkWidth)` | ≤R535 |
| PCIe max (device + system) | `nvmlDeviceGetMaxPcieLinkGeneration(nvmlDevice_t, unsigned int *maxLinkGen)`: "maximum PCIe link generation possible with this device **and system**", so a Gen2 GPU in a Gen1 slot reports 1. Also `nvmlDeviceGetMaxPcieLinkWidth(...)` | ≤R535 |
| PCIe max (GPU only) | `nvmlDeviceGetGpuMaxPcieLinkGeneration(nvmlDevice_t, unsigned int *maxLinkGenDevice)`: "maximum PCIe link generation supported by this device" | ≤R535 |
| PCIe extra | `nvmlDeviceGetPcieSpeed(nvmlDevice_t, unsigned int *pcieSpeed)`, `nvmlDeviceGetPcieLinkMaxSpeed(nvmlDevice_t, unsigned int *maxSpeed)`, `nvmlDeviceGetPcieThroughput(nvmlDevice_t, nvmlPcieUtilCounter_t, unsigned int *value)` | ≤R535 |
| clocks | `nvmlDeviceGetClockInfo(nvmlDevice_t, nvmlClockType_t type, unsigned int *clock)` with `NVML_CLOCK_GRAPHICS=0, NVML_CLOCK_SM=1, NVML_CLOCK_MEM=2, NVML_CLOCK_VIDEO=3` (MHz). Also `nvmlDeviceGetMaxClockInfo(...)` and `nvmlDeviceGetClock(nvmlDevice_t, nvmlClockType_t, nvmlClockId_t, unsigned int *clockMHz)` | ≤R535 |
| temperature (new) | `nvmlDeviceGetTemperatureV(nvmlDevice_t, nvmlTemperature_t *temperature)` | R565 |
| temperature (old) | `DEPRECATED(13.0) nvmlDeviceGetTemperature(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int *temp)` | ≤R535 |
| field values | `nvmlDeviceGetFieldValues(nvmlDevice_t, int valuesCount, nvmlFieldValue_t *values)` | ≤R535 |
| power usage | `nvmlDeviceGetPowerUsage(nvmlDevice_t, unsigned int *power)` (mW). On Ampere (except GA100) and newer this is a **1-second average**; older GPUs return instantaneous. Use `NVML_FI_DEV_POWER_INSTANT` (186) or `NVML_FI_DEV_POWER_AVERAGE` (185) for an explicit choice. | ≤R535 |
| energy | `nvmlDeviceGetTotalEnergyConsumption(nvmlDevice_t, unsigned long long *energy)` (mJ since driver load) | ≤R535 |
| power limit | `nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t, unsigned int *limit)` (mW, the effective limit), `nvmlDeviceGetPowerManagementLimit(...)`, `nvmlDeviceGetPowerManagementDefaultLimit(...)`, `nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t, unsigned int *minLimit, unsigned int *maxLimit)`. Also `NVML_FI_DEV_POWER_CURRENT_LIMIT` (190). | ≤R535 |
| fan | `nvmlDeviceGetNumFans(nvmlDevice_t, unsigned int *numFans)`, `nvmlDeviceGetFanSpeed_v2(nvmlDevice_t, unsigned int fan, unsigned int *speed)` (% of max, may exceed 100, "intended" speed), `nvmlDeviceGetFanSpeedRPM(nvmlDevice_t, nvmlFanSpeedInfo_t *fanSpeed)` | RPM: R565 |
| utilization | `nvmlDeviceGetUtilizationRates(nvmlDevice_t, nvmlUtilization_t *utilization)` | ≤R535 |
| memory | `nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t, nvmlMemory_v2_t *memory)` (set `version = nvmlMemory_v2`) | ≤R535 |
| clock event (throttle) reasons | `nvmlDeviceGetCurrentClocksEventReasons(nvmlDevice_t, unsigned long long *clocksEventReasons)`, `nvmlDeviceGetSupportedClocksEventReasons(...)`. The old `nvmlDeviceGetCurrentClocksThrottleReasons` is `DEPRECATED(13.0)`. | ≤R535 |
| P-state | `nvmlDeviceGetPerformanceState(nvmlDevice_t, nvmlPstates_t *pState)` (`NVML_PSTATE_0 … _15`, `NVML_PSTATE_UNKNOWN = 32`). `nvmlDeviceGetPowerState` is `DEPRECATED(13.0)`. | ≤R535 |
| optional | `nvmlDeviceGetMarginTemperature`, `nvmlDeviceGetThermalSettings`, `nvmlDeviceGetCoolerInfo`, `nvmlDeviceGetBusType`, `nvmlDeviceGetDynamicPstatesInfo`, `nvmlDeviceGetClockOffsets` | varies |

Struct layouts you must replicate **[verified]**:
```c
#define NVML_STRUCT_VERSION(data, ver) (unsigned int)(sizeof(nvml##data##_v##ver##_t) | (ver << 24U))
typedef struct { char busIdLegacy[16]; unsigned int domain, bus, device, pciDeviceId, pciSubSystemId; char busId[32]; } nvmlPciInfo_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;              // percent over last sample period
typedef struct { unsigned int version; unsigned long long total, reserved, free, used; } nvmlMemory_v2_t;  // version must be nvmlMemory_v2
typedef struct { unsigned int version; nvmlTemperatureSensors_t sensorType; int temperature; } nvmlTemperature_v1_t; // nvmlTemperature_v1
typedef struct { unsigned int version, fan, speed; } nvmlFanSpeedInfo_v1_t;                                    // nvmlFanSpeedInfo_v1
typedef union { double dVal; int siVal; unsigned int uiVal; unsigned long ulVal; unsigned long long ullVal; signed long long sllVal; unsigned short usVal; } nvmlValue_t;
typedef struct { unsigned int fieldId, scopeId; long long timestamp, latencyUsec; nvmlValueType_t valueType; nvmlReturn_t nvmlReturn; nvmlValue_t value; } nvmlFieldValue_t;
```
(`busIdLegacy` uses `NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE`, which is 16 bytes.)

### 1.4 Temperatures: what is exposed
- **GPU core:** `nvmlDeviceGetTemperatureV` with `sensorType = NVML_TEMPERATURE_GPU (0)`. **[verified]**
- **Hotspot:** `NVML_TEMPERATURE_GPU_MAX = 1` (the header comment says "Temperature from the hottest part of the GPU die") is **new**. It is present in the headers for 615.71.09 (2026-09-09) and 615.78.08, and absent in 610.43.02 and every older header. **[verified]** by header history. Which GPUs or drivers actually return it is **[unverified]**, so expect `NVML_ERROR_NOT_SUPPORTED` or `NVML_ERROR_INVALID_ARGUMENT` on older drivers and GPUs and fall back gracefully. Before R615 NVML has **no** public hotspot value.
- **Memory:** `NVML_FI_DEV_MEMORY_TEMP` (field id 82) through `nvmlDeviceGetFieldValues`. **[verified]** that it exists. Whether GeForce GDDR boards report it is **[unverified]** (it is commonly reported only on HBM data-center parts), so treat NOT_SUPPORTED as normal.
- **Thresholds:** the header says that on Ada and later, `nvmlDeviceGetTemperatureThreshold` is "no longer the preferred interface". Use the fields `NVML_FI_DEV_TEMPERATURE_SHUTDOWN_TLIMIT` (193), `_SLOWDOWN_TLIMIT` (194), `_MEM_MAX_TLIMIT` (195) and `_GPU_MAX_TLIMIT` (196) instead. **[verified]**

### 1.5 Clock event (throttle) reason bits (nvml.h @615.78.08) **[verified]**
```
nvmlClocksEventReasonGpuIdle                    0x0001
nvmlClocksEventReasonApplicationsClocksSetting  0x0002
nvmlClocksEventReasonSwPowerCap                 0x0004
nvmlClocksThrottleReasonHwSlowdown              0x0008   (HW slowdown: temp, power brake, or over-current; no "Event" alias exists)
nvmlClocksEventReasonSyncBoost                  0x0010
nvmlClocksEventReasonSwThermalSlowdown          0x0020
nvmlClocksThrottleReasonHwThermalSlowdown       0x0040   (no "Event" alias)
nvmlClocksThrottleReasonHwPowerBrakeSlowdown    0x0080   (no "Event" alias)
nvmlClocksEventReasonDisplayClockSetting        0x0100
nvmlClocksEventReasonBoardLimit                 0x0200
nvmlClocksEventReasonReliability                0x0400
nvmlClocksEventReasonNone                       0x0000
```
The deprecated `nvmlClocksThrottleReason*` aliases map to the same values.

### 1.6 License of nvml.h: can it be redistributed?
The header notice reads: *"Users and possessors of this source code are hereby granted a nonexclusive, royalty-free license to use this code in individual and commercial software … Any use of this source code in individual and commercial software must include, in the user documentation and internal comments to the code, the above Disclaimer and U.S. Government End Users Notice."* **[verified]** [nvml.h header](https://github.com/NVIDIA/nvidia-settings/blob/615.78.08/src/nvml.h)
- It grants **use**, not an explicit right to redistribute or modify, and it is not MIT. NVIDIA's own GPL and Apache projects (nvidia-settings, go-nvml) ship it, but that is NVIDIA's prerogative.
- **Verdict: declare your own minimal prototypes, enums and structs** (the subset above) in an MIT file, and resolve symbols with `dlsym`/`GetProcAddress`. nvtop, which is GPLv3, does exactly this: it dlopens `libnvidia-ml.so.1` and declares function pointers itself. **[verified]** [nvtop](https://github.com/Syllo/nvtop/blob/master/src/extract_gpuinfo_nvidia.c)
- nvtop also optionally dlopens `libnvidia-api.so.1` (NVAPI on Linux) for extra clock domains. That route uses undocumented interface IDs and is not recommended for an MIT project.

---

## 2. AMD

### 2a. ADLX (Windows only)

**License: NOT MIT.** The repository's only license is `ADLX SDK License Agreement.pdf`, and the README says *"ADLX SDK is distributed under the license included in ADLX SDK License Agreement.pdf"*. **[verified]** [ADLX repo @32b5a74](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/tree/32b5a740d42295c5dfe9026b9f52683da0f3af91)

Key clauses, read from the PDF **[verified]**:
- §1.5/1.8: "Licensed Materials" include **header files**, and "Sample Code" means "the header files and Source Code identified … as sample code".
- §2(c): you may distribute "**solely in Object Code form**, Sample Code and Derivative Works as incorporated in Your Software", only under an EULA meeting §3.
- §3: the EULA must prohibit reverse engineering, and AMD must be a third-party beneficiary.
- §4(b): you may not "distribute, publish … the Licensed Materials". §4(f): you may not use them "in way that requires that the Licensed Materials … be licensed under a Free Software License".
- §12: confidentiality obligations.
- **Consequence:** ADLX headers, or vtable declarations transcribed from them, cannot go into an MIT source repository. An MIT app that uses ADLX would also have to ship under an EULA that forbids reverse engineering, which conflicts with distributing the source.

Loading **[verified]** ([ADLX.h](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/blob/32b5a740d42295c5dfe9026b9f52683da0f3af91/SDK/Include/ADLX.h), [ADLXHelper.c](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/blob/32b5a740d42295c5dfe9026b9f52683da0f3af91/SDK/ADLXHelper/Windows/C/ADLXHelper.c)):
- DLL: `amdadlx64.dll` (64-bit) or `amdadlx32.dll` (32-bit). It ships with the AMD driver, and the helper loads it with `LOAD_LIBRARY_SEARCH_SYSTEM32 | …DEFAULT_DIRS | …APPLICATION_DIR | …USER_DIRS`.
- Exports (all `__cdecl`):
  - `ADLXQueryFullVersion(adlx_uint64* fullVersion)`
  - `ADLXQueryVersion(const char** version)`
  - `ADLXInitialize(adlx_uint64 version, IADLXSystem** ppSystem)`
  - `ADLXInitializeWithIncompatibleDriver(...)` (same signature as `ADLXInitialize`)
  - `ADLXInitialize2(adlx_uint64 version, IADLXSystem**, IADLMapping**)`
  - `ADLXInitializeWithIncompatibleDriver2(...)` (same signature as `ADLXInitialize2`)
  - `ADLXInitializeWithCallerAdl(adlx_uint64 version, IADLXSystem**, IADLMapping**, adlx_handle adlContext, ADLX_ADL_Main_Memory_Free adlMainMemoryFree)`
  - `ADLXTerminate()`
- The `version` argument is `ADLX_FULL_VERSION = (major<<48)|(minor<<32)|(release<<16)|build`. The current header `ADLXVersion.h` has 2.0.0.125.
- Interface methods are `__stdcall`. Success is `ADLX_OK = 0`, and `ADLX_SUCCEEDED(x)` also accepts `ADLX_ALREADY_ENABLED` and `ADLX_ALREADY_INITIALIZED`.

What calling ADLX without the SDK headers would take: these are COM-like objects, a pointer to a vtable whose first three slots for most interfaces are `Acquire`, `Release` and `QueryInterface(const wchar_t* iid, void**)`. You would need:
1. Exact vtable slot order for `IADLXSystem`. Note that its C vtable starts with `GetHybridGraphicsType, GetGPUs, QueryInterface, …, GetPerformanceMonitoringServices (0-based slot 9), TotalSystemRAM, GetI2C`, not the usual Acquire/Release/QueryInterface trio.
2. Vtable orders for `IADLXGPUList` (`…, At_GPUList`), `IADLXGPU`/`IADLXGPU1`/`IADLXGPU2`, `IADLXPerformanceMonitoringServices`, `IADLXGPUMetricsSupport` and `IADLXGPUMetrics`/`IADLXGPUMetrics1`.
3. Interface ID strings (wide strings equal to the interface name, for example `L"IADLXGPUMetrics1"`), the `ADLX_RESULT` enum values, and the full version constant.

All of that is transcribed from the restricted headers, so this route carries **license risk**. **[verified]** for the layouts; the legal conclusion is my reading and is not legal advice.

Metrics interfaces **[verified]** ([IPerformanceMonitoring.h](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/blob/32b5a740d42295c5dfe9026b9f52683da0f3af91/SDK/Include/IPerformanceMonitoring.h), [IPerformanceMonitoring2.h](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/blob/32b5a740d42295c5dfe9026b9f52683da0f3af91/SDK/Include/IPerformanceMonitoring2.h), [ISystem.h / ISystem1.h](https://github.com/GPUOpen-LibrariesAndSDKs/ADLX/tree/32b5a740d42295c5dfe9026b9f52683da0f3af91/SDK/Include)):
- `IADLXSystem::GetPerformanceMonitoringServices(IADLXPerformanceMonitoringServices**)` returns `GetSupportedGPUMetrics(IADLXGPU*, IADLXGPUMetricsSupport**)` and `GetCurrentGPUMetrics(IADLXGPU*, IADLXGPUMetrics**)`. It also offers sampling interval, history, `StartPerformanceMetricsTracking` and `GetCurrentAllMetrics`.
- `IADLXGPUMetrics` methods:
  - `TimeStamp(adlx_int64*)`
  - `GPUUsage(adlx_double*)` (%)
  - `GPUClockSpeed(adlx_int*)` (MHz)
  - `GPUVRAMClockSpeed(adlx_int*)`
  - `GPUTemperature(adlx_double*)` (edge)
  - **`GPUHotspotTemperature(adlx_double*)`**
  - `GPUPower(adlx_double*)` (W)
  - `GPUTotalBoardPower(adlx_double*)`
  - `GPUFanSpeed(adlx_int*)` (**RPM**)
  - `GPUVRAM(adlx_int*)` (dedicated memory used, **MB**)
  - `GPUVoltage(adlx_int*)`
  - `GPUIntakeTemperature(adlx_double*)`
- `IADLXGPUMetrics1` (obtained by QueryInterface): **`GPUMemoryTemperature(adlx_double*)`**, `NPUFrequency`, `NPUActivityLevel`. Each value has an `IsSupportedXxx` check on `IADLXGPUMetricsSupport`, `...Support1` or `...Support2`.
- GPU identity: `IADLXGPU`: `Name`, `VendorId`, `DeviceId`, `BIOSInfo(partNumber, version, date)`, `TotalVRAM` (MB), `VRAMType`, `UniqueId`, `PNPString`. `IADLXGPU2`: `DriverVersion`, `AMDSoftwareVersion`, `LUID(ADLX_LUID*)`, which you can use to match DXGI adapters.
- **PCIe:** only `IADLXGPU1::PCIBusType(ADLX_PCI_BUS_TYPE*)` (an enum ending at `PCIE_5_0`) and `IADLXGPU1::PCIBusLaneWidth(adlx_uint*)`. The headers do not say whether these are current or maximum values, and there is no PCIe Gen 6 entry. **[verified]** that this is all there is.
- **Throttle reasons:** none. No throttle or limit-reason API exists anywhere in the ADLX headers (grep for "throttl" returns nothing). **[verified]**
- ADLX has no Linux implementation (only `ADLXHelper/Windows` exists). **[verified]**

### 2a'. Windows alternative: ADL (`atiadlxx.dll`), whose headers carry MIT notices
- The header files `adl_sdk.h`, `adl_defines.h` and `adl_structures.h` each carry a per-file **"MIT LICENSE: Permission is hereby granted…"** notice. **[verified]** [display-library/include @84c881c](https://github.com/GPUOpen-LibrariesAndSDKs/display-library/tree/84c881cb2136f04287eb3af12de00a9e2867aa9e/include)
- **Caveat:** the same repository also ships `Public-Documents/ADL SDK EULA.pdf`. That EULA is restrictive: object-code-only distribution, a ban on "Excluded License" (copyleft) terms, and "may not publish the SDK". Its terms conflict with the per-file MIT notices. **[verified]** that both exist. Which one governs the headers is ambiguous. Lower-risk option: write your own declarations for the handful of functions and structs you use, and keep the MIT notice if you copy anything.
- DLLs: `atiadlxx.dll` (64-bit) and `atiadlxy.dll` (32-bit), shipped with the driver (System32). Context: `ADL2_Main_Control_Create`/`ADL2_Main_Control_Destroy`. **[verified]** from the samples.
- Sensor snapshot: `ADL2_New_QueryPMLogData_Get(ADL_CONTEXT_HANDLE, int iAdapterIndex, ADLPMLogDataOutput*)` returns `sensors[ADL_PMLOG_MAX_SENSORS=256]` of `{int supported; int value;}`. The ADL 18.1 release notes say "**Deprecate ADL2_New_QueryPMLogData_Get**". The replacement shown in the Overdrive8 sample is `ADL2_Overdrive8_PMLog_ShareMemory_Support/Start/Read/Stop`. **[verified]** [README.md release notes](https://github.com/GPUOpen-LibrariesAndSDKs/display-library/blob/84c881cb2136f04287eb3af12de00a9e2867aa9e/Public-Documents/README.md)
- Sensor indices (`adl_defines.h`) **[verified]**:
  - Clocks: `ADL_PMLOG_CLK_GFXCLK=1`, `CLK_MEMCLK=2`
  - Temperatures: `TEMPERATURE_EDGE=8`, `TEMPERATURE_MEM=9`, **`TEMPERATURE_HOTSPOT=27`**, `TEMPERATURE_HOTSPOT_GCD=50`, `TEMPERATURE_HOTSPOT_MCD=51`, `TEMPERATURE_INTAKE=74`
  - Fan: `FAN_RPM=14`, `FAN_PERCENTAGE=15`
  - Activity: `INFO_ACTIVITY_GFX=19`, `INFO_ACTIVITY_MEM=20`
  - Power: `ASIC_POWER=23`, `GFX_POWER=30`, `BOARD_POWER=73`
  - Throttling: **`THROTTLER_STATUS=35`**, plus `THROTTLE_PERCENTAGE_*` 52–57
  - PCIe: **`BUS_SPEED=40`**, **`BUS_LANES=41`**, `BUS_CURR_MAX_SPEED=58`
  - Throttle bit flags: `ADL_PMLOG_THROTTLE_POWER=1<<0`, `_THERMAL=1<<1`, `_CURRENT=1<<2`

### 2b. Linux amdgpu (sysfs/hwmon), kernel `master` (7.3 cycle)
Base path: `/sys/class/drm/cardN/device/` (the PCI device). hwmon lives at `…/device/hwmon/hwmonX/`. **[verified]** [amdgpu_pm.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/amdgpu_pm.c)

| Metric | File | Units / notes |
|---|---|---|
| GFX clock | `hwmon/freq1_input` (label `freq1_label` = "sclk") | **Hz** ("the gfx/compute clock in hertz") |
| Memory clock | `hwmon/freq2_input` (dGPU only) | Hz |
| DPM levels | `pp_dpm_sclk`, `pp_dpm_mclk`, `pp_dpm_socclk`, `pp_dpm_fclk`, `pp_dpm_dcefclk`, `pp_dpm_pcie` | text like `1: 800Mhz *`, where `*` marks the current level and `S:` a deep-sleep level |
| Edge temperature | `hwmon/temp1_input`, `temp1_label` = `edge` | millidegrees C |
| Junction (hotspot) | `hwmon/temp2_input`, `temp2_label` = `junction` | SOC15 (Vega+) dGPUs only |
| Memory temperature | `hwmon/temp3_input`, `temp3_label` = `mem` | SOC15 dGPUs only; also `temp*_crit`, `_crit_hyst`, `_emergency` |
| Power | `hwmon/power1_average` (average) and/or `hwmon/power1_input` (instantaneous) | µW. "Not all products support both". Each is hidden when the matching sensor returns -EOPNOTSUPP, and `power1_average` is hidden on SI and on APUs older than Renoir. Read `power1_average` if present, else `power1_input`. On APUs the value includes the CPU. |
| Power cap | `hwmon/power1_cap`, `power1_cap_min`, `power1_cap_max` | µW |
| Fan | `hwmon/fan1_input` (RPM), `pwm1` (0–255), `pwm1_enable`, `fan1_min`, `fan1_max`, `fan1_target` | |
| GPU busy | `gpu_busy_percent` | % (SMU-computed) |
| Memory busy | `mem_busy_percent` | % |
| VRAM | `mem_info_vram_used`, `mem_info_vram_total` (also `mem_info_vis_vram_*`, `mem_info_vram_vendor`) | bytes. **[verified]** [amdgpu_vram_mgr.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c) |
| VBIOS | `vbios_version` (also `vbios_build`) | **[verified]** [amdgpu_atombios.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_atombios.c) |
| Unique ID | `unique_id` | 16 hex digits. GFX9 and newer only; the file is absent on GFX8 and older. |
| PCIe link | `current_link_speed`, `current_link_width`, `max_link_speed`, `max_link_width` | generic PCI sysfs attributes. **[verified]** [pci-sysfs.c](https://github.com/torvalds/linux/blob/master/drivers/pci/pci-sysfs.c). Caveat below. |
| PCIe bandwidth estimate | `pcie_bw` | received and sent message counts plus max payload size |
| All-in-one snapshot | `gpu_metrics` (binary) | see below |

**`gpu_metrics` binary file** **[verified]** ([kgd_pp_interface.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/include/kgd_pp_interface.h)):
- **Header.** Every version starts with `struct metrics_table_header { uint16_t structure_size; uint8_t format_revision; uint8_t content_revision; }`. Always dispatch on `(format_revision, content_revision)` and bound reads by `structure_size`.
- **Defined versions:** `v1_0` … `v1_9`, `v2_0` … `v2_4` and `v3_0`.
  - `v1.x` is for dGPUs. `v1_4` to `v1_8` are the MI300-class layouts. `v1_9` is a new self-describing format: `attr_count` plus a `gpu_metrics_attr { uint64_t attr_encoding; void* attr_value; }[]` array.
  - `v2.x` and `v3.0` are for APUs.
- **Consumer dGPUs use `gpu_metrics_v1_3`.** Navi10 (SMU11), Sienna Cichlid (RDNA2), SMU 13.0.0/13.0.7 (RDNA3) and SMU 14.0.2 (RDNA4) all fill it. **[verified]** [swsmu sources](https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/amd/pm/swsmu)
- **`v1_3` fields:**
  - temperatures: `temperature_edge`, `temperature_hotspot`, `temperature_mem`, `temperature_vrgfx/vrsoc/vrmem`
  - activity: `average_gfx_activity`, `average_umc_activity`, `average_mm_activity`
  - power: `average_socket_power`, `energy_accumulator`
  - clocks: `average_gfxclk_frequency`, `average_uclk_frequency`, `current_gfxclk`, `current_uclk`, and others
  - **`throttle_status`** (uint32, ASIC-specific bit meanings)
  - `current_fan_speed`
  - **`pcie_link_width`**, **`pcie_link_speed`** (in units of **0.1 GT/s**)
  - voltages: `voltage_soc/gfx/mem`
  - **`indep_throttle_status`** (uint64, ASIC-independent)
- `indep_throttle_status` exists in `v1_3`, `v2_2`, `v2_3` and `v2_4`. Its bit meanings (`SMU_THROTTLER_*_BIT` in [amdgpu_smu.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/pm/swsmu/inc/amdgpu_smu.h)) are:
  - power: `PPT0..3 = 0..3`, `SPL = 4`, `FPPT = 5`, `SPPT = 6`, `SPPT_APU = 7`
  - current: `TDC_GFX = 16`, `TDC_SOC = 17`, `TDC_MEM = 18`, `TDC_VDD = 19`, `TDC_CVIP = 20`, `EDC_CPU = 21`, `EDC_GFX = 22`, `APCC = 23`
  - temperature: `TEMP_GPU = 32`, `TEMP_CORE = 33`, `TEMP_MEM = 34`, `TEMP_EDGE = 35`, `TEMP_HOTSPOT = 36`, `TEMP_SOC = 37`, `TEMP_VR_GFX = 38`, `TEMP_VR_SOC = 39`, `TEMP_VR_MEM0 = 40`, `TEMP_VR_MEM1 = 41`, `TEMP_LIQUID0 = 42`, `TEMP_LIQUID1 = 43`, `VRHOT0 = 44`, `VRHOT1 = 45`, `PROCHOT_CPU = 46`, `PROCHOT_GFX = 47`
  - other: `PPM = 56`, `FIT = 57`
- Fields the firmware does not fill read back as all-ones (`0xFFFF` / `UINT32_MAX`), which is common practice in tools such as amdgpu_top. **[unverified]** as a formal guarantee.

**PCIe caveat (AMD dGPUs):** Navi-era boards have an on-package PCIe switch, so the GPU function's own `current_link_*` can show the internal link (for example 16 GT/s x16) while the real slot link sits on an upstream bridge. To get the true slot link, walk up the PCI parents (`/sys/bus/pci/devices/<bdf>/..`) and read the upstream port's `current_link_*`. There are also reports of `current_link_speed` disagreeing with `pp_dpm_pcie`. **[snippet]** [drm/amd#1447](https://gitlab.freedesktop.org/drm/amd/-/issues/1447), [drm/amd#1967](https://gitlab.freedesktop.org/drm/amd/-/issues/1967), [HWiNFO forum: RX 7600](https://www.hwinfo.com/forum/threads/pcie-link-speed-is-not-displayed-correctly-for-amd-rx-7600.11005/latest)

---

## 3. Intel

### 3a. IGCL (Intel Graphics Control Library, Windows)
**License:** the "Intel Software License Agreement 10.07.21" (`LICENSE` and `License.txt`). **[verified]** [intel/drivers.gpu.control-library @b6c4629 (v298)](https://github.com/intel/drivers.gpu.control-library/blob/b6c462933502e13d1537dd5024949a51be30e63d/LICENSE)
- Grant: *"You may use and redistribute Intel's Control API software and the header files … **solely for use on Intel platforms**, provided … Redistributions must reproduce the above copyright notice and the following terms of use … No reverse engineering … of Software provided to you in binary form."*
- The header itself says *"…you may not use, modify, copy, publish, distribute … without Intel's prior written permission"* unless the License provides otherwise.
- Net effect: **not open source** and not MIT. You may redistribute the header unmodified as a separately licensed third-party file, with its terms, used only for the Intel backend. Writing your own minimal declarations is cleaner.

Loading **[verified]** ([cApiWrapper.cpp](https://github.com/intel/drivers.gpu.control-library/blob/b6c462933502e13d1537dd5024949a51be30e63d/Source/cApiWrapper.cpp), [README](https://github.com/intel/drivers.gpu.control-library/blob/b6c462933502e13d1537dd5024949a51be30e63d/README.md)):
- **DLL:** `ControlLib.dll` (x64) or `ControlLib32.dll` (x86). It is "distributed as part of Intel Graphics driver package". The official wrapper loads it with `LoadLibraryExW(L"ControlLib.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)`, then `GetProcAddress(h, "ctlInit")` and similar.
- **Telemetry is 64-bit only.** The README says "Performance & Telemetry API's, i.e., Engine/Fan/Telemetry/Frequency/Memory/Overclock/PCI/Power/Temperature are limited to 64-bit applications … This is a Level0 limitation."
- **Init:**
  - `ctl_result_t ctlInit(ctl_init_args_t* pInitDesc, ctl_api_handle_t* phAPIHandle)` and `ctlClose(ctl_api_handle_t)`.
  - `ctl_init_args_t = { uint32_t Size; uint8_t Version; ctl_version_info_t AppVersion; ctl_init_flags_t flags; ctl_version_info_t SupportedVersion; ctl_application_id_t ApplicationUID; }`.
  - Set `AppVersion = CTL_MAKE_VERSION(1,1)` (the header's `CTL_IMPL_VERSION`) and `flags = CTL_INIT_FLAG_USE_LEVEL_ZERO` (bit 0, "usually required for telemetry").

Functions (`igcl_api.h`, all `CTL_APICALL`) **[verified]** ([igcl_api.h](https://github.com/intel/drivers.gpu.control-library/blob/b6c462933502e13d1537dd5024949a51be30e63d/include/igcl_api.h)):
- **Devices:** `ctlEnumerateDevices(ctl_api_handle_t, uint32_t* pCount, ctl_device_adapter_handle_t* phDevices)`. Then `ctlGetDeviceProperties(hDA, ctl_device_adapter_properties_t*)`, which returns `pDeviceID` (an OS LUID; set `device_id_size`), `driver_version`, `pci_vendor_id`, `pci_device_id`, `rev_id`, `name[]`, `pci_subsys_id`, and `adapter_bdf` (Version>1).
- **Bulk telemetry (best single call):** `ctlPowerTelemetryGet(hDA, ctl_power_telemetry_t*)`, and since recent headers `ctlPowerTelemetryGetV2(hDA, ctl_power_telemetry_v2_t*)` (rate-limited to 50 ms). Each field is a `ctl_oc_telemetry_item_t {bool bSupported; ctl_units_t units; ctl_data_type_t type; ctl_data_value_t value;}`. Fields:
  - energy and time: `timeStamp`, `gpuEnergyCounter` (monotonic, so power = Δenergy/Δtime), `totalCardEnergyCounter`, `vramEnergyCounter`
  - voltage: `gpuVoltage`
  - clocks: `gpuCurrentClockFrequency`, `vramCurrentClockFrequency`, `vramCurrentEffectiveFrequency`, `gpuEffectiveClock` (Version>0)
  - temperatures: **`gpuCurrentTemperature`** ("read from the sensor reporting the highest value"), **`vramCurrentTemperature`**, VR temperatures (Version>0)
  - activity counters: `globalActivityCounter`, `renderComputeActivityCounter`, `mediaActivityCounter`
  - fans: `fanSpeed[CTL_FAN_COUNT=5]`
  - `psu[5]`
  - limit flags: **`gpuPowerLimited`, `gpuTemperatureLimited`, `gpuCurrentLimited`, `gpuVoltageLimited`, `gpuUtilizationLimited`**. The `vram*Limited` flags are "Deprecated / Not-supported".
- **Frequency:**
  - `ctlEnumFrequencyDomains(hDA, uint32_t* pCount, ctl_freq_handle_t*)`, `ctlFrequencyGetProperties` (type `CTL_FREQ_DOMAIN_GPU=0 / MEMORY=1 / MEDIA=2`, min, max)
  - `ctlFrequencyGetState(ctl_freq_handle_t, ctl_freq_state_t*)` returns `{currentVoltage, request, tdp, efficient, actual (MHz), throttleReasons}`
  - `ctlFrequencyGetThrottleTime(ctl_freq_handle_t, ctl_freq_throttle_time_t*)` returns `{throttleTime µs, timestamp µs}`
- **Throttle reason flags:** `CTL_FREQ_THROTTLE_REASON_FLAG_AVE_PWR_CAP=1<<0` (PL1), `BURST_PWR_CAP=1<<1` (PL2), `CURRENT_LIMIT=1<<2` (PL4), `THERMAL_LIMIT=1<<3`, `PSU_ALERT=1<<4`, `SW_RANGE=1<<5`, `HW_RANGE=1<<6`.
- **Temperature:** `ctlEnumTemperatureSensors(hDA, uint32_t*, ctl_temp_handle_t*)`, `ctlTemperatureGetProperties`, then `ctlTemperatureGetState(ctl_temp_handle_t, double* pTemperature)` (°C). Sensor types: `CTL_TEMP_SENSORS_GLOBAL=0`, `GPU=1` (max over GPU sensors, effectively hotspot), `MEMORY=2`, plus `*_MIN` variants 3–5.
- **Power:** `ctlEnumPowerDomains`, `ctlPowerGetProperties`, `ctlPowerGetEnergyCounter(ctl_pwr_handle_t, ctl_power_energy_counter_t*)` (energy in **µJ**, timestamp µs), `ctlPowerGetLimits(ctl_pwr_handle_t, ctl_power_limits_t*)`, `ctlOverclockPowerLimitGet`.
- **Fan:** `ctlEnumFans`, `ctlFanGetProperties`, `ctlFanGetState(ctl_fan_handle_t, ctl_fan_speed_units_t {CTL_FAN_SPEED_UNITS_RPM=0, PERCENT=1}, int32_t* pSpeed)` (-1 means not measurable).
- **Memory:** `ctlEnumMemoryModules`, `ctlMemoryGetProperties`, `ctlMemoryGetState(ctl_mem_handle_t, ctl_mem_state_t*)` returning `{free, size}` in bytes, and `ctlMemoryGetBandwidth`.
- **Utilization:** `ctlEnumEngineGroups`, `ctlEngineGetProperties`, `ctlEngineGetActivity(ctl_engine_handle_t, ctl_engine_stats_t*)` returning `{activeTime µs, timestamp µs}`.
- **PCIe:** `ctlPciGetProperties(hDA, ctl_pci_properties_t*)` returns `{address, maxSpeed{gen,width,maxBandwidth}, resizable_bar_supported, resizable_bar_enabled}`. `ctlPciGetState(hDA, ctl_pci_state_t*)` returns `{speed{gen,width,maxBandwidth}}`, with -1 meaning unknown.

### 3b. Level Zero Sysman (`zes*`)
- **License: MIT.** The loader repo `LICENSE` is MIT, and the headers carry `SPDX-License-Identifier: MIT`. **[verified]** [oneapi-src/level-zero @2a4fd8e](https://github.com/oneapi-src/level-zero/blob/2a4fd8e0ce866fdb5ed237d5d662dd2dd37b9657/LICENSE) (loader v1.34.0, spec v1.19.12, `ZE_API_VERSION_1_19`).
- **Libraries:** Linux `libze_loader.so.1` (CMake target `ze_loader`, `SOVERSION` = major version). Windows `ze_loader.dll`. The Windows loader finds the GPU driver through the display adapter's `LevelZeroDriverPath` registry value. **[verified]** [source/CMakeLists.txt](https://github.com/oneapi-src/level-zero/blob/2a4fd8e0ce866fdb5ed237d5d662dd2dd37b9657/source/CMakeLists.txt), [driver_discovery_win.cpp](https://github.com/oneapi-src/level-zero/blob/2a4fd8e0ce866fdb5ed237d5d662dd2dd37b9657/source/loader/windows/driver_discovery_win.cpp).
  - On Linux you also need Intel's compute-runtime (the GPU driver plugin, `libze_intel_gpu.so.1`) installed; it is often absent on desktop distributions. **[unverified]** as a distro-by-distro statement.
  - On Windows, Intel's driver installs the Level Zero driver, and IGCL's README implies its telemetry is built on Level Zero. How complete Sysman coverage is on Windows is **[unverified]**, so prefer IGCL on Windows.
- **Signatures** ([zes_api.h](https://github.com/oneapi-src/level-zero/blob/2a4fd8e0ce866fdb5ed237d5d662dd2dd37b9657/include/zes_api.h)) **[verified]**:
  - Init: `zesInit(zes_init_flags_t flags)` (`ZES_INIT_FLAG_GPU_ONLY = 1<<0`; the `ZES_ENABLE_SYSMAN` env-var method is the legacy path).
  - Enumeration: `zesDriverGet(uint32_t* pCount, zes_driver_handle_t*)`, `zesDeviceGet(zes_driver_handle_t, uint32_t* pCount, zes_device_handle_t*)`, `zesDeviceGetProperties(zes_device_handle_t, zes_device_properties_t*)`.
  - Frequency: `zesDeviceEnumFrequencyDomains(zes_device_handle_t, uint32_t*, zes_freq_handle_t*)`. `zesFrequencyGetState(zes_freq_handle_t, zes_freq_state_t*)` returns `{currentVoltage, request, tdp, efficient, actual (MHz), throttleReasons}`. Also `zesFrequencyGetThrottleTime(zes_freq_handle_t, zes_freq_throttle_time_t*)`.
  - Temperature: `zesDeviceEnumTemperatureSensors(zes_device_handle_t, uint32_t*, zes_temp_handle_t*)`, `zesTemperatureGetProperties`, `zesTemperatureGetState(zes_temp_handle_t, double*)`.
  - Power: `zesDeviceEnumPowerDomains(zes_device_handle_t, uint32_t*, zes_pwr_handle_t*)`, `zesDeviceGetCardPowerDomain(zes_device_handle_t, zes_pwr_handle_t*)`, `zesPowerGetEnergyCounter(zes_pwr_handle_t, zes_power_energy_counter_t*)` (µJ plus µs timestamp), `zesPowerGetLimitsExt(zes_pwr_handle_t, uint32_t* pCount, zes_power_limit_ext_desc_t*)`.
  - Fan: `zesDeviceEnumFans(zes_device_handle_t, uint32_t*, zes_fan_handle_t*)`, `zesFanGetState(zes_fan_handle_t, zes_fan_speed_units_t {RPM=0, PERCENT=1}, int32_t*)`.
  - PCIe: `zesDevicePciGetProperties(zes_device_handle_t, zes_pci_properties_t*)` (address plus `maxSpeed{int32 gen, int32 width, int64 maxBandwidth}`) and `zesDevicePciGetState(zes_device_handle_t, zes_pci_state_t*)` (`status`, `qualityIssues`, `stabilityIssues`, `speed{gen,width,maxBandwidth}`, with -1 meaning unknown).
  - Memory: `zesDeviceEnumMemoryModules(zes_device_handle_t, uint32_t*, zes_mem_handle_t*)`, `zesMemoryGetProperties`, `zesMemoryGetState(zes_mem_handle_t, zes_mem_state_t*)` (`health`, `free`, `size` in bytes).
  - Utilization: `zesDeviceEnumEngineGroups`, `zesEngineGetActivity(zes_engine_handle_t, zes_engine_stats_t*)`.
  - UUID match: `zesDriverGetDeviceByUuidExp`.
- **Throttle flags:** `ZES_FREQ_THROTTLE_REASON_FLAG_AVE_PWR_CAP=1<<0`, `BURST_PWR_CAP=1<<1`, `CURRENT_LIMIT=1<<2`, `THERMAL_LIMIT=1<<3`, `PSU_ALERT=1<<4`, `SW_RANGE=1<<5`, `HW_RANGE=1<<6`, and the newer `VOLTAGE=1<<7`, `THERMAL=1<<8`, `POWER=1<<9`.
- **Temperature sensors:** `ZES_TEMP_SENSORS_GLOBAL=0`, `GPU=1`, `MEMORY=2`, `*_MIN=3..5`, `GPU_BOARD=6`, `GPU_BOARD_MIN=7`, `VOLTAGE_REGULATOR=8`, `COMPOSITE=9`, `GPU_BOARD_SINGLE=10`, `VOLTAGE_REGULATOR_SINGLE=11`.
- **Frequency domains:** `ZES_FREQ_DOMAIN_GPU=0`, `MEMORY=1`, `MEDIA=2`.

### 3c. Linux i915 / xe sysfs (kernel `master`)
**i915** **[verified]** ([ABI i915-hwmon](https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/sysfs-driver-intel-i915-hwmon), [intel_gt_sysfs_pm.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel_gt_sysfs_pm.c), [i915_sysfs.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915_sysfs.c)):
- hwmon (`/sys/class/drm/cardN/device/hwmon/hwmonX/`):
  - `energy1_input` (µJ, kernel 6.2+)
  - `power1_max` (PL1, µW, read/write), `power1_rated_max` (TDP), `power1_max_interval`, `power1_crit`, `curr1_crit`
  - `in0_input` (mV)
  - `fan1_input` (RPM, **6.12+**)
  - `temp1_input` (package temperature in m°C, **6.12+**)
  - All of these are "only supported for particular platforms".
- Frequency:
  - Legacy device-level files: `/sys/class/drm/cardN/gt_act_freq_mhz` and `gt_cur_freq_mhz`, plus `gt_boost/max/min/RP0/RP1/RPn_freq_mhz`.
  - Per-GT files: `/sys/class/drm/cardN/gt/gtX/rps_act_freq_mhz`, `rps_cur_freq_mhz`, `rps_*`, and `punit_req_freq_mhz`.
- **Throttle (per GT):** `/sys/class/drm/cardN/gt/gtX/throttle_reason_status`, `throttle_reason_pl1`, `_pl2`, `_pl4`, `_thermal`, `_prochot`, `_ratl`, `_vr_thermalert`, `_vr_tdc`. Each is a 0/1 boolean.

**xe** **[verified]** ([ABI xe-hwmon](https://github.com/torvalds/linux/blob/master/Documentation/ABI/testing/sysfs-driver-intel-xe-hwmon), [xe_hwmon.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/xe/xe_hwmon.c), [xe_gt_freq.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/xe/xe_gt_freq.c), [xe_gt_throttle.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/xe/xe_gt_throttle.c), [xe_tile_sysfs.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/xe/xe_tile_sysfs.c)):
- hwmon power and energy:
  - `power1_max` (card PL1, µW, kernel 6.5+), `power1_rated_max`, `power1_crit`, `power1_cap`
  - `power2_max` and related files for the package (6.8+)
  - `energy1_input` (card, µJ), `energy2_input` (package)
  - `in1_input` (package voltage, mV)
- hwmon temperatures (labels via `tempN_label`):
  - `temp2_input` "pkg", 6.15+
  - `temp3_input` "vram", 6.15+
  - `temp4_input` "mctrl" (memory controller)
  - `temp5_input` "pcie"
  - `temp6..21_input` "vram_ch_N"
- hwmon fans: `fan1_input`, `fan2_input`, `fan3_input` (fan1 since 6.16).
- **Frequency:** `/sys/class/drm/cardN/device/tile<T>/gt<G>/freq0/`, which contains `act_freq` (actual, decided by PCODE), `cur_freq` (requested), `rpn_freq`, `rpe_freq`, `rpa_freq`, `rp0_freq`, `min_freq`, `max_freq` and `power_profile`.
- **Throttle:** `…/freq0/throttle/status` (0/1), `reasons` (space-separated list, or "none"; preferred because reading `status` and then the individual files is racy), and per-reason files:
  - most platforms: `reason_pl1`, `reason_pl2`, `reason_pl4`, `reason_thermal`, `reason_prochot`, `reason_ratl`, `reason_vr_thermalert`, `reason_vr_tdc`
  - Crescent Island adds: `reason_soc_thermal`, `reason_mem_thermal`, `reason_vr_thermal`, `reason_iccmax`, `reason_pwrbrk`, `reason_soc_avg_thermal`, `reason_fastvmode`, `reason_psys_pl1/2`, `reason_p0_freq`, `reason_psys_crit`
- **Utilization and VRAM on Intel Linux:** there is no simple sysfs file. Use DRM fdinfo (`drm-engine-<key>` ns, `drm-cycles-*`/`drm-total-cycles-*`, `drm-total-<region>`, `drm-resident-<region>`), the perf PMU, or Level Zero. **[verified]** [drm-usage-stats.rst](https://github.com/torvalds/linux/blob/master/Documentation/gpu/drm-usage-stats.rst)

---

## 4. AMD FidelityFX Super Resolution (FSR)

| Version | Where | License | Source? | Backends | Notes |
|---|---|---|---|---|---|
| **FSR 1** (EASU + RCAS) | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (last commit 2021-11-15) | **MIT** ([license.txt](https://github.com/GPUOpen-Effects/FidelityFX-FSR/blob/a21ffb8f6c13233ba336352bdff293894c706575/license.txt)) | Yes | **Header-only**: `ffx-fsr/ffx_a.h` + `ffx_fsr1.h`. Define `A_GPU` with `A_GLSL` or `A_HLSL` (`A_CPU` for constant setup), then call `FsrEasuCon`/`FsrEasuF` and `FsrRcasCon`/`FsrRcasF`. | **Easiest to embed** in both Vulkan (GLSL to SPIR-V) and D3D12 (HLSL). Spatial only. **[verified]** |
| **FSR 2.2** | [GPUOpen-Effects/FidelityFX-FSR2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) (last commit 2023-05-31) | **MIT** ([LICENSE.txt](https://github.com/GPUOpen-Effects/FidelityFX-FSR2/blob/1680d1edd5c034f88ebbbb793d8b88f8842cf804/LICENSE.txt)) | Yes | `src/ffx-fsr2-api/{dx12,vk}` (the README says "comes with DirectX(R)12 and Vulkan(R) backends") | Shader build uses Windows-only `tools/sc/FidelityFX_SC.exe` + `glslangValidator.exe` (target vulkan1.1). Linux builds need your own glslang/dxc step. Temporal, so it needs motion vectors and depth. **[verified]** |
| **FSR 3.1.x** (upscaler + FG) | FidelityFX SDK **v1.1.4** (tag, 2025-05-08) | **MIT** ([LICENSE.txt @v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/LICENSE.txt)) | Yes, including `sdk/include/FidelityFX/gpu/fsr1/ffx_fsr1.h` | `sdk/src/backends/{dx12,vk}`, plus `PrebuiltSignedDLL/amd_fidelityfx_{dx12,vk}.dll` | The last SDK release with Vulkan. Uses `FidelityFX_SC.exe` for shader permutations. **[verified]** |
| **FSR SDK 2.x** ("AMD FSR SDK 2.3.0 Redstone", tag v2.3.0, 2026-06-24) | [FidelityFX-SDK main](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.3.0) | Mixed (see below) | FSR 2.3.4 and 3.1.5 upscaler shaders and headers are included (`Kits/FidelityFX/upscalers/fsr3`). **FSR 4.x is binary-only.** | **DX12 only**: "Vulkan is currently not supported in AMD FSR™ SDK 2.3" | Apps "must use one of the provided signed DLLs": `amd_fidelityfx_loader_dx12.dll` plus `amd_fidelityfx_upscaler_dx12.dll`, which contains the FSR 4 ML, FSR 3.1 and FSR 2 providers. API: `ffxCreateContext`, `ffxDestroyContext`, `ffxDispatch`, `ffxQuery`, `ffxConfigure`, loaded with `LoadLibrary` and `GetProcAddress`. **[verified]** [ffx-api.md](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/Kits/FidelityFX/docs/getting-started/ffx-api.md) |
| **FSR 4** (ML upscaler, 4.1.1 in SDK 2.3.0) | signed DLL only | Covered by the SDK license (below) | **No official source.** It was briefly published on GitHub in Aug 2025 and pulled. **[snippet]** [VideoCardz](https://videocardz.com/newz/amd-releases-fidelityfx-sdk-2-0), [Igor'sLAB](https://www.igorslab.de/en/amd-fsr-4-leak-and-fidelityfx-sdk-2-0-a-sober-evaluation/) | DX12 | **Not RDNA4-only any more.** The SDK 2.3.0 docs say "FSR4 requires an AMD 7000 series discrete GPU or AMD 9000 series GPU or later", and the 4.1.1 notes say "Added support for AMD FSR™ Upscaling 4.1 on AMD Radeon RX 7000 Series (AMD RDNA™ 3 architecture) discrete GPUs". **[verified]** [super-resolution-ml.md](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/Kits/FidelityFX/docs/techniques/super-resolution-ml.md), [whats-new](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/Kits/FidelityFX/docs/whats-new/index.md) |

**SDK 2.x license structure** **[verified]** [docs/license.md @v2.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/docs/license.md):
- The preamble license "applies to all files except as noted below". It allows distribution "**in binary form only**", says "No reverse engineering, decompilation, or disassembly", and requires reproducing the notice.
- A long list of files (about 845, which by my comparison is every file in the checkout) is then declared MIT, and that list **includes the `signedbin/*.dll`**.
- The README also says "The AMD FSR™ SDK is open source, and available under the MIT license."
- So the document is internally ambiguous. Conservative reading: treat the signed DLLs as redistributable binaries (unmodified, no reverse engineering, notice reproduced). That is fine for an optional runtime plugin, and the MIT app code calls them through `ffx_api.h`, which is MIT.

**Recommendation for a Vulkan + D3D12 app built from source:** FSR 1 is header-only, MIT, and works with any API. Next best is FSR 2.2 or FSR 3.1 (SDK 1.1.4), which are MIT with VK and DX12 backends but need shader-compiler plumbing. FSR 4 is only an optional DX12 runtime plugin through the signed DLLs, on Windows with RDNA3/RDNA4.

---

## 5. DLSS, Streamline and XeSS licensing for an MIT app

### NVIDIA DLSS SDK (NGX, `nvngx_dlss.dll`): "NVIDIA RTX SDKs LICENSE" (v. March 14, 2024)
Source: [NVIDIA/DLSS @9981c63 (DLSS 310.9.16, 2026-10-16) LICENSE.txt](https://github.com/NVIDIA/DLSS/blob/9981c6350dc58c0ce21f024fd65e31e851c31689/LICENSE.txt). **[verified]**
- **§1(c):** you may distribute SDK materials only "as incorporated in object code format into a software application".
- **§2:**
  - (a) the application needs "material additional functionality"
  - (b) modified source must carry "This software contains source code provided by NVIDIA Corporation."
  - (c) **you must "distribute the SDK subject to the terms at least as protective as the terms of this license"**
  - (d) you must notify NVIDIA of non-compliant use
- **§4(e):** **"You may not use the SDK in any manner that would cause it to become subject to an open source software license"** (examples: source disclosure, a derivative-works license, redistributable at no charge).
- **§4(a):** no reverse engineering.
- **Supplement:**
  - §1: DLSS and NGX are licensed only for systems with NVIDIA GPUs.
  - §4: **notify NVIDIA before commercial release** (https://developer.nvidia.com/sw-notification).
  - §6.1: NVIDIA may push over-the-air updates.
  - §7.1(b): **"you must attribute the use of the applicable SDK and include the NVIDIA Marks on splash screens, in the about box of the application (if present)…"**, and mark usage needs NVIDIA's prior written approval (§7.2(c)).
- Headers `nvsdk_ngx*.h` are `SPDX-License-Identifier: LicenseRef-NvidiaProprietary` ("Any use, reproduction, disclosure or distribution … without an express license agreement … is strictly prohibited"), so they **cannot live in an MIT repo**.
- Runtimes:
  - Linux: `libnvidia-ngx-dlss.so.310.9.16` (+`dlssd`, `dlssg`), with the app linking `libnvsdk_ngx.a`, so **building needs the SDK**.
  - Windows: `nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`.
- **Verdict:** DLSS cannot be part of the MIT build. It is possible only as a separately built, separately licensed optional plugin (its own repo or `extras/`, built by whoever accepts the NVIDIA terms). It needs the attribution and notification obligations, and binaries must carry terms at least as protective as NVIDIA's. Shipping `nvngx_dlss.dll` in an MIT release archive without a protective EULA conflicts with §2(c). Safest is to have the user point the app at an existing `nvngx_dlss.dll`. **[verified]** terms; the conclusion is my reading, not legal advice.

### NVIDIA Streamline (SL) 2.14.16
Source: [NVIDIA-RTX/Streamline @625db07 license.txt](https://github.com/NVIDIA-RTX/Streamline/blob/625db07d4bca5abc67d781c42af7e7fb0761f1b6/license.txt). **[verified]**
- **Streamline framework source and `include/sl*.h` are MIT** (2023 NVIDIA). Exceptions:
  - `sl_nvperf.h` and `sl_nvperf.dll` fall under the Nsight Perf SDK license.
  - `external/ngx-sdk` is under the **RTX SDK license above**.
  - The DLSS-G plugin "is provided as prebuilt DLLs only".
- **Platform and distribution:**
  - Windows only (premake/VS, D3D11, D3D12, Vulkan on Windows).
  - Mandatory DLLs: `sl.interposer.dll` and `sl.common.dll`. For DLSS you also need `sl.dlss.dll` + **`nvngx_dlss.dll`**.
  - SL can be loaded dynamically; the docs recommend `sl::security::verifyEmbeddedSignature` before loading.
  - `sl::Preferences` needs either an NVIDIA-issued `applicationId` or `engine = eCustom` + `engineVersion` + `projectId` (GUID).
- **Verdict:** the MIT headers can be vendored. The DLSS feature itself is still governed by the RTX SDK license (`nvngx_dlss.dll`), so the same obligations apply.

### Intel XeSS 3 SDK
Source: [intel/xess @de0fb9c](https://github.com/intel/xess/tree/de0fb9c1c510661c571164e1418ceca8101dab69). **[verified]**
- **Public headers are MIT** (`inc/MIT.txt`; `inc/xess/xess.h` says `SPDX-License-Identifier: MIT`). This came from the commit "Update license to MIT for public headers only" on 2026-09-09.
- **Binaries** (`libxess.dll`, `libxess_dx11.dll`, `libxess_fg.dll`, `libxell.dll`) are under the **Intel Simplified Software License (Oct 2022)**: "use and redistribute the software, which is provided in binary form only … without modification", reproduce the notice, no reverse engineering or modification "including during execution", and no endorsement.
- **No notification or attribution-on-splash obligations.**
- Windows only: no Linux binaries ship. APIs: `xess_d3d12.h`, `xess_d3d11.h`, `xess_vk.h`.
- XeSS-SR runs "on all GPUs with SM 6.4 (DP4a) support".
- **Verdict: compatible.** Vendor the MIT headers and ship (or let the user supply) `libxess.dll` unmodified with Intel's license text, loaded with `LoadLibrary` as an optional plugin.

---

## 6. Qt licensing and features

- **Qt Charts, Qt Graphs and Qt Data Visualization are GPL-3.0-only or commercial, never LGPL.** In the 6.12 branches, each repo's `LICENSES/` holds only `GPL-3.0-only`, `LicenseRef-Qt-Commercial`, `BSD-3-Clause` (examples), `GFDL` (docs) and `Qt-GPL-exception-1.0`. Library sources carry `SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only`: 396 files in qtcharts, 288 in qtgraphs, 213 in qtdatavis3d. **[verified]** [qtcharts 6.12](https://github.com/qt/qtcharts/tree/6.12/LICENSES), [qtgraphs 6.12](https://github.com/qt/qtgraphs/tree/6.12/LICENSES), [qtdatavis3d 6.12](https://github.com/qt/qtdatavis3d/tree/6.12/LICENSES)
  - **Consequence:** binaries that link any of them must be distributed under GPLv3. Your own source can stay MIT, since MIT is GPL-compatible, but the shipped app becomes GPLv3.
  - qtbase, by contrast, is `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`. **[verified]**
- **Deprecations:** Qt Charts and Qt Data Visualization are "deprecated since Qt 6.10 … use the Qt Graphs module instead". Qt Graphs 2D is Qt Quick (QML/Shapes) based. 3D also has widget wrappers (`Q3DGraphsWidgetItem`, `Q3DScatterWidgetItem` …). For a Widgets UI, embed it with `QQuickWidget`. **[verified]** [qtcharts deprecation-phase.qdocinc](https://github.com/qt/qtcharts/blob/6.12/src/charts/doc/src/deprecation-phase.qdocinc)
- **LGPL alternatives if you want to avoid GPL:** JKQtPlotter (LGPL-2.1; [LICENSE](https://github.com/jkriege2/JKQtPlotter/blob/master/LICENSE)) **[verified]**, Qwt (LGPL + exceptions, "Qwt License 1.0", seen in a GitHub mirror) **[snippet-level]**, or custom QPainter/QRhi plots.
- **Latest Qt LTS:**
  - Tag `v6.12.0` is dated **2026-09-30** **[verified]** (qtbase tag date).
  - Qt's blog post is titled "Qt 6.12 LTS Released!" and describes a 5-year commercial LTS. **[snippet]** [qt.io blog](https://www.qt.io/blog/qt-6.12-released)
  - The previous LTS is 6.8 (tag `v6.8.0`, 2024-10-07, with `v6.8.4-lts-lgpl` and `v6.8.5-lts-lgpl` tags present). **[verified]**
  - 6.11 (2026-03-23) is **not** LTS; Qt moved the LTS to 6.12. **[snippet]** [endoflife.date/qt](https://endoflife.date/qt), [vfx-platform-discuss](https://groups.google.com/g/vfx-platform-discuss/c/_BATn6-XpTw)
  - LTS patch releases are commercial-only for an initial period. **[snippet]**
- **Feature checks** (by inspecting qtbase at the version tags) **[verified]** [qtbase v6.12.0](https://github.com/qt/qtbase/tree/v6.12.0):

  | Feature | Since | Evidence |
  |---|---|---|
  | `QStyleHints::setColorScheme(Qt::ColorScheme)` / `unsetColorScheme()` | **6.8** | absent in `v6.7.0`, present in `v6.8.0` `qstylehints.h` |
  | `QPalette::Accent` | **6.6** | `qpalette.h` |
  | `"windows11"` widget style (`QWindows11Style`, keys `windowsvista`, `windows11`) | **6.7** | `src/plugins/styles/modernwindows/modernwindowsstyles.json` |
  | XDG portal `org.freedesktop.appearance` → `color-scheme` (also `contrast`, `reduced-motion`) | xdgdesktopportal platform theme since **6.5**; GNOME/generic Unix theme since **6.6** | `qxdgdesktopportaltheme.cpp`, `qdbussettings_p.h` |
  | XDG portal **`accent-color`** | **Not read by qtbase** as of v6.12.0 and `dev` (2026-09-26) | no `accent-color` key anywhere under `src/gui/platform/unix` or `src/plugins/platformthemes` |

  On KDE, accent colors arrive through KDE's own platform theme, not qtbase. **[unverified]** in detail.

---

## 7. Windows without vendor libraries: DXGI and D3DKMT

- **`IDXGIAdapter3::QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*)`** returns `{Budget, CurrentUsage, AvailableForReservation, CurrentReservation}` **for the calling process**. `DXGI_MEMORY_SEGMENT_GROUP_LOCAL` is VRAM and `_NON_LOCAL` is shared system memory. It is good for the stress tool's own allocation budget, but it **does not give system-wide VRAM used**. **[snippet]** [MS Learn QueryVideoMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo), [asawicki.info](https://www.asawicki.info/news_1695_there_is_a_way_to_query_gpu_memory_usage_in_vulkan_-_use_dxgi). The cross-API equivalent is Vulkan `VK_EXT_memory_budget` (`VkPhysicalDeviceMemoryBudgetPropertiesEXT{heapBudget[], heapUsage[]}`). **[verified]** [vulkan_core.h](https://github.com/KhronosGroup/Vulkan-Headers/blob/main/include/vulkan/vulkan_core.h)
- **D3DKMT (gdi32 thunks):**
  - Header: Microsoft publishes `d3dkmthk.h` under the **MIT License** in [microsoft/libdxg include/dxg/d3dkmthk.h](https://github.com/microsoft/libdxg/blob/main/include/dxg/d3dkmthk.h), so it can be vendored. **[verified]**
  - `D3DKMTQueryAdapterInfo` with `KMTQAITYPE_ADAPTERPERFDATA = 62` (WDDM 2.4+) returns `D3DKMT_ADAPTER_PERFDATA { UINT32 PhysicalAdapterIndex(in); ULONGLONG MemoryFrequency (Hz), MaxMemoryFrequency, MaxMemoryFrequencyOC, MemoryBandwidth (bytes), PCIEBandwidth (bytes); ULONG FanRPM; ULONG Power (tenths of a percent); ULONG Temperature (deci-°C); UCHAR PowerStateOverride; }`.
  - `KMTQAITYPE_ADAPTERPERFDATA_CAPS = 63` returns `{MaxMemoryBandwidth, MaxPCIEBandwidth, MaxFanRPM, TemperatureMax, TemperatureWarning}`.
  - `KMTQAITYPE_NODEPERFDATA = 61` returns per-engine `{Frequency (Hz), MaxFrequency, MaxFrequencyOC, Voltage (mV), VoltageMax, VoltageMaxOC, MaxTransitionLatency}`.
  - Sources: **[verified]** the libdxg header and [System Informer gpumon.c / gpudetails.c](https://github.com/winsiderss/systeminformer/blob/master/plugins/ExtendedTools/gpumon.c) (MIT), which consumes exactly these. **[snippet]** [MS Learn D3DKMT_ADAPTER_PERFDATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adapter_perfdata) for "Temperature … 1 = 0.1C" and "Power … 1 = 0.1%".
  - The driver fills the values, so each vendor and driver populates them differently. Expect zeros on some adapters. Which vendors fill which fields is **[unverified]**.
  - The temperature is a single "main" sensor (no edge/hotspot split), and power is a percentage, not watts.
- **Utilization and memory:** `D3DKMTQueryStatistics` with `D3DKMT_QUERYSTATISTICS_NODE` returns `NodeInformation.GlobalInformation.RunningTime` per engine; take deltas over wall time, as Task Manager-like tools do. `D3DKMT_QUERYSTATISTICS_SEGMENT` returns per-segment committed and resident bytes (system-wide dedicated VRAM usage). `D3DKMT_QUERYSTATISTICS_ADAPTER` returns the node and segment counts. **[verified]** System Informer `gpumon.c`. The Microsoft doc mirror describes the function as "reserved for system use", so treat it as semi-documented, stable in practice. **[snippet]** [NtDoc](https://ntdoc.m417z.com/d3dkmtquerystatistics)
- **Simpler alternative:** PDH counters `\GPU Engine(*)\Utilization Percentage` and `\GPU Adapter Memory(*)\Dedicated Usage`. Instance names embed `luid_0x…_phys_N`. **[snippet, not Microsoft-sourced]**; check with `typeperf -qx "GPU Engine"`.
- **PCIe without vendor libraries:** D3DKMT gives only `PCIEBandwidth` (traffic). Link generation and width come from PnP device properties `DEVPKEY_PciDevice_CurrentLinkSpeed`, `CurrentLinkWidth`, `MaxLinkSpeed`, `MaxLinkWidth` (`pciprop.h`, read with `CM_Get_DevNode_PropertyW` or SetupAPI). The values are raw codes (speed = PCIe generation index), and some devices return ERROR_NOT_FOUND. **[snippet]** [gist](https://gist.github.com/brokenwindupdoll/07575ac26b002c2111bce950b780ccc1), [MSDN forum](https://social.msdn.microsoft.com/Forums/windowshardware/en-US/be5b8715-6852-4e72-a28f-f2eb83bb5015/enumerating-the-pcie-ports-on-the-motherboard?forum=wdk). The exact `pciprop.h` GUIDs and pids were not checked; take them from the Windows SDK or WDK header. The same internal-switch caveat as Linux applies to AMD.
- **Device matching across APIs:**
  - Windows: use the **LUID** (`DXGI_ADAPTER_DESC1::AdapterLuid`, `VkPhysicalDeviceIDProperties::deviceLUID`, ADLX `IADLXGPU2::LUID`, IGCL `pDeviceID`).
  - Everywhere: use the PCI **domain:bus:device.function** (`VK_EXT_pci_bus_info` → `pciDomain/pciBus/pciDevice/pciFunction`, NVML `nvmlDeviceGetHandleByPciBusId_v2`, L0 `zes_pci_properties_t.address`, IGCL `adapter_bdf`, Linux `/sys/class/drm/cardN/device` → BDF).
  - On Windows, the BDF for a LUID comes from `D3DKMTOpenAdapterFromLuid` + `KMTQAITYPE_ADAPTERADDRESS`. **[verified]** that these types exist in d3dkmthk.h.

---

## Takeaways for implementation

### Per vendor and OS: what to load and what you get

| Vendor / OS | Load | Entry points | Clocks | Temp edge / hotspot / mem | Power | Fan | Util | VRAM | Throttle reasons | PCIe gen/width |
|---|---|---|---|---|---|---|---|---|---|---|
| **NVIDIA / Linux** | `dlopen("libnvidia-ml.so.1")` | `nvmlInit_v2`, `nvmlDeviceGetCount_v2`, `nvmlDeviceGetHandleByPciBusId_v2`/`ByIndex_v2`, … `nvmlShutdown` | GFX, SM, MEM (`nvmlDeviceGetClockInfo`) | core: yes. hotspot: **R615+ only** (`NVML_TEMPERATURE_GPU_MAX` via `GetTemperatureV`). mem: `NVML_FI_DEV_MEMORY_TEMP` (often unsupported on GeForce) | mW usage (1-s average on Ampere+), instant through a field, limits | % per fan (`_v2`), RPM (R565+) | gpu/mem % | `GetMemoryInfo_v2` | **Yes**: 64-bit bitmask (`GetCurrentClocksEventReasons`) | current + max (device+system) + GPU max |
| **NVIDIA / Windows** | `LoadLibraryExW(<GetSystemDirectoryW()>\nvml.dll, …)`, else `<%ProgramW6432% expanded>\NVIDIA Corporation\NVSMI\nvml.dll` (LoadLibrary does not expand env vars) | same | same | same | same | same | same | same | same | same |
| **AMD / Linux** | no library: sysfs/hwmon under `/sys/class/drm/cardN/device/` | n/a | `hwmon freq1_input` (sclk, Hz), `freq2_input` (mclk); `pp_dpm_*` levels; `gpu_metrics` current/avg clocks | `temp1` edge / `temp2` junction / `temp3` mem (SOC15+), all with labels | `power1_average` else `power1_input` (µW); `power1_cap` | `fan1_input` RPM, `pwm1` | `gpu_busy_percent`, `mem_busy_percent` | `mem_info_vram_used/total` | **Yes**: `gpu_metrics` v1_3 `throttle_status` (ASIC-specific) + **`indep_throttle_status`** (uint64, `SMU_THROTTLER_*_BIT`) | `current/max_link_speed/width` (walk up past the internal switch), `pp_dpm_pcie`, `gpu_metrics.pcie_link_*` |
| **AMD / Windows (MIT-safe path)** | `LoadLibraryW(L"atiadlxx.dll")` (ADL; headers MIT per file, but see the EULA caveat) | `ADL2_Main_Control_Create`, `ADL2_Adapter_NumberOfAdapters_Get`, `ADL2_Adapter_AdapterInfo_Get`, `ADL2_New_QueryPMLogData_Get` (deprecated → `ADL2_Overdrive8_PMLog_ShareMemory_*`) | GFX/MEM (`PMLOG_CLK_*`) | edge 8 / **hotspot 27** / mem 9 | ASIC 23 / board 73 | RPM 14, % 15 | activity GFX 19 / MEM 20 | not in PMLog; use D3DKMT segments or DXGI | **Yes**: `PMLOG_THROTTLER_STATUS` 35 | `PMLOG_BUS_SPEED` 40 / `BUS_LANES` 41 |
| **AMD / Windows (ADLX)** | `amdadlx64.dll` → `ADLXQueryFullVersion`, `ADLXInitialize[2]`/`ADLXInitializeWithCallerAdl`, `ADLXTerminate` | `IADLXSystem::GetPerformanceMonitoringServices` → `GetCurrentGPUMetrics` | GPU + VRAM clock | **edge + hotspot + mem (Metrics1)** | GPU power + total board power | RPM | % | MB used | **No** | bus type + lane width only |
| **Intel / Windows** | `LoadLibraryExW(L"ControlLib.dll", 0, LOAD_LIBRARY_SEARCH_SYSTEM32)` (**64-bit process required** for telemetry) | `ctlInit` (flags `CTL_INIT_FLAG_USE_LEVEL_ZERO`), `ctlEnumerateDevices`, `ctlPowerTelemetryGet[V2]`, `ctlEnum*` + `*GetState`, `ctlClose` | GPU/VRAM (telemetry) or domains | GPU (max sensor ≈ hotspot) + VRAM; sensor types GLOBAL/GPU/MEMORY | energy counter → W; limits | RPM or % | activity counters (Δ) | `ctlMemoryGetState` | **Yes**: `throttleReasons` flags + `gpu*Limited` bools + throttle time | `ctlPciGetState` (current) / `ctlPciGetProperties` (max) |
| **Intel / Linux** | sysfs first (no library); optional `dlopen("libze_loader.so.1")` for Sysman | `zesInit`, `zesDriverGet`, `zesDeviceGet`, `zesDeviceEnum*`, `zes*GetState` | i915: `gt/gtX/rps_act_freq_mhz`; xe: `tileT/gtG/freq0/act_freq` | i915: `temp1_input` (pkg, 6.12+); xe: `temp2` pkg / `temp3` vram / `temp4` mctrl / `temp5` pcie / vram_ch (6.15+); L0: GPU/MEMORY sensors | `energy1_input` (µJ, Δ→W), `power1_max` (PL1) | `fan1_input` (i915 6.12+, xe 6.16+) | fdinfo, PMU or L0 `zesEngineGetActivity` | L0 `zesMemoryGetState` or fdinfo | **Yes**: i915 `gt/gtX/throttle_reason_*`; xe `freq0/throttle/reasons`; L0 `throttleReasons` | generic PCI sysfs `current/max_link_*`; L0 `zesDevicePciGetState` |
| **Any / Windows fallback** | `gdi32.dll` (`D3DKMTOpenAdapterFromLuid`, `D3DKMTQueryAdapterInfo`, `D3DKMTQueryStatistics`, `D3DKMTCloseAdapter`) + DXGI + cfgmgr32 | see §7 | node Hz (`NODEPERFDATA`), mem Hz | 1 main sensor (deci-°C), driver-dependent | % of TDP only | RPM | per-engine running-time deltas | segments (system-wide), DXGI (per process) | No | `DEVPKEY_PciDevice_*` (cfgmgr32) |

Implementation notes:
- Resolve every symbol individually and treat missing symbols as "feature unavailable". NVML: `nvmlDeviceGetTemperatureV` is R565+, `GetFanSpeedRPM` R565+, `NVML_TEMPERATURE_GPU_MAX` R615+.
- Version-stamp NVML, IGCL and L0 structs (`version`, `Size`, `Version`, `stype` fields).
- All AMD and Intel sysfs values are plain text integers except `gpu_metrics` (binary) and the `pp_dpm_*` lists.

### Licensing verdict

| Component | License (verified where noted) | OK to ship with or inside an MIT app? | Conditions / recommendation |
|---|---|---|---|
| `nvml.h` | NVIDIA custom notice: use granted; redistribution and modification not expressly granted; disclaimer must appear in docs and code comments [verified] | **Don't vendor** | Write your own prototypes and structs (MIT). Never ship `nvml.dll`/`libnvidia-ml`; they come with the driver. |
| ADLX SDK (headers, helper, docs) | AMD ADLX SDK License Agreement (proprietary, PDF) [verified] | **No** | Object-code-only distribution, EULA with an RE ban, no publishing, no Free Software License use. Avoid it, or isolate it in a separately licensed plugin. The runtime `amdadlx64.dll` comes from the driver. |
| ADL SDK headers | Per-file MIT notice, but the repo also has a restrictive `ADL SDK EULA.pdf` [verified] | **Probably yes** (ambiguous) | Prefer your own declarations; keep AMD's MIT notice if you copy. Runtime `atiadlxx.dll` comes from the driver. |
| amdgpu / i915 / xe sysfs | Kernel ABI | Yes | None. |
| IGCL `igcl_api.h` / `cApiWrapper.cpp` | Intel Software License Agreement 10.07.21 ("solely for use on Intel platforms", reproduce terms, no binary RE) [verified] | **Yes as a separate third-party file**, not relicensed as MIT | Keep it unmodified with its license text, or write your own declarations. `ControlLib.dll` comes from the driver. |
| Level Zero headers + loader | MIT [verified] | **Yes** | Keep the copyright notice. Prefer the system `ze_loader`. |
| FSR 1, FSR 2.2, FidelityFX SDK 1.1.4 (FSR 3.1) source | MIT [verified] | **Yes** | Keep notices. |
| FSR SDK 2.x headers / FFX API | MIT (listed) [verified] | Yes | — |
| FSR SDK 2.x signed DLLs (incl. FSR 4) | Ambiguous: binary-only + no-RE preamble vs. the MIT list that includes the DLLs [verified] | **Yes as an optional runtime plugin** | Ship unmodified, with notices. DX12 only. FSR 4 needs RDNA3 or RDNA4. |
| DLSS SDK (NGX headers, libs, `nvngx_dlss*.dll`) | NVIDIA RTX SDKs License (proprietary; headers `LicenseRef-NvidiaProprietary`) [verified] | **No (not in the MIT tree or build)** | Only as a separately licensed plugin. Distribute under protective terms (§2c), no OSS-license exposure (§4e), notify NVIDIA before commercial release, NVIDIA marks on splash and about screens. Better still, let the user supply the DLL. |
| Streamline core + `sl*.h` | MIT (except nvperf files under the Nsight Perf license; `ngx-sdk` under RTX) [verified] | **Yes** (headers, interposer) | Windows only. DLSS through SL still needs `nvngx_dlss.dll` under the RTX license. Verify signatures. |
| XeSS headers | MIT (since 2026-09-09) [verified] | **Yes** | — |
| XeSS DLLs (`libxess*.dll`, `libxell.dll`) | Intel Simplified Software License (binary-only, unmodified, no RE) [verified] | **Yes** (separately licensed binaries) | Include Intel's license text. Windows only. |
| Qt Base / Widgets / Gui (6.8–6.12) | LGPL-3.0-only / GPL-2.0 / GPL-3.0 / commercial [verified] | Yes (dynamic link, LGPL obligations) | Allow relinking: ship Qt as shared libraries with its license. |
| Qt Charts / Qt Graphs / Qt Data Visualization | **GPL-3.0-only** or commercial [verified] | **Only if the shipped binaries are GPLv3** | Source can stay MIT, but distributions become GPLv3. Alternatives: own QPainter/QRhi charts, JKQtPlotter (LGPL-2.1), Qwt (LGPL + exceptions). |
| `d3dkmthk.h` (microsoft/libdxg) | MIT [verified] | Yes | Keep the notice. |
| Vulkan-Headers | Apache-2.0 OR MIT [verified] | Yes | — |

### Unverified items, to re-check
1. Which NVIDIA GPUs or drivers populate `NVML_TEMPERATURE_GPU_MAX` (R615+) and `NVML_FI_DEV_MEMORY_TEMP` on GeForce.
2. Whether ADLX `PCIBusType`/`PCIBusLaneWidth` report current or maximum values.
3. How much of Level Zero Sysman works on Windows.
4. Which vendors fill `D3DKMT_ADAPTER_PERFDATA` fields.
5. The exact `DEVPKEY_PciDevice_*` GUIDs and pids (read them from `pciprop.h`).
6. Qt 6.12 LTS designation and support dates, which came from qt.io and endoflife.date search snippets; the tag date is verified.
7. NVML Windows DLL paths, which came from a search snippet of docs.nvidia.com.
8. Whether ADL headers are governed by the per-file MIT notice or by the repository EULA (needs legal judgment).
