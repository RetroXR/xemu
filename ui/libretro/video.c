/*
 * xemu libretro frontend - video
 *
 * The NV2A renderers hand out the guest framebuffer as a GL texture that
 * lives in a context group owned by the core. The frame is drawn into an
 * offscreen FBO and read back, so the core works with any frontend video
 * driver.
 *
 * Copyright (c) 2026 xemu contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "ui/console.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-widescreen.h"
#include "ui/xui/xemu-hud.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "xemu-libretro.h"
#include "libretro.h"

#include <epoxy/gl.h>
#include <SDL3/SDL.h>

static SDL_Window *window;
static SDL_GLContext context;

static GLuint prog, vao;
static GLint flip_loc, tex_loc, palette_loc;

static GLuint fbo, fbo_tex;
static unsigned fbo_width, fbo_height;
static unsigned max_width = 1920, max_height = 1080;
static unsigned surface_scale = 1;

static uint32_t *frame_buf;
static size_t frame_buf_size;

static GMutex surface_lock;
static DisplaySurface *vga_surface;

static const char *vert_src =
    "#version 400 core\n"
    "uniform bool flip_y;\n"
    "out vec2 texcoord;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
    "    texcoord = flip_y ? vec2(p.x, 1.0 - p.y) : p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Same gamma ramp lookup as the standalone UI */
static const char *frag_src =
    "#version 400 core\n"
    "uniform sampler2D tex;\n"
    "uniform uint palette[256];\n"
    "in vec2 texcoord;\n"
    "out vec4 out_color;\n"
    "float gamma_ch(int ch, float col) {\n"
    "    return float(bitfieldExtract(palette[uint(col * 255.0)], ch * 8, 8))"
    " / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 col = texture(tex, texcoord);\n"
    "    out_color = vec4(gamma_ch(0, col.r), gamma_ch(1, col.g),"
    " gamma_ch(2, col.b), 1.0);\n"
    "}\n";

void xemu_libretro_video_set_surface(DisplaySurface *surface);

void xemu_libretro_video_set_surface(DisplaySurface *surface)
{
    g_mutex_lock(&surface_lock);
    vga_surface = surface;
    g_mutex_unlock(&surface_lock);
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);

    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char err_buf[512];
        glGetShaderInfoLog(shader, sizeof(err_buf), NULL, err_buf);
        xemu_libretro_log(RETRO_LOG_ERROR, "Shader compilation failed: %s\n",
                          err_buf);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool init_blit(void)
{
    GLuint vert = compile_shader(GL_VERTEX_SHADER, vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!vert || !frag) {
        return false;
    }

    prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glBindFragDataLocation(prog, 0, "out_color");
    glLinkProgram(prog);
    glDeleteShader(vert);
    glDeleteShader(frag);

    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        xemu_libretro_log(RETRO_LOG_ERROR, "Shader link failed\n");
        return false;
    }

    flip_loc = glGetUniformLocation(prog, "flip_y");
    tex_loc = glGetUniformLocation(prog, "tex");
    palette_loc = glGetUniformLocation(prog, "palette");

    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fbo);
    return true;
}

bool xemu_libretro_video_init(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        xemu_libretro_log(RETRO_LOG_ERROR,
                          "Failed to initialize SDL video subsystem: %s\n",
                          SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);

    window = SDL_CreateWindow("xemu libretro", 640, 480,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == NULL) {
        xemu_libretro_log(RETRO_LOG_ERROR, "Failed to create window: %s\n",
                          SDL_GetError());
        return false;
    }

    context = SDL_GL_CreateContext(window);
    if (context != NULL && epoxy_gl_version() < 40) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(context);
        context = NULL;
    }
    if (context == NULL) {
        xemu_libretro_log(RETRO_LOG_ERROR,
                          "Unable to create an OpenGL 4.0 context: %s\n",
                          SDL_GetError());
        SDL_DestroyWindow(window);
        window = NULL;
        return false;
    }

    xemu_libretro_log(RETRO_LOG_INFO, "GL_VENDOR: %s\n",
                      glGetString(GL_VENDOR));
    xemu_libretro_log(RETRO_LOG_INFO, "GL_RENDERER: %s\n",
                      glGetString(GL_RENDERER));
    xemu_libretro_log(RETRO_LOG_INFO, "GL_VERSION: %s\n",
                      glGetString(GL_VERSION));

    if (!init_blit()) {
        return false;
    }

    /* Creates the renderer contexts, shared with the one current now */
    nv2a_context_init();

    SDL_GL_MakeCurrent(window, context);
    return true;
}

bool xemu_libretro_video_make_current(void)
{
    return SDL_GL_MakeCurrent(window, context);
}

void xemu_libretro_video_finalize(void)
{
    g_free(frame_buf);
    frame_buf = NULL;
    frame_buf_size = 0;

    if (context) {
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(context);
        context = NULL;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = NULL;
    }
}

void xemu_libretro_video_set_scale(unsigned scale, unsigned width,
                                   unsigned height)
{
    surface_scale = MAX(1, scale);
    max_width = width;
    max_height = height;
}

static void upload_vga_surface(DisplaySurface *surface)
{
    GLenum format, type;

    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        format = GL_BGRA;
        type = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        format = GL_RGBA;
        type = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_r5g6b5:
        format = GL_RGB;
        type = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        g_assert_not_reached();
    }

    glPixelStorei(GL_UNPACK_ROW_LENGTH,
                  surface_stride(surface) / surface_bytes_per_pixel(surface));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, surface_width(surface),
                 surface_height(surface), 0, format, type,
                 surface_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void resize_target(unsigned width, unsigned height)
{
    if (width == fbo_width && height == fbo_height) {
        return;
    }

    if (fbo_tex) {
        glDeleteTextures(1, &fbo_tex);
    }
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, NULL);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fbo_tex, 0);

    fbo_width = width;
    fbo_height = height;

    size_t size = (size_t)width * height * sizeof(uint32_t);
    if (size > frame_buf_size) {
        frame_buf = g_realloc(frame_buf, size);
        frame_buf_size = size;
    }
}

bool xemu_libretro_video_render(XemuLibretroFrame *frame)
{
    GLuint vga_tex = 0;
    /*
     * Renderer surfaces follow the GL convention (origin at the bottom
     * left), and so does glReadPixels. libretro wants the top row first, so
     * flip those. Surfaces uploaded from guest memory are already top-down,
     * which the readback turns into the right order by itself.
     */
    bool flip = true;

    GLuint tex = nv2a_get_framebuffer_surface();
    if (tex == 0) {
        /*
         * The guest is not using accelerated rendering. Fall back to the
         * VGA surface, as the standalone UI does.
         */
        xemu_main_loop_lock();
        g_mutex_lock(&surface_lock);
        if (vga_surface) {
            glGenTextures(1, &vga_tex);
            glBindTexture(GL_TEXTURE_2D, vga_tex);
            upload_vga_surface(vga_surface);
        }
        g_mutex_unlock(&surface_lock);
        xemu_main_loop_unlock();

        tex = vga_tex;
        flip = false;
    }

    static int debug = -1;
    static unsigned debug_count;
    if (debug < 0) {
        debug = g_getenv("XEMU_LIBRETRO_DEBUG") != NULL;
    }
    if (debug && (debug_count++ % 120) == 0) {
        xemu_libretro_log(RETRO_LOG_DEBUG,
                          "render: %s tex=%u screen_off=%d vga_surface=%p\n",
                          flip ? "nv2a" : "vga", tex, nv2a_get_screen_off(),
                          vga_surface);
    }

    if (tex == 0) {
        nv2a_release_framebuffer_surface();
        return false;
    }

    GLint tex_width = 0, tex_height = 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_height);
    if (tex_width <= 0 || tex_height <= 0) {
        if (vga_tex) {
            glDeleteTextures(1, &vga_tex);
        }
        nv2a_release_framebuffer_surface();
        return false;
    }

    unsigned width = tex_width, height = tex_height;
    if (width > max_width || height > max_height) {
        double scale = MIN((double)max_width / width,
                           (double)max_height / height);
        width = MAX(1, (unsigned)(width * scale));
        height = MAX(1, (unsigned)(height * scale));
    }
    resize_target(width, height);

    bool linear = g_config.display.filtering == CONFIG_DISPLAY_FILTERING_LINEAR;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    linear ? GL_LINEAR : GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!nv2a_get_screen_off()) {
        uint32_t palette[256];
        const uint8_t *dac_palette = nv2a_get_dac_palette();
        for (int i = 0; i < 256; i++) {
            palette[i] = (dac_palette[i * 3 + 2] << 16) |
                         (dac_palette[i * 3 + 1] << 8) | dac_palette[i * 3];
        }

        glUseProgram(prog);
        glUniform1i(flip_loc, flip);
        glUniform1i(tex_loc, 0);
        glUniform1uiv(palette_loc, 256, palette);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame_buf);

    if (vga_tex) {
        glDeleteTextures(1, &vga_tex);
    }
    nv2a_release_framebuffer_surface();

    frame->data = frame_buf;
    frame->width = width;
    frame->height = height;
    frame->pitch = (size_t)width * sizeof(uint32_t);
    /* 720p and 1080i are widescreen modes; tell them by the native height */
    unsigned scale = flip ? surface_scale : 1;
    frame->widescreen = tex_height / scale >= 720 || xemu_get_widescreen();
    return true;
}
