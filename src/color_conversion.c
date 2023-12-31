#include "../include/color_conversion.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define MAX_SHADERS 2
#define MAX_FRAMEBUFFERS 2

static float abs_f(float v) {
    return v >= 0.0f ? v : -v;
}

#define ROTATE_Z   "mat4 rotate_z(in float angle) {\n"                        \
                   "    return mat4(cos(angle), -sin(angle), 0.0, 0.0,\n"     \
                   "                sin(angle),  cos(angle), 0.0, 0.0,\n"     \
                   "                0.0,           0.0,      1.0, 0.0,\n"     \
                   "                0.0,           0.0,      0.0, 1.0);\n"    \
                   "}\n"

#define RGB_TO_YUV "const mat4 RGBtoYUV = mat4(0.257,  0.439, -0.148, 0.0,\n" \
                   "                           0.504, -0.368, -0.291, 0.0,\n" \
                   "                           0.098, -0.071,  0.439, 0.0,\n" \
                   "                           0.0625, 0.500,  0.500, 1.0);"

static int load_shader_y(gsr_shader *shader, gsr_egl *egl, int *rotation_uniform) {
    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                   \n"
        "in vec2 pos;                                      \n"
        "in vec2 texcoords;                                \n"
        "out vec2 texcoords_out;                           \n"
        "uniform float rotation;                           \n"
        ROTATE_Z
        "void main()                                       \n"
        "{                                                 \n"
        "  texcoords_out = texcoords;                      \n"
        "  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0) * rotate_z(rotation);    \n"
        "}                                                 \n");

    char fragment_shader[] =
        "#version 300 es                                                                 \n"
        "precision mediump float;                                                        \n"
        "in vec2 texcoords_out;                                                          \n"
        "uniform sampler2D tex1;                                                         \n"
        "out vec4 FragColor;                                                             \n"
        RGB_TO_YUV
        "void main()                                                                     \n"
        "{                                                                               \n"
        "  vec4 pixel = texture(tex1, texcoords_out);                                    \n"
        "  FragColor.x = (RGBtoYUV * vec4(pixel.rgb, 1.0)).x;                            \n"
        "  FragColor.w = pixel.a;                                                        \n"
        "}                                                                               \n";

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    *rotation_uniform = egl->glGetUniformLocation(shader->program_id, "rotation");
    return 0;
}

static unsigned int load_shader_uv(gsr_shader *shader, gsr_egl *egl, int *rotation_uniform) {
    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
        "uniform float rotation;                         \n"
        ROTATE_Z
        "void main()                                     \n"
        "{                                               \n"
        "  texcoords_out = texcoords;                    \n"
        "  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0) * rotate_z(rotation) * vec4(0.5, 0.5, 1.0, 1.0) - vec4(0.5, 0.5, 0.0, 0.0);   \n"
        "}                                               \n");

    char fragment_shader[] =
        "#version 300 es                                                                       \n"
        "precision mediump float;                                                              \n"
        "in vec2 texcoords_out;                                                                \n"
        "uniform sampler2D tex1;                                                               \n"
        "out vec4 FragColor;                                                                   \n"
        RGB_TO_YUV
        "void main()                                                                           \n"
        "{                                                                                     \n"
        "  vec4 pixel = texture(tex1, texcoords_out);                                          \n"
        "  FragColor.xy = (RGBtoYUV * vec4(pixel.rgb, 1.0)).zy;                                \n"
        "  FragColor.w = pixel.a;                                                              \n"
        "}                                                                                     \n";

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    *rotation_uniform = egl->glGetUniformLocation(shader->program_id, "rotation");
    return 0;
}

static int loader_framebuffers(gsr_color_conversion *self) {
    const unsigned int draw_buffer = GL_COLOR_ATTACHMENT0;
    self->params.egl->glGenFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[0], 0);
    self->params.egl->glDrawBuffers(1, &draw_buffer);
    if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for Y\n");
        goto err;
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
    self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[1], 0);
    self->params.egl->glDrawBuffers(1, &draw_buffer);
    if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for UV\n");
        goto err;
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;

    err:
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return -1;
}

static int create_vertices(gsr_color_conversion *self) {
    self->params.egl->glGenVertexArrays(1, &self->vertex_array_object_id);
    self->params.egl->glBindVertexArray(self->vertex_array_object_id);

    self->params.egl->glGenBuffers(1, &self->vertex_buffer_object_id);
    self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), NULL, GL_STREAM_DRAW);

    self->params.egl->glEnableVertexAttribArray(0);
    self->params.egl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    self->params.egl->glEnableVertexAttribArray(1);
    self->params.egl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    self->params.egl->glBindVertexArray(0);
    return 0;
}

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params) {
    assert(params);
    assert(params->egl);
    memset(self, 0, sizeof(*self));
    self->params.egl = params->egl;
    self->params = *params;

    if(self->params.num_destination_textures != 2) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 2 destination textures for destination color NV12, got %d destination texture(s)\n", self->params.num_destination_textures);
        return -1;
    }

    if(load_shader_y(&self->shaders[0], self->params.egl, &self->rotation_uniforms[0]) != 0) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y shader\n");
        goto err;
    }

    if(load_shader_uv(&self->shaders[1], self->params.egl, &self->rotation_uniforms[1]) != 0) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV shader\n");
        goto err;
    }

    if(loader_framebuffers(self) != 0)
        goto err;

    if(create_vertices(self) != 0)
        goto err;

    return 0;

    err:
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gsr_color_conversion_deinit(self);
    return -1;
}

void gsr_color_conversion_deinit(gsr_color_conversion *self) {
    if(!self->params.egl)
        return;

    if(self->vertex_buffer_object_id) {
        self->params.egl->glDeleteBuffers(1, &self->vertex_buffer_object_id);
        self->vertex_buffer_object_id = 0;
    }

    if(self->vertex_array_object_id) {
        self->params.egl->glDeleteVertexArrays(1, &self->vertex_array_object_id);
        self->vertex_array_object_id = 0;
    }

    self->params.egl->glDeleteFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);
    for(int i = 0; i < MAX_FRAMEBUFFERS; ++i) {
        self->framebuffers[i] = 0;
    }

    for(int i = 0; i < MAX_SHADERS; ++i) {
        gsr_shader_deinit(&self->shaders[i]);
    }

    self->params.egl = NULL;
}

/* |source_pos| is in pixel coordinates and |source_size|  */
int gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i source_pos, vec2i source_size, vec2i texture_pos, vec2i texture_size, float rotation) {
    /* TODO: Do not call this every frame? */
    vec2i dest_texture_size = {0, 0};
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->params.destination_textures[0]);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &dest_texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &dest_texture_size.y);

    /* TODO: Do not call this every frame? */
    vec2i source_texture_size = {0, 0};
    self->params.egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &source_texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &source_texture_size.y);

    if(abs_f(M_PI * 0.5f - rotation) <= 0.001f || abs_f(M_PI * 1.5f - rotation) <= 0.001f) {
        float tmp = source_texture_size.x;
        source_texture_size.x = source_texture_size.y;
        source_texture_size.y = tmp;
    }

    const vec2f pos_norm = {
        ((float)source_pos.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f,
        ((float)source_pos.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f,
    };

    const vec2f size_norm = {
        ((float)source_size.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f,
        ((float)source_size.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f,
    };

    const vec2f texture_pos_norm = {
        (float)texture_pos.x / (source_texture_size.x == 0 ? 1.0f : (float)source_texture_size.x),
        (float)texture_pos.y / (source_texture_size.y == 0 ? 1.0f : (float)source_texture_size.y),
    };

    const vec2f texture_size_norm = {
        (float)texture_size.x / (source_texture_size.x == 0 ? 1.0f : (float)source_texture_size.x),
        (float)texture_size.y / (source_texture_size.y == 0 ? 1.0f : (float)source_texture_size.y),
    };

    const float vertices[] = {
        -1.0f + pos_norm.x,               -1.0f + pos_norm.y + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + pos_norm.x,               -1.0f + pos_norm.y,               texture_pos_norm.x,                       texture_pos_norm.y,
        -1.0f + pos_norm.x + size_norm.x, -1.0f + pos_norm.y,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,

        -1.0f + pos_norm.x,               -1.0f + pos_norm.y + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + pos_norm.x + size_norm.x, -1.0f + pos_norm.y,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,
        -1.0f + pos_norm.x + size_norm.x, -1.0f + pos_norm.y + size_norm.y, texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y + texture_size_norm.y
    };

    self->params.egl->glBindVertexArray(self->vertex_array_object_id);
    self->params.egl->glViewport(0, 0, dest_texture_size.x, dest_texture_size.y);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, texture_id);

    /* TODO: this, also cleanup */
    //self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferSubData(GL_ARRAY_BUFFER, 0, 24 * sizeof(float), vertices);

    {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT); // TODO: Do this in a separate clear_ function. We want to do that when using multiple drm to create the final image (multiple monitors for example)

        gsr_shader_use(&self->shaders[0]);
        self->params.egl->glUniform1f(self->rotation_uniforms[0], rotation);
        self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        //cap_xcomp->egl.glClear(GL_COLOR_BUFFER_BIT);

        gsr_shader_use(&self->shaders[1]);
        self->params.egl->glUniform1f(self->rotation_uniforms[1], rotation);
        self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    self->params.egl->glBindVertexArray(0);
    gsr_shader_use_none(&self->shaders[0]);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
}
