#!/usr/bin/env python3
"""Prepara una copia de Shandalar lista para Autorun (Wine para Nintendo Switch).

Uso:
    python switch/preparar_switch.py DESTINO [--slim] [--sin-pad] [--window N]

DESTINO es la carpeta donde quedará el juego, normalmente en la microSD:
    E:\\switch\\wine\\drive_c\\Shandalar          (Windows)
    /media/usuario/SD/switch/wine/drive_c/Shandalar (Linux)

Si DESTINO es la carpeta del propio repositorio, los ajustes se aplican ahí
mismo sin copiar nada.

Qué hace:
  1. Copia el juego al destino. Omite .git, src y switch, y en las ejecuciones
     siguientes solo copia lo que cambió.
  2. Instala ShandalarPad: renombra image.dll a image_orig.dll y pone la
     versión de ShandalarPad en su lugar, junto con ShandalarPad.ini.
  3. Copia Shandalar.keys.txt, el perfil de controles de Autorun.
  4. Desactiva el modo debug por defecto en config.txt (Debug:0).
  5. Pone Window = 2 en Shandalar.ini: la aventura en una ventana de 1024x768 y
     los duelos a pantalla completa. Con Window = 0 el juego se cae en la
     pantalla de 1280x720 de Autorun.

--slim    también omite copias duplicadas y herramientas de desarrollo
          (Program, Manalink3, Editor, magic_updater, PlayDeckAnalyser). Ahorra
          unos 700 MB; si algo falla, vuelve a la copia completa.
--sin-pad no instala ShandalarPad (solo los controles de Autorun).
--window N  valor de Window en Shandalar.ini (0, 1 o 2; por defecto 2).
"""

import argparse
import os
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SWITCH = os.path.join(REPO, "switch")
PAD_DLL = os.path.join(SWITCH, "ShandalarPad", "bin", "image.dll")
PAD_INI = os.path.join(SWITCH, "ShandalarPad", "ShandalarPad.ini")
KEYS = os.path.join(SWITCH, "autorun", "Shandalar.keys.txt")

ALWAYS_SKIP = {".git", "src", "switch"}
SLIM_SKIP = {"Program", "Manalink3", "Editor", "magic_updater", "PlayDeckAnalyser"}
PAD_MARKER = b"ShandalarPad"


def same_file(src, dst):
    try:
        a, b = os.stat(src), os.stat(dst)
    except OSError:
        return False
    return a.st_size == b.st_size and int(a.st_mtime) <= int(b.st_mtime)


def copy_game(dest, slim):
    skip = ALWAYS_SKIP | (SLIM_SKIP if slim else set())
    copied = 0
    for root, dirs, files in os.walk(REPO):
        rel = os.path.relpath(root, REPO)
        if rel == ".":
            dirs[:] = [d for d in dirs if d not in skip]
        target_dir = dest if rel == "." else os.path.join(dest, rel)
        os.makedirs(target_dir, exist_ok=True)
        for name in files:
            src = os.path.join(root, name)
            dst = os.path.join(target_dir, name)
            # image.dll is handled by install_pad once ShandalarPad has replaced it.
            if rel == "." and name.lower() in ("image.dll", "image_orig.dll") and os.path.exists(dst):
                continue
            if same_file(src, dst):
                continue
            shutil.copy2(src, dst)
            copied += 1
            if copied % 500 == 0:
                print(f"  {copied} archivos copiados...")
    print(f"  {copied} archivos copiados o actualizados.")


def is_pad(path):
    with open(path, "rb") as f:
        return PAD_MARKER in f.read()


def install_pad(dest):
    image = os.path.join(dest, "image.dll")
    orig = os.path.join(dest, "image_orig.dll")
    if not os.path.exists(PAD_DLL):
        sys.exit(f"No encuentro {PAD_DLL}. Compílalo con 'make' en switch/ShandalarPad.")
    if not os.path.exists(orig):
        if not os.path.exists(image):
            sys.exit(f"No encuentro {image}; ¿la carpeta destino es la del juego?")
        if is_pad(image):
            sys.exit("image.dll ya es ShandalarPad pero falta image_orig.dll. "
                     "Copia el image.dll original del repositorio como image_orig.dll.")
        os.replace(image, orig)
        print("  image.dll original -> image_orig.dll")
    shutil.copy2(PAD_DLL, image)
    print("  ShandalarPad instalado como image.dll")
    ini = os.path.join(dest, "ShandalarPad.ini")
    if os.path.exists(ini):
        print("  ShandalarPad.ini ya existe; se conserva tu configuración")
    else:
        shutil.copy2(PAD_INI, ini)
        print("  ShandalarPad.ini copiado")


def set_debug_off(dest):
    path = os.path.join(dest, "config.txt")
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="latin-1", newline="") as f:
        text = f.read()
    new = text.replace("\nDebug:1", "\nDebug:0")
    if new != text:
        with open(path, "w", encoding="latin-1", newline="") as f:
            f.write(new)
        print("  config.txt: Debug:0")


def set_window_mode(dest, mode):
    path = os.path.join(dest, "Shandalar.ini")
    if not os.path.exists(path):
        return
    with open(path, "r", encoding="latin-1", newline="") as f:
        lines = f.read().split("\n")
    for i, line in enumerate(lines):
        key = line.split("=", 1)[0].strip()
        if key == "Window" and not line.lstrip().startswith(";"):
            lines[i] = f"Window = {mode}" + ("\r" if line.endswith("\r") else "")
            break
    with open(path, "w", encoding="latin-1", newline="") as f:
        f.write("\n".join(lines))
    print(f"  Shandalar.ini: Window = {mode}")


def main():
    parser = argparse.ArgumentParser(description="Prepara Shandalar para Autorun en Switch.")
    parser.add_argument("destino")
    parser.add_argument("--slim", action="store_true")
    parser.add_argument("--sin-pad", action="store_true")
    parser.add_argument("--window", type=int, choices=(0, 1, 2), default=2,
                        help="valor de Window en Shandalar.ini (por defecto 2)")
    args = parser.parse_args()

    dest = os.path.abspath(args.destino)
    in_place = os.path.normcase(dest) == os.path.normcase(REPO)
    print(f"Destino: {dest}")
    if in_place:
        print("El destino es el repositorio: se aplican los ajustes sin copiar.")
    else:
        print("Copiando el juego (la primera vez tarda, son unos 2 GB)...")
        copy_game(dest, args.slim)
    if not args.sin_pad:
        install_pad(dest)
    shutil.copy2(KEYS, os.path.join(dest, "Shandalar.keys.txt"))
    print("  Shandalar.keys.txt copiado")
    set_debug_off(dest)
    set_window_mode(dest, args.window)
    print("Listo. En Autorun: + -> Add game -> elige Shandalar.exe.")


if __name__ == "__main__":
    main()
