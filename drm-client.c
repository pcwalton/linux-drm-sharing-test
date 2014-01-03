#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
    int fd = open("/dev/dri/card0", O_RDWR);
    struct gbm_device *gbm = gbm_create_device(fd);
    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);

    EGLint major, minor;
    eglInitialize(dpy, &major, &minor);

    const char *extensions = eglQueryString(dpy, EGL_EXTENSIONS);
    fprintf(stderr, "extensions: %s\n", extensions);

    eglBindAPI(EGL_OPENGL_API);
    EGLContext ctx = eglCreateContext(dpy, NULL, EGL_NO_CONTEXT, NULL);

    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    EGLint attrib_list[] = {
        EGL_WIDTH, 400,
        EGL_HEIGHT, 300,
        EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
        EGL_DRM_BUFFER_USE_MESA, EGL_DRM_BUFFER_USE_SHARE_MESA,
        EGL_NONE
    };
    EGLImageKHR image = eglCreateDRMImageMESA(dpy, attrib_list);

    if (!image) {
        fprintf(stderr, "couldn't create image!\n");
    }

    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, fb);
    if (!image) {
        fprintf(stderr, "couldn't create image\n");
        return 1;
    }

    GLuint color_rb;
    glGenRenderbuffers(1, &color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER_EXT, color_rb);
    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, image);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,
                                 GL_COLOR_ATTACHMENT0_EXT,
                                 GL_RENDERBUFFER_EXT,
                                 color_rb);

    GLuint depth_rb;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER_EXT, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, 400, 300);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT,
                                 GL_DEPTH_ATTACHMENT_EXT,
                                 GL_RENDERBUFFER_EXT,
                                 depth_rb);

    if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "framebuffer is not complete!\n");
        return 1;
    }

    glViewport(0, 0, 400, 300);

    glClearColor(0.0, 0.0, 1.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    glBegin(GL_TRIANGLES);
    glVertex3f(0.5f, 1.0f, 0.0f);
    glVertex3f(1.0f, 0.0f, 0.0f);
    glVertex3f(0.0f, 0.0f, 0.0f);
    glEnd();

    glFinish();

    EGLint name, handle, stride;
    if (!eglExportDRMImageMESA(dpy, image, &name, &handle, &stride)) {
        fprintf(stderr, "couldn't export image!\n");
        return 1;
    }

    fprintf(stderr,
            "OK. name=%d, stride=%d, display=%p, framebuffer=%u, EGLImage=%p\n",
            (int)name,
            (int)stride,
            (void *)dpy,
            (unsigned)fb,
            (void *)image);

    fgetc(stdin);

    return 0;
}

