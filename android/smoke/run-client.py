#!/usr/bin/env python3
"""Run a packaged Wayland or X11 client as the debug application's UID."""
import os
import shlex
import subprocess
import sys

adb = [os.environ.get('ADB', 'adb'), '-s', os.environ.get('ADB_SERIAL', '10AFA31610002QH')]
package = 'io.taowen.anhyprland.smoke'
apk = subprocess.check_output(adb + ['shell', 'pm', 'path', package], text=True).strip().split(':', 1)[1]
libraries = apk.rsplit('/', 1)[0] + '/lib/arm64'
arguments = sys.argv[1:]
executable = '/libx11-smoke.so' if arguments == ['--x11'] else '/libshm-smoke.so'
if arguments == ['--x11']: arguments = []
command = ['run-as', package, 'env', 'LD_LIBRARY_PATH=' + libraries,
           'XDG_RUNTIME_DIR=/data/user/0/' + package + '/files/runtime',
           'WAYLAND_DISPLAY=' + os.environ.get('SMOKE_WAYLAND_DISPLAY', 'wayland-0'),
           'sh', '-c', 'exec ' + shlex.join([libraries + executable] + arguments)]
raise SystemExit(subprocess.call(adb + ['shell', shlex.join(command)]))
