#!/usr/bin/env python3
"""Build pinned Hyprland dependencies with the NDK and arlinux's native prefix."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
PREFIX = BUILD / "android-prefix"
HOST = BUILD / "host-prefix"
ARLINUX = Path(os.environ.get("ARLINUX_DIR", ROOT.parent / "arlinux")).resolve()
SHARED = ARLINUX / "build/ndk-prefix"
NDK = Path(os.environ.get("ANDROID_NDK_HOME", Path.home() / "Android/Sdk/ndk/29.0.14206865"))
NDK_HOST = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
NDK_TOOLBIN = NDK / "toolchains/llvm/prebuilt" / NDK_HOST / "bin"
LOCK = json.loads((ROOT / "android/deps.lock.json").read_text())
JOBS = os.environ.get("JOBS", str(min(os.cpu_count() or 2, 8)))


def native_ninja():
    configured = os.environ.get("CMAKE_MAKE_PROGRAM")
    if configured and Path(configured).is_file():
        return configured
    if os.name == "nt":
        sdk = Path(os.environ.get("ANDROID_HOME", os.environ.get("ANDROID_SDK_ROOT", "")))
        bundled = sorted(sdk.glob("cmake/*/bin/ninja.exe"), reverse=True) if sdk else []
        found = next((path for path in bundled if path.is_file()), None)
        if found:
            return str(found)
    found = shutil.which("ninja")
    if not found:
        raise SystemExit("Ninja was not found on PATH")
    return found


def add_windows_runtime_path(env):
    if os.name == "nt":
        mingw = Path(os.environ.get("MSYS2_ROOT", "C:/tools/msys64")) / "ucrt64/bin"
        env["PATH"] = str(mingw) + os.pathsep + env["PATH"]
    return env


def pkg_config_executable():
    """Return a pkg-config whose path syntax matches the native build host."""
    if os.name != "nt":
        return "pkg-config"
    candidate = Path(os.environ.get("MSYS2_ROOT", "C:/tools/msys64")) / "ucrt64/bin/pkg-config.exe"
    if not candidate.is_file():
        raise SystemExit(
            f"Missing native Windows pkg-config at {candidate}; install "
            "mingw-w64-ucrt-x86_64-pkgconf with MSYS2 pacman"
        )
    return str(candidate)


def run(args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def msys_path(path):
    value = Path(path).resolve().as_posix()
    if os.name == "nt" and len(value) >= 3 and value[1:3] == ":/":
        return f"/{value[0].lower()}{value[2:]}"
    return value


def run_posix(args, *, cwd, env):
    """Run a POSIX build-system command, using MSYS2 Bash on Windows."""
    if os.name != "nt":
        run(args, cwd=cwd, env=env)
        return
    bash = Path(os.environ.get("MSYS2_ROOT", "C:/tools/msys64")) / "usr/bin/bash.exe"
    command = " ".join(shlex.quote(msys_path(arg) if isinstance(arg, Path) else str(arg)) for arg in args)
    script = f"cd {shlex.quote(msys_path(cwd))} && {command}"
    run([bash, "-lc", script], env=env)


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
    if os.name == "nt":
        env["CMAKE_MAKE_PROGRAM"] = native_ninja()
        env["PKG_CONFIG"] = pkg_config_executable()
        add_windows_runtime_path(env)
        if host:
            mingw = Path(os.environ.get("MSYS2_ROOT", "C:/tools/msys64")) / "ucrt64/bin"
            env["CC"] = str(mingw / "gcc.exe")
            env["CXX"] = str(mingw / "g++.exe")
    paths = [HOST / "lib/pkgconfig"] if host else [PREFIX / "lib/pkgconfig", PREFIX / "share/pkgconfig", SHARED / "lib/pkgconfig"]
    env["PKG_CONFIG_PATH"] = os.pathsep.join(map(str, paths))
    if not host:
        env["PKG_CONFIG_LIBDIR"] = env["PKG_CONFIG_PATH"]
    flags = [f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DBUILD_TESTING=OFF"]
    if os.name == "nt":
        flags.append(f"-DCMAKE_MAKE_PROGRAM={env['CMAKE_MAKE_PROGRAM']}")
        flags.append(f"-DPKG_CONFIG_EXECUTABLE={env['PKG_CONFIG']}")
    if not host:
        flags += [f"-DCMAKE_TOOLCHAIN_FILE={NDK}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
                  "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_shared", f"-DCMAKE_PREFIX_PATH={PREFIX};{SHARED}",
                  f"-DCMAKE_FIND_ROOT_PATH={PREFIX};{SHARED}",
                  f"-DOPENGL_INCLUDE_DIR={NDK_TOOLBIN.parent}/sysroot/usr/include",
                  f"-DCMAKE_SHARED_LINKER_FLAGS=-L{PREFIX}/lib -L{SHARED}/lib",
                  f"-DCMAKE_EXE_LINKER_FLAGS=-L{PREFIX}/lib -L{SHARED}/lib"]
    else:
        flags += [f"-DCMAKE_PREFIX_PATH={HOST}"]
    run(["cmake", "-S", src, "-B", build, "-G", "Ninja", *flags, *options], env=env)
    run(["cmake", "--build", build, "-j", JOBS, *( ["--target", target] if target else [])], env=env)
    run(["cmake", "--install", build], env=env)


def cross_env():
    env = os.environ.copy()
    suffix = ".cmd" if os.name == "nt" else ""
    exe = ".exe" if os.name == "nt" else ""
    hp = (lambda path: path.as_posix()) if os.name == "nt" else str
    pkg_paths = os.pathsep.join(map(hp, [PREFIX / "lib/pkgconfig", PREFIX / "share/pkgconfig", SHARED / "lib/pkgconfig"]))
    env.update(CC=hp(NDK_TOOLBIN / ("aarch64-linux-android28-clang" + suffix)),
               CXX=hp(NDK_TOOLBIN / ("aarch64-linux-android28-clang++" + suffix)), AR=hp(NDK_TOOLBIN / ("llvm-ar" + exe)),
               RANLIB=hp(NDK_TOOLBIN / ("llvm-ranlib" + exe)), STRIP=hp(NDK_TOOLBIN / ("llvm-strip" + exe)),
               PKG_CONFIG_PATH=pkg_paths,
               PKG_CONFIG_LIBDIR=pkg_paths,
               CPPFLAGS=f"-I{hp(PREFIX)}/include -I{hp(SHARED)}/include",
               CFLAGS="-O2 -fPIC", CXXFLAGS="-O2 -fPIC",
               LDFLAGS=f"-L{hp(PREFIX)}/lib -L{hp(SHARED)}/lib")
    if os.name == "nt":
        env["PKG_CONFIG"] = pkg_config_executable()
        add_windows_runtime_path(env)
    return env


def meson(name, options=()):
    src = source(name)
    build = BUILD / ("android-" + name)
    cross = BUILD / "android-cross.ini"
    env = cross_env()
    def mp(value):
        return str(value).replace("\\", "/") if os.name == "nt" else str(value)

    cross.write_text("[binaries]\n" + "\n".join(f"{k} = {mp(env[v])!r}" for k, v in [("c", "CC"), ("cpp", "CXX"), ("ar", "AR"), ("strip", "STRIP")]) +
                     f"\npkg-config = {mp(env.get('PKG_CONFIG', 'pkg-config'))!r}\n[host_machine]\nsystem = 'android'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n" +
                     f"[built-in options]\nc_args = ['-fPIC', '-I{mp(PREFIX)}/include', '-I{mp(SHARED)}/include']\n" +
                     f"c_link_args = ['-L{mp(PREFIX)}/lib', '-L{mp(SHARED)}/lib']\npkg_config_path = '{mp(env['PKG_CONFIG_PATH'])}'\n" +
                     f"[properties]\nneeds_exe_wrapper = true\npkg_config_libdir = '{mp(env['PKG_CONFIG_LIBDIR'])}'\n")
    # Meson validates --prefix using the target OS. Android therefore needs a
    # POSIX prefix even though the install staging directory is on Windows.
    meson_prefix = "/" if os.name == "nt" else str(PREFIX)
    run(["meson", "setup", *( ["--reconfigure"] if (build / "meson-info").exists() else []), build, src,
         "--cross-file", cross, "--prefix", meson_prefix, "--libdir=lib", "--buildtype=release", *options], env=env)
    run(["meson", "compile", "-C", build, "-j", JOBS], env=env)
    install = ["meson", "install", "-C", build]
    if os.name == "nt":
        install += ["--destdir", PREFIX]
    run(install, env=env)
    if os.name == "nt":
        # pkgconf is later invoked by native Windows CMake, so generated
        # absolute target prefixes must use native host syntax.
        for pc in PREFIX.rglob("*.pc"):
            contents = pc.read_text()
            if contents.startswith("prefix=/\n"):
                pc.write_text(f"prefix={PREFIX.as_posix()}\n" + contents[len("prefix=/\n"):])


def autotools(name, options=(), host=False):
    src = source(name)
    env = os.environ.copy() if host else cross_env()
    if os.name != "nt":
        env["PATH"] = "/home/linuxbrew/.linuxbrew/bin:" + env["PATH"]
        env["ACLOCAL_PATH"] = "/home/linuxbrew/.linuxbrew/share/aclocal:" + str(SHARED / "share/aclocal")
    else:
        env["ACLOCAL_PATH"] = msys_path(SHARED / "share/aclocal")
    if not (src / "configure").exists() or name == "libxcb-errors":
        run_posix(["autoreconf", "-fi"], cwd=src, env=env)
    build = BUILD / (("host-" if host else "android-") + name)
    build.mkdir(exist_ok=True)
    prefix = HOST if host else PREFIX
    prefix_arg = msys_path(prefix) if os.name == "nt" else str(prefix)
    run_posix([src / "configure", f"--prefix={prefix_arg}", *( [] if host else ["--host=aarch64-linux-android"]), *options], cwd=build, env=env)
    file_compile = msys_path(HOST / "bin/file") if os.name == "nt" else str(HOST / "bin/file")
    makevars = [f"FILE_COMPILE={file_compile}"] if name == "file" and not host else []
    run_posix(["make", "-j", JOBS, *makevars], cwd=build, env=env)
    run_posix(["make", "install", *makevars], cwd=build, env=env)


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
        lua_source = source(name)
        if os.name == "nt":
            lua_source = lua_source.as_posix()
        cmake(name, [f"-DLUA_SOURCE_DIR={lua_source}"], src_override=ROOT / "android/deps/lua")
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
