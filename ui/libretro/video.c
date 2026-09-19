/*
 * xemu libretro frontend - video
 *
 * The core always hands the frontend a software framebuffer, which keeps it
 * independent of the frontend's video driver. Where the frame comes from
 * depends on the renderer:
 *
 * - The OpenGL renderer hands out a texture in a context group owned by the
 *   core. It is drawn into an FBO and read back.
 * - The Vulkan renderer reads its display image back itself. No GL is
 *   involved, which is what makes platforms without desktop GL possible.
 * - When the guest does not use the GPU at all, the VGA surface in guest
 *   memory is used, as the standalone UI does.
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
#include "hw/xbox/nv2a/debug.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "xemu-libretro.h"
#include "libretro.h"

#ifdef CONFIG_OPENGL
#include <epoxy/gl.h>
#include <SDL3/SDL.h>
#endif

static unsigned max_width = 1920, max_height = 1080;
static unsigned surface_scale = 1;

static uint32_t *frame_buf;
static size_t frame_buf_size;

static GMutex surface_lock;
static DisplaySurface *vga_surface;

void xemu_libretro_video_set_surface(DisplaySurface *surface)
{
    g_mutex_lock(&surface_lock);
    vga_surface = surface;
    g_mutex_unlock(&surface_lock);
}

void xemu_libretro_video_set_scale(unsigned scale, unsigned width,
                                   unsigned height)
{
    surface_scale = MAX(1, scale);
    max_width = width;
    max_height = height;
}

static void reserve_frame(unsigned width, unsigned height)
{
    size_t size = (size_t)width * height * sizeof(uint32_t);
    if (size > frame_buf_size) {
        frame_buf = g_realloc(frame_buf, size);
        frame_buf_size = size;
    }
}

/* Keep what is handed to the frontend within the advertised maximum */
static void fit_size(unsigned *width, unsigned *height)
{
    if (*width > max_width || *height > max_height) {
        double scale = MIN((double)max_width / *width,
                           (double)max_height / *height);
        *width = MAX(1, (unsigned)(*width * scale));
        *height = MAX(1, (unsigned)(*height * scale));
    }
}

/* ------------------------------------------------------------------------ */
/* OpenGL renderer: draw the texture into an FBO and read it back           */

#ifdef CONFIG_OPENGL

static SDL_Window *window;
static SDL_GLContext context;

static GLuint prog, vao;
static GLint flip_loc, tex_loc, palette_loc;

static GLuint fbo, fbo_tex;
static unsigned fbo_width, fbo_height;

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

static bool init_gl(void)
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

    return init_blit();
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
}

static bool render_gl_texture(GLuint tex, XemuLibretroFrame *frame)
{
    GLint tex_width = 0, tex_height = 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tex_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &tex_height);
    if (tex_width <= 0 || tex_height <= 0) {
        return false;
    }

    unsigned width = tex_width, height = tex_height;
    fit_size(&width, &height);
    resize_target(width, height);
    reserve_frame(width, height);

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

        /*
         * The texture follows the GL convention (origin at the bottom left),
         * and so does glReadPixels. libretro wants the top row first.
         */
        glUseProgram(prog);
        glUniform1i(flip_loc, true);
        glUniform1i(tex_loc, 0);
        glUniform1uiv(palette_loc, 256, palette);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, frame_buf);

    frame->width = width;
    frame->height = height;
    frame->widescreen = tex_height / surface_scale >= 720;
    return true;
}

#endif /* CONFIG_OPENGL */

/* ------------------------------------------------------------------------ */
/* Frames that are already in memory                                        */

/* The RAMDAC gamma ramp, or NULL while it is the identity */
static const uint8_t *get_gamma_ramp(void)
{
    const uint8_t *palette = nv2a_get_dac_palette();
    for (int i = 0; i < 256; i++) {
        if (palette[i * 3] != i || palette[i * 3 + 1] != i ||
            palette[i * 3 + 2] != i) {
            return palette;
        }
    }
    return NULL;
}

static inline uint32_t apply_gamma(const uint8_t *ramp, uint32_t px)
{
    return (ramp[((px >> 16) & 0xff) * 3] << 16) |
           (ramp[((px >> 8) & 0xff) * 3 + 1] << 8) |
           ramp[(px & 0xff) * 3 + 2];
}

/*
 * Copy XRGB8888 rows into the frame, scaling down (nearest) when the source is
 * larger than what the frontend was promised.
 */
static void copy_xrgb8888(const uint8_t *src, unsigned src_width,
                          unsigned src_height, int src_stride, bool bottom_up,
                          XemuLibretroFrame *frame)
{
    unsigned width = src_width, height = src_height;
    fit_size(&width, &height);
    reserve_frame(width, height);

    const uint8_t *ramp = get_gamma_ramp();
    bool blank = nv2a_get_screen_off();

    for (unsigned y = 0; y < height; y++) {
        unsigned sy = (uint64_t)y * src_height / height;
        if (bottom_up) {
            sy = src_height - 1 - sy;
        }
        const uint32_t *in = (const uint32_t *)(src + (size_t)sy * src_stride);
        uint32_t *out = frame_buf + (size_t)y * width;

        if (blank) {
            memset(out, 0, width * sizeof(uint32_t));
        } else if (width == src_width && !ramp) {
            memcpy(out, in, width * sizeof(uint32_t));
        } else {
            for (unsigned x = 0; x < width; x++) {
                uint32_t px = in[(uint64_t)x * src_width / width];
                out[x] = ramp ? apply_gamma(ramp, px) : px;
            }
        }
    }

    frame->width = width;
    frame->height = height;
}

static bool copy_vga_surface(XemuLibretroFrame *frame)
{
    bool ok = false;

    xemu_main_loop_lock();
    g_mutex_lock(&surface_lock);

    DisplaySurface *surface = vga_surface;
    if (surface && surface_width(surface) > 0 && surface_height(surface) > 0) {
        unsigned width = surface_width(surface);
        unsigned height = surface_height(surface);

        switch (surface_format(surface)) {
        case PIXMAN_x8r8g8b8:
        case PIXMAN_a8r8g8b8:
            copy_xrgb8888(surface_data(surface), width, height,
                          surface_stride(surface), false, frame);
            ok = true;
            break;
        case PIXMAN_r5g6b5: {
            /* Expand into a scratch image first; this is a rare format */
            uint32_t *tmp = g_malloc((size_t)width * height * 4);
            for (unsigned y = 0; y < height; y++) {
                const uint16_t *in =
                    (const uint16_t *)((uint8_t *)surface_data(surface) +
                                       (size_t)y * surface_stride(surface));
                for (unsigned x = 0; x < width; x++) {
                    unsigned r = in[x] >> 11, g = (in[x] >> 5) & 0x3f,
                             b = in[x] & 0x1f;
                    tmp[(size_t)y * width + x] =
                        ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) |
                        (b << 3 | b >> 2);
                }
            }
            copy_xrgb8888((uint8_t *)tmp, width, height, width * 4, false,
                          frame);
            g_free(tmp);
            ok = true;
            break;
        }
        default:
            break;
        }

        frame->widescreen = height >= 720;
    }

    g_mutex_unlock(&surface_lock);
    xemu_main_loop_unlock();
    return ok;
}

/* ------------------------------------------------------------------------ */

static bool uses_gl(void)
{
#ifdef CONFIG_OPENGL
    return g_config.display.renderer == CONFIG_DISPLAY_RENDERER_OPENGL;
#else
    return false;
#endif
}

bool xemu_libretro_video_init(void)
{
#ifdef CONFIG_OPENGL
    if (uses_gl() && !init_gl()) {
        return false;
    }
#endif

    /* The renderer creates its contexts, shared with the one current now */
    nv2a_context_init();

    return xemu_libretro_video_make_current();
}

bool xemu_libretro_video_make_current(void)
{
#ifdef CONFIG_OPENGL
    if (uses_gl()) {
        return SDL_GL_MakeCurrent(window, context);
    }
#endif
    return true;
}

void xemu_libretro_video_release_current(void)
{
#ifdef CONFIG_OPENGL
    if (uses_gl()) {
        SDL_GL_MakeCurrent(NULL, NULL);
    }
#endif
}

void xemu_libretro_video_pump_events(void)
{
#ifdef CONFIG_OPENGL
    if (uses_gl()) {
        /* Hidden windows still own a message queue that has to be served */
        SDL_PumpEvents();
    }
#endif
}

/* Per guest frame averages of the renderer's counters since the last call */
static void log_renderer_stats(void)
{
    static unsigned int last_frame_count;
    unsigned int frames = g_nv2a_stats.frame_count - last_frame_count;
    last_frame_count = g_nv2a_stats.frame_count;
    if (frames == 0) {
        return;
    }
    frames = MIN(frames, NV2A_PROF_NUM_FRAMES);

    GString *line = g_string_new(NULL);
    int64_t ms = 0;
    for (unsigned int i = 0; i < frames; i++) {
        unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES -
                            1 - i) % NV2A_PROF_NUM_FRAMES;
        ms += g_nv2a_stats.frame_history[idx].mspf;
    }
    g_string_append_printf(line, "frames=%u mspf=%.1f", frames,
                           (double)ms / frames);
    for (unsigned int cnt = 0; cnt < NV2A_PROF__COUNT; cnt++) {
        int64_t sum = 0;
        for (unsigned int i = 0; i < frames; i++) {
            unsigned int idx = (g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES -
                                1 - i) % NV2A_PROF_NUM_FRAMES;
            sum += g_nv2a_stats.frame_history[idx].counters[cnt];
        }
        if (sum) {
            g_string_append_printf(line, " %s=%.1f",
                                   nv2a_profile_get_counter_name(cnt),
                                   (double)sum / frames);
        }
    }
    xemu_libretro_log(RETRO_LOG_DEBUG, "nv2a: %s\n", line->str);
    g_string_free(line, TRUE);
}

bool xemu_libretro_video_render(XemuLibretroFrame *frame)
{
    static int debug = -1;
    static unsigned debug_count;
    const char *path = "none";
    bool ok = false;

    int tex = nv2a_get_framebuffer_surface();
    if (tex) {
#ifdef CONFIG_OPENGL
        ok = render_gl_texture(tex, frame);
        path = "gl texture";
#endif
    } else {
        int width, height, stride;
        const uint8_t *pixels =
            nv2a_get_framebuffer_pixels(&width, &height, &stride);
        if (pixels) {
            /* The display image is composed upside down, for GL's benefit */
            copy_xrgb8888(pixels, width, height, stride, true, frame);
            frame->widescreen = height / surface_scale >= 720;
            path = "readback";
            ok = true;
        }
    }
    nv2a_release_framebuffer_surface();

    if (!ok) {
        /* The guest is not rendering with the GPU */
        ok = copy_vga_surface(frame);
        path = "vga";
    }

    if (debug < 0) {
        debug = g_getenv("XEMU_LIBRETRO_DEBUG") != NULL;
    }
    if (debug && (debug_count++ % 120) == 0) {
        xemu_libretro_log(RETRO_LOG_DEBUG, "render: %s ok=%d screen_off=%d\n",
                          path, ok, nv2a_get_screen_off());
        log_renderer_stats();
    }

    if (!ok) {
        return false;
    }

    frame->data = frame_buf;
    frame->pitch = (size_t)frame->width * sizeof(uint32_t);
    frame->widescreen |= xemu_get_widescreen();
    return true;
}
