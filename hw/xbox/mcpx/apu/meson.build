mcpx_ss.add(sdl, files(
	'apu.c',
	'debug.c',
	))

if have_libretro
  mcpx_ss.add(files('monitor-libretro.c'))
else
  mcpx_ss.add(files('monitor.c'))
endif

subdir('vp')
subdir('dsp')
