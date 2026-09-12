#ifdef __ANDROID__
#include "OpenGL.hpp"
#include "../Compositor.hpp"
#include <aquamarine/backend/Android.hpp>
#include <android/hardware_buffer.h>
#include <android/native_window.h>

using namespace Render::GL;

static SP<Aquamarine::CAndroidBackend> androidBackend() {
    for (const auto& backend : g_pCompositor->m_aqBackend->getImplementations()) {
        if (backend->type() == Aquamarine::AQ_BACKEND_ANDROID)
            return Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAndroidBackend>(backend);
    }
    return nullptr;
}

void CHyprOpenGLImpl::initAndroidEGL() {
    m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    RASSERT(m_eglDisplay != EGL_NO_DISPLAY && eglInitialize(m_eglDisplay, nullptr, nullptr), "Could not initialize Android EGL");
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLint count = 0;
    RASSERT(eglChooseConfig(m_eglDisplay, attributes, &m_androidConfig, 1, &count) && count == 1, "No Android GLES3 EGL configuration");
    const EGLint context32[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    m_eglContext             = eglCreateContext(m_eglDisplay, m_androidConfig, EGL_NO_CONTEXT, context32);
    if (m_eglContext == EGL_NO_CONTEXT) {
        const EGLint context30[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_eglContext             = eglCreateContext(m_eglDisplay, m_androidConfig, EGL_NO_CONTEXT, context30);
        m_eglContextVersion      = EGL_CONTEXT_GLES_3_0;
    }
    RASSERT(m_eglContext != EGL_NO_CONTEXT, "Could not create Android GLES3 context");
    const EGLint idle[]  = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    m_androidIdleSurface = eglCreatePbufferSurface(m_eglDisplay, m_androidConfig, idle);
    RASSERT(m_androidIdleSurface != EGL_NO_SURFACE, "Could not create Android idle EGL surface");
    RASSERT(eglMakeCurrent(m_eglDisplay, m_androidIdleSurface, m_androidIdleSurface, m_eglContext), "Could not bind Android EGL context");
    auto backend = androidBackend();
    RASSERT(backend && attachAndroidWindow(backend->window()), "Could not attach Android window");
    backend->attachWindow = [this](ANativeWindow* window) { return attachAndroidWindow(window); };
    backend->present      = [this](SP<Aquamarine::IBuffer> buffer) { return presentAndroidBuffer(buffer); };
    // AHB import does not imply a Linux DRM node or linux-dmabuf support.
    m_exts.EXT_image_dma_buf_import           = false;
    m_exts.EXT_image_dma_buf_import_modifiers = false;
}

bool CHyprOpenGLImpl::attachAndroidWindow(ANativeWindow* window) {
    if (!eglMakeCurrent(m_eglDisplay, m_androidIdleSurface, m_androidIdleSurface, m_eglContext))
        return false;
    if (m_androidWindowSurface != EGL_NO_SURFACE)
        eglDestroySurface(m_eglDisplay, m_androidWindowSurface);
    m_androidWindowSurface = EGL_NO_SURFACE;
    if (!window)
        return true;
    EGLint visual = 0;
    if (!eglGetConfigAttrib(m_eglDisplay, m_androidConfig, EGL_NATIVE_VISUAL_ID, &visual) || ANativeWindow_setBuffersGeometry(window, 0, 0, visual) != 0)
        return false;
    m_androidWindowSurface = eglCreateWindowSurface(m_eglDisplay, m_androidConfig, window, nullptr);
    return m_androidWindowSurface != EGL_NO_SURFACE;
}

EGLImageKHR CHyprOpenGLImpl::createAndroidImage(AHardwareBuffer* buffer) {
    if (!buffer)
        return EGL_NO_IMAGE_KHR;
    const auto getClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    if (!getClientBuffer)
        return EGL_NO_IMAGE_KHR;
    const auto   client       = getClientBuffer(buffer);
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    return m_proc.eglCreateImageKHR(m_eglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attributes);
}

bool CHyprOpenGLImpl::presentAndroidBuffer(SP<Aquamarine::IBuffer> buffer) {
    if (m_androidWindowSurface == EGL_NO_SURFACE || !buffer || !buffer->androidBuffer())
        return false;
    if (!eglMakeCurrent(m_eglDisplay, m_androidWindowSurface, m_androidWindowSurface, m_eglContext))
        return false;
    const auto image = createAndroidImage(buffer->androidBuffer());
    if (image == EGL_NO_IMAGE_KHR) {
        eglMakeCurrent(m_eglDisplay, m_androidIdleSurface, m_androidIdleSurface, m_eglContext);
        return false;
    }
    GLint previousRead = 0, previousDraw = 0, previousRenderbuffer = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
    const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    GLuint renderbuffer = 0, framebuffer = 0;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    m_proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    bool   ok    = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    EGLint width = 0, height = 0;
    ok = ok && eglQuerySurface(m_eglDisplay, m_androidWindowSurface, EGL_WIDTH, &width) && eglQuerySurface(m_eglDisplay, m_androidWindowSurface, EGL_HEIGHT, &height);
    if (ok) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, buffer->size.x, buffer->size.y, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        ok = glGetError() == GL_NO_ERROR;
        // The existing android_wlegl contract has no compositor release fence.
        // Finish sampling before this output buffer or client AHB can be reused.
        glFinish();
        ok = ok && eglSwapBuffers(m_eglDisplay, m_androidWindowSurface);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
    glBindRenderbuffer(GL_RENDERBUFFER, previousRenderbuffer);
    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteRenderbuffers(1, &renderbuffer);
    m_proc.eglDestroyImageKHR(m_eglDisplay, image);
    return eglMakeCurrent(m_eglDisplay, m_androidIdleSurface, m_androidIdleSurface, m_eglContext) && ok;
}

void CHyprOpenGLImpl::destroyAndroidEGL() {
    if (auto backend = androidBackend()) {
        backend->attachWindow = {};
        backend->present      = {};
    }
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_androidWindowSurface != EGL_NO_SURFACE)
        eglDestroySurface(m_eglDisplay, m_androidWindowSurface);
    if (m_androidIdleSurface != EGL_NO_SURFACE)
        eglDestroySurface(m_eglDisplay, m_androidIdleSurface);
}
#endif
