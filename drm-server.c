#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <X11/Xlib-xcb.h>
#include <drm/drm.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/dri2.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

int drmGetMagic(int fd, drm_magic_t *magic);

void setup_x11(Display **dpy,
               xcb_connection_t **connection,
               int *x_screen,
               xcb_window_t *window) {
    *dpy = XOpenDisplay(NULL);
    if (!*dpy) {
        fprintf(stderr, "failed to open display\n");
        exit(1);
    }

    *connection = XGetXCBConnection(*dpy);

    const xcb_setup_t *setup = xcb_get_setup(*connection);
    xcb_screen_t *screen = xcb_setup_roots_iterator(setup).data;
    if (screen == 0) {
        fprintf(stderr, "failed to open screen\n");
        exit(1);
    }

    xcb_query_extension_cookie_t xfixes_query_extension_cookie =
        xcb_query_extension(*connection, 6, "XFIXES");
    xcb_query_extension_cookie_t dri2_query_extension_cookie =
        xcb_query_extension(*connection, 4, "DRI2");
    xcb_xfixes_query_version_cookie_t query_version_cookie =
        xcb_xfixes_query_version(*connection, 5, 0);

    *window = xcb_generate_id(*connection);

    uint32_t window_attribs[] = {
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS
    };
    xcb_void_cookie_t create_cookie =
        xcb_create_window_checked(*connection,
                                  XCB_COPY_FROM_PARENT,
                                  *window,
                                  screen->root,
                                  0,
                                  0,
                                  400,
                                  300,
                                  0,
                                  XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  screen->root_visual,
                                  XCB_CW_EVENT_MASK,
                                  window_attribs);

    xcb_query_extension_reply_t *xfixes_query_extension_reply =
        xcb_query_extension_reply(*connection, xfixes_query_extension_cookie, NULL);
    if (!xfixes_query_extension_reply->present) {
        fprintf(stderr, "no XFIXES!\n");
        exit(1);
    }
    free(xfixes_query_extension_reply);

    xcb_query_extension_reply_t *dri2_query_extension_reply =
        xcb_query_extension_reply(*connection, dri2_query_extension_cookie, NULL);
    if (!xfixes_query_extension_reply->present) {
        fprintf(stderr, "no DRI2!\n");
        exit(1);
    }
    free(dri2_query_extension_reply);

    xcb_xfixes_query_version_reply_t *query_version_reply =
        xcb_xfixes_query_version_reply(*connection, query_version_cookie, NULL);
    free(query_version_reply);

    xcb_request_check(*connection, create_cookie);
}

void setup_dri2(xcb_connection_t *conn,
                xcb_window_t window,
                struct gbm_device **gbm,
                EGLDisplay *dpy,
                EGLContext *ctx,
                EGLint *buffer_name,
                EGLint *buffer_pitch) {
    int fd = open("/dev/dri/card0", O_RDWR);
    *gbm = gbm_create_device(fd);
    *dpy = eglGetDisplay((EGLNativeDisplayType)*gbm);

    EGLint major, minor;
    eglInitialize(*dpy, &major, &minor);

    eglBindAPI(EGL_OPENGL_API);
    *ctx = eglCreateContext(*dpy, NULL, EGL_NO_CONTEXT, NULL);

    // Connect to DRI2.

    xcb_dri2_connect_cookie_t connect_cookie = xcb_dri2_connect(conn, window, XCB_DRI2_DRIVER_TYPE_DRI);

    drm_magic_t magic;
    if (drmGetMagic(fd, &magic)) {
        fprintf(stderr, "couldn't get magic!\n");
    }

    xcb_dri2_authenticate_cookie_t authenticate_cookie =
        xcb_dri2_authenticate(conn, window, magic);

    xcb_void_cookie_t create_drawable_cookie = xcb_dri2_create_drawable(conn, window);

    uint32_t attachments[] = { XCB_DRI2_ATTACHMENT_BUFFER_BACK_LEFT };
    xcb_dri2_get_buffers_cookie_t get_buffers_cookie =
        xcb_dri2_get_buffers(conn, window, 1, 1, attachments);

    xcb_flush(conn);

    // Read responses.

    xcb_dri2_connect_reply_t *connect_reply = xcb_dri2_connect_reply(conn,
                                                                     connect_cookie,
                                                                     NULL);
    fprintf(stderr,
            "connected, driver name=%s\n",
            xcb_dri2_connect_driver_name(connect_reply));
    free(connect_reply);

    xcb_dri2_authenticate_reply_t *authenticate_reply =
        xcb_dri2_authenticate_reply(conn, authenticate_cookie, NULL);
    fprintf(stderr, "authenticated? %d\n", authenticate_reply->authenticated);
    free(authenticate_reply);

    xcb_request_check(conn, create_drawable_cookie);

    xcb_dri2_get_buffers_reply_t *get_buffers_reply =
        xcb_dri2_get_buffers_reply(conn, get_buffers_cookie, NULL);
    fprintf(stderr, "got %d buffers\n", (int)get_buffers_reply->count);
    xcb_dri2_dri2_buffer_t *buffers = xcb_dri2_get_buffers_buffers(get_buffers_reply);
    fprintf(stderr,
            "buffer name is %d, pitch=%d\n",
            (int)buffers[0].name,
            (int)buffers[0].pitch);

    *buffer_name = buffers[0].name;
    *buffer_pitch = buffers[0].pitch;
}

void draw(xcb_connection_t *conn,
          xcb_window_t window,
          struct gbm_device *gbm,
          EGLDisplay dpy,
          EGLContext ctx,
          EGLint name,
          EGLint dest_name,
          EGLint pitch) {
    fprintf(stderr, "drawing to name %d to dest_name %d\n", (int)name, (int)dest_name);

    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "failed to make EGL context current!\n");
        exit(1);
    }

    // Create the EGLImageKHR for the DRI buffer.

    EGLint attribs[] = {
        EGL_WIDTH, 400,
        EGL_HEIGHT, 300,
        EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
        EGL_DRM_BUFFER_STRIDE_MESA, pitch,
        EGL_NONE
    };
    EGLImageKHR image = eglCreateImageKHR(dpy,
                                          ctx,
                                          EGL_DRM_BUFFER_MESA,
                                          (EGLClientBuffer)dest_name,
                                          attribs);
    if (!image) {
        fprintf(stderr, "couldn't create image\n");
        exit(1);
    }
    fprintf(stderr, "created destination EGL image: %p\n", (void *)image);

    // Set up the buffer we're rendering to as an FBO.

    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER_EXT, fb);

    GLuint color_rb;
    glGenRenderbuffers(1, &color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER_EXT, color_rb);

    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER_EXT, image);
    if (glGetError() != GL_NO_ERROR || eglGetError() != EGL_SUCCESS) {
        fprintf(stderr, "failed to set image target renderbuffer storage!\n");
        exit(1);
    }

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
        fprintf(stderr,
                "framebuffer is not complete: %x\n",
                (unsigned)glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT));
        exit(1);
    }

    EGLint host_attribs[] = {
        EGL_WIDTH, 400,
        EGL_HEIGHT, 300,
        EGL_DRM_BUFFER_FORMAT_MESA, EGL_DRM_BUFFER_FORMAT_ARGB32_MESA,
        EGL_DRM_BUFFER_STRIDE_MESA, pitch,
        EGL_NONE
    };
    EGLImageKHR host_image = eglCreateImageKHR(dpy,
                                               ctx,
                                               EGL_DRM_BUFFER_MESA,
                                               (EGLClientBuffer)name,
                                               host_attribs);
    if (!host_image) {
        fprintf(stderr, "couldn't create host image\n");
        exit(1);
    }
    fprintf(stderr, "created host EGL image: %p\n", (void *)host_image);

    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(0, 0, 400, 300);

    glEnable(GL_TEXTURE_2D);

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, host_image);

    if (glGetError() != GL_NO_ERROR || eglGetError() != EGL_SUCCESS) {
        fprintf(stderr, "failed to texture: %x\n", glGetError());
        exit(1);
    }

    glLoadIdentity();

    glBegin(GL_QUADS);
    glTexCoord2f(0, 0);
    glVertex3f(0, 0, 0);
    glTexCoord2f(1, 0);
    glVertex3f(1, 0, 0);
    glTexCoord2f(1, 1);
    glVertex3f(1, 1, 0);
    glTexCoord2f(0, 1);
    glVertex3f(0, 1, 0);
    glEnd();

    glFinish();

    // Swap buffers.
   
    xcb_xfixes_region_t region = xcb_generate_id(conn);

    xcb_rectangle_t rect = { 0, 0, 400, 300 };
    xcb_void_cookie_t create_region_cookie =
        xcb_xfixes_create_region(conn, region, 1, &rect);

    xcb_flush(conn);

    xcb_request_check(conn, create_region_cookie);

    xcb_dri2_copy_region_cookie_t copy_region_cookie =
        xcb_dri2_copy_region(conn,
                             window,
                             region,
                             XCB_DRI2_ATTACHMENT_BUFFER_FRONT_LEFT,
                             XCB_DRI2_ATTACHMENT_BUFFER_BACK_LEFT);

    xcb_flush(conn);

    // Get responses for swapping buffers.

    xcb_request_check(conn, create_region_cookie);

    xcb_dri2_copy_region_reply_t *copy_region_reply =
        xcb_dri2_copy_region_reply(conn, copy_region_cookie, NULL);
    free(copy_region_reply);
}

void go(xcb_connection_t *conn,
        xcb_window_t window,
        struct gbm_device *gbm,
        EGLDisplay dpy,
        EGLContext ctx,
        EGLint name,
        EGLint dest_name,
        EGLint pitch) {
    xcb_map_window(conn, window);
    xcb_flush(conn);

    xcb_generic_event_t *event;
    while ((event = xcb_wait_for_event(conn))) {
        switch (event->response_type & ~0x80) {
        case XCB_EXPOSE:
            draw(conn, window, gbm, dpy, ctx, name, dest_name, pitch);
            break;
        default:
            break;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s NAME\n", argv[0]);
        exit(0);
    }
    EGLint name = strtol(argv[1], NULL, 0);

    Display *x_dpy;
    xcb_connection_t *x_conn;
    int x_screen;
    xcb_window_t x_window;
    setup_x11(&x_dpy, &x_conn, &x_screen, &x_window);

    struct gbm_device *gbm;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLint dest_name;
    EGLint dest_pitch;
    setup_dri2(x_conn, x_window, &gbm, &dpy, &ctx, &dest_name, &dest_pitch);

    go(x_conn, x_window, gbm, dpy, ctx, name, dest_name, dest_pitch);
    return 0;
}

