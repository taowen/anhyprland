#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <cstdlib>
#include <cstdio>
#include <string>
#include "Embed.h"

static void ready(void*) {
    __android_log_print(ANDROID_LOG_INFO, "anhyprland-smoke", "READY WAYLAND_DISPLAY=%s DISPLAY=%s", getenv("WAYLAND_DISPLAY"), getenv("DISPLAY"));
}
extern "C" JNIEXPORT jint JNICALL Java_io_taowen_anhyprland_smoke_MainActivity_run(JNIEnv* env, jobject, jobject surface, jint width, jint height, jstring home,
                                                                                   jstring libraries) {
    const char*       homeChars = env->GetStringUTFChars(home, nullptr);
    const std::string root(homeChars);
    env->ReleaseStringUTFChars(home, homeChars);
    const char*       libChars = env->GetStringUTFChars(libraries, nullptr);
    const std::string libs(libChars);
    env->ReleaseStringUTFChars(libraries, libChars);
    freopen((root + "/native.log").c_str(), "w", stderr);
    setvbuf(stderr, nullptr, _IONBF, 0);
    setenv("HOME", root.c_str(), 1);
    setenv("XDG_RUNTIME_DIR", (root + "/runtime").c_str(), 1);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("XKB_CONFIG_ROOT", (root + "/xkb").c_str(), 1);
    setenv("ARLINUX_XWAYLAND", (libs + "/libxwayland.so").c_str(), 1);
    setenv("ARLINUX_XKM", (root + "/xkb/compiled/arlinux-default.xkm").c_str(), 1);
    setenv("LD_LIBRARY_PATH", libs.c_str(), 1);
    setenv("MAGIC", (root + "/magic.mgc").c_str(), 1);
    setenv("FONTCONFIG_FILE", (root + "/fonts.conf").c_str(), 1);
    setenv("TMPDIR", (root + "/runtime").c_str(), 1);
    auto* window = ANativeWindow_fromSurface(env, surface);
    if (!window)
        return -1;
    const int result = anhyprland_run(window, width, height, (root + "/hyprland.lua").c_str(), ready, nullptr);
    ANativeWindow_release(window);
    return result;
}
extern "C" JNIEXPORT jint JNICALL Java_io_taowen_anhyprland_smoke_MainActivity_window(JNIEnv* env, jobject, jobject surface, jint width, jint height) {
    auto*     window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    const int result = anhyprland_window(window, width, height);
    if (window)
        ANativeWindow_release(window);
    return result;
}
extern "C" JNIEXPORT jint JNICALL Java_io_taowen_anhyprland_smoke_MainActivity_pointer(JNIEnv*, jobject, jfloat x, jfloat y, jint button, jboolean pressed) {
    return anhyprland_pointer(x, y, button, pressed);
}
extern "C" JNIEXPORT jint JNICALL Java_io_taowen_anhyprland_smoke_MainActivity_key(JNIEnv*, jobject, jint code, jboolean pressed) {
    return anhyprland_key(code, pressed);
}
extern "C" JNIEXPORT jint JNICALL Java_io_taowen_anhyprland_smoke_MainActivity_stop(JNIEnv*, jobject) {
    return anhyprland_stop();
}
