# xemu libretro core

Builds xemu as `xemu_libretro.{dll,so,dylib}` instead of the standalone
executable. Only the Windows build (cross-compiled, as CI does for xemu itself)
has been tested so far.

```bash
./build.sh -p win64-cross --libretro    # dist/xemu_libretro.dll
./build.sh --libretro                   # native; same as configure --enable-libretro
```

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

The shader cache is written to the same folder. An `xemu.toml` placed there is
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
- Video is a software framebuffer. The NV2A renderers (OpenGL and Vulkan alike)
  present through a GL texture in a context group owned by the core, so the
  frame is drawn into an FBO on a hidden SDL window's context and read back.
  That makes the core independent of the frontend's video driver, at the cost
  of a readback per frame. It still needs an OpenGL 4.0 driver.

## Frontend facing features

- Controller ports 1-4: `Xbox Controller` (Duke), `Xbox Controller S`, `None`;
  analog triggers, both sticks, rumble. RetroPad B/A/Y/X are A/B/X/Y, L/R are
  White/Black, L2/R2 the triggers, Select is Back.
- Core options (v2 with categories, v1 fallback): memory, AV pack, boot
  animation, hard FPU, renderer, internal resolution scale, aspect ratio, output
  filtering, shader cache, DSP, HRTF, and a memory unit per port.
- `xemu_libretro.info` in this directory is the matching core info file.

Set `XEMU_LIBRETRO_LOG=<file>` to send the core's stdout/stderr (QEMU's own
messages do not go through the frontend's log interface) to a file, and
`XEMU_LIBRETRO_DEBUG=1` for periodic notes about the video path.
