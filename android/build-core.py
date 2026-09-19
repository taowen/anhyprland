#!/usr/bin/env python3
"""Configure and compile the Android compositor core (embedding is in development)."""
import argparse
import importlib.util
import os
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--target", default="Hyprland", choices=["Hyprland", "hyprland_lib"])
args = parser.parse_args()

spec = importlib.util.spec_from_file_location("deps", Path(__file__).with_name("build-deps.py"))
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)
env = deps.cross_env()
env["PATH"] = str(deps.HOST / "bin") + os.pathsep + env["PATH"]
ninja = deps.native_ninja()
protocol = Path(os.environ.get("WAYLAND_CORE_PROTOCOL_DIR", deps.ARLINUX / "build/ndk-src/wayland-1.25.0/protocol"))
if not (protocol / "wayland.xml").is_file():
    raise SystemExit("Set WAYLAND_CORE_PROTOCOL_DIR to the matching Wayland source protocol directory")
build = deps.BUILD / "android-hyprland"
deps.run(["cmake", "-S", deps.ROOT, "-B", build, "-G", "Ninja",
          f"-DCMAKE_MAKE_PROGRAM={ninja}",
          f"-DCMAKE_TOOLCHAIN_FILE={deps.NDK}/build/cmake/android.toolchain.cmake",
          "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_shared",
          "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
          "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=OFF", "-DBUILD_TESTING=OFF",
          f"-DCMAKE_INSTALL_PREFIX={deps.PREFIX}", "-DCMAKE_INSTALL_LIBDIR=lib",
          f"-DCMAKE_PREFIX_PATH={deps.PREFIX};{deps.SHARED}",
          f"-DCMAKE_FIND_ROOT_PATH={deps.PREFIX};{deps.SHARED}",
          f"-Dhyprwayland-scanner_DIR={deps.HOST}/lib/cmake/hyprwayland-scanner",
          f"-DWAYLAND_CORE_PROTOCOL_DIR={protocol}",
          f"-DOPENGL_INCLUDE_DIR={deps.NDK_TOOLBIN.parent}/sysroot/usr/include",
          f"-DCMAKE_SHARED_LINKER_FLAGS=-L{deps.PREFIX}/lib -L{deps.SHARED}/lib",
          f"-DCMAKE_EXE_LINKER_FLAGS=-L{deps.PREFIX}/lib -L{deps.SHARED}/lib"], env=env)
deps.run(["cmake", "--build", build, "--target", args.target, "-j", deps.JOBS, "--", "-k", "0"], env=env)
