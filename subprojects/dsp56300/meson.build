project('dsp56300', version: '0.1.3')

fs = import('fs')

host_cpu = host_machine.cpu_family()
host_os = host_machine.system()
# Android is 'linux' to Meson
add_languages('c', native: false)
if meson.get_compiler('c').get_define('__ANDROID__') != ''
  host_os = 'android'
endif

if host_os == 'darwin'
  os_suffix = 'apple-darwin'
elif host_os == 'windows'
  os_suffix = (host_cpu == 'aarch64') ? 'pc-windows-gnullvm' : 'pc-windows-gnu'
elif host_os == 'linux'
  os_suffix = 'unknown-linux-gnu'
elif host_os == 'android'
  os_suffix = 'linux-android'
else
  error('Unsupported host OS: ' + host_os)
endif
rust_target = host_cpu + '-' + os_suffix

ver = meson.project_version()
prefix = 'dsp56300-@0@-@1@'.format(ver, rust_target)
archive = '@0@.tar.gz'.format(prefix)
url = 'https://github.com/mborgerson/dsp56300/releases/download/v@0@/@1@'.format(ver, archive)
lib = meson.current_build_dir() / prefix / 'lib' / 'libdsp56300_emu_ffi.a'

if not fs.exists(lib)
  src_archive = meson.current_source_dir() / archive
  if not fs.exists(src_archive) and host_os == 'android'
    # No release is made for Android. An archive of the same layout is built
    # from the source of this version with
    #   cargo build --release --target aarch64-linux-android -p dsp56300-emu-ffi
    error('@0@ not found, xemu will use the DSP interpreter'.format(archive))
  endif
  if not fs.exists(src_archive)
    message('Downloading dsp56300 @0@ for @1@...'.format(ver, rust_target))
    run_command(find_program('curl'), '-fsSL', url, '-o', src_archive, check: true)
  endif
  run_command('tar', 'xzf', src_archive, '-C', meson.current_build_dir(), check: true)
endif

link_args = []
if host_os == 'linux'
  link_args = ['-lpthread', '-ldl', '-lm']
elif host_os == 'android'
  link_args = ['-ldl', '-lm']
elif host_os == 'windows'
  link_args = ['-lbcrypt', '-lntdll', '-luserenv', '-lws2_32']
elif host_os == 'darwin'
  link_args = ['-lpthread', '-lm', '-framework', 'Security']
endif

dsp56300_dep = declare_dependency(
  link_args: [lib] + link_args,
  include_directories: include_directories(prefix / 'include'),
)

meson.override_dependency('dsp56300-emu-ffi', dsp56300_dep)
