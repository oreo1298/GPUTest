# Upscaler plugins

The benchmark has AMD FSR 1 built in (`--upscale fsr1`). Vendor upscalers whose runtimes are not
open source — Intel XeSS, NVIDIA DLSS — can only be added as **plugins**: shared libraries the runner
loads at run time, **off by default**. Nothing vendor-licensed is distributed with the app.

## Using plugins

1. Put the plugin (and the vendor runtime it needs) in the `plugins` folder next to `kiln-run`.
2. Run with `--upscaler-plugins`, e.g. `kiln-run --test feature.upscale --upscaler-plugins --api d3d12`.
3. `feature.upscale` then also runs each usable plugin on a panning, jittered test sequence at quality mode
   (1.5x per axis) and reports its cost (`plugin_<id>_ms`) and image quality after the sequence
   (`plugin_<id>_psnr`, PSNR against a supersampled reference). The results' `details.plugins` lists
   every plugin found and why it was or was not measured.

### Intel XeSS (Windows, Direct3D 12)

The Windows build ships `plugins/upscaler-xess.dll`, built from XeSS's MIT-licensed public headers. It
needs Intel's runtime: copy `libxess.dll` from the XeSS SDK (https://github.com/intel/xess, folder `bin`)
into the same `plugins` folder. `libxess.dll` is under the Intel Simplified Software License, which allows
redistribution of the unmodified binary with its notice; this project still leaves it to the user.

### NVIDIA DLSS

The DLSS SDK's license (NVIDIA RTX SDKs license) does not let its headers, libraries or runtime be
distributed under open-source terms, so no DLSS plugin is part of this repository. Anyone may build one for
their own use from NVIDIA's SDK against the C interface below, under NVIDIA's terms.

## Writing a plugin

The interface is a single C header, `src/bench/gt/bench/upscaler_plugin.h` (MIT). A plugin:

- is named `upscaler-<id>.dll` (Windows) or `upscaler-<id>.so` / `libupscaler-<id>.so` (Linux);
- defines `GT_UPSCALER_PLUGIN` before including the header and exports
  `GT_UPSCALER_EXPORT const gt_upscaler_plugin* gt_upscaler_plugin_get(void)`;
- returns a `gt_upscaler_plugin` with `abi_version = GT_UPSCALER_ABI_VERSION`, the APIs it supports
  (Vulkan, Direct3D 12) and four functions:
  - `query(device)`: whether it can run on this device (and why not);
  - `create(device, info)`: one instance for a maximum render size, an output size and a quality mode;
  - `evaluate(context, frame)`: records the upscale into the host's command buffer / command list;
  - `destroy(context)`: called after the GPU finished all of the instance's work.

Per frame the host provides, at render size: colour (RGBA8 UNORM, display-referred), depth (R32 float,
1 = far), motion vectors (RG16 float, render pixels from the current position to the previous frame's);
and the output (RGBA8 UNORM storage image, output size). It also passes the jitter (offset of the sample
positions from the pixel centres, render pixels) and a history reset flag. Inputs are shader-readable
(`SHADER_READ_ONLY_OPTIMAL`, or `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`) and the output is a
storage image (`GENERAL`, or `UNORDERED_ACCESS`); the exact layout/state is in each image's `layout` field
and every image must be left in it. Plugins may change any pipeline state; the host re-binds its own.

`src/plugins/example` is a minimal Vulkan plugin (a blit) used by CI to test the interface;
`src/plugins/xess` is a complete one.
