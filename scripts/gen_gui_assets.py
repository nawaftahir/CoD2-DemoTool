#!/usr/bin/env python3
"""
Generate src/gui_assets.h: the GUI's logo + UI font baked into C byte arrays so the
exe is self-contained (no external image/font files at runtime).

Inputs:
  assets/cod2x.jpg            the window logo (decoded with stb_image at startup)
  <font>                      a .ttf for the UI (default: Roboto-Regular, Apache-2.0)

Run from the tool root:  python3 scripts/gen_gui_assets.py [path/to/font.ttf]
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = os.path.join(ROOT, "assets", "cod2x.jpg")
FONT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "assets", "Roboto-Regular.ttf")
OUT  = os.path.join(ROOT, "src", "gui_assets.h")

def emit(f, name, data):
    f.write(f"static const unsigned char {name}[] = {{\n")
    for i in range(0, len(data), 16):
        f.write("\t" + ",".join(str(b) for b in data[i:i+16]) + ",\n")
    f.write("};\n")
    f.write(f"static const unsigned int {name}_len = {len(data)};\n\n")

def main():
    if not os.path.exists(LOGO): sys.exit(f"missing logo: {LOGO}")
    if not os.path.exists(FONT): sys.exit(f"missing font: {FONT}")
    logo = open(LOGO, "rb").read()
    font = open(FONT, "rb").read()
    with open(OUT, "w") as f:
        f.write("// AUTO-GENERATED embedded GUI assets (logo + UI font), so the exe stays\n")
        f.write("// self-contained. Regenerate with scripts/gen_gui_assets.py.\n")
        f.write("#ifndef GUI_ASSETS_H\n#define GUI_ASSETS_H\n\n")
        f.write("// CoD2x logo (JPEG) -> decoded with stb_image at startup.\n")
        emit(f, "g_logoJpg", logo)
        f.write("// UI font (Apache-2.0 Roboto by default) -> loaded as the ImGui font.\n")
        emit(f, "g_robotoTtf", font)
        f.write("#endif\n")
    print(f"wrote {OUT}: logo {len(logo)}B, font {len(font)}B")

if __name__ == "__main__":
    main()
