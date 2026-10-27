# AV1 parser test streams

Small libaom streams (640x360, 5 frames) that use AV1 tools the 1080p reference stream in `data/video` does not:

| File | Encoder settings (via FFmpeg) | Exercises |
|---|---|---|
| `av1_tiles_segmentation.obu` | realtime, 2x2 tiles, `aq-mode 3` | several tiles, cyclic-refresh segmentation |
| `av1_global_motion.obu` | `cpu-used 1`, `enable-global-motion=1` | global motion parameters |

Each `.json` holds FFmpeg's `trace_headers` view of every frame header (field values, where the header and the
tile group end, the OBU size); `tests/test_av1.cpp` compares the reader with it. The pictures are Kiln's own
synthetic pattern, so the files are part of Kiln under its MIT license. Regenerate them together with the
reference streams: `python3 scripts/make-video-streams.py`.
