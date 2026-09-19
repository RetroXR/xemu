/*
 * xemu libretro frontend
 *
 * Takes the place of ui/xemu.c when xemu is built as a libretro core.
 *
 * QEMU is neither frame-stepped nor re-entrant, so the machine free-runs on
 * its own threads exactly as it does in standalone xemu. retro_run() only
 * picks up the most recent frame, drains the audio queue and forwards input.
 * For the same reason the machine is created once per process: unloading a
 * game pauses it, and loading the next one swaps the disc and resets.
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
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "ui/console.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "tcg/startup.h"
#include "hw/xbox/smbus.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "ui/xemu-input.h"
#include "ui/xemu-settings.h"
#include "xemu-version.h"
#include "xemu-libretro.h"
#include "libretro.h"

#include <locale.h>
#include <glib/gstdio.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#define BASE_WIDTH 640
#define BASE_HEIGHT 480
#define VBLANK_INTERVAL_NS 16666666LL
#define FRAME_TIMEOUT_US 100000
#define START_TIMEOUT_US (60 * G_USEC_PER_SEC)
#define AUDIO_FRAMES_PER_RUN 2048
#define FRAME_RATE 60.0
#define DEVICE_CONTROLLER_S RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;
static struct retro_rumble_interface rumble_iface;
static bool have_rumble;
static bool can_dupe;
static GThread *retro_thread;

typedef enum FrameState {
    FRAME_IDLE,
    FRAME_REQUESTED,
    FRAME_READY,
} FrameState;

/* Everything below is shared with the core thread, under state_lock */
static GMutex state_lock;
static GCond state_cond;
static enum { CORE_STOPPED, CORE_STARTING, CORE_RUNNING, CORE_FAILED } core_state;
static FrameState frame_state;
static bool frame_valid;
static XemuLibretroFrame frame;
static bool cmd_reset, cmd_pause, cmd_resume, cmd_eject, cmd_sync_ports;
static char *cmd_load_disc;
static int cmd_scale;

static QemuThread core_thread, qemu_thread, vblank_thread;
static QemuSemaphore display_init_sem;
static bool qemu_exiting;
static DisplayChangeListener dcl;

static unsigned geom_width = BASE_WIDTH, geom_height = BASE_HEIGHT;
static bool geom_widescreen;
static bool geom_dirty;
static enum { ASPECT_AUTO, ASPECT_4_3, ASPECT_16_9 } aspect_mode;
static int surface_scale = 1;
static bool game_loaded;

/* ------------------------------------------------------------------------ */

void xemu_libretro_log(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Frontends do not promise that their callbacks are thread safe */
    if (log_cb && g_thread_self() == retro_thread) {
        log_cb((enum retro_log_level)level, "[xemu] %s", buf);
    } else {
        fprintf(stderr, "[xemu] %s", buf);
    }
}

void xemu_main_loop_lock(void)
{
    qemu_mutex_lock_main_loop();
    bql_lock();
}

void xemu_main_loop_unlock(void)
{
    bql_unlock();
    qemu_mutex_unlock_main_loop();
}

static void eject_disc(Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    qmp_eject("ide0-cd1", NULL, true, false, &error);
    if (error) {
        error_propagate(errp, error);
    }

    xbox_smc_update_tray_state();
}

static void load_disc(const char *path, Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    qmp_blockdev_change_medium("ide0-cd1", NULL, path, "raw", false, false,
                               false, 0, &error);
    if (error) {
        error_propagate(errp, error);
    } else {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, path);
    }

    xbox_smc_update_tray_state();
}

/* ------------------------------------------------------------------------ */
/* QEMU display                                                             */

static void xlr_gfx_switch(DisplayChangeListener *l, DisplaySurface *surface)
{
    xemu_libretro_video_set_surface(surface);
}

static bool xlr_gfx_check_format(DisplayChangeListener *l,
                                 pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name = "xemu-libretro",
    .dpy_gfx_switch = xlr_gfx_switch,
    .dpy_gfx_check_format = xlr_gfx_check_format,
};

/* Sleep for most of the wait, then spin: sleeping alone is too coarse */
static void delay_until(int64_t deadline_ns)
{
    const int64_t spin_ns = 1500000;

    int64_t remaining = deadline_ns - qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (remaining > spin_ns) {
        g_usleep((remaining - spin_ns) / 1000);
    }
    while (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < deadline_ns) {
        /* spin */
    }
}

static void *vblank_thread_fn(void *opaque)
{
    int64_t next_vblank = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (!qatomic_read(&qemu_exiting)) {
        next_vblank += VBLANK_INTERVAL_NS;

        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < next_vblank) {
            delay_until(next_vblank);
        } else if (now > next_vblank + VBLANK_INTERVAL_NS) {
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            xemu_main_loop_lock();
            graphic_hw_update(dcl.con);
            xemu_main_loop_unlock();
        }
    }

    return NULL;
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);
    display_opengl = 1;
}

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);

    QemuConsole *con = qemu_console_lookup_by_index(0);
    assert(con != NULL);
    dcl.ops = &dcl_ops;
    dcl.con = con;
    register_displaychangelistener(&dcl);

    qemu_thread_create(&vblank_thread, "vblank-timer", vblank_thread_fn, NULL,
                       QEMU_THREAD_JOINABLE);

    qemu_sem_post(&display_init_sem);
}

static QemuDisplay qemu_display_xemu = {
    .type = DISPLAY_TYPE_XEMU,
    .early_init = display_early_init,
    .init = display_init,
};

static void register_xemu_display(void)
{
    qemu_display_register(&qemu_display_xemu);
}

type_init(register_xemu_display);

/* ------------------------------------------------------------------------ */
/* Core thread                                                              */

static void *qemu_thread_fn(void *opaque)
{
    static char arg0[] = "xemu";
    static char *argv[] = { arg0, NULL };

    qemu_init(1, argv);
    qemu_main_loop();
    qatomic_set(&qemu_exiting, true);
    bql_unlock();
    qemu_mutex_unlock_main_loop();
    return NULL;
}

static void set_core_state(int state)
{
    g_mutex_lock(&state_lock);
    core_state = state;
    g_cond_broadcast(&state_cond);
    g_mutex_unlock(&state_lock);
}

static void process_commands(void)
{
    g_mutex_lock(&state_lock);
    bool reset = cmd_reset, pause = cmd_pause, resume = cmd_resume;
    bool eject = cmd_eject, sync_ports = cmd_sync_ports;
    char *disc_path = cmd_load_disc;
    int scale = cmd_scale;
    cmd_reset = cmd_pause = cmd_resume = cmd_eject = cmd_sync_ports = false;
    cmd_load_disc = NULL;
    cmd_scale = 0;
    g_mutex_unlock(&state_lock);

    if (!reset && !pause && !resume && !eject && !sync_ports && !disc_path &&
        !scale) {
        return;
    }

    xemu_main_loop_lock();

    if (scale) {
        nv2a_set_surface_scale_factor(scale);
    }
    if (sync_ports) {
        xemu_libretro_input_sync_ports();
    }
    if (pause && runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    if (eject) {
        Error *err = NULL;
        eject_disc(&err);
        if (err) {
            error_report_err(err);
        }
    }
    if (disc_path) {
        Error *err = NULL;
        load_disc(disc_path, &err);
        if (err) {
            error_report_err(err);
        }
        g_free(disc_path);
    }
    if (reset) {
        qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_UI);
    }
    if (resume && !runstate_is_running()) {
        vm_start();
    }

    xemu_main_loop_unlock();
}

static void *core_thread_fn(void *opaque)
{
    if (!xemu_libretro_video_init()) {
        set_core_state(CORE_FAILED);
        return NULL;
    }
    /* The renderer takes its contexts from here to its own thread */
    xemu_libretro_video_release_current();

    qemu_sem_init(&display_init_sem, 0);
    qemu_thread_create(&qemu_thread, "qemu_main", qemu_thread_fn, NULL,
                       QEMU_THREAD_JOINABLE);
    qemu_sem_wait(&display_init_sem);

    /* See main() in ui/xemu.c */
    tcg_register_init_ctx();
    qemu_set_current_aio_context(qemu_get_aio_context());

    xemu_main_loop_lock();
    xemu_input_init();
    xemu_main_loop_unlock();

    if (!xemu_libretro_video_make_current()) {
        set_core_state(CORE_FAILED);
        return NULL;
    }

    set_core_state(CORE_RUNNING);

    while (!qatomic_read(&qemu_exiting)) {
        xemu_libretro_video_pump_events();

        process_commands();

        g_mutex_lock(&state_lock);
        if (frame_state != FRAME_REQUESTED) {
            g_cond_wait_until(&state_cond, &state_lock,
                              g_get_monotonic_time() + 10000);
        }
        bool requested = frame_state == FRAME_REQUESTED;
        g_mutex_unlock(&state_lock);

        if (requested) {
            XemuLibretroFrame f;
            bool valid = xemu_libretro_video_render(&f);

            g_mutex_lock(&state_lock);
            frame = f;
            frame_valid = valid;
            frame_state = FRAME_READY;
            g_cond_broadcast(&state_cond);
            g_mutex_unlock(&state_lock);
        }
    }

    set_core_state(CORE_STOPPED);
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */

static const char *get_variable(const char *key)
{
    struct retro_variable var = { key, NULL };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        return var.value;
    }
    return NULL;
}

static bool variable_is(const char *key, const char *value, bool def)
{
    const char *v = get_variable(key);
    return v ? !strcmp(v, value) : def;
}

static int get_scale_variable(void)
{
    const char *v = get_variable("xemu_resolution_scale");
    int scale = v ? atoi(v) : 1;
    return CLAMP(scale, 1, 8);
}

/* Runtime-changeable options */
static void apply_variables(bool startup)
{
    g_config.display.filtering =
        variable_is("xemu_filtering", "nearest", false) ?
            CONFIG_DISPLAY_FILTERING_NEAREST :
            CONFIG_DISPLAY_FILTERING_LINEAR;

    g_config.audio.hrtf = variable_is("xemu_hrtf", "enabled", true);

    const char *aspect = get_variable("xemu_aspect_ratio");
    int mode = !g_strcmp0(aspect, "4:3")  ? ASPECT_4_3 :
               !g_strcmp0(aspect, "16:9") ? ASPECT_16_9 :
                                            ASPECT_AUTO;
    if (mode != aspect_mode) {
        aspect_mode = mode;
        geom_dirty = true;
    }

    const char *save_dir = NULL;
    environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
    for (int port = 0; port < XEMU_LIBRETRO_NUM_PORTS; port++) {
        char key[32];
        snprintf(key, sizeof(key), "xemu_memory_unit_port%d", port + 1);

        char *path = NULL;
        if (save_dir && *save_dir && variable_is(key, "enabled", false)) {
            char *dir = g_build_filename(save_dir, "xemu", NULL);
            char *name = g_strdup_printf("memory_unit_port%d.img", port + 1);
            g_mkdir_with_parents(dir, 0755);
            path = g_build_filename(dir, name, NULL);
            g_free(name);
            g_free(dir);
        }
        xemu_libretro_input_set_memory_unit(port, path);
        g_free(path);
    }
    g_mutex_lock(&state_lock);
    cmd_sync_ports = true;
    g_mutex_unlock(&state_lock);

    int scale = get_scale_variable();
    if (scale != surface_scale || startup) {
        surface_scale = scale;
        xemu_libretro_video_set_scale(scale, MAX(1920, BASE_WIDTH * scale),
                                      MAX(1080, BASE_HEIGHT * scale));
        if (startup) {
            g_config.display.quality.surface_scale = scale;
        } else {
            g_mutex_lock(&state_lock);
            cmd_scale = scale;
            g_mutex_unlock(&state_lock);
        }
    }
}

static char *find_file(const char *dirs[], const char *names[],
                       const char *suffix, int64_t sizes[])
{
    for (int d = 0; dirs[d]; d++) {
        for (int n = 0; names && names[n]; n++) {
            char *path = g_build_filename(dirs[d], names[n], NULL);
            if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                return path;
            }
            g_free(path);
        }
    }

    /* Not under a well known name: take anything that looks right */
    for (int d = 0; dirs[d]; d++) {
        GDir *dir = g_dir_open(dirs[d], 0, NULL);
        if (!dir) {
            continue;
        }
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            char *lower = g_ascii_strdown(name, -1);
            bool match = g_str_has_suffix(lower, suffix);
            g_free(lower);
            if (!match) {
                continue;
            }

            char *path = g_build_filename(dirs[d], name, NULL);
            GStatBuf st;
            bool ok = g_stat(path, &st) == 0 && S_ISREG(st.st_mode);
            if (ok && sizes) {
                ok = false;
                for (int s = 0; sizes[s]; s++) {
                    ok |= st.st_size == sizes[s];
                }
            }
            if (ok) {
                g_dir_close(dir);
                return path;
            }
            g_free(path);
        }
        g_dir_close(dir);
    }

    return NULL;
}

static void show_message(const char *msg)
{
    struct retro_message m = { msg, 600 };
    xemu_libretro_log(RETRO_LOG_ERROR, "%s\n", msg);
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static bool configure(const char *game_path)
{
    const char *system_dir = NULL;
    if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) ||
        !system_dir || !*system_dir) {
        show_message("xemu: the frontend did not provide a system directory");
        return false;
    }

    /* Some frontends already hand every core a system directory of its own */
    char *system_name = g_path_get_basename(system_dir);
    char *xemu_dir = !g_ascii_strcasecmp(system_name, "xemu") ?
                         g_strdup(system_dir) :
                         g_build_filename(system_dir, "xemu", NULL);
    g_free(system_name);
    g_mkdir_with_parents(xemu_dir, 0755);

    /* Shader cache, EEPROM and the optional xemu.toml all live here */
    char *base_path = g_strconcat(xemu_dir, G_DIR_SEPARATOR_S, NULL);
    xemu_settings_set_base_path(base_path);
    g_free(base_path);

    if (!xemu_settings_load()) {
        xemu_libretro_log(RETRO_LOG_WARN, "%s",
                          xemu_settings_get_error_message());
    }

    const char *dirs[] = { xemu_dir, system_dir, NULL };

    const char *bootrom_names[] = { "mcpx_1.0.bin", "mcpx.bin", NULL };
    int64_t bootrom_sizes[] = { 512, 0 };
    const char *flashrom_names[] = {
        "Complex_4627v1.03.bin", "Complex_4627.bin", "complex_4627.bin",
        "xbox_bios.bin", "bios.bin", NULL
    };
    int64_t flashrom_sizes[] = { 256 * 1024, 1024 * 1024, 0 };
    const char *hdd_names[] = { "xbox_hdd.qcow2", NULL };
    const char *eeprom_names[] = { "xbox_eeprom.bin", "eeprom.bin", NULL };
    int64_t eeprom_sizes[] = { 256, 0 };

    if (!g_file_test(g_config.sys.files.bootrom_path, G_FILE_TEST_IS_REGULAR)) {
        char *path = find_file(dirs, bootrom_names, ".bin", bootrom_sizes);
        xemu_settings_set_string(&g_config.sys.files.bootrom_path,
                                 path ?: "");
        g_free(path);
    }
    if (!g_file_test(g_config.sys.files.flashrom_path,
                     G_FILE_TEST_IS_REGULAR)) {
        char *path = find_file(dirs, flashrom_names, ".bin", flashrom_sizes);
        xemu_settings_set_string(&g_config.sys.files.flashrom_path,
                                 path ?: "");
        g_free(path);
    }
    if (!g_file_test(g_config.sys.files.hdd_path, G_FILE_TEST_IS_REGULAR)) {
        char *path = find_file(dirs, hdd_names, ".qcow2", NULL);
        xemu_settings_set_string(&g_config.sys.files.hdd_path, path ?: "");
        g_free(path);
    }

    if (!g_file_test(g_config.sys.files.eeprom_path, G_FILE_TEST_IS_REGULAR)) {
        /* Generated on first start when there is none */
        char *path = find_file(dirs, eeprom_names, "eeprom.bin", eeprom_sizes);
        if (!path) {
            path = g_build_filename(xemu_dir, eeprom_names[0], NULL);
        }
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, path);
        g_free(path);
    }

    xemu_libretro_log(RETRO_LOG_INFO, "MCPX boot ROM: %s\n",
                      g_config.sys.files.bootrom_path);
    xemu_libretro_log(RETRO_LOG_INFO, "Flash ROM: %s\n",
                      g_config.sys.files.flashrom_path);
    xemu_libretro_log(RETRO_LOG_INFO, "Hard disk: %s\n",
                      g_config.sys.files.hdd_path);

    /* QEMU exits the process on a missing BIOS, so catch that here */
    char *msg = NULL;
    if (!*g_config.sys.files.flashrom_path) {
        msg = g_strdup_printf("xemu: flash ROM / BIOS (e.g. "
                              "Complex_4627v1.03.bin) not found in %s",
                              xemu_dir);
    }
    /* Not needed by every BIOS or every program, so only complain */
    if (!*g_config.sys.files.bootrom_path) {
        xemu_libretro_log(RETRO_LOG_WARN, "MCPX boot ROM (mcpx_1.0.bin) not "
                          "found in %s\n", xemu_dir);
    }
    if (!*g_config.sys.files.hdd_path) {
        xemu_libretro_log(RETRO_LOG_WARN, "Hard disk image (xbox_hdd.qcow2) "
                          "not found in %s\n", xemu_dir);
    }
    g_free(xemu_dir);
    if (msg) {
        show_message(msg);
        g_free(msg);
        return false;
    }

    xemu_settings_set_string(&g_config.sys.files.dvd_path, game_path ?: "");

    g_config.general.show_welcome = false;
    g_config.general.updates.check = false;
    g_config.general.skip_boot_anim =
        variable_is("xemu_skip_boot_anim", "enabled", false);
    g_config.display.renderer = variable_is("xemu_renderer", "vulkan", false) ?
                                    CONFIG_DISPLAY_RENDERER_VULKAN :
                                    CONFIG_DISPLAY_RENDERER_OPENGL;
    g_config.sys.mem_limit = variable_is("xemu_memory", "128 MiB", false) ?
                                 CONFIG_SYS_MEM_LIMIT_128 :
                                 CONFIG_SYS_MEM_LIMIT_64;
    g_config.perf.hard_fpu = variable_is("xemu_hard_fpu", "enabled", true);
    g_config.perf.cache_shaders =
        variable_is("xemu_cache_shaders", "enabled", true);
    g_config.audio.use_dsp = variable_is("xemu_use_dsp", "enabled", false);

    const char *avpack = get_variable("xemu_avpack");
    static const struct {
        const char *name;
        CONFIG_SYS_AVPACK value;
    } avpacks[] = {
        { "hdtv", CONFIG_SYS_AVPACK_HDTV },
        { "composite", CONFIG_SYS_AVPACK_COMPOSITE },
        { "svideo", CONFIG_SYS_AVPACK_SVIDEO },
        { "scart", CONFIG_SYS_AVPACK_SCART },
        { "vga", CONFIG_SYS_AVPACK_VGA },
        { "rfu", CONFIG_SYS_AVPACK_RFU },
    };
    for (int i = 0; avpack && i < ARRAY_SIZE(avpacks); i++) {
        if (!strcmp(avpack, avpacks[i].name)) {
            g_config.sys.avpack = avpacks[i].value;
        }
    }

    apply_variables(true);
    return true;
}

/*
 * QEMU starts threads from constructors (RCU), and the machine outlives the
 * game, so the core must never be unmapped. This has to happen as the core
 * is loaded: frontends also load cores just to query them.
 */
static void __attribute__((constructor)) pin_module(void)
{
#ifdef _WIN32
    HMODULE module;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)pin_module, &module);
#else
    Dl_info info;
    if (dladdr(pin_module, &info) && info.dli_fname) {
        dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
    }
#endif
}

/* ------------------------------------------------------------------------ */
/* libretro API                                                             */

RETRO_API unsigned retro_api_version(void)
{
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    bool no_game = true;
    cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
    xemu_libretro_options_register(cb);

    static const struct retro_controller_description pads[] = {
        { "Xbox Controller", RETRO_DEVICE_JOYPAD },
        { "Xbox Controller S", DEVICE_CONTROLLER_S },
        { "None", RETRO_DEVICE_NONE },
    };
    static const struct retro_controller_info ports[] = {
        { pads, ARRAY_SIZE(pads) }, { pads, ARRAY_SIZE(pads) },
        { pads, ARRAY_SIZE(pads) }, { pads, ARRAY_SIZE(pads) },
        { NULL, 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)
{
    video_cb = cb;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)
{
    audio_cb = cb;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    audio_batch_cb = cb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t cb)
{
    input_poll_cb = cb;
}

RETRO_API void retro_set_input_state(retro_input_state_t cb)
{
    input_state_cb = cb;
}

RETRO_API void retro_init(void)
{
    retro_thread = g_thread_self();

    struct retro_log_callback log;
    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log)) {
        log_cb = log.log;
    }

    const char *log_path = g_getenv("XEMU_LIBRETRO_LOG");
    if (log_path && *log_path) {
        freopen(log_path, "a", stdout);
        freopen(log_path, "a", stderr);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "xemu";
    info->library_version = xemu_version;
    info->valid_extensions = "iso|xiso";
    info->need_fullpath = true;
    info->block_extract = true;
}

static void fill_geometry(struct retro_game_geometry *geom)
{
    geom->base_width = geom_width;
    geom->base_height = geom_height;
    geom->max_width = MAX(1920, BASE_WIDTH * surface_scale);
    geom->max_height = MAX(1080, BASE_HEIGHT * surface_scale);
    bool widescreen = aspect_mode == ASPECT_AUTO ? geom_widescreen :
                                                   aspect_mode == ASPECT_16_9;
    geom->aspect_ratio = widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    fill_geometry(&info->geometry);
    info->timing.fps = FRAME_RATE;
    info->timing.sample_rate = XEMU_LIBRETRO_AUDIO_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port >= XEMU_LIBRETRO_NUM_PORTS) {
        return;
    }

    xemu_libretro_input_set_port_device(port, device != RETRO_DEVICE_NONE,
                                        device == DEVICE_CONTROLLER_S);

    g_mutex_lock(&state_lock);
    cmd_sync_ports = true;
    g_mutex_unlock(&state_lock);
}

RETRO_API void retro_reset(void)
{
    g_mutex_lock(&state_lock);
    cmd_reset = true;
    g_mutex_unlock(&state_lock);
}

static void set_input_descriptors(void)
{
    static const struct {
        unsigned device, index, id;
        const char *description;
    } map[] = {
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,
          "D-Pad Right" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "White" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Black" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Left Trigger" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Right Trigger" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,
          "Left Stick Button" },
        { RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,
          "Right Stick Button" },
        { RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
          RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" },
        { RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
          RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" },
        { RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
          RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" },
        { RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
          RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
    };
    static struct retro_input_descriptor
        desc[XEMU_LIBRETRO_NUM_PORTS * ARRAY_SIZE(map) + 1];

    int n = 0;
    for (int port = 0; port < XEMU_LIBRETRO_NUM_PORTS; port++) {
        for (int i = 0; i < ARRAY_SIZE(map); i++) {
            desc[n++] = (struct retro_input_descriptor){
                port, map[i].device, map[i].index, map[i].id,
                map[i].description
            };
        }
    }
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    const char *game_path = game ? game->path : NULL;
    retro_thread = g_thread_self();

    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        show_message("xemu: the frontend does not support XRGB8888");
        return false;
    }

    environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
    have_rumble =
        environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble_iface);
    set_input_descriptors();

    g_mutex_lock(&state_lock);
    int state = core_state;
    g_mutex_unlock(&state_lock);

    if (state == CORE_RUNNING) {
        /* The machine is parked from a previous game: swap discs and reboot */
        apply_variables(false);
        g_mutex_lock(&state_lock);
        g_free(cmd_load_disc);
        cmd_load_disc = game_path ? g_strdup(game_path) : NULL;
        cmd_eject = !game_path;
        cmd_pause = false;
        cmd_reset = true;
        cmd_resume = true;
        g_mutex_unlock(&state_lock);
        xemu_libretro_audio_reset();
        game_loaded = true;
        return true;
    } else if (state != CORE_STOPPED) {
        show_message("xemu: the emulator failed to start earlier; restart "
                     "the frontend to try again");
        return false;
    }

    setlocale(LC_NUMERIC, "C");

    if (!configure(game_path)) {
        return false;
    }

    core_state = CORE_STARTING;
    qemu_thread_create(&core_thread, "xemu-libretro", core_thread_fn, NULL,
                       QEMU_THREAD_DETACHED);

    g_mutex_lock(&state_lock);
    int64_t deadline = g_get_monotonic_time() + START_TIMEOUT_US;
    while (core_state == CORE_STARTING) {
        if (!g_cond_wait_until(&state_cond, &state_lock, deadline)) {
            break;
        }
    }
    state = core_state;
    g_mutex_unlock(&state_lock);

    if (state != CORE_RUNNING) {
        show_message("xemu: failed to start the emulator");
        return false;
    }

    game_loaded = true;
    return true;
}

RETRO_API bool retro_load_game_special(unsigned type,
                                       const struct retro_game_info *info,
                                       size_t num)
{
    return false;
}

RETRO_API void retro_unload_game(void)
{
    if (!game_loaded) {
        return;
    }
    game_loaded = false;

    /* Park the machine. Stopping it also flushes the disk images. */
    g_mutex_lock(&state_lock);
    cmd_pause = true;
    cmd_resume = false;
    cmd_eject = true;
    g_mutex_unlock(&state_lock);
}

static int16_t analog_button(unsigned port, unsigned id)
{
    int16_t v = input_state_cb(port, RETRO_DEVICE_ANALOG,
                               RETRO_DEVICE_INDEX_ANALOG_BUTTON, id);
    if (v == 0 && input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)) {
        v = 0x7fff;
    }
    return v;
}

static void poll_input(void)
{
    static const struct {
        unsigned id;
        uint16_t mask;
    } buttons[] = {
        { RETRO_DEVICE_ID_JOYPAD_B, CONTROLLER_BUTTON_A },
        { RETRO_DEVICE_ID_JOYPAD_A, CONTROLLER_BUTTON_B },
        { RETRO_DEVICE_ID_JOYPAD_Y, CONTROLLER_BUTTON_X },
        { RETRO_DEVICE_ID_JOYPAD_X, CONTROLLER_BUTTON_Y },
        { RETRO_DEVICE_ID_JOYPAD_LEFT, CONTROLLER_BUTTON_DPAD_LEFT },
        { RETRO_DEVICE_ID_JOYPAD_UP, CONTROLLER_BUTTON_DPAD_UP },
        { RETRO_DEVICE_ID_JOYPAD_RIGHT, CONTROLLER_BUTTON_DPAD_RIGHT },
        { RETRO_DEVICE_ID_JOYPAD_DOWN, CONTROLLER_BUTTON_DPAD_DOWN },
        { RETRO_DEVICE_ID_JOYPAD_SELECT, CONTROLLER_BUTTON_BACK },
        { RETRO_DEVICE_ID_JOYPAD_START, CONTROLLER_BUTTON_START },
        { RETRO_DEVICE_ID_JOYPAD_L, CONTROLLER_BUTTON_WHITE },
        { RETRO_DEVICE_ID_JOYPAD_R, CONTROLLER_BUTTON_BLACK },
        { RETRO_DEVICE_ID_JOYPAD_L3, CONTROLLER_BUTTON_LSTICK },
        { RETRO_DEVICE_ID_JOYPAD_R3, CONTROLLER_BUTTON_RSTICK },
    };

    input_poll_cb();

    for (unsigned port = 0; port < XEMU_LIBRETRO_NUM_PORTS; port++) {
        XemuLibretroPadState pad = { 0 };

        for (int i = 0; i < ARRAY_SIZE(buttons); i++) {
            if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, buttons[i].id)) {
                pad.buttons |= buttons[i].mask;
            }
        }

        pad.axis[CONTROLLER_AXIS_LTRIG] =
            analog_button(port, RETRO_DEVICE_ID_JOYPAD_L2);
        pad.axis[CONTROLLER_AXIS_RTRIG] =
            analog_button(port, RETRO_DEVICE_ID_JOYPAD_R2);

        /* libretro has +Y pointing down, the Xbox pad has it pointing up */
        int x, y;
        x = input_state_cb(port, RETRO_DEVICE_ANALOG,
                           RETRO_DEVICE_INDEX_ANALOG_LEFT,
                           RETRO_DEVICE_ID_ANALOG_X);
        y = input_state_cb(port, RETRO_DEVICE_ANALOG,
                           RETRO_DEVICE_INDEX_ANALOG_LEFT,
                           RETRO_DEVICE_ID_ANALOG_Y);
        pad.axis[CONTROLLER_AXIS_LSTICK_X] = x;
        pad.axis[CONTROLLER_AXIS_LSTICK_Y] = -1 - y;
        x = input_state_cb(port, RETRO_DEVICE_ANALOG,
                           RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                           RETRO_DEVICE_ID_ANALOG_X);
        y = input_state_cb(port, RETRO_DEVICE_ANALOG,
                           RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                           RETRO_DEVICE_ID_ANALOG_Y);
        pad.axis[CONTROLLER_AXIS_RSTICK_X] = x;
        pad.axis[CONTROLLER_AXIS_RSTICK_Y] = -1 - y;

        xemu_libretro_input_set_pad_state(port, &pad);

        uint16_t left, right;
        if (have_rumble &&
            xemu_libretro_input_get_rumble(port, &left, &right)) {
            rumble_iface.set_rumble_state(port, RETRO_RUMBLE_STRONG, left);
            rumble_iface.set_rumble_state(port, RETRO_RUMBLE_WEAK, right);
        }
    }
}

RETRO_API void retro_run(void)
{
    retro_thread = g_thread_self();

    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) &&
        updated) {
        apply_variables(false);
    }

    poll_input();
    xemu_libretro_flush_messages(environ_cb);

    /* Ask the core thread for a frame and give it a moment to deliver */
    XemuLibretroFrame f = { 0 };
    bool have_frame = false;

    g_mutex_lock(&state_lock);
    if (frame_state == FRAME_IDLE) {
        frame_state = FRAME_REQUESTED;
        g_cond_broadcast(&state_cond);
    }
    int64_t deadline = g_get_monotonic_time() + FRAME_TIMEOUT_US;
    while (frame_state != FRAME_READY) {
        if (!g_cond_wait_until(&state_cond, &state_lock, deadline)) {
            break;
        }
    }
    if (frame_state == FRAME_READY) {
        f = frame;
        have_frame = frame_valid;
        frame_state = FRAME_IDLE;
    }
    g_mutex_unlock(&state_lock);

    /*
     * The core thread does not touch the frame buffer again before the next
     * request, which is only made above.
     */
    if (have_frame) {
        if (f.width != geom_width || f.height != geom_height ||
            f.widescreen != geom_widescreen || geom_dirty) {
            geom_dirty = false;
            geom_width = f.width;
            geom_height = f.height;
            geom_widescreen = f.widescreen;

            struct retro_game_geometry geom;
            fill_geometry(&geom);
            environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
        }
        video_cb(f.data, f.width, f.height, f.pitch);
    } else if (can_dupe) {
        video_cb(NULL, geom_width, geom_height, 0);
    } else {
        static uint32_t *blank;
        static size_t blank_size;
        size_t size = (size_t)geom_width * geom_height * sizeof(uint32_t);
        if (size > blank_size) {
            blank = g_realloc(blank, size);
            memset(blank, 0, size);
            blank_size = size;
        }
        video_cb(blank, geom_width, geom_height,
                 geom_width * sizeof(uint32_t));
    }

    /*
     * The APU paces itself on how full its output queue is, as it would with
     * a sound card draining it, so take exactly one video frame of audio.
     */
    static int16_t audio_buf[AUDIO_FRAMES_PER_RUN * 2];
    static double audio_frames_owed;
    audio_frames_owed += XEMU_LIBRETRO_AUDIO_RATE / FRAME_RATE;
    size_t want = MIN((size_t)audio_frames_owed, AUDIO_FRAMES_PER_RUN);
    audio_frames_owed -= want;
    size_t num_frames = xemu_libretro_audio_pop(audio_buf, want);
    if (num_frames && audio_batch_cb) {
        audio_batch_cb(audio_buf, num_frames);
    }
}

RETRO_API size_t retro_serialize_size(void)
{
    return 0;
}

RETRO_API bool retro_serialize(void *data, size_t size)
{
    return false;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
    return false;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

RETRO_API unsigned retro_get_region(void)
{
    return RETRO_REGION_NTSC;
}

RETRO_API void *retro_get_memory_data(unsigned id)
{
    return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    return 0;
}
