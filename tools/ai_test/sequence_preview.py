"""Create a contact sheet from the local four-column benchmark output."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
args = parser.parse_args()
font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 20)
frames = [0, 9, 18, 27, 36, 45]
labels = ["Source", "Single-frame AI", "Build 5", "Build 6"]
tile_w, tile_h, header = 256, 366, 34
canvas = Image.new("RGB", (4 * tile_w, len(frames) * (tile_h + header)), "#161616")
draw = ImageDraw.Draw(canvas)
for row, frame in enumerate(frames):
    with Image.open(args.directory / f"compare-{frame}.bmp") as source:
        w = source.width // 4
        for col, label in enumerate(labels):
            image = source.crop((col * w, 0, (col + 1) * w, source.height))
            image = image.resize((tile_w, tile_h), Image.Resampling.LANCZOS)
            x, y = col * tile_w, row * (tile_h + header)
            canvas.paste(image, (x, y + header))
            draw.text((x + 8, y + 7), f"{label} / {frame}", fill="white", font=font)
canvas.save(args.directory / "contact-sheet.png")
print(args.directory / "contact-sheet.png")
