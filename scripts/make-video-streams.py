#!/usr/bin/env python3
"""Makes the reference streams that feature.video decodes (data/video/), with FFmpeg's software encoders.

Every GPU decodes the same streams, so decode throughput is comparable across GPUs, and because H.264, H.265
and AV1 decoding is bit-exact, each decoded frame must match FFmpeg's own decode: the expected CRC-32 of every
frame (NV12, 1920x1080, luma plane then interleaved CbCr) is stored in data/video/streams.json.

The pictures are the same synthetic pattern feature.video encodes on the GPU (makeSource in
src/bench/tests/features/video.cpp): eight 1080p frames, one IDR/key frame and seven inter frames, each
referencing only the frame before it (no B-frames, no hidden frames).

Run from the repository root:  python3 scripts/make-video-streams.py
Needs: numpy and an ffmpeg with libx264, libx265 and libaom.
"""

import json
import pathlib
import subprocess
import sys
import zlib

import numpy as np

W, H, FRAMES = 1920, 1080, 8
OUT = pathlib.Path("data/video")


def source_frames():
    """NV12 frames, identical to makeSource() in video.cpp but at 1080 rows."""
    frames = []
    for f in range(FRAMES):
        dx, dy = 6.0 * f, 3.0 * f
        y = np.arange(H, dtype=np.float64)[:, None] + dy
        x = np.arange(W, dtype=np.float64)[None, :] + dx
        bx = np.floor(x).astype(np.int64) >> 6
        by = np.floor(y).astype(np.int64) >> 6
        g = 120.0 + 45.0 * np.sin(x * 0.011) * np.cos(y * 0.017)
        g = g + np.where(((bx + by) & 1) == 1, 30.0, -30.0)
        stripes = np.where(((np.floor(x).astype(np.int64) >> 2) & 1) == 1, 18.0, -18.0)
        g = g + np.where((bx * 7 + by * 3) % 5 == 0, stripes, 0.0)
        # Texture noise fixed to the panning pattern (inter frames can predict it), as in video.cpp.
        xi = np.broadcast_to(np.floor(x).astype(np.uint32), (H, W))
        yi = np.broadcast_to(np.floor(y).astype(np.uint32), (H, W))
        h = (xi * np.uint32(73856093)) ^ (yi * np.uint32(19349663))
        h = (h ^ (h >> np.uint32(13))) * np.uint32(1274126177)
        g = g + ((h >> np.uint32(24)) & np.uint32(15)).astype(np.float64) - 8.0
        # Grain that changes every frame (so inter frames carry residual, as camera footage does).
        px = np.broadcast_to(np.arange(W, dtype=np.uint32)[None, :], (H, W))
        py = np.broadcast_to(np.arange(H, dtype=np.uint32)[:, None], (H, W))
        t = (px * np.uint32(2654435761)) ^ (py * np.uint32(2246822519)) ^ np.uint32(f * 3266489917 & 0xFFFFFFFF)
        t = (t ^ (t >> np.uint32(15))) * np.uint32(2246822519)
        g = g + ((t >> np.uint32(28)) & np.uint32(7)).astype(np.float64) - 4.0
        luma = np.clip(np.rint(g), 16, 235).astype(np.uint8)
        cy = np.minimum(2 * np.arange(H // 2), H - 2).astype(np.float64)[:, None] + dy
        cx = 2.0 * np.arange(W // 2, dtype=np.float64)[None, :] + dx
        cb = np.clip(np.rint(128.0 + 40.0 * np.sin(cx * 0.006 + cy * 0.002)), 16, 235).astype(np.uint8)
        cr = np.clip(np.rint(128.0 + 40.0 * np.cos(cy * 0.008 - cx * 0.001)), 16, 235).astype(np.uint8)
        uv = np.stack([cb, cr], axis=-1).reshape(H // 2, W)
        frames.append(luma.tobytes() + uv.tobytes())
    return frames


# Encoder settings: every inter frame references only the previous frame, one slice / tile, no SEI.
STREAMS = [
    {
        "key": "h264", "file": "h264_1080p.h264", "codec": "H.264", "profile": "High",
        "args": ["-c:v", "libx264", "-profile:v", "high", "-preset", "medium",
                 "-x264-params", "qp=24:ipratio=1.0:mbtree=0:aq-mode=0:psy=0:ref=1:bframes=0:weightp=0:keyint=8:"
                                 "min-keyint=8:scenecut=0:threads=1:sliced-threads=0",
                 "-bsf:v", "filter_units=remove_types=6", "-f", "h264"],
    },
    {
        "key": "h265", "file": "h265_1080p.h265", "codec": "H.265", "profile": "Main",
        "args": ["-c:v", "libx265", "-profile:v", "main", "-preset", "medium",
                 "-x265-params", "qp=24:ipratio=1.0:aq-mode=0:psy-rd=0:psy-rdoq=0:cutree=0:rdoq-level=0:"
                                 "ref=1:bframes=0:keyint=8:min-keyint=8:scenecut=0:open-gop=0:info=0:"
                                 "repeat-headers=1:pools=1:frame-threads=1:rc-lookahead=0:log-level=error",
                 "-bsf:v", "filter_units=remove_types=39|40", "-f", "hevc"],
    },
    {
        "key": "av1", "file": "av1_1080p.obu", "codec": "AV1", "profile": "Main",
        "args": ["-c:v", "libaom-av1", "-cpu-used", "6", "-crf", "40", "-b:v", "0", "-g", "8", "-keyint_min", "8",
                 "-lag-in-frames", "0", "-auto-alt-ref", "0", "-tiles", "1x1", "-row-mt", "0", "-threads", "1",
                 "-denoise-noise-level", "0", "-aq-mode", "0",
                 "-aom-params", "enable-tpl-model=0:deltaq-mode=0:enable-keyframe-filtering=0", "-f", "obu"],
    },
]


def run(cmd, data=None):
    r = subprocess.run(cmd, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode(errors="replace"))
        raise SystemExit(f"failed: {' '.join(cmd)}")
    return r.stdout


def psnr(a, b):
    a = np.frombuffer(a[: W * H], np.uint8).astype(np.float64)
    b = np.frombuffer(b[: W * H], np.uint8).astype(np.float64)
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse < 1e-10 else float(10.0 * np.log10(255.0 * 255.0 / mse))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    src = source_frames()
    raw = b"".join(src)
    meta = {"width": W, "height": H, "frames": FRAMES, "generator": "scripts/make-video-streams.py",
            "ffmpeg": run(["ffmpeg", "-hide_banner", "-version"]).decode().splitlines()[0], "streams": []}
    for s in STREAMS:
        path = OUT / s["file"]
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "nv12",
               "-s", f"{W}x{H}", "-r", "60", "-i", "-", "-frames:v", str(FRAMES)] + s["args"] + [str(path)]
        run(cmd, raw)
        decoded = run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path), "-f", "rawvideo",
                       "-pix_fmt", "nv12", "-"])
        frame_bytes = W * H * 3 // 2
        if len(decoded) != frame_bytes * FRAMES:
            raise SystemExit(f"{path}: decoded {len(decoded) // frame_bytes} frames, expected {FRAMES}")
        frames = [decoded[i * frame_bytes:(i + 1) * frame_bytes] for i in range(FRAMES)]
        meta["streams"].append({
            "key": s["key"], "file": s["file"], "codec": s["codec"], "profile": s["profile"],
            "bytes": path.stat().st_size,
            "crc32": [f"{zlib.crc32(fr):08x}" for fr in frames],
            "psnr": [round(psnr(fr, src[i]), 2) for i, fr in enumerate(frames)],
            "encoder": " ".join(s["args"]),
        })
        print(f"{path}: {path.stat().st_size} bytes, PSNR {min(meta['streams'][-1]['psnr'])} dB min")
    (OUT / "streams.json").write_text(json.dumps(meta, indent=2) + "\n")


if __name__ == "__main__":
    main()
