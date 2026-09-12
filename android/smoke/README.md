# Android compositor smoke host

This standalone APK runs the complete anhyprland compositor in a SurfaceView. Its package is `io.taowen.anhyprland.smoke`; it is independent of the arlinux application and anlabwc. anhyprland launches the packaged bionic Xwayland itself and supplies the XWM.

## Build and install

Build the dependencies and core as described in [../README.md](../README.md), then:

```sh
python3 android/smoke/build.py
```

The script uses the sibling arlinux checkout's Gradle wrapper and NDK dependency prefix. `ARLINUX_DIR`, `ANDROID_HOME` and `ANDROID_NDK_HOME` override those locations. Host `wayland-scanner` and `xkbcomp` must be on PATH, with XKB data in `/usr/share/X11/xkb`. The APK is `android/smoke/app/build/outputs/apk/debug/app-debug.apk`. Build artifacts, copied native libraries and generated assets are ignored by Git. The original unstripped compositor remains in `build/android-hyprland` for symbolizing crashes.

For the vivo X300, use arctrl's device installer helper, which handles its package installer confirmation screen:

```sh
../arlinux/third_party/arctrl/scripts/vivo_x300_install_apk.sh android/smoke/app/build/outputs/apk/debug/app-debug.apk
adb -s 10AFA31610002QH shell am start -n io.taowen.anhyprland.smoke/.MainActivity
```

For other devices, install with their normal APK workflow. Set `ADB_SERIAL` when invoking the clients below. The default is the X300.

## Clients and lifecycle checks

Run each command in a separate terminal; the client stays connected until the compositor stops:

```sh
python3 android/smoke/run-client.py
python3 android/smoke/run-client.py --ahb-v1
python3 android/smoke/run-client.py --ahb-v2
python3 android/smoke/run-client.py --x11
```

The first client draws a 640×480 SHM checkerboard. The AHB clients draw four colored rectangles directly with GLES, using client allocation (android_wlegl v1) or server allocation (v2). They do not use CPU readback. The X11 client paints orange rectangles on a blue window through Xwayland. These test clients deliberately keep their fixed buffer size when tiled; unused space inside a larger Wayland tile is expected.

`run-client.py` executes the packaged native client under the debug application's UID. `SMOKE_WAYLAND_DISPLAY` overrides the default `wayland-1`. The X11 probe currently connects to display 0 inside this application's runtime directory.

Touch a window and send a key, for example `adb -s SERIAL shell input keyevent KEYCODE_A`. Wayland logs should show button 272 and key 30; X11 logs show button 1 and key 38. Press Home, then reopen the Activity: its Surface is destroyed and recreated, while the compositor PID and clients remain. Back requests a clean stop. Logcat should show `Compositor returned 0`, the host process should remain alive and its Xwayland child should disappear.

Only one compositor lifetime per Android process is supported. After stopping, use `adb -s SERIAL shell am force-stop io.taowen.anhyprland.smoke` before launching a new session. Surface replacement itself does not require a process restart.

Compositor logs use the `anhyprland` logcat tag; lifecycle logs use `anhyprland-smoke`. Xwayland stderr is available with `adb -s SERIAL shell run-as io.taowen.anhyprland.smoke cat files/native.log`.

## Device results, 2026-09-12

| Check | vivo X300, Android 16, Mali-G1-Ultra MC12 | Redmi K40, Android 13, Adreno 650 |
| --- | --- | --- |
| Compositor startup and visible SHM checkerboard | Pass | Pass |
| GLES-produced android_wlegl v1 and v2 buffers | Pass | Pass |
| Multiple tiled Wayland windows | Pass | Pass |
| Xwayland window drawing and XWM integration | Pass | Pass |
| Pointer and keyboard delivery to Wayland/X11 clients | Pass | Pass |
| Surface destruction/recreation preserves clients | Pass | Pass |
| Clean stop returns 0 and reaps Xwayland | Pass | Pass |

These are native bionic smoke clients. They validate the compositor's GLES/AHB boundary on both GPU families, not the full glibc → Zink → Turnip/libhybris client stack. Omarchy, the arlinux host integration, Android application windows, IME/clipboard integration, repeated buffer reuse under load, and long-running desktop workloads still need separate end-to-end acceptance. The smoke host is not a desktop distribution.
