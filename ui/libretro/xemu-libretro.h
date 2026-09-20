/*
 * xemu libretro frontend
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

#ifndef XEMU_LIBRETRO_H
#define XEMU_LIBRETRO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Threading model
 *
 * The libretro frontend calls into the core from its own thread, which is
 * not known to QEMU. All interaction with QEMU therefore happens on the
 * "core thread", which takes the place of the main thread in standalone
 * xemu. The functions below are the mailboxes between the two.
 */

/* Take the locks a thread needs to call into QEMU, as ui/xemu.c does */
void xemu_main_loop_lock(void);
void xemu_main_loop_unlock(void);

/* Logging through the frontend log interface, falling back to stderr */
void xemu_libretro_log(int level, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

/* Audio: written by the APU thread, drained by retro_run */
#define XEMU_LIBRETRO_AUDIO_RATE 48000
void xemu_libretro_audio_reset(void);
void xemu_libretro_audio_push(const int16_t *frames, size_t num_frames);
int xemu_libretro_audio_queued_bytes(void);
size_t xemu_libretro_audio_pop(int16_t *frames, size_t max_frames);

/* Input: written by retro_run, read by the emulated controllers */
#define XEMU_LIBRETRO_NUM_PORTS 4
typedef struct XemuLibretroPadState {
    uint16_t buttons; /* CONTROLLER_BUTTON_* */
    int16_t axis[6]; /* CONTROLLER_AXIS_* */
} XemuLibretroPadState;

void xemu_libretro_input_set_pad_state(int port,
                                       const XemuLibretroPadState *state);
void xemu_libretro_input_set_port_device(int port, bool connected,
                                         bool controller_s);
#define XEMU_LIBRETRO_NUM_SLOTS 2
void xemu_libretro_input_set_memory_unit(int port, int slot, const char *path);
void xemu_libretro_input_report_memory_units(void);
void xemu_libretro_queue_message(int level, const char *msg);
bool xemu_libretro_input_get_rumble(int port, uint16_t *left, uint16_t *right);

/* Called on the core thread, with the BQL held (init is in xemu-input.h) */
void xemu_libretro_input_sync_ports(void);

/* Video: rendered and read back on the core thread */
typedef struct XemuLibretroFrame {
    const uint32_t *data; /* XRGB8888, top-down */
    unsigned width, height;
    size_t pitch;
    bool widescreen;
    bool duplicate; /* Same as the frame before */
} XemuLibretroFrame;

struct DisplaySurface;

bool xemu_libretro_video_init(void);
void xemu_libretro_video_set_surface(struct DisplaySurface *surface);
bool xemu_libretro_video_make_current(void);
void xemu_libretro_video_release_current(void);
void xemu_libretro_video_pump_events(void);
void xemu_libretro_video_finalize(void);
void xemu_libretro_video_set_scale(unsigned scale, unsigned max_width,
                                   unsigned max_height);
bool xemu_libretro_video_render(XemuLibretroFrame *frame);

void xemu_libretro_options_register(bool (*environ_cb)(unsigned cmd,
                                                        void *data));

/* Messages for the user, shown from retro_run */
void xemu_libretro_flush_messages(bool (*environ_cb)(unsigned cmd, void *data));

#ifdef __cplusplus
}
#endif

#endif
