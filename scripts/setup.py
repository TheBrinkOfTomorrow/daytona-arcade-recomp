#!/usr/bin/env python3
"""Fetch everything needed to build daytona-arcade-recomp and build it. The
same steps on Linux, macOS and Windows; run ./setup.sh (Linux, macOS) or
setup.ps1 (Windows) first to install the toolchain, or run this directly if
you already have one.

  setup.py [--jobs N] [--test-extras] [--with-mame] [--build-mame] [--no-build]
  setup.py --list-roms

Always:
  - checks the toolchain: git, CMake >= 3.20, a C++20 compiler (Ninja used
    when present; Visual Studio's generator on Windows);
  - fetches Berkeley SoftFloat 3e, SDL 3, ymfm, the LZMA SDK and Dear ImGui
    at their pinned commits into extern/ (all built from source with the
    project, statically);
  - configures and builds the tools and tests, runs the tests;
  - if your ROM set is at roms/daytona93.zip (or .7z): imports it, recompiles the
    game's code to native C++ and builds it (all under build/, git-ignored);
  - the same for the 1994 version (MAME's daytona, Revision A) if it is at
    roms/daytona.zip (or .7z), in build-daytona/ (a separate build: each
    build directory is recompiled from one ROM set).
Options:
  --jobs N, -j N compile N files at once (default: one per CPU thread; the
                 tests run as many at once too). Lower it if the machine runs
                 out of memory compiling the game's code.
  --list-roms    list every file each ROM set must contain (name, size, CRC32),
                 and check roms/<set>.zip against it if it is there; builds
                 nothing.
  --test-extras  pip packages and modules for the optional tests (lupa for the
                 Lua plugin tests, pypcode + the Ghidra i960 module for the
                 second decode oracle).
  --with-mame    MAME source at its pinned commit, patched (the disassembler
                 oracle test; needed before --build-mame).
  --build-mame   build the patched, Model-2-only MAME that records the traces
                 the lockstep checks compare against (Linux and macOS; tens of
                 minutes). Only for validation: the game never uses MAME.

Nothing fetched here is committed (extern/ is git-ignored), and no ROM data
ever leaves roms/ and build/.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTERN = os.path.join(ROOT, "extern")
WINDOWS = os.name == "nt"

# Pinned third-party sources (THIRD_PARTY.md).
SOFTFLOAT = ("https://github.com/ucb-bar/berkeley-softfloat-3.git", "a0c6494cdc11865811dec815d5c0049fba9d82a8")
SDL3 = ("https://github.com/libsdl-org/SDL.git", "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c")  # release-3.4.16
IMGUI = ("https://github.com/ocornut/imgui.git", "f1cc2ae15e53a861a874c3034aae6798fde194ab")  # v1.92.9b
YMFM = ("https://github.com/aaronsgiles/ymfm.git", "81aec25ccbb98f4873a255f7551ac4dadac59b4a")  # YM3438 (sound board FM)
LZMA_SDK = ("https://github.com/ip7z/7zip.git", "0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17")  # 7-Zip 26.03 (C/: LZMA SDK)
GHIDRA_I960 = ("https://github.com/mumbel/ghidra_i960.git", "727ef7872c5b1cd6ceb5a81f5e474d1ced92945c")
MAME_COMMIT = "dddd73680656e355bb2b5beecab1167c9f07bf81"


def say(msg):
    print(f"\n== {msg}", flush=True)


def run(cmd, cwd=ROOT, check=True):
    print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, check=check)


def need(tool, hint):
    path = shutil.which(tool)
    if not path:
        sys.exit(f"setup: {tool} not found. {hint}")
    return path


def check_toolchain():
    say("Checking the toolchain")
    need("git", "Install Git (setup.sh / setup.ps1 do this).")
    need("cmake", "Install CMake 3.20 or newer (setup.sh / setup.ps1 do this).")
    out = subprocess.run(["cmake", "--version"], capture_output=True, text=True).stdout
    m = re.search(r"(\d+)\.(\d+)", out)
    if not m or (int(m.group(1)), int(m.group(2))) < (3, 20):
        sys.exit(f"setup: CMake 3.20 or newer is needed, found {out.splitlines()[0] if out else 'none'}")
    if not WINDOWS and not (shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")):
        sys.exit("setup: no C++ compiler found (setup.sh installs one)")
    print(f"{platform.system()} {platform.machine()}, Python {platform.python_version()}, {out.splitlines()[0]}")


def fetch(url, commit, dest):
    dest = os.path.join(EXTERN, dest)
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--no-checkout", url, dest])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", commit], check=False)
    if run(["git", "-C", dest, "checkout", "--detach", commit], check=False).returncode:
        # our patches (patches/) changed files the new commit changes too: start
        # clean at the commit; apply_patches puts them back
        run(["git", "-C", dest, "checkout", "--force", "--detach", commit])


def apply_patches(dest, name):
    """Apply patches/<name>/*.patch to extern/<dest>, skipping ones already applied."""
    dest = os.path.join(EXTERN, dest)
    patches = os.path.join(ROOT, "patches", name)
    for p in sorted(os.listdir(patches)):
        if not p.endswith(".patch"):
            continue
        path = os.path.join(patches, p)
        if subprocess.run(["git", "-C", dest, "apply", "--check", path], capture_output=True).returncode == 0:
            run(["git", "-C", dest, "apply", path])
        elif subprocess.run(["git", "-C", dest, "apply", "--reverse", "--check", path], capture_output=True).returncode:
            print(f"setup: warning: patches/{name}/{p} does not apply to extern/{os.path.basename(dest)}")


def fetch_mame(full):
    """The pinned MAME with our oracle patches. Sparse (the files the tests
    read) unless the full tree is needed to build it."""
    dest = os.path.join(EXTERN, "mame")
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--filter=blob:none", "--no-checkout", "https://github.com/mamedev/mame.git", dest])
    if full:
        run(["git", "-C", dest, "sparse-checkout", "disable"], check=False)
    else:
        run(["git", "-C", dest, "sparse-checkout", "set", "--no-cone",
             "/src/devices/cpu/i960/", "/src/devices/cpu/mb86233/",
             "/src/devices/cpu/m68000/m68000.cpp", "/src/devices/cpu/m68000/m68000.h",
             "/src/mame/sega/model2.cpp", "/src/mame/sega/model2.h",
             "/src/mame/sega/model2_v.cpp", "/src/mame/sega/model2_m.cpp",
             "/src/mame/shared/segam1audio.cpp", "/src/mame/shared/segam1audio.h"])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", MAME_COMMIT])
    run(["git", "-C", dest, "checkout", "--detach", MAME_COMMIT])
    apply_patches("mame", "mame")


VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")


def vs_with_clang():
    """Return a Visual Studio 2022 instance with both compiler and toolset.

    Match setup.ps1's VS 2022 install, even when a newer VS also has Clang.
    CMake must use this same instance, not its default (possibly newer) VS.
    """
    if not os.path.exists(VSWHERE):
        return None
    out = subprocess.run([VSWHERE, "-latest", "-version", "[17,18)", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
                          "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset", "-format", "json", "-utf8"],
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    instances = json.loads(out)
    return instances[0] if instances else None


def cached(build, key):
    try:
        with open(os.path.join(build, "CMakeCache.txt")) as cache:
            for line in cache:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def configure_and_build(build, romset="daytona93", jobs=None):
    say("Configuring and building" + ("" if romset == "daytona93" else f" ({romset})"))
    cmd = ["cmake", "-S", ".", "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DM2_ROMSET=" + romset]
    want = {}  # cache entries that must match, or the build directory is reconfigured
    if WINDOWS and not (shutil.which("cl") and shutil.which("ninja")):
        # Visual Studio generator: finds the compiler without a developer prompt.
        # Clang (clang-cl) when Visual Studio has it, else MSVC.
        cmd += ["-A", "x64"]
        choice = os.environ.get("M2_COMPILER", "").lower()  # clang or msvc forces one (CI builds both)
        clang_vs = vs_with_clang() if choice != "msvc" else None
        if choice == "clang" and not clang_vs:
            sys.exit("setup: M2_COMPILER=clang, but Visual Studio 2022 has no Clang tools (setup.ps1 adds them)")
        toolset = "ClangCL" if clang_vs else ""
        if toolset:
            major = clang_vs["installationVersion"].split(".", 1)[0]
            capabilities = json.loads(subprocess.run(["cmake", "-E", "capabilities"],
                                      capture_output=True, text=True, check=True).stdout)
            generator = next((g["name"] for g in capabilities["generators"]
                              if g["name"].startswith(f"Visual Studio {major} ")), None)
            if not generator:
                sys.exit(f"setup: this CMake does not support Visual Studio {major}, where Clang is installed")
            instance = clang_vs["installationPath"].replace("\\", "/")
            cmd += ["-G", generator, "-DCMAKE_GENERATOR_INSTANCE=" + instance, "-T", toolset]
            want["CMAKE_GENERATOR"] = generator
            want["CMAKE_GENERATOR_INSTANCE"] = instance
            print(f"Compiler: Clang ({generator}, {instance})")
        else:
            print("Compiler: MSVC. For Clang, run setup.ps1, which adds Visual Studio's Clang tools.")
        want["CMAKE_GENERATOR_TOOLSET"] = toolset
    elif shutil.which("ninja"):
        cmd += ["-G", "Ninja"]
        if WINDOWS and shutil.which("clang-cl"):  # a developer prompt with Clang on PATH
            cmd += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
    if os.path.exists(os.path.join(build, "CMakeCache.txt")):
        if any((cached(build, k) or "") != v for k, v in want.items()):
            # A generator, instance or toolset cannot change in place.
            print("The build directory was configured for another compiler or Visual Studio installation; reconfiguring it.")
            os.remove(os.path.join(build, "CMakeCache.txt"))
            shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    if not os.path.exists(os.path.join(build, "CMakeCache.txt")):
        run(cmd)
    else:
        run(["cmake", "-S", ".", "-B", build, "-DM2_ROMSET=" + romset])
    run(["cmake", "--build", build, "--config", "Release", "--parallel"] + ([str(jobs)] if jobs else []))


def default_jobs():
    return os.cpu_count() or 1


ROM_IMPORT = os.path.join(ROOT, "src", "runtime", "rom_import.cpp")


def rom_manifest():
    """{set: [(file, size, crc32, shared)]}, read from the importer's tables
    (src/runtime/rom_import.cpp), so the list is the one the game checks."""
    src = open(ROM_IMPORT, encoding="utf-8").read()
    entry = re.compile(r'\{"([^"]+)", 0x([0-9A-Fa-f]+), \w+, 0x[0-9A-Fa-f]+, 0x([0-9A-Fa-f]+)\}')
    shared_block = re.search(r"#define SHARED_LOADS(.*?)\n\n", src, re.S).group(1)
    shared = [(f, int(size, 16), int(crc, 16), True) for f, crc, size in entry.findall(shared_block)]
    sets = {}
    for var, romset in (("kDaytona93", "daytona93"), ("kDaytona", "daytona"), ("kSrallyc", "srallyc")):
        body = re.search(r"const Load " + var + r"\[\] = \{(.*?)\n\};", src, re.S).group(1)
        own = [(f, int(size, 16), int(crc, 16), False) for f, crc, size in entry.findall(body)]
        sets[romset] = own + (shared if "SHARED_LOADS" in body else [])
    return sets


def archive_entries(romset):
    """{name: (size, crc)} of roms/<set>.zip, or None when there is no zip
    (a .7z is reported as present but not checked here)."""
    import zipfile
    path = os.path.join(ROOT, "roms", romset + ".zip")
    if not os.path.exists(path):
        return None
    try:
        with zipfile.ZipFile(path) as z:
            # by file name, ignoring folders, as the importer reads them (src/runtime/zip.cpp)
            return {i.filename.rsplit("/", 1)[-1]: (i.file_size, i.CRC) for i in z.infolist() if not i.is_dir()}
    except zipfile.BadZipFile:
        return {}


def list_roms():
    manifest = rom_manifest()
    for romset, label in SETS:
        files = manifest[romset]
        entries = archive_entries(romset)
        print(f"\n{label}: roms/{romset}.zip (or .7z), {len(files)} files")
        if entries is None:
            seven = os.path.exists(os.path.join(ROOT, "roms", romset + ".7z"))
            print(f"  roms/{romset}.7z is there; not checked here (setup checks it)" if seven
                  else f"  roms/{romset}.zip is not there yet")
        missing = shared_missing = 0
        for name, size, crc, shared in files:
            status = ""
            if entries is not None:
                got = entries.get(name)
                status = "ok" if got == (size, crc) else "MISSING" if got is None else \
                    f"WRONG (size {got[0]:#x}, crc {got[1]:08x})"
                missing += status != "ok"
                shared_missing += shared and got is None
            print(f"  {name:<16} {size:#9x}  {crc:08x}  {'shared' if shared else romset:<9}  {status}".rstrip())
        if entries is not None:
            print(f"  {len(files) - missing} of {len(files)} present and correct")
            shared_count = sum(f[3] for f in files)
            if shared_missing == shared_count and missing < len(files):
                print("  All the shared files are missing: a split set keeps them in its parent's archive. "
                      "Use a merged or non-merged set, or add them to this zip.")
            names = {f[0] for f in files}
            other = sorted(n for o, fs in manifest.items() if o != romset for n, _, _, _ in fs
                           if n not in names and n in entries)
            if other:
                print(f"  It also holds {len(other)} file{'s' * (len(other) > 1)} of the other set ({', '.join(other)}): "
                      "the two sets may have been mixed up.")
    print("\n'shared' files are the same in both Daytona sets.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", "-j", type=int, default=None, metavar="N")
    ap.add_argument("--list-roms", action="store_true")
    ap.add_argument("--test-extras", action="store_true")
    ap.add_argument("--with-mame", action="store_true")
    ap.add_argument("--build-mame", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--build-dir", default="build")
    args = ap.parse_args()
    if args.list_roms:
        list_roms()
        return
    jobs = args.jobs or default_jobs()
    if jobs < 1:
        sys.exit("setup: --jobs must be 1 or more")
    build = os.path.join(ROOT, args.build_dir)

    check_toolchain()
    os.makedirs(EXTERN, exist_ok=True)

    say("Fetching SoftFloat 3e (extF80 reference for i960 FP)")
    fetch(*SOFTFLOAT, "softfloat")

    say("Fetching SDL 3.4.16 (window, input, SDL_GPU: Vulkan / Direct3D 12 / Metal)")
    fetch(*SDL3, "sdl3")
    apply_patches("sdl3", "sdl3")  # Vulkan application name (THIRD_PARTY.md)

    say("Fetching ymfm (the sound board's YM3438)")
    fetch(*YMFM, "ymfm")

    say("Fetching the 7-Zip LZMA SDK 26.03 (7z ROM sets)")
    fetch(*LZMA_SDK, "lzma")

    say("Fetching Dear ImGui 1.92.9b (launcher interface)")
    fetch(*IMGUI, "imgui")

    if args.test_extras:
        say("Optional test extras")
        run([sys.executable, "-m", "pip", "install", "--user", "lupa", "pypcode"], check=False)
        fetch(*GHIDRA_I960, "ghidra_i960")

    if args.with_mame or args.build_mame:
        say("Fetching MAME (validation oracle only) and applying our patches")
        fetch_mame(full=args.build_mame)

    if args.build_mame:
        if WINDOWS:
            print("setup: building MAME on Windows needs MAME's MSYS2 toolchain; see "
                  "https://docs.mamedev.org/initialsetup/compilingmame.html. Skipped.")
        else:
            say("Building the Model-2-only MAME (this takes a while)")
            run(["sh", os.path.join("scripts", "build_mame.sh")])

    if args.no_build:
        return
    print(f"Building with {jobs} parallel jobs (--jobs to change)")
    configure_and_build(build, jobs=jobs)

    games = []  # (set, executable)
    for romset, label in SETS:
        if not any(os.path.exists(os.path.join(ROOT, "roms", romset + "." + e)) for e in ("zip", "7z")):
            continue
        set_dir = args.build_dir if romset == "daytona93" else args.build_dir + "-" + romset
        set_build = os.path.join(ROOT, set_dir)
        if set_build != build:
            configure_and_build(set_build, romset, jobs)
        say(f"Recompiling {label} to native code (your ROM set, kept in {set_dir}/)")
        r = run([sys.executable, os.path.join("scripts", "recompile.py"), "--set", romset, "--build-dir", set_dir,
                 "--jobs", str(jobs)],
                check=False)
        if r.returncode == 3:  # recompile.py ROM_REJECTED_EXIT
            sys.exit(ROM_REJECTED.format(set=romset))
        if r.returncode:
            sys.exit(BUILD_FAILED)
        name = "segarally" if romset == "srallyc" else "daytona"  # CMake's game target, by board
        exe = next((p for p in (os.path.join(set_build, name + EXE), os.path.join(set_build, "Release", name + EXE))
                    if os.path.exists(p)), None)
        if exe:
            games.append((label, exe))
    if not games:
        say("No ROM set: the tools are built, the game is not")
        print(no_rom_help())

    say("Running the tests")
    run(["ctest", "--test-dir", build, "-C", "Release", "--output-on-failure", "-j", str(jobs)], check=False)
    say("Done")
    for label, exe in games:
        print(f"\n{label} is built. Start it with:\n\n    " + os.path.relpath(exe, ROOT))
    if games:
        print()


EXE = ".exe" if WINDOWS else ""
# The ROM sets the game can be recompiled from: roms/<set>.zip or .7z.
SETS = [("daytona93", "Daytona USA Deluxe '93 (daytona93)"), ("daytona", "Daytona USA Revision A, 1994 (daytona)"),
        ("srallyc", "Sega Rally Championship Revision C (srallyc; no sound yet)")]

BUILD_FAILED = """
setup: your ROM set was accepted, but recompiling or building the game
failed (the errors are above, just before this message). That is a problem
in the build, not in your ROM set: please report it with those errors.
"""

ROM_REJECTED = """
setup: your {set} ROM set (roms/{set}.zip or .7z) was rejected (the line
starting "m2import:" above names the first file that is missing or wrong),
so that game was not built.

The game can be built from two of MAME's Daytona USA sets, each under its own
name: daytona93 (Daytona USA Deluxe '93: epr-16530a.12, epr-16531a.13,
epr-16534a.6, epr-16535a.7) at roms/daytona93.zip, and daytona (Revision A,
1994: epr-16722a.12, epr-16723a.13, epr-16724a.6, epr-16725a.7) at
roms/daytona.zip (or .7z). Other sets (daytonas, daytonat, daytonase, ...)
have different program ROMs and cannot be used, whatever the file is called.

Put the right set under the right name and run setup again.
See docs/getting-started.md, "Troubleshooting".
"""


def no_rom_help():
    roms = os.path.join(ROOT, "roms")
    named = {romset + ext for romset, _ in SETS for ext in (".zip", ".7z")}
    found = sorted(f for f in os.listdir(roms) if f.lower().endswith((".zip", ".7z")) and f not in named) \
        if os.path.isdir(roms) else []
    lines = ["To build the game, put your own ROM set at roms/daytona93.zip (Daytona USA Deluxe '93,",
             "MAME's daytona93) or roms/daytona.zip (Revision A, 1994, MAME's daytona), or .7z,",
             "exactly that name, and run setup again. Both can be there: each is built."]
    if found:
        lines += ["", "Found in roms/, but not under those names: " + ", ".join(found),
                  "If one of these is one of those sets, rename it to that set's name."]
    return "\n".join(lines)


if __name__ == "__main__":
    main()
