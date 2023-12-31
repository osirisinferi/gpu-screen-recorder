#include "../../include/capture/kms_vaapi.h"
#include "../../kms/client/kms_client.h"
#include "../../include/egl.h"
#include "../../include/utils.h"
#include "../../include/color_conversion.h"
#include "../../include/cursor.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

typedef enum {
    X11_ROT_0    = 1 << 0,
    X11_ROT_90   = 1 << 1,
    X11_ROT_180  = 1 << 2,
    X11_ROT_270  = 1 << 3
} X11Rotation;

typedef struct {
    gsr_capture_kms_vaapi_params params;
    Display *dpy;
    XEvent xev;

    bool should_stop;
    bool stop_is_error;
    bool created_hw_frame;

    gsr_egl egl;
    
    gsr_kms_client kms_client;
    gsr_kms_response kms_response;

    vec2i screen_size;
    vec2i capture_pos;
    vec2i capture_size;
    bool screen_capture;
    MonitorId monitor_id;

    VADisplay va_dpy;

    bool requires_rotation;
    X11Rotation x11_rot;

    VADRMPRIMESurfaceDescriptor prime;

    unsigned int input_texture;
    unsigned int target_textures[2];

    gsr_color_conversion color_conversion;

    gsr_cursor cursor;
} gsr_capture_kms_vaapi;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context);

static bool drm_create_codec_context(gsr_capture_kms_vaapi *cap_kms, AVCodecContext *video_codec_context) {
    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, cap_kms->params.card_path, NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_YUV420P;//AV_PIX_FMT_0RGB32;//AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    hw_frame_context->initial_pool_size = 1; // TODO: (and in other places)

    AVVAAPIDeviceContext *vactx =((AVHWDeviceContext*)device_ctx->data)->hwctx;
    cap_kms->va_dpy = vactx->display;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

#define DRM_FORMAT_MOD_INVALID 72057594037927935

// TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc.

typedef struct {
    gsr_capture_kms_vaapi *cap_kms;
    const Atom randr_connector_id_atom;
    const char *monitor_to_capture;
    int monitor_to_capture_len;
    int num_monitors;
    int rotation;
} MonitorCallbackUserdata;

static bool properties_has_atom(Atom *props, int nprop, Atom atom) {
    for(int i = 0; i < nprop; ++i) {
        if(props[i] == atom)
            return true;
    }
    return false;
}

static void monitor_callback(const XRROutputInfo *output_info, const XRRCrtcInfo *crt_info, const XRRModeInfo *mode_info, void *userdata) {
    (void)mode_info;
    MonitorCallbackUserdata *monitor_callback_userdata = userdata;
    ++monitor_callback_userdata->num_monitors;

    if(strcmp(monitor_callback_userdata->monitor_to_capture, "screen") == 0)
        monitor_callback_userdata->rotation = crt_info->rotation;

    if(monitor_callback_userdata->monitor_to_capture_len != output_info->nameLen || memcmp(monitor_callback_userdata->monitor_to_capture, output_info->name, output_info->nameLen) != 0)
        return;

    monitor_callback_userdata->rotation = crt_info->rotation;
    for(int i = 0; i < crt_info->noutput && monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids < MAX_CONNECTOR_IDS; ++i) {
        int nprop = 0;
        Atom *props = XRRListOutputProperties(monitor_callback_userdata->cap_kms->dpy, crt_info->outputs[i], &nprop);
        if(!props)
            continue;

        if(!properties_has_atom(props, nprop, monitor_callback_userdata->randr_connector_id_atom)) {
            XFree(props);
            continue;
        }

        Atom type = 0;
        int format = 0;
        unsigned long bytes_after = 0;
        unsigned long nitems = 0;
        unsigned char *prop = NULL;
        XRRGetOutputProperty(monitor_callback_userdata->cap_kms->dpy, crt_info->outputs[i],
            monitor_callback_userdata->randr_connector_id_atom,
            0, 128, false, false, AnyPropertyType,
            &type, &format, &nitems, &bytes_after, &prop);

        if(type == XA_INTEGER && format == 32) {
            monitor_callback_userdata->cap_kms->monitor_id.connector_ids[monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids] = *(long*)prop;
            ++monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids;
        }

        XFree(props);
    }

    if(monitor_callback_userdata->cap_kms->monitor_id.num_connector_ids == MAX_CONNECTOR_IDS)
        fprintf(stderr, "gsr warning: reached max connector ids\n");
}

static int gsr_capture_kms_vaapi_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    if(gsr_kms_client_init(&cap_kms->kms_client, cap_kms->params.card_path) != 0) {
        return -1;
    }

    const Atom randr_connector_id_atom = XInternAtom(cap_kms->dpy, "CONNECTOR_ID", False);
    cap_kms->monitor_id.num_connector_ids = 0;
    MonitorCallbackUserdata monitor_callback_userdata = {
        cap_kms, randr_connector_id_atom,
        cap_kms->params.display_to_capture, strlen(cap_kms->params.display_to_capture),
        0,
        X11_ROT_0
    };
    for_each_active_monitor_output(cap_kms->dpy, monitor_callback, &monitor_callback_userdata);

    cap_kms->screen_size.x = WidthOfScreen(DefaultScreenOfDisplay(cap_kms->dpy));
    cap_kms->screen_size.y = HeightOfScreen(DefaultScreenOfDisplay(cap_kms->dpy));

    gsr_monitor monitor;
    if(strcmp(cap_kms->params.display_to_capture, "screen") == 0) {
        monitor.pos.x = 0;
        monitor.pos.y = 0;
        monitor.size = cap_kms->screen_size;
        cap_kms->screen_capture = true;
    } else if(!get_monitor_by_name(cap_kms->dpy, cap_kms->params.display_to_capture, &monitor)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_start: failed to find monitor by name \"%s\"\n", cap_kms->params.display_to_capture);
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    // TODO: Find a better way to do this. Is this info available somewhere in drm? it should be!

    // Note: workaround AMD/Intel issue. If there is one monitor enabled and it's rotated then
    // the drm buf will also be rotated. This only happens when you only have one monitor enabled.
    cap_kms->x11_rot = monitor_callback_userdata.rotation;
    if(monitor_callback_userdata.num_monitors == 1 && cap_kms->x11_rot != X11_ROT_0) {
        cap_kms->requires_rotation = true;
    } else {
        cap_kms->requires_rotation = false;
    }

    cap_kms->capture_pos = monitor.pos;
    cap_kms->capture_size = monitor.size;

    if(!gsr_egl_load(&cap_kms->egl, cap_kms->dpy)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_start: failed to load opengl\n");
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    /* Disable vsync */
    cap_kms->egl.eglSwapInterval(cap_kms->egl.egl_display, 0);

    video_codec_context->width = max_int(2, cap_kms->capture_size.x & ~1);
    video_codec_context->height = max_int(2, cap_kms->capture_size.y & ~1);

    if(!drm_create_codec_context(cap_kms, video_codec_context)) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    if(gsr_cursor_init(&cap_kms->cursor, &cap_kms->egl, cap_kms->dpy) != 0) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        return -1;
    }

    gsr_cursor_change_window_target(&cap_kms->cursor, DefaultRootWindow(cap_kms->dpy));
    gsr_cursor_update(&cap_kms->cursor, &cap_kms->xev);

    return 0;
}

static uint32_t fourcc(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (d << 24) | (c << 16) | (b << 8) | a;
}

#define FOURCC_NV12 842094158

static void gsr_capture_kms_vaapi_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    // TODO:
    cap_kms->egl.glClear(GL_COLOR_BUFFER_BIT);

    while(XPending(cap_kms->dpy)) {
        XNextEvent(cap_kms->dpy, &cap_kms->xev);
        gsr_cursor_update(&cap_kms->cursor, &cap_kms->xev);
    }

    if(!cap_kms->created_hw_frame) {
        cap_kms->created_hw_frame = true;

        av_frame_free(frame);
        *frame = av_frame_alloc();
        if(!frame) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: failed to allocate frame\n");
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
        (*frame)->format = video_codec_context->pix_fmt;
        (*frame)->width = video_codec_context->width;
        (*frame)->height = video_codec_context->height;
        (*frame)->color_range = video_codec_context->color_range;
        (*frame)->color_primaries = video_codec_context->color_primaries;
        (*frame)->color_trc = video_codec_context->color_trc;
        (*frame)->colorspace = video_codec_context->colorspace;
        (*frame)->chroma_location = video_codec_context->chroma_sample_location;

        int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, *frame, 0);
        if(res < 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: av_hwframe_get_buffer failed: %d\n", res);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }

        VASurfaceID target_surface_id = (uintptr_t)(*frame)->data[3];

        VAStatus va_status = vaExportSurfaceHandle(cap_kms->va_dpy, target_surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &cap_kms->prime);
        if(va_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: vaExportSurfaceHandle failed, error: %d\n", va_status);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
        vaSyncSurface(cap_kms->va_dpy, target_surface_id);

        cap_kms->egl.glGenTextures(1, &cap_kms->input_texture);
        cap_kms->egl.glBindTexture(GL_TEXTURE_2D, cap_kms->input_texture);
        cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        cap_kms->egl.glBindTexture(GL_TEXTURE_2D, 0);

        if(cap_kms->prime.fourcc == FOURCC_NV12) {
            cap_kms->egl.glGenTextures(2, cap_kms->target_textures);
            for(int i = 0; i < 2; ++i) {
                const uint32_t formats[2] = { fourcc('R', '8', ' ', ' '), fourcc('G', 'R', '8', '8') };
                const int layer = i;
                const int plane = 0;

                const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

                const intptr_t img_attr[] = {
                    EGL_LINUX_DRM_FOURCC_EXT,       formats[i],
                    EGL_WIDTH,                      cap_kms->prime.width / div[i],
                    EGL_HEIGHT,                     cap_kms->prime.height / div[i],
                    EGL_DMA_BUF_PLANE0_FD_EXT,      cap_kms->prime.objects[cap_kms->prime.layers[layer].object_index[plane]].fd,
                    EGL_DMA_BUF_PLANE0_OFFSET_EXT,  cap_kms->prime.layers[layer].offset[plane],
                    EGL_DMA_BUF_PLANE0_PITCH_EXT,   cap_kms->prime.layers[layer].pitch[plane],
                    EGL_NONE
                };

                while(cap_kms->egl.eglGetError() != EGL_SUCCESS){}
                EGLImage image = cap_kms->egl.eglCreateImage(cap_kms->egl.egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
                if(!image) {
                    fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: failed to create egl image from drm fd for output drm fd, error: %d\n", cap_kms->egl.eglGetError());
                    cap_kms->should_stop = true;
                    cap_kms->stop_is_error = true;
                    return;
                }

                cap_kms->egl.glBindTexture(GL_TEXTURE_2D, cap_kms->target_textures[i]);
                cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                cap_kms->egl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

                while(cap_kms->egl.glGetError()) {}
                while(cap_kms->egl.eglGetError() != EGL_SUCCESS){}
                cap_kms->egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
                if(cap_kms->egl.glGetError() != 0 || cap_kms->egl.eglGetError() != EGL_SUCCESS) {
                    fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: failed to bind egl image to gl texture, error: %d\n", cap_kms->egl.eglGetError());
                    cap_kms->should_stop = true;
                    cap_kms->stop_is_error = true;
                    cap_kms->egl.eglDestroyImage(cap_kms->egl.egl_display, image);
                    cap_kms->egl.glBindTexture(GL_TEXTURE_2D, 0);
                    return;
                }

                cap_kms->egl.eglDestroyImage(cap_kms->egl.egl_display, image);
                cap_kms->egl.glBindTexture(GL_TEXTURE_2D, 0);
            }

            gsr_color_conversion_params color_conversion_params = {0};
            color_conversion_params.egl = &cap_kms->egl;
            color_conversion_params.source_color = GSR_SOURCE_COLOR_RGB;
            color_conversion_params.destination_color = GSR_DESTINATION_COLOR_NV12;

            color_conversion_params.destination_textures[0] = cap_kms->target_textures[0];
            color_conversion_params.destination_textures[1] = cap_kms->target_textures[1];
            color_conversion_params.num_destination_textures = 2;

            if(gsr_color_conversion_init(&cap_kms->color_conversion, &color_conversion_params) != 0) {
                fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: failed to create color conversion\n");
                cap_kms->should_stop = true;
                cap_kms->stop_is_error = true;
                return;
            }
        } else {
            fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_tick: unexpected fourcc %u for output drm fd, expected nv12\n", cap_kms->prime.fourcc);
            cap_kms->should_stop = true;
            cap_kms->stop_is_error = true;
            return;
        }
    }
}

static bool gsr_capture_kms_vaapi_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    if(cap_kms->should_stop) {
        if(err)
            *err = cap_kms->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static gsr_kms_response_fd* find_drm_by_connector_id(gsr_kms_response *kms_response, uint32_t connector_id) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].connector_id == connector_id)
            return &kms_response->fds[i];
    }
    return NULL;
}

static gsr_kms_response_fd* find_first_combined_drm(gsr_kms_response *kms_response) {
    for(int i = 0; i < kms_response->num_fds; ++i) {
        if(kms_response->fds[i].is_combined_plane)
            return &kms_response->fds[i];
    }
    return NULL;
}

static gsr_kms_response_fd* find_largest_drm(gsr_kms_response *kms_response) {
    if(kms_response->num_fds == 0)
        return NULL;

    int64_t largest_size = 0;
    gsr_kms_response_fd *largest_drm = &kms_response->fds[0];
    for(int i = 0; i < kms_response->num_fds; ++i) {
        const int64_t size = (int64_t)kms_response->fds[i].width * (int64_t)kms_response->fds[i].height;
        if(size > largest_size) {
            largest_size = size;
            largest_drm = &kms_response->fds[i];
        }
    }
    return largest_drm;
}

static int gsr_capture_kms_vaapi_capture(gsr_capture *cap, AVFrame *frame) {
    (void)frame;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    if(gsr_kms_client_get_kms(&cap_kms->kms_client, &cap_kms->kms_response) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_capture: failed to get kms, error: %d (%s)\n", cap_kms->kms_response.result, cap_kms->kms_response.err_msg);
        return -1;
    }

    if(cap_kms->kms_response.num_fds == 0) {
        static bool error_shown = false;
        if(!error_shown) {
            error_shown = true;
            fprintf(stderr, "gsr error: no drm found, capture will fail\n");
        }
        return -1;
    }

    bool requires_rotation = cap_kms->requires_rotation;

    gsr_kms_response_fd *drm_fd = NULL;
    if(cap_kms->screen_capture) {
        drm_fd = find_first_combined_drm(&cap_kms->kms_response);
        if(!drm_fd)
            drm_fd = find_largest_drm(&cap_kms->kms_response);
    } else {
        for(int i = 0; i < cap_kms->monitor_id.num_connector_ids; ++i) {
            drm_fd = find_drm_by_connector_id(&cap_kms->kms_response, cap_kms->monitor_id.connector_ids[i]);
            if(drm_fd) {
                requires_rotation = cap_kms->x11_rot != X11_ROT_0;
                break;
            }
        }

        if(!drm_fd) {
            drm_fd = find_first_combined_drm(&cap_kms->kms_response);
            if(!drm_fd)
                drm_fd = find_largest_drm(&cap_kms->kms_response);
        }
    }

    bool capture_is_combined_plane = drm_fd->is_combined_plane || ((int)drm_fd->width == cap_kms->screen_size.x && (int)drm_fd->height == cap_kms->screen_size.y);

    // TODO: This causes a crash sometimes on steam deck, why? is it a driver bug? a vaapi pure version doesn't cause a crash.
    // Even ffmpeg kmsgrab causes this crash. The error is:
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // EE ../jupiter-mesa/src/gallium/drivers/radeonsi/radeon_vcn_enc.c:516 radeon_create_encoder UVD - Can't create CPB buffer.
    // [hevc_vaapi @ 0x55ea72b09840] Failed to upload encode parameters: 2 (resource allocation failed).
    // [hevc_vaapi @ 0x55ea72b09840] Encode failed: -5.
    // Error: avcodec_send_frame failed, error: Input/output error
    // Assertion pic->display_order == pic->encode_order failed at libavcodec/vaapi_encode_h265.c:765
    // kms server info: kms client shutdown, shutting down the server
    const intptr_t img_attr[] = {
        EGL_LINUX_DRM_FOURCC_EXT,       drm_fd->pixel_format,
        EGL_WIDTH,                      drm_fd->width,
        EGL_HEIGHT,                     drm_fd->height,
        EGL_DMA_BUF_PLANE0_FD_EXT,      drm_fd->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  drm_fd->offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   drm_fd->pitch,
        EGL_NONE
    };

    EGLImage image = cap_kms->egl.eglCreateImage(cap_kms->egl.egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    cap_kms->egl.glBindTexture(GL_TEXTURE_2D, cap_kms->input_texture);
    cap_kms->egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    cap_kms->egl.eglDestroyImage(cap_kms->egl.egl_display, image);
    cap_kms->egl.glBindTexture(GL_TEXTURE_2D, 0);

    float texture_rotation = 0.0f;
    if(requires_rotation) {
        switch(cap_kms->x11_rot) {
            case X11_ROT_90:
                texture_rotation = M_PI*0.5f;
                break;
            case X11_ROT_180:
                texture_rotation = M_PI;
                break;
            case X11_ROT_270:
                texture_rotation = M_PI*1.5f;
                break;
            default:
                texture_rotation = 0.0f;
                break;
        }
    }

    gsr_cursor_tick(&cap_kms->cursor);

    vec2i capture_pos = cap_kms->capture_pos;
    vec2i capture_size = cap_kms->capture_size;
    vec2i cursor_capture_pos = (vec2i){cap_kms->cursor.position.x - cap_kms->cursor.hotspot.x - capture_pos.x, cap_kms->cursor.position.y - cap_kms->cursor.hotspot.y - capture_pos.y};
    if(!capture_is_combined_plane) {
        capture_pos = (vec2i){0, 0};
        //cursor_capture_pos = (vec2i){cap_kms->cursor.position.x - cap_kms->cursor.hotspot.x, cap_kms->cursor.position.y - cap_kms->cursor.hotspot.y};
    }

    gsr_color_conversion_draw(&cap_kms->color_conversion, cap_kms->input_texture,
        (vec2i){0, 0}, capture_size,
        capture_pos, capture_size,
        texture_rotation);

    gsr_color_conversion_draw(&cap_kms->color_conversion, cap_kms->cursor.texture_id,
        cursor_capture_pos, (vec2i){cap_kms->cursor.size.x, cap_kms->cursor.size.y},
        (vec2i){0, 0}, (vec2i){cap_kms->cursor.size.x, cap_kms->cursor.size.y},
        0.0f);

    cap_kms->egl.eglSwapBuffers(cap_kms->egl.egl_display, cap_kms->egl.egl_surface);

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    return 0;
}

static void gsr_capture_kms_vaapi_stop(gsr_capture *cap, AVCodecContext *video_codec_context) {
    gsr_capture_kms_vaapi *cap_kms = cap->priv;

    gsr_cursor_deinit(&cap_kms->cursor);
    gsr_color_conversion_deinit(&cap_kms->color_conversion);

    for(uint32_t i = 0; i < cap_kms->prime.num_objects; ++i) {
        if(cap_kms->prime.objects[i].fd > 0) {
            close(cap_kms->prime.objects[i].fd);
            cap_kms->prime.objects[i].fd = 0;
        }
    }

    if(cap_kms->input_texture) {
        cap_kms->egl.glDeleteTextures(1, &cap_kms->input_texture);
        cap_kms->input_texture = 0;
    }

    cap_kms->egl.glDeleteTextures(2, cap_kms->target_textures);
    cap_kms->target_textures[0] = 0;
    cap_kms->target_textures[1] = 0;

    for(int i = 0; i < cap_kms->kms_response.num_fds; ++i) {
        if(cap_kms->kms_response.fds[i].fd > 0)
            close(cap_kms->kms_response.fds[i].fd);
        cap_kms->kms_response.fds[i].fd = 0;
    }
    cap_kms->kms_response.num_fds = 0;

    if(video_codec_context->hw_device_ctx)
        av_buffer_unref(&video_codec_context->hw_device_ctx);
    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);

    gsr_egl_unload(&cap_kms->egl);
    gsr_kms_client_deinit(&cap_kms->kms_client);
    if(cap_kms->dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(cap_kms->dpy);
        cap_kms->dpy = NULL;
    }
}

static void gsr_capture_kms_vaapi_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms_vaapi *cap_kms = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_vaapi_stop(cap, video_codec_context);
        free((void*)cap_kms->params.display_to_capture);
        cap_kms->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_vaapi_create(const gsr_capture_kms_vaapi_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms_vaapi *cap_kms = calloc(1, sizeof(gsr_capture_kms_vaapi));
    if(!cap_kms) {
        free(cap);
        return NULL;
    }

    Display *display = XOpenDisplay(NULL);
    if(!display) {
        fprintf(stderr, "gsr error: gsr_capture_kms_vaapi_create failed: XOpenDisplay failed\n");
        free(cap);
        free(cap_kms);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        /* TODO XCloseDisplay */
        free(cap);
        free(cap_kms);
        return NULL;
    }

    cap_kms->dpy = display;
    cap_kms->params = *params;
    cap_kms->params.display_to_capture = display_to_capture;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_kms_vaapi_start,
        .tick = gsr_capture_kms_vaapi_tick,
        .should_stop = gsr_capture_kms_vaapi_should_stop,
        .capture = gsr_capture_kms_vaapi_capture,
        .destroy = gsr_capture_kms_vaapi_destroy,
        .priv = cap_kms
    };

    return cap;
}
