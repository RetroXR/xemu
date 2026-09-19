/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * libretro monitor output: frames are queued for the frontend to drain
 * instead of being sent to an audio device.
 *
 * Copyright (c) 2026 xemu contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_int.h"
#include "ui/libretro/xemu-libretro.h"

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    d->monitor.stream = NULL;

    /*
     * The frontend drains roughly one video frame of audio at a time
     * (~17-20ms), so keep a little more than that queued.
     */
    int frame_bytes = sizeof(d->monitor.frame_buf);
    d->monitor.queued_bytes_low = 6 * frame_bytes;
    d->monitor.queued_bytes_high = 12 * frame_bytes;

    xemu_libretro_audio_reset();
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
}

bool mcpx_apu_monitor_is_active(MCPXAPUState *d)
{
    return true;
}

int mcpx_apu_monitor_get_queued_bytes(MCPXAPUState *d)
{
    return xemu_libretro_audio_queued_bytes();
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    float vu = pow(fmax(0.0, fmin(g_config.audio.volume_limit, 1.0)), M_E);
    if (vu < 1.0f) {
        for (int i = 0; i < ARRAY_SIZE(d->monitor.frame_buf); i++) {
            d->monitor.frame_buf[i][0] *= vu;
            d->monitor.frame_buf[i][1] *= vu;
        }
    }

    xemu_libretro_audio_push(&d->monitor.frame_buf[0][0],
                             ARRAY_SIZE(d->monitor.frame_buf));

    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
}
