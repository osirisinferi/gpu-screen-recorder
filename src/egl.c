#include "../include/egl.h"
#include "../include/library_loader.h"
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

static bool gsr_egl_create_window(gsr_egl *self) {
    EGLConfig  ecfg;
    int32_t    num_config = 0;
    EGLDisplay egl_display = NULL;
    EGLSurface egl_surface = NULL;
    EGLContext egl_context = NULL;
    Window window = None;
    
    int32_t attr[] = {
        EGL_BUFFER_SIZE, 24,
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    int32_t ctxattr[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    window = XCreateWindow(self->dpy, DefaultRootWindow(self->dpy), 0, 0, 1, 1, 0, CopyFromParent, InputOutput, CopyFromParent, 0, NULL);

    if(!window) {
        fprintf(stderr, "gsr error: gsr_gl_create_window failed: failed to create gl window\n");
        goto fail;
    }

    egl_display = self->eglGetDisplay(self->dpy);
    if(!egl_display) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglGetDisplay failed\n");
        goto fail;
    }

    if(!self->eglInitialize(egl_display, NULL, NULL)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: eglInitialize failed\n");
        goto fail;
    }
    
    if(!self->eglChooseConfig(egl_display, attr, &ecfg, 1, &num_config) || num_config != 1) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to find a matching config\n");
        goto fail;
    }
    
    egl_surface = self->eglCreateWindowSurface(egl_display, ecfg, (EGLNativeWindowType)window, NULL);
    if(!egl_surface) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create window surface\n");
        goto fail;
    }
    
    egl_context = self->eglCreateContext(egl_display, ecfg, NULL, ctxattr);
    if(!egl_context) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to create egl context\n");
        goto fail;
    }

    if(!self->eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        fprintf(stderr, "gsr error: gsr_egl_create_window failed: failed to make context current\n");
        goto fail;
    }

    self->egl_display = egl_display;
    self->egl_surface = egl_surface;
    self->egl_context = egl_context;
    self->window = window;
    return true;

    fail:
    if(egl_context)
        self->eglDestroyContext(egl_display, egl_context);
    if(egl_surface)
        self->eglDestroySurface(egl_display, egl_surface);
    if(egl_display)
        self->eglTerminate(egl_display);
    if(window)
        XDestroyWindow(self->dpy, window);
    return false;
}

static bool gsr_egl_load_egl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->eglGetError, "eglGetError" },
        { (void**)&self->eglGetDisplay, "eglGetDisplay" },
        { (void**)&self->eglInitialize, "eglInitialize" },
        { (void**)&self->eglTerminate, "eglTerminate" },
        { (void**)&self->eglChooseConfig, "eglChooseConfig" },
        { (void**)&self->eglCreateWindowSurface, "eglCreateWindowSurface" },
        { (void**)&self->eglCreateContext, "eglCreateContext" },
        { (void**)&self->eglMakeCurrent, "eglMakeCurrent" },
        { (void**)&self->eglCreateImage, "eglCreateImage" },
        { (void**)&self->eglDestroyContext, "eglDestroyContext" },
        { (void**)&self->eglDestroySurface, "eglDestroySurface" },
        { (void**)&self->eglDestroyImage, "eglDestroyImage" },
        { (void**)&self->eglSwapInterval, "eglSwapInterval" },
        { (void**)&self->eglSwapBuffers, "eglSwapBuffers" },
        { (void**)&self->eglGetProcAddress, "eglGetProcAddress" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libEGL.so.1\n");
        return false;
    }

    return true;
}

static bool gsr_egl_proc_load_egl(gsr_egl *self) {
    self->eglExportDMABUFImageQueryMESA = (FUNC_eglExportDMABUFImageQueryMESA)self->eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    self->eglExportDMABUFImageMESA = (FUNC_eglExportDMABUFImageMESA)self->eglGetProcAddress("eglExportDMABUFImageMESA");
    self->glEGLImageTargetTexture2DOES = (FUNC_glEGLImageTargetTexture2DOES)self->eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if(!self->glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: could not find glEGLImageTargetTexture2DOES\n");
        return false;
    }

    return true;
}

static bool gsr_egl_load_gl(gsr_egl *self, void *library) {
    dlsym_assign required_dlsym[] = {
        { (void**)&self->glGetError, "glGetError" },
        { (void**)&self->glGetString, "glGetString" },
        { (void**)&self->glClear, "glClear" },
        { (void**)&self->glClearColor, "glClearColor" },
        { (void**)&self->glGenTextures, "glGenTextures" },
        { (void**)&self->glDeleteTextures, "glDeleteTextures" },
        { (void**)&self->glBindTexture, "glBindTexture" },
        { (void**)&self->glTexParameteri, "glTexParameteri" },
        { (void**)&self->glGetTexLevelParameteriv, "glGetTexLevelParameteriv" },
        { (void**)&self->glTexImage2D, "glTexImage2D" },
        { (void**)&self->glCopyImageSubData, "glCopyImageSubData" },
        { (void**)&self->glClearTexImage, "glClearTexImage" },
        { (void**)&self->glGenFramebuffers, "glGenFramebuffers" },
        { (void**)&self->glBindFramebuffer, "glBindFramebuffer" },
        { (void**)&self->glDeleteFramebuffers, "glDeleteFramebuffers" },
        { (void**)&self->glViewport, "glViewport" },
        { (void**)&self->glFramebufferTexture2D, "glFramebufferTexture2D" },
        { (void**)&self->glDrawBuffers, "glDrawBuffers" },
        { (void**)&self->glCheckFramebufferStatus, "glCheckFramebufferStatus" },
        { (void**)&self->glBindBuffer, "glBindBuffer" },
        { (void**)&self->glGenBuffers, "glGenBuffers" },
        { (void**)&self->glBufferData, "glBufferData" },
        { (void**)&self->glBufferSubData, "glBufferSubData" },
        { (void**)&self->glDeleteBuffers, "glDeleteBuffers" },
        { (void**)&self->glGenVertexArrays, "glGenVertexArrays" },
        { (void**)&self->glBindVertexArray, "glBindVertexArray" },
        { (void**)&self->glDeleteVertexArrays, "glDeleteVertexArrays" },
        { (void**)&self->glCreateProgram, "glCreateProgram" },
        { (void**)&self->glCreateShader, "glCreateShader" },
        { (void**)&self->glAttachShader, "glAttachShader" },
        { (void**)&self->glBindAttribLocation, "glBindAttribLocation" },
        { (void**)&self->glCompileShader, "glCompileShader" },
        { (void**)&self->glLinkProgram, "glLinkProgram" },
        { (void**)&self->glShaderSource, "glShaderSource" },
        { (void**)&self->glUseProgram, "glUseProgram" },
        { (void**)&self->glGetProgramInfoLog, "glGetProgramInfoLog" },
        { (void**)&self->glGetShaderiv, "glGetShaderiv" },
        { (void**)&self->glGetShaderInfoLog, "glGetShaderInfoLog" },
        { (void**)&self->glDeleteProgram, "glDeleteProgram" },
        { (void**)&self->glDeleteShader, "glDeleteShader" },
        { (void**)&self->glGetProgramiv, "glGetProgramiv" },
        { (void**)&self->glVertexAttribPointer, "glVertexAttribPointer" },
        { (void**)&self->glEnableVertexAttribArray, "glEnableVertexAttribArray" },
        { (void**)&self->glDrawArrays, "glDrawArrays" },
        { (void**)&self->glEnable, "glEnable" },
        { (void**)&self->glBlendFunc, "glBlendFunc" },
        { (void**)&self->glGetUniformLocation, "glGetUniformLocation" },
        { (void**)&self->glUniform1f, "glUniform1f" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(library, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_egl_load failed: missing required symbols in libGL.so.1\n");
        return false;
    }

    return true;
}

bool gsr_egl_load(gsr_egl *self, Display *dpy) {
    memset(self, 0, sizeof(gsr_egl));
    self->dpy = dpy;

    void *egl_lib = NULL;
    void *gl_lib = NULL;

    dlerror(); /* clear */
    egl_lib = dlopen("libEGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libEGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    gl_lib = dlopen("libGL.so.1", RTLD_LAZY);
    if(!egl_lib) {
        fprintf(stderr, "gsr error: gsr_egl_load: failed to load libGL.so.1, error: %s\n", dlerror());
        goto fail;
    }

    if(!gsr_egl_load_egl(self, egl_lib))
        goto fail;

    if(!gsr_egl_load_gl(self, gl_lib))
        goto fail;

    if(!gsr_egl_proc_load_egl(self))
        goto fail;

    if(!gsr_egl_create_window(self))
        goto fail;

    self->glEnable(GL_BLEND);
    self->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    self->egl_library = egl_lib;
    self->gl_library = gl_lib;
    return true;

    fail:
    if(egl_lib)
        dlclose(egl_lib);
    if(gl_lib)
        dlclose(gl_lib);
    memset(self, 0, sizeof(gsr_egl));
    return false;
}

void gsr_egl_unload(gsr_egl *self) {
    if(self->egl_context) {
        self->eglDestroyContext(self->egl_display, self->egl_context);
        self->egl_context = NULL;
    }

    if(self->egl_surface) {
        self->eglDestroySurface(self->egl_display, self->egl_surface);
        self->egl_surface = NULL;
    }

    if(self->egl_display) {
        self->eglTerminate(self->egl_display);
        self->egl_display = NULL;
    }

    if(self->window) {
        XDestroyWindow(self->dpy, self->window);
        self->window = None;
    }

    if(self->egl_library) {
        dlclose(self->egl_library);
        self->egl_library = NULL;
    }

    if(self->gl_library) {
        dlclose(self->gl_library);
        self->gl_library = NULL;
    }

    memset(self, 0, sizeof(gsr_egl));
}
