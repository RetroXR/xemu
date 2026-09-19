/*
 * xemu libretro frontend - replacements for the standalone UI
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
#include "ui/xemu-notifications.h"
#include "ui/xemu-snapshots.h"
#include "xemu-libretro.h"
#include "libretro.h"

/*
 * Notifications are raised on QEMU threads, but the frontend may only be
 * called from the thread running retro_run().
 */
static GMutex msg_lock;
static GQueue msg_queue = G_QUEUE_INIT;

typedef struct QueuedMessage {
    int level;
    char *text;
} QueuedMessage;

/* Logged when flushed: only then is it safe to use the frontend's log */
static void queue_message(int level, const char *msg)
{
    QueuedMessage *m = g_new(QueuedMessage, 1);
    m->level = level;
    m->text = g_strdup(msg);

    g_mutex_lock(&msg_lock);
    g_queue_push_tail(&msg_queue, m);
    g_mutex_unlock(&msg_lock);
}

void xemu_libretro_queue_message(int level, const char *msg)
{
    queue_message(level, msg);
}

void xemu_queue_notification(const char *msg)
{
    queue_message(RETRO_LOG_INFO, msg);
}

void xemu_queue_error_message(const char *msg)
{
    queue_message(RETRO_LOG_ERROR, msg);
}

void xemu_libretro_flush_messages(bool (*environ_cb)(unsigned cmd, void *data))
{
    for (;;) {
        g_mutex_lock(&msg_lock);
        QueuedMessage *msg = g_queue_pop_head(&msg_queue);
        g_mutex_unlock(&msg_lock);
        if (!msg) {
            break;
        }

        xemu_libretro_log(msg->level, "%s\n", msg->text);
        struct retro_message m = { msg->text, 300 };
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
        g_free(msg->text);
        g_free(msg);
    }
}

/* Snapshot thumbnails are rendered by the standalone UI */
void xemu_snapshots_set_framebuffer_texture(GLuint tex, bool flip)
{
}

bool xemu_snapshots_load_png_to_texture(GLuint tex, void *buf, size_t size)
{
    return false;
}

void *xemu_snapshots_create_framebuffer_thumbnail_png(size_t *size)
{
    *size = 0;
    return NULL;
}
