"""Gera assets/app.ico: barras de equalizador ciano com um pico magenta.

Cada tamanho é desenhado nativamente (em vez de reduzir o de 256 px) para os
traços ficarem nítidos no ícone da bandeja, que é 16/20/24 px.
Uso: python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

BG = (6, 8, 12, 255)
CYAN = (0, 229, 255, 255)
MAGENTA = (255, 42, 109, 255)
SIZES = [16, 20, 24, 32, 48, 64, 256]
HEIGHTS = [0.40, 0.75, 0.55, 0.90]  # fração da altura útil de cada barra


def draw(size: int) -> Image.Image:
    scale = 4  # supersampling para bordas suaves
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=int(s * 0.18), fill=BG)

    left, right = s * 0.20, s * 0.80
    bottom, top = s * 0.78, s * 0.22
    slot = (right - left) / len(HEIGHTS)
    bar = slot * 0.62
    for i, h in enumerate(HEIGHTS):
        x = left + slot * i + (slot - bar) / 2
        y = bottom - (bottom - top) * h
        d.rectangle([round(x), round(y), round(x + bar), round(bottom)], fill=CYAN)
    # pico da barra mais alta
    x = left + slot * 3 + (slot - bar) / 2
    cap = max(scale, round(s * 0.06))
    d.rectangle([round(x), round(top - cap * 1.8), round(x + bar), round(top - cap * 0.8)], fill=MAGENTA)
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "assets" / "app.ico"
    images = [draw(size) for size in SIZES]
    images[-1].save(out, format="ICO", sizes=[(i.width, i.height) for i in images],
                    append_images=images[:-1])
    print(f"ok: {out}")


if __name__ == "__main__":
    main()
