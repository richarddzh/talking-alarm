"""Prepare cutouts from user references without redrawing their contents."""

from collections import deque
from pathlib import Path
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageOps


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "esp32_idf_s3n16r8" / "main" / "assets"
REFERENCES = ASSETS / "small_apps" / "references"
OUTPUT = ASSETS / "small_apps"
sys.path.insert(0, str(ROOT / "tools"))
from generate_launcher_icons import format_values, rgb565  # noqa: E402


def remove_background(image, color, tolerance):
    image = image.convert("RGBA")
    width, height = image.size
    pixels = list(image.getdata())
    allowed = bytearray(
        max(abs(pixel[c] - color[c]) for c in range(3)) <= tolerance
        for pixel in pixels
    )
    outside = bytearray(width * height)
    queue = deque()
    for x in range(width):
        queue.extend((x, (height - 1) * width + x))
    for y in range(height):
        queue.extend((y * width, y * width + width - 1))
    while queue:
        index = queue.popleft()
        if outside[index] or not allowed[index]:
            continue
        outside[index] = 1
        x, y = index % width, index // width
        if x: queue.append(index - 1)
        if x + 1 < width: queue.append(index + 1)
        if y: queue.append(index - width)
        if y + 1 < height: queue.append(index + width)
    alpha = Image.new("L", image.size)
    alpha.putdata([0 if outside[i] else pixel[3]
                   for i, pixel in enumerate(pixels)])
    image.putalpha(alpha)
    return image


def largest_component(image):
    width, height = image.size
    mask = bytearray(value > 0 for value in image.getchannel("A").getdata())
    best = []
    for start in range(len(mask)):
        if not mask[start]:
            continue
        component = []
        queue = deque([start])
        mask[start] = 0
        while queue:
            index = queue.popleft()
            component.append(index)
            x, y = index % width, index // width
            neighbors = []
            if x: neighbors.append(index - 1)
            if x + 1 < width: neighbors.append(index + 1)
            if y: neighbors.append(index - width)
            if y + 1 < height: neighbors.append(index + width)
            for other in neighbors:
                if mask[other]:
                    mask[other] = 0
                    queue.append(other)
        if len(component) > len(best):
            best = component
    if not best:
        raise ValueError("No foreground object found")
    alpha = Image.new("L", image.size)
    data = bytearray(width * height)
    for index in best:
        data[index] = 255
    alpha.frombytes(bytes(data))
    image.putalpha(alpha)
    return image.crop(alpha.getbbox())


def clean(image):
    alpha = image.getchannel("A")
    rgb = image.convert("RGB").filter(ImageFilter.MedianFilter(3))
    rgb = rgb.quantize(colors=96, method=Image.Quantize.MEDIANCUT).convert("RGB")
    rgb = rgb.filter(ImageFilter.UnsharpMask(radius=0.7, percent=110, threshold=4))
    rgb.putalpha(alpha)
    return rgb


def remove_footer_strip(image):
    image = image.convert("RGBA")
    pixels = image.load()
    for y in range(image.height - 48, image.height):
        for x in range(image.width):
            r, g, b, a = pixels[x, y]
            if min(r, g, b) > 140 and max(r, g, b) - min(r, g, b) < 18:
                pixels[x, y] = (r, g, b, 0)
    return image


def fit(image, size):
    contained = ImageOps.contain(image, size, Image.Resampling.LANCZOS)
    result = Image.new("RGBA", size)
    result.alpha_composite(contained, ((size[0] - contained.width) // 2,
                                       (size[1] - contained.height) // 2))
    return result


def emit_bitmaps(images):
    lines = ["#pragma once", "", "#include <stdint.h>", ""]
    for name, image in images.items():
        rgba = list(image.getdata())
        lines += [
            f"#define {name.upper()}_W {image.width}",
            f"#define {name.upper()}_H {image.height}",
            f"static const uint16_t s_{name}_rgb565[{image.width * image.height}] = {{",
            format_values([rgb565(r, g, b) for r, g, b, _ in rgba], 12, 4),
            "};",
            f"static const uint8_t s_{name}_alpha[{image.width * image.height}] = {{",
            format_values([a for _, _, _, a in rgba], 16, 2),
            "};", "",
        ]
    (ROOT / "esp32_idf_s3n16r8" / "main" / "small_apps_art.h").write_text(
        "\n".join(lines), encoding="ascii", newline="\n")


def main():
    orange = Image.open(REFERENCES / "fish_orange.png")
    # Keep the original complete in references; exclude only the outer page.
    orange = orange.crop((70, 180, 945, 815))
    orange = clean(largest_component(remove_background(orange, (255, 212, 149), 28)))
    purple = Image.open(REFERENCES / "fish_purple.png").crop((75, 110, 430, 375))
    purple = clean(largest_component(remove_background(purple, (226, 216, 241), 32)))
    tomato = remove_footer_strip(Image.open(REFERENCES / "tomato_face.png"))
    tomato = clean(largest_component(remove_background(tomato, (255, 255, 255), 20)))
    clock = Image.open(REFERENCES / "tomato_clock.png").convert("RGBA")
    for name, image in (("fish_orange", orange), ("fish_purple", purple),
                        ("tomato_face", tomato), ("tomato_clock", clock)):
        image.save(OUTPUT / f"{name}_cutout.png")
    fit(orange, (256, 256)).save(ASSETS / "launcher_icons" / "aquarium.png")
    fit(tomato, (256, 256)).save(ASSETS / "launcher_icons" / "pomodoro.png")
    clock_frame = clock.copy()
    alpha = clock_frame.getchannel("A")
    # Keep the reference shell; the dial is a live numeric clock layer.
    ImageDraw.Draw(alpha).ellipse((90, 120, 222, 252), fill=0)
    clock_frame.putalpha(alpha)
    images = {
        "fish_orange": fit(orange, (40, 30)),
        "fish_purple": fit(purple, (40, 30)),
        "tomato_face": fit(tomato, (34, 34)),
        "tomato_clock": fit(clock_frame, (144, 144)),
    }
    for name, image in images.items():
        image.save(OUTPUT / f"{name}_display.png")
    emit_bitmaps(images)


if __name__ == "__main__":
    main()
