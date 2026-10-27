# Intel XeSS SDK public headers

`xess/xess.h` and `xess/xess_d3d12.h` from https://github.com/intel/xess at commit de0fb9c1c510661c571164e1418ceca8101dab69
("Update license to MIT for public headers only", 2026-09-09). The headers are MIT-licensed
(SPDX notice in each file); the XeSS runtime library is not part of this repository — its own
license (Intel Simplified Software License) applies to it, and users install it themselves
(docs/PLUGINS.md). Only the upscaler plugin in `src/plugins/xess` uses these headers.
