/*
 * xemu libretro frontend - core options
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
#include "libretro.h"

#define RESTART " Takes effect the next time the frontend is started."

static struct retro_core_option_v2_category categories[] = {
    { "system", "System", "Emulated console configuration." },
    { "video", "Video", "Renderer and picture settings." },
    { "audio", "Audio", "Audio processing settings." },
    { "input", "Input", "Controller expansion slots." },
    { NULL, NULL, NULL },
};

#define MEMORY_UNIT(port)                                                    \
    {                                                                        \
        "xemu_memory_unit_port" #port,                                       \
        "Input > Port " #port " Memory Unit",                                \
        "Port " #port " Memory Unit",                                        \
        "Insert an 8 MiB memory unit into the top expansion slot of the "    \
        "controller. The image is kept in the save directory.",              \
        NULL,                                                                \
        "input",                                                             \
        { { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL } },       \
        "disabled",                                                          \
    }

static struct retro_core_option_v2_definition definitions[] = {
    {
        "xemu_memory",
        "System > Memory",
        "Memory",
        "Retail consoles have 64 MiB. Development kits and some homebrew "
        "use 128 MiB." RESTART,
        NULL,
        "system",
        { { "64 MiB", NULL }, { "128 MiB", NULL }, { NULL, NULL } },
        "64 MiB",
    },
    {
        "xemu_avpack",
        "System > AV Pack",
        "AV Pack",
        "The video cable the console believes is attached. HDTV allows "
        "480p, 720p and 1080i in games that support them." RESTART,
        NULL,
        "system",
        {
            { "hdtv", "HDTV (Component)" },
            { "composite", "Composite" },
            { "svideo", "S-Video" },
            { "scart", "SCART" },
            { "vga", "VGA" },
            { "rfu", "RFU" },
            { NULL, NULL },
        },
        "hdtv",
    },
    {
        "xemu_skip_boot_anim",
        "System > Skip Boot Animation",
        "Skip Boot Animation",
        "Skip the startup animation." RESTART,
        NULL,
        "system",
        { { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL } },
        "disabled",
    },
    {
        "xemu_hard_fpu",
        "System > Hardware FPU",
        "Hardware FPU",
        "Use the host FPU for floating point. Faster, and required by "
        "some games for correct behavior." RESTART,
        NULL,
        "system",
        { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } },
        "enabled",
    },
    {
        "xemu_hdd_location",
        "System > Hard Disk Image",
        "Hard Disk Image",
        "Game saves are stored on the hard disk. By default the image and "
        "the EEPROM in the system directory are copied to the save directory "
        "once, and the copies are used from then on. Large images with "
        "installed software may be better used in place." RESTART,
        NULL,
        "system",
        {
            { "save", "Copy in the save directory" },
            { "system", "System directory, in place" },
            { NULL, NULL },
        },
        "save",
    },
    {
        "xemu_renderer",
        "Video > Renderer",
        "Renderer",
        "Graphics API used to emulate the GPU." RESTART,
        NULL,
        "video",
        {
            { "opengl", "OpenGL" },
            { "vulkan", "Vulkan" },
            { NULL, NULL },
        },
        "opengl",
    },
    {
        "xemu_resolution_scale",
        "Video > Internal Resolution Scale",
        "Internal Resolution Scale",
        "Render at a multiple of the native resolution.",
        NULL,
        "video",
        {
            { "1x", NULL }, { "2x", NULL }, { "3x", NULL }, { "4x", NULL },
            { "5x", NULL }, { "6x", NULL }, { "7x", NULL }, { "8x", NULL },
            { NULL, NULL },
        },
        "1x",
    },
    {
        "xemu_aspect_ratio",
        "Video > Aspect Ratio",
        "Aspect Ratio",
        "Auto follows the widescreen setting of the console and the video "
        "mode selected by the game.",
        NULL,
        "video",
        {
            { "auto", "Auto" },
            { "4:3", NULL },
            { "16:9", NULL },
            { NULL, NULL },
        },
        "auto",
    },
    {
        "xemu_filtering",
        "Video > Output Filtering",
        "Output Filtering",
        "Filter used when the rendered picture is scaled to the output "
        "size.",
        NULL,
        "video",
        {
            { "linear", "Linear" },
            { "nearest", "Nearest" },
            { NULL, NULL },
        },
        "linear",
    },
    {
        "xemu_cache_shaders",
        "Video > Cache Shaders to Disk",
        "Cache Shaders to Disk",
        "Reduces stutter in later sessions." RESTART,
        NULL,
        "video",
        { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } },
        "enabled",
    },
    {
        "xemu_use_dsp",
        "Audio > Emulate DSP",
        "Emulate DSP",
        "Run the audio DSPs for more accurate audio, at the cost of "
        "performance." RESTART,
        NULL,
        "audio",
        { { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL } },
        "disabled",
    },
    {
        "xemu_hrtf",
        "Audio > 3D Audio (HRTF)",
        "3D Audio (HRTF)",
        "Apply head-related transfer function filtering to 3D voices.",
        NULL,
        "audio",
        { { "enabled", NULL }, { "disabled", NULL }, { NULL, NULL } },
        "enabled",
    },
    MEMORY_UNIT(1),
    MEMORY_UNIT(2),
    MEMORY_UNIT(3),
    MEMORY_UNIT(4),
    { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 options_v2 = {
    categories,
    definitions,
};

/* Frontends without v2 support get "Description; default|other|..." */
static void register_legacy_variables(retro_environment_t cb)
{
    static struct retro_variable variables[ARRAY_SIZE(definitions)];
    static bool variables_built;

    /* The table is static, so the strings only have to be built once */
    int n = 0;
    for (const struct retro_core_option_v2_definition *def = definitions;
         def->key && !variables_built; def++) {
        GString *s = g_string_new(def->desc);
        g_string_append_printf(s, "; %s", def->default_value);
        for (int i = 0; def->values[i].value; i++) {
            if (strcmp(def->values[i].value, def->default_value)) {
                g_string_append_printf(s, "|%s", def->values[i].value);
            }
        }
        variables[n].key = def->key;
        variables[n].value = g_string_free(s, false);
        n++;
    }
    variables_built = true;

    cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}

void xemu_libretro_options_register(bool (*cb)(unsigned cmd, void *data))
{
    /*
     * Not just once: the library stays loaded, and a frontend that believes
     * it has loaded the core afresh expects to be told again.
     */
    unsigned version = 0;
    if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) &&
        version >= 2) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
    } else {
        register_legacy_variables(cb);
    }
}
