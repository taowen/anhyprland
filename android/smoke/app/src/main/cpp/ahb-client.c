// GPU-only producer for android_wlegl v1 and v2 smoke tests.
#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wayland-android-client-protocol.h"

struct native_handle {
    int version, num_fds, num_ints;
    int data[];
};
static AHardwareBuffer*  allocation;
static struct wl_buffer* result_buffer;
static int               fds[64], fd_count;
static int32_t           ints[1024];
static size_t            int_count;
static int               failure;

static int               paint(AHardwareBuffer* buffer) {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(display, NULL, NULL))
        return -1;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLConfig    config;
    EGLint       count;
    if (!eglChooseConfig(display, attrs, &config, 1, &count) || !count)
        return -1;
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext   context         = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    const EGLint surface_attrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface   surface         = eglCreatePbufferSurface(display, config, surface_attrs);
    if (!eglMakeCurrent(display, surface, surface, context))
        return -1;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_client    = (void*)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    PFNEGLCREATEIMAGEKHRPROC               create_image  = (void*)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC              destroy_image = (void*)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC    bind_image    = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!get_client || !create_image || !destroy_image || !bind_image)
        return -1;
    const EGLint image_attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR  image         = create_image(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, get_client(buffer), image_attrs);
    if (image == EGL_NO_IMAGE_KHR)
        return -1;
    GLuint texture, framebuffer;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    bind_image(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return -1;
    const float colors[4][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}};
    glEnable(GL_SCISSOR_TEST);
    for (int i = 0; i < 4; ++i) {
        glScissor((i % 2) * 320, (i / 2) * 240, 320, 240);
        glClearColor(colors[i][0], colors[i][1], colors[i][2], 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glFinish();
    int ok = glGetError() == GL_NO_ERROR;
    fprintf(stderr, "AHB GLES producer: %s; GL=%s renderer=%s\n", ok ? "painted" : "failed", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    destroy_image(display, image);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    eglTerminate(display);
    return ok ? 0 : -1;
}

static void received_fd(void* data, struct android_wlegl_server_buffer_handle* handle, int32_t fd) {
    if (fd_count >= 64) {
        close(fd);
        failure = 1;
        return;
    }
    fds[fd_count++] = fd;
}
static void received_ints(void* data, struct android_wlegl_server_buffer_handle* handle, struct wl_array* array) {
    if (array->size > sizeof(ints) || array->size % 4) {
        failure = 1;
        return;
    }
    int_count = array->size / 4;
    memcpy(ints, array->data, array->size);
}
static void received_buffer(void* data, struct android_wlegl_server_buffer_handle* handle, struct wl_buffer* buffer, int32_t format, int32_t stride) {
    void* library                                                                                   = dlopen("libnativewindow.so", RTLD_NOW);
    int (*import)(const AHardwareBuffer_Desc*, const struct native_handle*, int, AHardwareBuffer**) = library ? dlsym(library, "AHardwareBuffer_createFromHandle") : NULL;
    struct native_handle* native                                                                    = calloc(1, sizeof(*native) + (fd_count + int_count) * sizeof(int));
    if (!native || !import || failure) {
        failure = 1;
        goto out;
    }
    native->version  = sizeof(*native);
    native->num_fds  = fd_count;
    native->num_ints = int_count;
    memcpy(native->data, fds, fd_count * sizeof(int));
    memcpy(native->data + fd_count, ints, int_count * sizeof(int));
    AHardwareBuffer_Desc desc = {
        .width = 640, .height = 480, .layers = 1, .format = format, .stride = stride, .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE};
    if (import(&desc, native, 3 /* CLONE */, &allocation) || paint(allocation))
        failure = 1;
out:
    for (int i = 0; i < fd_count; ++i)
        close(fds[i]);
    free(native);
    if (library)
        dlclose(library);
    result_buffer = buffer;
    android_wlegl_server_buffer_handle_destroy(handle);
}
static const struct android_wlegl_server_buffer_handle_listener listener = {received_fd, received_ints, received_buffer};

struct wl_buffer*                                               make_ahb_buffer(struct wl_display* display, struct android_wlegl* android, int version) {
    if (!android)
        return NULL;
    const int usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (version == 2) {
        struct android_wlegl_server_buffer_handle* handle = android_wlegl_get_server_buffer_handle(android, 640, 480, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, usage);
        android_wlegl_server_buffer_handle_add_listener(handle, &listener, NULL);
        while (!result_buffer && !failure)
            if (wl_display_dispatch(display) < 0)
                return NULL;
        return failure ? NULL : result_buffer;
    }
    AHardwareBuffer_Desc desc = {.width = 640, .height = 480, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, .usage = usage};
    if (AHardwareBuffer_allocate(&desc, &allocation) || paint(allocation))
        return NULL;
    AHardwareBuffer_describe(allocation, &desc);
    void* library                                                     = dlopen("libnativewindow.so", RTLD_NOW);
    const struct native_handle* (*get_handle)(const AHardwareBuffer*) = library ? dlsym(library, "AHardwareBuffer_getNativeHandle") : NULL;
    if (!get_handle)
        return NULL;
    const struct native_handle*  native = get_handle(allocation);
    struct wl_array              array  = {.size = native->num_ints * sizeof(int), .data = (void*)(native->data + native->num_fds)};
    struct android_wlegl_handle* handle = android_wlegl_create_handle(android, native->num_fds, &array);
    for (int i = 0; i < native->num_fds; ++i)
        android_wlegl_handle_add_fd(handle, native->data[i]);
    result_buffer = android_wlegl_create_buffer(android, desc.width, desc.height, desc.stride, desc.format, usage, handle);
    android_wlegl_handle_destroy(handle);
    dlclose(library);
    return result_buffer;
}
