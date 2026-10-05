#!/usr/bin/env python3
"""Cut oval irises and blank faces from the sitting plush sprites."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Talkbot" / "Assets.xcassets"
IDLE = ASSETS / "DinoIdle.imageset" / "DinoIdle.png"
FRONT = ASSETS / "DinoHappyFront.imageset" / "DinoHappyFront.png"
WINK = ASSETS / "DinoWink.imageset" / "DinoWink.png"

# Measured on DinoIdle 400x440: dark iris bbox, not the highlight.
LEFT = (129.4, 195.9)
RIGHT = (263.0, 196.2)
IRIS_W, IRIS_H = 34, 48
SHUT_SIZE = (40, 22)
SHUT_CENTER_Y = 198
SKIN = (248, 232, 210, 255)
SOCKET = (28, 12, 10, 255)

CONTENTS = """{\n  \"images\": [{\"filename\": \"%s\", \"idiom\": \"universal\"}],\n  \"info\": {\"author\": \"xcode\", \"version\": 1}\n}\n"""


def is_skin(pixel) -> bool:
    r, g, b, a = pixel
    return a > 180 and r > 205 and g > 175 and b > 140 and (r - b) > 20


def oval_crop(src: Image.Image, center) -> Image.Image:
    cx, cy = center
    rx, ry = IRIS_W / 2, IRIS_H / 2
    size = (IRIS_W, IRIS_H)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    # Tiny upper-left spec, not a sclera cap.
    hx, hy, hr = rx * 0.38, ry * 0.42, min(rx, ry) * 0.22
    for y in range(IRIS_H):
        for x in range(IRIS_W):
            nx = (x + 0.5 - rx) / rx
            ny = (y + 0.5 - ry) / ry
            dist = (nx * nx + ny * ny) ** 0.5
            if dist > 1:
                continue
            sx, sy = int(round(cx - rx + x)), int(round(cy - ry + y))
            if not (0 <= sx < src.width and 0 <= sy < src.height):
                continue
            pixel = src.getpixel((sx, sy))
            if is_skin(pixel):
                pixel = SOCKET
            lum = 0.3 * pixel[0] + 0.59 * pixel[1] + 0.11 * pixel[2]
            spec = ((x + 0.5 - hx) / hr) ** 2 + ((y + 0.5 - hy) / hr) ** 2
            if lum > 120 and spec > 1:
                pixel = SOCKET
            elif spec <= 1:
                t = max(0, 1 - spec)
                pixel = (
                    int(255 * t + pixel[0] * (1 - t)),
                    int(255 * t + pixel[1] * (1 - t)),
                    int(255 * t + pixel[2] * (1 - t)),
                    255,
                )
            alpha = 255
            if dist > 0.92:
                alpha = int(255 * max(0, (1 - dist) / 0.08))
            out.putpixel((x, y), (pixel[0], pixel[1], pixel[2], min(alpha, pixel[3] if len(pixel) > 3 else 255)))
    return out


def erase_eyes(src: Image.Image, centers) -> Image.Image:
    overlay = Image.new("RGBA", src.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    pad_x, pad_y = IRIS_W / 2 + 1, IRIS_H / 2 + 1
    for cx, cy in centers:
        box = (cx - pad_x, cy - pad_y + 1, cx + pad_x, cy + pad_y)
        draw.ellipse(box, fill=SOCKET)
    return Image.alpha_composite(src, overlay.filter(ImageFilter.GaussianBlur(radius=0.8)))


def shut_crop(src: Image.Image, center) -> Image.Image:
    cx, cy = center
    w, h = SHUT_SIZE
    x0, y0 = int(cx - w / 2), int(cy - h / 2)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    for y in range(h):
        for x in range(w):
            sx, sy = x0 + x, y0 + y
            if not (0 <= sx < src.width and 0 <= sy < src.height):
                continue
            pixel = src.getpixel((sx, sy))
            if pixel[3] < 180 or is_skin(pixel):
                continue
            lum = 0.3 * pixel[0] + 0.59 * pixel[1] + 0.11 * pixel[2]
            if lum > 150:
                continue
            dx = (x + 0.5 - w / 2) / (w / 2)
            dy = (y + 0.5 - h / 2) / (h / 2 * 0.9)
            if dx * dx + dy * dy > 1:
                continue
            out.putpixel((x, y), pixel)
    return out


def write_imageset(name: str, image: Image.Image) -> None:
    folder = ASSETS / f"{name}.imageset"
    folder.mkdir(parents=True, exist_ok=True)
    filename = f"{name}.png"
    image.save(folder / filename)
    (folder / "Contents.json").write_text(CONTENTS % filename)


def main() -> None:
    idle = Image.open(IDLE).convert("RGBA")
    front = Image.open(FRONT).convert("RGBA")
    wink = Image.open(WINK).convert("RGBA")
    write_imageset("DinoEyeOpen", oval_crop(idle, LEFT))
    write_imageset("DinoEyeShut", shut_crop(wink, (LEFT[0], SHUT_CENTER_Y)))
    write_imageset("DinoIdleBlank", erase_eyes(idle, (LEFT, RIGHT)))
    write_imageset("DinoTalkBlank", erase_eyes(front, (LEFT, RIGHT)))


if __name__ == "__main__":
    main()
