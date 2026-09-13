from pathlib import Path

from PIL import Image


ICON_SIZE = 48
ICON_NAMES = ("chat_alarm", "settings", "radio", "tetris", "snake")


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def format_values(values: list[int], width: int, digits: int) -> str:
    rows = []
    for offset in range(0, len(values), width):
        row = ", ".join(
            f"0x{value:0{digits}X}" for value in values[offset:offset + width]
        )
        rows.append(f"        {row},")
    return "\n".join(rows)


def main() -> None:
    repository = Path(__file__).resolve().parents[1]
    source_dir = (
        repository / "esp32_idf_s3n16r8" / "main" / "assets"
        / "launcher_icons"
    )
    output = (
        repository / "esp32_idf_s3n16r8" / "main"
        / "launcher_icons_data.h"
    )
    preview_dir = repository / "icon-previews"
    preview_dir.mkdir(exist_ok=True)

    pixel_sets = []
    alpha_sets = []
    for name in ICON_NAMES:
        image = Image.open(source_dir / f"{name}.png").convert("RGBA")
        image = image.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
        rgba = list(image.getdata())
        pixel_sets.append([rgb565(red, green, blue)
                           for red, green, blue, _ in rgba])
        alpha_sets.append([alpha for _, _, _, alpha in rgba])

        preview = Image.new("RGBA", image.size, (31, 41, 55, 255))
        preview.alpha_composite(image)
        preview.convert("RGB").save(
            preview_dir / f"{name.replace('_', '-')}-48.bmp",
            format="BMP",
        )

    pixel_blocks = []
    alpha_blocks = []
    for name, pixels, alpha in zip(ICON_NAMES, pixel_sets, alpha_sets):
        pixel_blocks.append(
            f"    // {name}\n"
            "    {\n"
            f"{format_values(pixels, 12, 4)}\n"
            "    },"
        )
        alpha_blocks.append(
            f"    // {name}\n"
            "    {\n"
            f"{format_values(alpha, 16, 2)}\n"
            "    },"
        )

    output.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"#define LAUNCHER_ICON_W {ICON_SIZE}\n"
        f"#define LAUNCHER_ICON_H {ICON_SIZE}\n\n"
        "static const uint16_t "
        "s_launcher_icon_rgb565[5][LAUNCHER_ICON_W * LAUNCHER_ICON_H] = {\n"
        + "\n".join(pixel_blocks)
        + "\n};\n\n"
        "static const uint8_t "
        "s_launcher_icon_alpha[5][LAUNCHER_ICON_W * LAUNCHER_ICON_H] = {\n"
        + "\n".join(alpha_blocks)
        + "\n};\n",
        encoding="ascii",
        newline="\n",
    )


if __name__ == "__main__":
    main()
