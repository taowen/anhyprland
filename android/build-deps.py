#!/usr/bin/env python3
"""Build pinned Hyprland dependencies with the NDK and arlinux's native prefix."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
PREFIX = BUILD / "android-prefix"
HOST = BUILD / "host-prefix"
ARLINUX = Path(os.environ.get("ARLINUX_DIR", ROOT.parent / "arlinux")).resolve()
SHARED = ARLINUX / "build/ndk-prefix"
NDK = Path(os.environ.get("ANDROID_NDK_HOME", Path.home() / "Android/Sdk/ndk/29.0.14206865"))
LOCK = json.loads((ROOT / "android/deps.lock.json").read_text())
JOBS = os.environ.get("JOBS", "2")


def run(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def source(name):
    if name == "aquamarine":
        return ROOT / "subprojects/aquamarine/android"
    src = BUILD / "sources" / name
    entry = LOCK[name]
    if not (src / ".git").exists():
        run(["git", "init", src])
        run(["git", "-C", src, "remote", "add", "origin", entry["url"]])
    head = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], capture_output=True, text=True)
    if head.stdout.strip() != entry["commit"]:
        run(["git", "-C", src, "fetch", "--depth=1", "origin", entry["commit"]])
        run(["git", "-C", src, "checkout", "--detach", "FETCH_HEAD"])
    patch = ROOT / "android/patches" / (name + ".patch")
    if patch.exists():
        applied = subprocess.run(["git", "-C", src, "apply", "--reverse", "--check", patch], capture_output=True)
        if applied.returncode:
            run(["git", "-C", src, "apply", patch])
    return src


def cmake(name, options=(), host=False, target=None, src_override=None):
    src = src_override or source(name)
    prefix = HOST if host else PREFIX
    build = BUILD / (("host-" if host else "android-") + name)
    env = os.environ.copy()
    paths = [HOST / "lib/pkgconfig"] if host else [PREFIX / "lib/pkgconfig", PREFIX / "share/pkgconfig", SHARED / "lib/pkgconfig"]
    env["PKG_CONFIG_PATH"] = os.pathsep.join(map(str, paths))
    if not host:
        env["PKG_CONFIG_LIBDIR"] = env["PKG_CONFIG_PATH"]
    flags = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DBUILD_TESTING=OFF"]
    if not host:
        flags += [f"-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
                  "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_shared", f"-DCMAKE_PREFIX_PATH={PREFIX};{SHARED}",
                  f"-DCMAKE_FIND_ROOT_PATH={PREFIX};{SHARED}",
                  f"-DOPENGL_INCLUDE_DIR={NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include",
                  f"-DCMAKE_SHARED_LINKER_FLAGS=-L{PREFIX}/lib -L{SHARED}/lib",
                  f"-DCMAKE_EXE_LINKER_FLAGS=-L{PREFIX}/lib -L{SHARED}/lib"]
    else:
        flags += [f"-DCMAKE_PREFIX_PATH={HOST}"]
    run(["cmake", "-S", src, "-B", build, "-G", "Ninja", *flags, *options], env=env)
    run(["cmake", "--build", build, "-j", JOBS, *( ["--target", target] if target else [])], env=env)
    run(["cmake", "--install", build], env=env)


def cross_env():
    env = os.environ.copy()
    toolbin = NDK / "toolchains/llvm/prebuilt/linux-x86_64/bin"
    env.update(CC=str(toolbin / "aarch64-linux-android28-clang"),
               CXX=str(toolbin / "aarch64-linux-android28-clang++"), AR=str(toolbin / "llvm-ar"),
               RANLIB=str(toolbin / "llvm-ranlib"), STRIP=str(toolbin / "llvm-strip"),
               PKG_CONFIG_PATH=f"{PREFIX}/lib/pkgconfig:{PREFIX}/share/pkgconfig:{SHARED}/lib/pkgconfig",
               PKG_CONFIG_LIBDIR=f"{PREFIX}/lib/pkgconfig:{PREFIX}/share/pkgconfig:{SHARED}/lib/pkgconfig",
               CPPFLAGS=f"-I{PREFIX}/include -I{SHARED}/include",
               CFLAGS="-O2 -fPIC", CXXFLAGS="-O2 -fPIC",
               LDFLAGS=f"-L{PREFIX}/lib -L{SHARED}/lib")
    return env


def meson(name, options=()):
    src = source(name)
    build = BUILD / ("android-" + name)
    cross = BUILD / "android-cross.ini"
    env = cross_env()
    cross.write_text("[binaries]\n" + "\n".join(f"{k} = {env[v]!r}" for k, v in [("c", "CC"), ("cpp", "CXX"), ("ar", "AR"), ("strip", "STRIP")]) +
                     "\npkg-config = 'pkg-config'\n[host_machine]\nsystem = 'android'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n" +
                     f"[built-in options]\nc_args = ['-fPIC', '-I{PREFIX}/include', '-I{SHARED}/include']\n" +
                     f"c_link_args = ['-L{PREFIX}/lib', '-L{SHARED}/lib']\npkg_config_path = '{env['PKG_CONFIG_PATH']}'\n" +
                     f"[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = '{env['PKG_CONFIG_LIBDIR']}'\n")
    run(["meson", "setup", *( ["--reconfigure"] if (build / "meson-info").exists() else []), build, src,
         "--cross-file", cross, "--prefix", PREFIX, "--libdir=lib", "--buildtype=release", *options], env=env)
    run(["meson", "compile", "-C", build, "-j", JOBS], env=env)
    run(["meson", "install", "-C", build], env=env)


def autotools(name, options=(), host=False):
    src = source(name)
    env = os.environ.copy() if host else cross_env()
    env["PATH"] = "/home/linuxbrew/.linuxbrew/bin:" + env["PATH"]
    env["ACLOCAL_PATH"] = "/home/linuxbrew/.linuxbrew/share/aclocal:" + str(SHARED / "share/aclocal")
    if not (src / "configure").exists() or name == "libxcb-errors":
        run(["autoreconf", "-fi"], cwd=src, env=env)
    build = BUILD / (("host-" if host else "android-") + name)
    build.mkdir(exist_ok=True)
    prefix = HOST if host else PREFIX
    run([src / "configure", f"--prefix={prefix}", *( [] if host else ["--host=aarch64-linux-android"]), *options], cwd=build, env=env)
    makevars = [f"FILE_COMPILE={HOST}/bin/file"] if name == "file" and not host else []
    run(["make", "-j", JOBS, *makevars], cwd=build, env=env)
    run(["make", "install", *makevars], cwd=build, env=env)


def build(name):
    if name == "scanner":
        cmake("pugixml", ["-DBUILD_SHARED_LIBS=OFF"], host=True)
        cmake("hyprwayland-scanner", host=True)
    elif name in ("hyprutils", "aquamarine", "hyprcursor", "hyprgraphics"):
        cmake(name)
    elif name == "hyprlang":
        cmake(name, target="hyprlang")
    elif name == "glslang":
        cmake(name, ["-DENABLE_OPT=OFF", "-DENABLE_HLSL=OFF", "-DBUILD_EXTERNAL=OFF", "-DENABLE_GLSLANG_BINARIES=OFF", "-DGLSLANG_TESTS=OFF"])
    elif name == "muparser":
        cmake(name, ["-DENABLE_OPENMP=OFF", "-DENABLE_SAMPLES=OFF", "-DENABLE_TESTS=OFF"])
    elif name == "libzip":
        cmake(name, ["-DENABLE_COMMONCRYPTO=OFF", "-DENABLE_GNUTLS=OFF", "-DENABLE_MBEDTLS=OFF", "-DENABLE_OPENSSL=OFF", "-DENABLE_WINDOWS_CRYPTO=OFF", "-DENABLE_BZIP2=OFF", "-DENABLE_LZMA=OFF", "-DENABLE_ZSTD=OFF", "-DBUILD_TOOLS=OFF", "-DBUILD_REGRESS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_DOC=OFF"])
    elif name == "tomlplusplus":
        cmake(name, ["-DBUILD_EXAMPLES=OFF"])
        (PREFIX / "lib/pkgconfig/tomlplusplus.pc").write_text(f"prefix={PREFIX}\nName: tomlplusplus\nDescription: Header-only TOML parser\nVersion: 3.4.0\nCflags: -I${{prefix}}/include\n")
    elif name == "libjpeg-turbo":
        cmake(name, ["-DENABLE_STATIC=OFF", "-DWITH_TURBOJPEG=OFF", "-DWITH_TOOLS=OFF", "-DWITH_TESTS=OFF"])
    elif name == "libwebp":
        cmake(name, ["-DBUILD_SHARED_LIBS=ON", "-DWEBP_BUILD_ANIM_UTILS=OFF", "-DWEBP_BUILD_CWEBP=OFF", "-DWEBP_BUILD_DWEBP=OFF", "-DWEBP_BUILD_GIF2WEBP=OFF", "-DWEBP_BUILD_IMG2WEBP=OFF", "-DWEBP_BUILD_VWEBP=OFF", "-DWEBP_BUILD_WEBPINFO=OFF", "-DWEBP_BUILD_WEBPMUX=OFF", "-DWEBP_BUILD_EXTRAS=OFF"])
    elif name == "re2":
        cmake(name, ["-DBUILD_SHARED_LIBS=ON", "-DRE2_BUILD_TESTING=OFF"])
        (PREFIX / "lib/pkgconfig/re2.pc").write_text(f"prefix={PREFIX}\nName: re2\nDescription: Regular expression library\nVersion: 2022.06.01\nLibs: -L${{prefix}}/lib -lre2\nCflags: -I${{prefix}}/include\n")
    elif name == "libxkbcommon":
        meson(name, ["-Denable-x11=false", "-Denable-wayland=false", "-Denable-tools=false", "-Denable-docs=false", "-Denable-xkbregistry=false"])
    elif name == "lunasvg":
        run(["git", "-C", source(name), "submodule", "update", "--init", "--depth=1"])
        cmake(name, ["-DBUILD_SHARED_LIBS=ON", "-DLUNASVG_BUILD_EXAMPLES=OFF"])
    elif name == "lcms":
        meson(name, ["-Dutils=false", "-Dtests=disabled", "-Dversionedlibs=false"])
    elif name == "wayland-protocols":
        meson(name, ["-Dtests=false"])
    elif name == "libeis":
        meson(name, ["-Dtests=disabled", "-Dliboeffis=disabled", "-Dlibei=disabled", "-Dlibeis=enabled"])
    elif name in ("libxcursor", "libxcb-errors", "libxrender", "libxfixes"):
        if name == "libxcb-errors":
            run(["git", "-C", source(name), "submodule", "update", "--init", "--depth=1"])
        autotools(name, ["--disable-static", "--disable-malloc0returnsnull"])
    elif name == "lua":
        cmake(name, [f"-DLUA_SOURCE_DIR={source(name)}"], src_override=ROOT / "android/deps/lua")
    elif name == "file":
        options = ["--disable-static", "--disable-bzlib", "--disable-xzlib", "--disable-zstdlib", "--disable-lzlib", "--disable-lrziplib", "--disable-seccomp"]
        autotools(name, options, host=True)
        autotools(name, [*options, f"FILE_COMPILE={HOST}/bin/file"])
    else:
        raise SystemExit(f"Unknown dependency: {name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*", default=["scanner", "hyprutils", "hyprlang", "glslang", "muparser"])
    args = parser.parse_args()
    for name in args.names:
        build(name)
