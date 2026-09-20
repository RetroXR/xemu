# xemu libretro core

Builds xemu as `xemu_libretro.{dll,so,dylib}` instead of the standalone
executable. The Windows build (cross-compiled, as CI does for xemu itself) and
the Android build have been tested; Linux and macOS have not been tried.

```bash
./build.sh -p win64-cross --libretro    # dist/xemu_libretro.dll
./build.sh --libretro                   # native; same as configure --enable-libretro
```

### Android

```bash
ANDROID_NDK_ROOT=... ANDROID_DEPS=... ./build.sh -p android   # dist/xemu_libretro_android.so
```

`ANDROID_DEPS` is a prefix with static glib (plus libffi, pcre2, intl), pixman,
libslirp, libsamplerate, libpcap and SDL3 built for `aarch64-linux-android`.
SDL is only there for its headers and platform independent helpers; none of
it that needs a Java side is called. There is no desktop GL on Android, so
this build has no GL at all (`--disable-opengl`): the Vulkan renderer is the
only one, its display image is read back, and the DSP JIT (a prebuilt library)
is replaced by the interpreter.

Tested on a Quest 3 (Adreno 740): Halo runs at its native 30 fps. What it took
beyond building, all in `hw/xbox/nv2a/pgraph/vk/` and harmless elsewhere:

- The geometry shader stage cannot be used for ordinary draws on Adreno, see
  `avoid_geometry_shader` in `GPUProperties`. Quads, lines and the line and
  point polygon modes still go through it.
- Images are initialized when created; mobile GPUs do not hand out zeroed
  memory.
- The scratch buffers respect `maxMemoryAllocationSize` and are much smaller
  on integrated GPUs.
- Mapped memory is required to be coherent, shaders declare that they need
  infinities and NaNs preserved, and `A4R4G4B4` textures use the mandatory
  `R4G4B4A4` format with the components rotated.

Android only loads Vulkan layers for debuggable applications.
`XEMU_VK_LAYER_PATH=<libVkLayer_khronos_validation.so>` chains the validation
layer in by hand instead (`vk/layer-shim.c`), which also works for a test host
started from `adb shell`.

## Setup

Put these in `<system directory>/xemu/` (a frontend that already gives the core
a system directory named `xemu` is detected, and the files are also found
directly in the system directory):

| File | |
|---|---|
| `mcpx_1.0.bin` | MCPX boot ROM, 512 bytes |
| `Complex_4627v1.03.bin` | Flash ROM / BIOS. Any 256 KiB or 1 MiB `.bin` is accepted |
| `xbox_hdd.qcow2` | Hard disk image |
| `xbox_eeprom.bin` | Optional, generated on first start |

The machine writes to its hard disk (game saves, caches, profiles) and its
EEPROM (dashboard settings), so those are save data: on first use they are
copied to `<save directory>/xemu/`, and from then on only the copies are used
and the files in the system directory stay as they were. Delete the copies to
start over. The "Hard Disk Image" core option uses the image in the system
directory in place instead, which suits large images with installed software.
Memory units enabled through the core options are 8 MiB FATX images in the
same folder of the save directory: `memory_unit_port<N>.img` for the top slot
(A) of a controller and `memory_unit_port<N>b.img` for the bottom one (B).
The options are applied while a game runs, and a unit that is disabled has
been unplugged and its image closed by the time `retro_run()` returns, so a
frontend can swap the files. A message tells when the console has detected a
unit and when it first reads from and writes to it, since few games show
memory units and the stock hard disk image has no dashboard that would. Written images are flushed once the guest
stops writing, so that a frontend that gets killed does not take a save with
it.

The shader cache is written to the system folder. An `xemu.toml` placed there is
read like the standalone configuration file (it is never written back), which
is the way to reach settings that have no core option; core options win where
both exist.

Content is an XISO image. Starting without content boots the dashboard.

## How it works

QEMU can neither be stepped a frame at a time nor be torn down and started
again in the same process, so this is not a conventional core:

- The machine free-runs in real time on QEMU's own threads, exactly as in
  standalone xemu. `retro_run()` fetches the most recent frame, takes one video
  frame worth of audio out of the APU's output queue and forwards input. The
  APU throttles on that queue, as it does on a sound card's. There are no save
  states, no rewind, no run-ahead and no fast-forward.
- All calls into QEMU are made from a "core thread" that plays the part of
  the main thread of `ui/xemu.c`. The frontend's thread only talks to it
  through the mailboxes in `core.c`, `input.c` and `audio.c`.
- The machine is created by the first `retro_load_game()` and lives until the
  process exits. `retro_unload_game()` pauses it (which flushes the disk
  images) and ejects the disc; the next `retro_load_game()` inserts the new
  disc, resets and resumes. Options that are baked into the machine (renderer,
  memory, AV pack, ...) therefore need the frontend to be restarted.
- The library pins itself in memory when it is loaded. QEMU starts threads
  from constructors, so a frontend that loads the core just to query it and
  unloads it again would otherwise pull the code out from under them.
- Frontends also load copies of the core file, one per instance they create
  (libretro-godot does). Every copy is a module with its own QEMU, but the
  machine has to stay the only one in the process: it holds the disk images.
  The module loaded first owns it and publishes its entry points in the
  `XEMU_LIBRETRO_PRIMARY` environment variable; modules loaded later forward
  their libretro calls there. Only one instance can have content loaded at a
  time, a second `retro_load_game()` fails with a message.
- Video is a software framebuffer, which makes the core independent of the
  frontend's video driver at the cost of a readback per frame. The Vulkan
  renderer reads its display image back itself and needs no GL. The OpenGL
  renderer presents through a texture in a context group owned by the core,
  which is drawn into an FBO on a hidden SDL window's context and read back;
  that one needs an OpenGL 4.0 driver.

## Frontend facing features

- Controller ports 1-4: `Xbox Controller` (Duke), `Xbox Controller S`, `None`;
  analog triggers, both sticks, rumble. RetroPad B/A/Y/X are A/B/X/Y, L/R are
  White/Black, L2/R2 the triggers, Select is Back.
- Core options (v2 with categories, v1 fallback): memory, AV pack, boot
  animation, hard FPU, hard disk location, renderer, internal resolution scale,
  aspect ratio, output filtering, shader cache, DSP, HRTF, voice resampler
  (linear by default on Android, sinc elsewhere), and a memory unit
  for each of the two expansion slots of every port.
- `xemu_libretro.info` in this directory is the matching core info file.

Set `XEMU_LIBRETRO_LOG=<file>` to send the core's stdout/stderr (QEMU's own
messages do not go through the frontend's log interface) to a file, and
`XEMU_LIBRETRO_DEBUG=1` for periodic notes about the video path and the
renderer's counters. `XEMU_LIBRETRO_TRACE=<pattern>[,<pattern>...]` enables
QEMU trace events, e.g. `usb_msd_*,usb_desc_*` to watch a memory unit.
