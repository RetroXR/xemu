# The JIT is a prebuilt Rust library that does not exist for Android
if host_is_android
  dsp56300_emu_ffi = not_found
  dsp_jit_src = 'dsp_jit_stub.c'
else
  dsp56300_emu_ffi = dependency('dsp56300-emu-ffi', fallback: ['dsp56300', 'dsp56300_dep'])
  dsp_jit_src = 'dsp_jit.c'
endif

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
