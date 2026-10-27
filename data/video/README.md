# Reference video streams

Decoded by `feature.video` to measure and check the GPU's video decoder. Every GPU decodes the same bits, so
decode frame rates are comparable, and because H.264, H.265 and AV1 define decoding exactly, each decoded frame
must match the CRC-32 in `streams.json` (FFmpeg's own decode of the stream).

| File | Codec | Encoder (via FFmpeg) | Structure |
|---|---|---|---|
| `h264_1080p.h264` | H.264 High, Annex B | libx264, CRF 20 | IDR + 7 P-frames, 1 reference, 1 slice |
| `h265_1080p.h265` | H.265 Main, Annex B | libx265, CRF 25 | IDR + 7 P-frames, 1 reference |
| `av1_1080p.obu` | AV1 Main, low-overhead OBUs | libaom, CRF 32 | key frame + 7 inter frames, no hidden frames, 1 tile |

The pictures are Kiln's own synthetic pattern (gradients, hard-edged blocks and texture noise panning by
(6, 3) pixels per frame, `makeSource` in `src/bench/tests/features/video_vk.cpp`), so the streams carry no
third-party content; they are part of Kiln under its MIT license. SEI messages (encoder settings strings) are
removed from the H.264 and H.265 streams.

Regenerate with `python3 scripts/make-video-streams.py` (needs numpy and an FFmpeg with libx264, libx265 and
libaom). Encoder versions change the bits, so the CRCs are written alongside; commit all files together.
