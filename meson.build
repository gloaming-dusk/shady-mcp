project('shady-mcp', 'c', version: '0.1.0',
  license: 'Apache-2.0',
  default_options: ['buildtype=debugoptimized', 'c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')
executable('shady-mcp',
  ['src/main.c', 'src/ipc.c', 'src/json.c'],
  c_args: ['-DSHADY_MCP_VERSION="' + meson.project_version() + '"'],
  dependencies: [cc.find_library('m', required: false)],
  install: true)
