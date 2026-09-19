# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

xemu is an original Xbox emulator implemented as a fork of QEMU. The vast majority of the tree is upstream QEMU; xemu-specific work lives in a small number of directories (see Architecture). Read `AGENTS.md` and `CONTRIBUTING.md` before making commits or PRs — their rules are mandatory and summarized under "Contribution rules" below.

## Build

`./build.sh` is the entry point. It runs `configure` with `--target-list=i386-softmmu` and `-DXBOX=1`, builds with make/ninja into `build/`, then packages into `dist/`.

```bash
./build.sh                  # release build; output: dist/xemu (Linux), dist/xemu.exe, dist/xemu.app
./build.sh --debug          # adds --enable-debug, --enable-trace-backends=log, -DXEMU_DEBUG_BUILD=1
./build.sh -j8              # job count (no space)
./build.sh -p win64-cross   # cross-compile for Windows (needs CROSSPREFIX/CROSSAR, as in CI)
./build.sh <configure args> # anything unrecognized is passed through to configure
```

- The built binary is `build/qemu-system-i386` (`qemu-system-i386w.exe` on Windows); build.sh copies it to `dist/` as `xemu`.
- On Windows, build.sh must be run from an MSYS2/MinGW shell (it keys off `uname -s` matching `MINGW*`/`MSYS*`), not PowerShell/cmd.
- On macOS it downloads prebuilt dependencies into `macos-libs/` via `scripts/download-macos-libs.py`.
- After the first configure, incremental rebuilds can be done with `make -C build qemu-system-i386` (or `ninja -C build`).
- Third-party dependencies (imgui, implot, glslang, SDL3, volk, VMA, SPIRV-Reflect, tomlplusplus, xxhash, nv2a_vsh_cpu, genconfig, …) are Meson wraps in `subprojects/*.wrap`. "meson: Bump X" commits update these.

### libretro core

`./build.sh --libretro` (Meson option `-Dlibretro=true`, `CONFIG_LIBRETRO`) builds `xemu_libretro.{dll,so}` as a `shared_module` in place of the executables. In that configuration `ui/libretro/` replaces `ui/xemu.c` and the whole `ui/xui/` ImGui UI, and `hw/xbox/mcpx/apu/monitor-libretro.c` replaces `monitor.c`. `ui/libretro/README.md` explains the design; the parts that are easy to get wrong:

- The frontend's thread must never call into QEMU. Everything goes through the core thread in `ui/libretro/core.c` (it stands in for `main()` of `ui/xemu.c`), via the mutex-protected mailboxes there and in `input.c` / `audio.c`.
- Many `nv2a_*` helpers (`nv2a_get/set_surface_scale_factor`, ...) drop and retake the BQL internally, so they assert unless called with the BQL held; `nv2a_get_framebuffer_surface()` on the other hand is called without it.
- The machine is created once per process and the library pins itself at load (QEMU starts its RCU thread from a constructor). "Unload" pauses and ejects, "load" swaps the disc and resets.
- Frontends load several copies of the core file (libretro-godot: one per instance). Only the module loaded first runs a machine; the others forward the stateful `retro_*` calls to it through the table published in the `XEMU_LIBRETRO_PRIMARY` environment variable. A new API entry point that touches state needs a `FORWARD_TO_PRIMARY` and a slot in `XemuLibretroApi` (bump `PRIMARY_API_VERSION`).
- The hard disk image and EEPROM are save data: copied once from `<system>/xemu/` to `<save>/xemu/` and used there.
- `retro_run()` must drain exactly one video frame of audio; the APU paces itself on the fill level of that queue.

## Tests

CI (`.github/workflows/build-tests.yml`) does a plain configure rather than build.sh:

```bash
./configure --target-list=i386-softmmu --disable-werror
make -j$(nproc) check-unit      # QEMU unit tests
```

xemu's own tests live in `tests/xbox/` and are registered under the Meson suite `xbox` (QEMU's `mtest2make.py` exposes each suite as `make check-<suite>`):

```bash
make -C build check-xbox
# or a single test through the meson in configure's venv (build/pyvenv), from the build dir:
cd build && ./pyvenv/bin/meson test --suite xbox test-nv2a-ptimer   # pyvenv/Scripts/ on Windows
```

- `tests/xbox/ptimer/` shows the pattern for unit-testing a hardware block: compile the real `hw/xbox/nv2a/ptimer.c` against a mock (`mock-nv2a-ptimer.c`) into a GLib TAP test.
- `tests/xbox/swizzle/` is a standalone Makefile-based test, not wired into Meson.
- Hardware-behavior changes are expected to come with (or suggest) a test XBE runnable on both real hardware and xemu; unit tests alone are not considered sufficient evidence of parity.

## Architecture

### Where xemu code lives

- `hw/xbox/` — the Xbox machine and its devices (`xbox.c` machine definition, `xbox_pci.c`, SMBus devices such as the SMC/EEPROM/video encoders, `xid*.c` USB controllers, `lpc47m157.c` Super I/O). Enabled by `CONFIG_XBOX` (`hw/i386/Kconfig`, `configs/devices/i386-softmmu/default.mak`).
- `hw/xbox/nv2a/` — the NV2A GPU.
- `hw/xbox/mcpx/` — the MCPX southbridge: `apu/` (audio), `aci.c` (AC97), `nvnet/` (Ethernet).
- `ui/xemu*.{c,cc,h}` and `ui/xui/` — the xemu frontend, replacing QEMU's stock UIs.
- `config_spec.yml` — schema for user settings.
- Elsewhere in the QEMU tree, xemu modifications to upstream files are guarded by `#ifdef XBOX` (notably `target/i386/` for CPU/FPU behavior, `accel/tcg/`, `hw/ide/piix.c`, `hw/usb/hcd-ohci-pci.c`). Keep new changes to upstream files behind that guard so QEMU rebases stay tractable.

### NV2A GPU (`hw/xbox/nv2a/`)

- One file per hardware engine, each handling its MMIO block: `pmc`, `pbus`, `pfifo`, `pfb`, `pvideo`, `ptimer`, `pcrtc`, `pramdac`, `prmcio`/`prmvio`/`prmdio`, `user`. `nv2a.c` wires them into the PCI device; `nv2a_regs.h` is the register/bitfield source of truth; `nv2a_int.h` is the shared internal state (`NV2AState`).
- **PFIFO runs on its own thread** (`nv2a.pfifo_thread`, created in `nv2a.c`). It pulls the guest's pushbuffer and dispatches graphics methods to PGRAPH. MMIO handlers run on the vCPU/main-loop thread, so PGRAPH state is shared across threads under `pgraph.lock` / `pgraph.renderer_lock`; pay attention to which lock is held when touching state from a method handler vs. an MMIO handler.
- `pgraph/pgraph.c` holds the backend-independent part: register state and the method handlers (table in `methods.h.inc`). Rendering is delegated through the `PGRAPHRenderer` ops vtable (`pgraph/pgraph.h`); backends self-register with `pgraph_renderer_register()`:
  - `pgraph/gl/` — OpenGL
  - `pgraph/vk/` — Vulkan
  - `pgraph/null/` — no-op
  The GL and VK backends mirror each other file-for-file (`draw.c`, `surface.c`, `texture.c`, `vertex.c`, `shaders.c`, `display.c`, `blit.c`, `reports.c`). A rendering fix usually has to be made in both, and commit subjects use `nv2a/gl:` / `nv2a/vk:` when only one is touched.
- `pgraph/glsl/` is shared by both backends: it translates NV2A pipeline state into GLSL — `vsh-prog.c` (programmable vertex shaders), `vsh-ff.c` (fixed-function T&L), `psh.c` (register combiners / texture stages), `geom.c`. Vulkan compiles that GLSL to SPIR-V via glslang (`vk/glsl.c`). Generated shaders are cached keyed on a hash of the relevant state, so any new state that influences generated code must be included in the shader state struct or stale shaders will be reused.
- Surfaces and textures live in guest RAM and are cached on the host; correctness depends on dirty tracking and on syncing between guest memory and host objects (`surface_update`, `surface_flush`, download/upload paths). Many game-specific bugs are in this area.
- Debug aids: `NV2A_DPRINTF` / `NV2A_UNIMPLEMENTED` / `NV2A_UNCONFIRMED` in `debug.h`, QEMU trace events (`trace-events`), optional RenderDoc integration (`pgraph/debug_renderdoc.c`), and `pgraph/profile.c` counters surfaced in the UI debug overlay.

### MCPX APU (`hw/xbox/mcpx/apu/`)

- `apu.c` — MMIO and the frame thread (`mcpx.apu_thread`) that drives audio processing.
- `vp/` — Voice Processor: mixes hardware voices (ADPCM decode, filters, HRTF) using a pool of `mcpx.voice_worker` threads.
- `dsp/` — the two Motorola DSP56300 cores (GP = global processor for effects, EP = encode processor), with DMA (`dsp_dma.c`). Execution is either the C interpreter (`dsp_c.c`, `dsp/interp/`) or the JIT from the `dsp56300` subproject (`dsp56300-emu-ffi`, wrapped by `dsp_jit.c`).

### Frontend (`ui/`)

- `ui/xemu.c` contains `main()`. It spawns QEMU's main loop on a separate `qemu_main` thread and keeps SDL event handling and presentation on the real main thread; `xemu_main_loop_lock()/unlock()` bridge the two. The guest framebuffer is fetched as a GL texture from the active PGRAPH renderer via `nv2a_get_framebuffer_surface()` rather than through QEMU's console layer.
- `ui/xui/` is the C++ Dear ImGui interface (main menu, menubar, popup menu, debug/monitor windows, snapshot manager, notifications, updater). C ↔ C++ boundaries go through the `xemu-*.h` headers and `xui/xemu-hud.h`.
- `ui/xemu-input.c`, `xemu-controllers.cc` map SDL gamepads/keyboard to the emulated XID devices in `hw/xbox/xid*.c`.
- `ui/xemu-snapshots.c` layers xemu's snapshot UX (thumbnails, per-game filtering) over QEMU savevm; renderers participate via the `pre_savevm_*` ops.

### Settings

`config_spec.yml` is compiled at build time by the `genconfig` subproject into `xemu-config.h`, which defines `struct config` and its TOML (de)serialization. The global instance is `g_config` (`ui/xemu-settings.h`), persisted as `xemu.toml`. To add a setting: add it to `config_spec.yml`, then use `g_config.<path>` — do not hand-write parsing code. `g_config` is read from device code too (e.g. renderer selection), not just the UI.

## Contribution rules (from AGENTS.md / CONTRIBUTING.md)

- Commit subject: `<subsystem>: <Imperative short description>` plus an explanatory body. Find the right prefix with `git log -n 5 <path>` (common: `nv2a`, `nv2a/vk`, `nv2a/gl`, `apu`, `mcpx`, `ui`, `meson`, `ci`, `docs`).
- One bug fix / feature per PR. Touch only the lines needed — no drive-by refactors, include reordering, or reformatting. Style-only changes go in a separate PR.
- Style is QEMU style (`docs/devel/style.rst`; other developer docs are in `docs/devel/`) with two exceptions: declarations at point of use are allowed, and `//` comments are allowed. Comment only non-obvious code.
- `clang-format` is required for **new** files. When editing existing files, match the local style instead of reformatting.
- Never invent register definitions, bitfields, or hardware behavior. Cross-reference `nv2a_regs.h`, `apu_regs.h`, existing code under `hw/xbox/`, or verified hardware documentation/test results.
- PR descriptions must include an agent declaration, e.g. `> **Agent Declaration**: This pull request was created with assistance from Claude Code (<model>).`
- Before opening a PR: rebase on upstream `master`, and search open PRs on `xemu-project/xemu` for overlapping work.
- Verify modified files compile without warnings.
