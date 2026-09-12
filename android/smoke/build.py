#!/usr/bin/env python3
"""Package the built compositor and its NDK dependencies into a device smoke APK."""
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
ARLINUX = Path(os.environ.get('ARLINUX_DIR', REPO.parent / 'arlinux'))
SDK = Path(os.environ.get('ANDROID_HOME', Path.home() / 'Android/Sdk'))
NDK = Path(os.environ.get('ANDROID_NDK_HOME', SDK / 'ndk/29.0.14206865'))
TOOLS = NDK / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
JNI = HERE / 'app/src/main/jniLibs/arm64-v8a'
ASSETS = HERE / 'app/src/main/assets/runtime'
JNI.mkdir(parents=True, exist_ok=True)
ASSETS.mkdir(parents=True, exist_ok=True)
PREFIX = REPO / 'build/android-prefix'
SEARCH = [PREFIX / 'lib', ARLINUX / 'build/ndk-prefix/lib',
          NDK / 'toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android']
SYSTEM = {'libandroid.so', 'liblog.so', 'libEGL.so', 'libGLESv2.so', 'libGLESv3.so',
          'libc.so', 'libm.so', 'libdl.so', 'libz.so', 'libvulkan.so', 'libjnigraphics.so',
          'libnativewindow.so'}
seen = set()
def copy_library(source, name=None):
    name = name or source.name
    if name in seen:
        return
    seen.add(name)
    dynamic = subprocess.check_output([TOOLS / 'llvm-readelf', '-d', source], text=True)
    for dependency in re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic):
        if dependency in SYSTEM:
            continue
        candidate = next((directory / dependency for directory in SEARCH if (directory / dependency).exists()), None)
        if candidate is None:
            raise RuntimeError(f'{source}: missing {dependency}')
        copy_library(candidate, dependency)
    shutil.copy2(source, JNI / name)
    subprocess.run([TOOLS / 'llvm-strip', '--strip-unneeded', JNI / name], check=True)

copy_library(REPO / 'build/android-hyprland/libanhyprland.so')
copy_library(ARLINUX / 'build/ndk-prefix/bin/Xwayland', 'libxwayland.so')
subprocess.run([TOOLS / 'aarch64-linux-android28-clang++', '-std=c++23', '-shared', '-fPIC',
                '-I' + str(REPO / 'src/android'), HERE / 'app/src/main/cpp/smoke.cpp',
                '-L' + str(JNI), '-lanhyprland', '-landroid', '-llog', '-o', JNI / 'libsmoke.so'], check=True)
generated = HERE / 'app/build/smoke-native'
generated.mkdir(parents=True, exist_ok=True)
protocol = PREFIX / 'share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'
for mode, filename in [('client-header', 'xdg-shell-client-protocol.h'), ('private-code', 'xdg-shell-protocol.c')]:
    subprocess.run(['wayland-scanner', mode, protocol, generated / filename], check=True)
for mode, filename in [('client-header', 'wayland-android-client-protocol.h'), ('private-code', 'wayland-android-protocol.c')]:
    subprocess.run(['wayland-scanner', mode, REPO / 'protocols/wayland-android.xml', generated / filename], check=True)
subprocess.run([TOOLS / 'aarch64-linux-android28-clang', '-I' + str(generated),
                '-I' + str(ARLINUX / 'build/ndk-prefix/include'),
                HERE / 'app/src/main/cpp/shm-client.c', HERE / 'app/src/main/cpp/ahb-client.c', generated / 'xdg-shell-protocol.c', generated / 'wayland-android-protocol.c',
                '-L' + str(ARLINUX / 'build/ndk-prefix/lib'), '-lwayland-client', '-landroid', '-lEGL', '-lGLESv3', '-ldl',
                '-o', JNI / 'libshm-smoke.so'], check=True)
copy_library(ARLINUX / 'build/ndk-prefix/lib/libwayland-client.so')
subprocess.run([TOOLS / 'aarch64-linux-android28-clang', '-I' + str(ARLINUX / 'build/ndk-prefix/include'),
                HERE / 'app/src/main/cpp/x11-client.c', '-L' + str(ARLINUX / 'build/ndk-prefix/lib'), '-lxcb',
                '-o', JNI / 'libx11-smoke.so'], check=True)
shutil.copytree('/usr/share/X11/xkb', ASSETS / 'xkb', dirs_exist_ok=True)
(ASSETS / 'xkb/compiled').mkdir(exist_ok=True)
subprocess.run(['xkbcomp', '-w', '0', '-I/usr/share/X11/xkb', '-xkm', '-', str(ASSETS / 'xkb/compiled/arlinux-default.xkm')], input='xkb_keymap { xkb_keycodes { include "evdev+aliases(qwerty)" }; xkb_types { include "complete" }; xkb_compat { include "complete" }; xkb_symbols { include "pc+us+inet(evdev)" }; };', text=True, check=True)
shutil.copy2(PREFIX / 'share/misc/magic.mgc', ASSETS / 'magic.mgc')
(ASSETS / 'fonts.conf').write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd"><fontconfig><dir>/system/fonts</dir><cachedir prefix="xdg">fontconfig</cachedir></fontconfig>\n')
(ASSETS / 'hyprland.lua').write_text('hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1 })\nhl.config({ misc = { disable_hyprland_logo = true, disable_splash_rendering = true }, debug = { disable_logs = false } })\n')
(HERE / 'local.properties').write_text(f'sdk.dir={SDK}\n')
subprocess.run([ARLINUX / 'android/gradlew', '-p', HERE, ':app:assembleDebug', '--console=plain'], check=True)
