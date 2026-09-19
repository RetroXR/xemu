/*
 * xemu libretro frontend - input
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
#include "ui/xemu-input.h"
#include "ui/xemu-notifications.h"
#include "fatx.h"
#include "xemu-libretro.h"

static GMutex pad_lock;
static XemuLibretroPadState pad_state[XEMU_LIBRETRO_NUM_PORTS];
static bool port_connected[XEMU_LIBRETRO_NUM_PORTS] = { true };
static bool port_controller_s[XEMU_LIBRETRO_NUM_PORTS];
static char *port_memory_unit[XEMU_LIBRETRO_NUM_PORTS];
static uint16_t rumble[XEMU_LIBRETRO_NUM_PORTS][2];
static bool rumble_dirty[XEMU_LIBRETRO_NUM_PORTS];

static ControllerState controllers[XEMU_LIBRETRO_NUM_PORTS];
static const char *controller_names[XEMU_LIBRETRO_NUM_PORTS] = {
    "RetroPad 1", "RetroPad 2", "RetroPad 3", "RetroPad 4",
};

void xemu_libretro_input_set_pad_state(int port,
                                       const XemuLibretroPadState *state)
{
    g_mutex_lock(&pad_lock);
    pad_state[port] = *state;
    g_mutex_unlock(&pad_lock);
}

void xemu_libretro_input_set_port_device(int port, bool connected,
                                         bool controller_s)
{
    g_mutex_lock(&pad_lock);
    port_connected[port] = connected;
    port_controller_s[port] = controller_s;
    g_mutex_unlock(&pad_lock);
}

void xemu_libretro_input_set_memory_unit(int port, const char *path)
{
    g_mutex_lock(&pad_lock);
    g_free(port_memory_unit[port]);
    port_memory_unit[port] = g_strdup(path);
    g_mutex_unlock(&pad_lock);
}

bool xemu_libretro_input_get_rumble(int port, uint16_t *left, uint16_t *right)
{
    g_mutex_lock(&pad_lock);
    bool dirty = rumble_dirty[port];
    rumble_dirty[port] = false;
    *left = rumble[port][0];
    *right = rumble[port][1];
    g_mutex_unlock(&pad_lock);
    return dirty;
}

void xemu_libretro_input_update_controller_state(ControllerState *state)
{
    int port = state - controllers;
    assert(port >= 0 && port < XEMU_LIBRETRO_NUM_PORTS);

    g_mutex_lock(&pad_lock);
    state->buttons = pad_state[port].buttons;
    memcpy(state->axis, pad_state[port].axis, sizeof(state->axis));
    g_mutex_unlock(&pad_lock);
}

void xemu_libretro_input_update_rumble(ControllerState *state)
{
    int port = state - controllers;
    assert(port >= 0 && port < XEMU_LIBRETRO_NUM_PORTS);

    g_mutex_lock(&pad_lock);
    if (rumble[port][0] != state->rumble_l ||
        rumble[port][1] != state->rumble_r) {
        rumble[port][0] = state->rumble_l;
        rumble[port][1] = state->rumble_r;
        rumble_dirty[port] = true;
    }
    g_mutex_unlock(&pad_lock);
}

void xemu_libretro_input_init(void)
{
    assert(bql_locked());

    for (int i = 0; i < XEMU_LIBRETRO_NUM_PORTS; i++) {
        ControllerState *con = &controllers[i];
        memset(con, 0, sizeof(*con));
        con->type = INPUT_DEVICE_LIBRETRO;
        con->name = controller_names[i];
        con->bound = -1;
        QTAILQ_INSERT_TAIL(&available_controllers, con, entry);
    }

    xemu_libretro_input_sync_ports();
}

/* Memory units go into the top expansion slot */
#define MEMORY_UNIT_SLOT 0
#define MEMORY_UNIT_SIZE (8 * 1024 * 1024)

static void sync_memory_unit(int port, const char *path)
{
    ControllerState *con = &controllers[port];
    XmuState *xmu = con->peripherals[MEMORY_UNIT_SLOT];
    const char *current = xmu ? xmu->filename : NULL;

    if (!g_strcmp0(current, path)) {
        return;
    }

    if (xmu) {
        xemu_input_unbind_xmu(port, MEMORY_UNIT_SLOT);
        g_free(xmu);
        con->peripherals[MEMORY_UNIT_SLOT] = NULL;
        con->peripheral_types[MEMORY_UNIT_SLOT] = PERIPHERAL_NONE;
    }

    if (!path) {
        return;
    }

    if (!g_file_test(path, G_FILE_TEST_EXISTS) &&
        !create_fatx_image(path, MEMORY_UNIT_SIZE)) {
        char *msg = g_strdup_printf("Failed to create memory unit %s", path);
        xemu_queue_error_message(msg);
        g_free(msg);
        return;
    }

    con->peripheral_types[MEMORY_UNIT_SLOT] = PERIPHERAL_XMU;
    con->peripherals[MEMORY_UNIT_SLOT] = g_new0(XmuState, 1);
    xemu_input_bind_xmu(port, MEMORY_UNIT_SLOT, path, true);
}

void xemu_libretro_input_sync_ports(void)
{
    assert(bql_locked());

    for (int i = 0; i < XEMU_LIBRETRO_NUM_PORTS; i++) {
        g_mutex_lock(&pad_lock);
        bool connected = port_connected[i];
        const char *driver = port_controller_s[i] ? DRIVER_S : DRIVER_DUKE;
        char *memory_unit = g_strdup(port_memory_unit[i]);
        g_mutex_unlock(&pad_lock);

        bool bound = controllers[i].bound >= 0;
        if (bound && (!connected || strcmp(bound_drivers[i], driver))) {
            /* Unplugging the pad takes its memory unit along */
            xemu_input_bind(i, NULL, 0);
            bound = false;
        }
        if (connected && !bound) {
            bound_drivers[i] = driver;
            xemu_input_bind(i, &controllers[i], 0);
            bound = true;
        }
        if (bound) {
            sync_memory_unit(i, memory_unit);
        }

        g_free(memory_unit);
    }
}
