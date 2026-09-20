# The JIT is a prebuilt Rust library. There is none for Android, where it is
# used if somebody has built it, see subprojects/dsp56300/meson.build.
dsp56300_emu_ffi = dependency('dsp56300-emu-ffi',
                              fallback: ['dsp56300', 'dsp56300_dep'],
                              required: not host_is_android)
dsp_jit_src = dsp56300_emu_ffi.found() ? 'dsp_jit.c' : 'dsp_jit_stub.c'

subdir('interp')

libdsp = static_library(
  'dsp',
  files(['dsp.c', 'dsp_c.c', dsp_jit_src, 'dsp_dma.c']) + genh,
  dependencies: [dsp56300_emu_ffi],
)

dsp = declare_dependency(
  objects: [
    libdsp.extract_all_objects(recursive: false),
    libdsp_cpu_interp.extract_all_objects(recursive: false),
  ],
  dependencies: [dsp56300_emu_ffi],
)

mcpx_ss.add(dsp, files('gp_ep.c'))
