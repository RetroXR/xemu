/*
 * xemu libretro frontend - audio
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
#include "xemu-libretro.h"

/* ~340ms of audio. The APU throttles itself well below this. */
#define RING_FRAMES 16384

static GMutex ring_lock;
static int16_t ring[RING_FRAMES][2];
static size_t ring_read, ring_count;

void xemu_libretro_audio_reset(void)
{
    g_mutex_lock(&ring_lock);
    ring_read = ring_count = 0;
    g_mutex_unlock(&ring_lock);
}

void xemu_libretro_audio_push(const int16_t *frames, size_t num_frames)
{
    g_mutex_lock(&ring_lock);

    if (num_frames > RING_FRAMES) {
        frames += (num_frames - RING_FRAMES) * 2;
        num_frames = RING_FRAMES;
    }

    /* Nobody is draining: drop the oldest audio rather than the newest */
    size_t free_frames = RING_FRAMES - ring_count;
    if (num_frames > free_frames) {
        size_t drop = num_frames - free_frames;
        ring_read = (ring_read + drop) % RING_FRAMES;
        ring_count -= drop;
    }

    size_t write = (ring_read + ring_count) % RING_FRAMES;
    size_t first = MIN(num_frames, RING_FRAMES - write);
    memcpy(ring[write], frames, first * sizeof(ring[0]));
    memcpy(ring[0], frames + first * 2, (num_frames - first) * sizeof(ring[0]));
    ring_count += num_frames;

    g_mutex_unlock(&ring_lock);
}

int xemu_libretro_audio_queued_bytes(void)
{
    g_mutex_lock(&ring_lock);
    int queued = ring_count * sizeof(ring[0]);
    g_mutex_unlock(&ring_lock);
    return queued;
}

size_t xemu_libretro_audio_pop(int16_t *frames, size_t max_frames)
{
    g_mutex_lock(&ring_lock);

    size_t num_frames = MIN(max_frames, ring_count);
    size_t first = MIN(num_frames, RING_FRAMES - ring_read);
    memcpy(frames, ring[ring_read], first * sizeof(ring[0]));
    memcpy(frames + first * 2, ring[0], (num_frames - first) * sizeof(ring[0]));
    ring_read = (ring_read + num_frames) % RING_FRAMES;
    ring_count -= num_frames;

    g_mutex_unlock(&ring_lock);
    return num_frames;
}
