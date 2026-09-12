# anhyprland

Android platform port of Hyprland for [arlinux](https://github.com/taowen/arlinux). Development lives on the `android` branch; upstream history is preserved. The starting Hyprland revision is `f05d73f3` (0.56.0). Aquamarine `61ddaca` (0.15.0) is vendored in `subprojects/aquamarine` so backend and renderer changes can be reviewed together.

## Graphics boundary

Linux applications retain arlinux's glibc runtime and the existing OpenGL → Zink → Vulkan → Turnip/libhybris rendering path. The compositor uses Android's native GLES to import the resulting AHardwareBuffers and present the desktop to the Activity Surface. This is the same division of responsibility used by anlabwc. The port does not replace the client Vulkan stack with desktop DRM/GBM.

## Current status

The Aquamarine Android backend builds with NDK 29 for arm64-v8a, API 28. It provides a single Surface output, AHardwareBuffer allocation, keyboard/pointer injection, frame scheduling, and a compositor-thread Surface attachment interface. Desktop DRM/seat backends are excluded from this Android build. AHB buffers do not advertise fabricated DMA-BUF descriptors.

The full Hyprland renderer, embedding lifecycle and `android_wlegl` integration are under development. A successful backend build is not yet a working Hyprland desktop. Device acceptance remains outstanding.

## Build prerequisites and dependency recipes

Use the shared NDK dependency prefix from a built arlinux checkout. Set `ARLINUX_DIR` if it is not the sibling `arlinux` directory. `ANDROID_NDK_HOME` can override NDK 29.0.14206865. The host needs CMake, Ninja, Meson, pkg-config, a C++23 compiler, Python, Autoconf, Automake and Libtool. `JOBS` defaults to two to limit memory use.

```sh
python3 android/build-deps.py scanner hyprutils hyprlang aquamarine
```

Native outputs are installed into `build/android-prefix`; host generators go into `build/host-prefix`. Sources are pinned in `deps.lock.json`. Small Android adaptations are kept in `patches/` and applied to the pinned build checkouts. Build outputs and fetched checkouts remain outside version control.

Additional recipes cover the shader compiler, Lua, image formats, SVG/cursor libraries, color management and X11/Wayland protocol dependencies. Android SVG support in hyprcursor and hyprgraphics uses LunaSVG's premultiplied ARGB output instead of requiring a Rust librsvg toolchain. Linux builds retain librsvg.

## Acceptance targets

- Start and stop the compositor without changing the Android process's signal handlers or exiting the host process.
- Display and interact with a wl_shm client, then GPU Wayland and Xwayland clients using the existing arlinux graphics stack.
- Preserve clients and resources across Surface destruction/recreation, including Android system file pickers.
- Import `android_wlegl` buffers so `arlinux-app` Android windows can participate in Hyprland tiling, focus and workspaces.
- Verify both the Turnip and libhybris client paths on devices before calling the port usable.
