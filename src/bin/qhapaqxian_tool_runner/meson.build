# Copyright (c) 2026, QhapaqXian DB contributors

fs = import('fs')
py3 = import('python').find_installation('python3')

qhapaqxian_tool_runner_deps = [frontend_code]
if ssl.found()
  qhapaqxian_tool_runner_deps += ssl
endif

qhapaqxian_tool_runner = executable('qhapaqxian-tool-runner',
  files('qhapaqxian_tool_runner.c'),
  dependencies: qhapaqxian_tool_runner_deps,
  link_with: [common_static],
  kwargs: default_bin_args,
)

if host_machine.system() == 'windows' and ssl.found()
  where = find_program('where', required: false)
  if where.found()
    foreach runtime_dll : ['libcrypto-3-x64__.dll', 'libssl-3-x64__.dll', 'zlib1__.dll']
      runtime_lookup = run_command(where, runtime_dll, check: false)
      if runtime_lookup.returncode() == 0
        runtime_path = runtime_lookup.stdout().strip().split('\n')[0].strip()
        if fs.exists(runtime_path)
          custom_target('qhapaqxian-tool-runner-' + runtime_dll,
            output: runtime_dll,
            command: [py3, '-c', 'import shutil, sys; shutil.copyfile(sys.argv[1], sys.argv[2])', runtime_path, '@OUTPUT@'],
            build_by_default: true,
            install: true,
            install_dir: dir_bin,
          )
        endif
      endif
    endforeach
  endif
endif

install_data('qx_remote_ed25519_private.pem', install_dir: dir_bin)
bin_targets += qhapaqxian_tool_runner
