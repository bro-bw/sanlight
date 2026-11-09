"""Inspect spatial angular drift after returning to the same source frame."""
import argparse
import struct
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFont

p = argparse.ArgumentParser()
p.add_argument("sequence", type=Path)
p.add_argument("directory", type=Path)
a = p.parse_args()
with a.sequence.open("rb") as f:
    w, h, count, fps = struct.unpack("<IIIf", f.read(16))
    rgba = np.frombuffer(f.read(w * h * 4), dtype=np.uint8).reshape(h, w, 4)
with (a.directory / "roundtrip-surfaces.bin").open("rb") as f:
    ow, oh, states, ofps = struct.unpack("<IIIf", f.read(16))
    assert (w, h, 2, fps) == (ow, oh, states, ofps)
    surface = np.frombuffer(f.read(), dtype="<f4").reshape(2, 4 * w * h)
normals = surface[:, w * h:].reshape(2, h, w, 3) * 2 - 1
normals /= np.maximum(1e-8, np.linalg.norm(normals, axis=-1, keepdims=True))
angle = np.degrees(np.arccos(np.clip(np.sum(normals[0] * normals[1], axis=-1), -1, 1)))
alpha = rgba[:, :, 3:4] / 255.0
mask = rgba[:, :, 3] >= 253
valid = np.zeros_like(mask)
valid[8:-8, 8:-8] = mask[8:-8, 8:-8]
values = angle[valid]
print("Opaque return-pose pixels:", len(values))
for threshold in [1, 5, 10, 20]:
    print(f"Angular error above {threshold} degrees: {100 * np.mean(values > threshold):.3f}%")
print("Finite state:", bool(np.all(np.isfinite(surface))))
t = np.clip(angle / 20, 0, 1)
heat = np.stack([np.minimum(t * 2, 1), np.minimum((1 - t) * 2, 1), np.zeros_like(t)], axis=-1)
heat *= np.minimum(t * 6, 1)[..., None]
tiles = [rgba[:, :, :3] / 255, (normals[0] + 1) / 2, (normals[1] + 1) / 2, heat]
labels = ["Source / frame 0", "Initial normal", "Returned normal", "Error: black 0 / red 20+ deg"]
header = 40
canvas = Image.new("RGB", (w * 4, h + header), "#161616")
draw = ImageDraw.Draw(canvas)
font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 20)
for i, (tile, label) in enumerate(zip(tiles, labels)):
    image = Image.fromarray(np.uint8(np.clip(tile * alpha, 0, 1) * 255))
    canvas.paste(image, (i * w, header))
    draw.text((i * w + 8, 10), label, fill="white", font=font)
canvas.save(a.directory / "roundtrip-error.png")
print(a.directory / "roundtrip-error.png")
