"""Builds the two release archives in ModSource\\dist:
  DragonkinModLoader-1.0.0.zip  (dwmapi.dll, example mods, docs, SDK)
  DragonkinPlus-1.0.0.zip       (Mods\\DragonkinPlus)
Both are rooted at DragonkinTheBanished\\ so they extract into the game folder."""
import hashlib
import os
import zipfile

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(SRC, "dist")
W64 = "DragonkinTheBanished/Binaries/Win64/"

notices = open(os.path.join(SRC, "THIRD_PARTY_NOTICES.txt"), encoding="utf-8").read()

def notices_for(dll):
    return notices.replace("compiled into dwmapi.dll", "compiled into " + dll)

LOADER = [
    (W64 + "dwmapi.dll", "build/dwmapi.dll"),
    (W64 + "Mods/ExampleBasics/mod.json", "examples/ExampleBasics/mod.json"),
    (W64 + "Mods/ExampleProperties/mod.json", "examples/ExampleProperties/mod.json"),
    (W64 + "Mods/ExamplePlugin/mod.json", "examples/ExamplePlugin/mod.json"),
    (W64 + "Mods/ExamplePlugin/ExamplePlugin.dll", "examples/ExamplePlugin/ExamplePlugin.dll"),
    (W64 + "DragonkinModLoader/README.md", "release/loader/README.md"),
    (W64 + "DragonkinModLoader/README_TR.md", "release/loader/README_TR.md"),
    (W64 + "DragonkinModLoader/docs/MODDING.md", "docs/MODDING.md"),
    (W64 + "DragonkinModLoader/sdk/dml_plugin.h", "sdk/dml_plugin.h"),
    (W64 + "DragonkinModLoader/sdk/example/ExamplePlugin.c", "sdk/example/ExamplePlugin.c"),
    (W64 + "DragonkinModLoader/sdk/example/ExamplePlugin.rc", "sdk/example/ExamplePlugin.rc"),
    (W64 + "DragonkinModLoader/THIRD_PARTY_NOTICES.txt", ("text", notices_for("dwmapi.dll"))),
]
PLUS = [
    (W64 + "Mods/DragonkinPlus/mod.json", "plus/mod.json"),
    (W64 + "Mods/DragonkinPlus/DragonkinPlus.dll", "build/plus/DragonkinPlus.dll"),
    (W64 + "Mods/DragonkinPlus/README.md", "release/plus/README.md"),
    (W64 + "Mods/DragonkinPlus/README_TR.md", "release/plus/README_TR.md"),
    (W64 + "Mods/DragonkinPlus/THIRD_PARTY_NOTICES.txt", ("text", notices_for("DragonkinPlus.dll"))),
]

def build(name, entries):
    os.makedirs(DIST, exist_ok=True)
    path = os.path.join(DIST, name)
    flat = os.path.join(DIST, os.path.splitext(name)[0])  # loose copies for per-file scans
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, src in entries:
            data = src[1].encode("utf-8") if isinstance(src, tuple) else open(os.path.join(SRC, src), "rb").read()
            z.writestr(arc, data)
            out = os.path.join(flat, arc.replace(W64, ""))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, "wb").write(data)
            print("  %-60s %8d  %s" % (arc.replace(W64, ""), len(data), hashlib.sha256(data).hexdigest()[:16]))
    print(name, os.path.getsize(path), "bytes")

build("DragonkinModLoader-1.0.0.zip", LOADER)
build("DragonkinPlus-1.0.0.zip", PLUS)
