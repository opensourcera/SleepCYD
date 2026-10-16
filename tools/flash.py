"""Flash SleepCYD onto a Cheap Yellow Display (ESP32-2432S028, 4 MB flash).

usage: python tools/flash.py COM5 [all|app] [--build DIR]
  all   bootloader + partition table + OTA boot record + the firmware   <- first time, or after a partition change
  app   only the firmware (0x10000)                                     <- the usual update (default)
The build directory (default: build/) must hold the arduino-cli output: SleepCYD.ino.bin, SleepCYD.ino.bootloader.bin,
SleepCYD.ino.partitions.bin. boot_app0.bin is taken from the build directory or from the installed ESP32 core.
esptool is looked for in the Arduino ESP32 core (Arduino15/packages), then on the PATH (pip install esptool).
"""
import glob, os, shutil, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
port = args[0] if args else 'COM5'
what = args[1] if len(args) > 1 else 'app'
build = sys.argv[sys.argv.index('--build') + 1] if '--build' in sys.argv else 'build'
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = build if os.path.isabs(build) else os.path.join(root, build)

home = os.path.expanduser('~')
cands = sorted(glob.glob(os.path.join(home, 'AppData', 'Local', 'Arduino15', 'packages', 'esp32', 'tools', 'esptool_py', '*', 'esptool*')))
cands += sorted(glob.glob(os.path.join(home, '.arduino15', 'packages', 'esp32', 'tools', 'esptool_py', '*', 'esptool*')))
cands = [c for c in cands if os.path.isfile(c) and (c.endswith('.exe') or c.endswith('.py') or os.access(c, os.X_OK))]
if cands:
    tool = [cands[-1]] if not cands[-1].endswith('.py') else [sys.executable, cands[-1]]
elif shutil.which('esptool'):
    tool = [shutil.which('esptool')]
elif shutil.which('esptool.py'):
    tool = [shutil.which('esptool.py')]
else:
    sys.exit('esptool not found: install the ESP32 Arduino core or `pip install esptool`')

app = os.path.join(build, 'SleepCYD.ino.bin')
if not os.path.exists(app):
    sys.exit(f'{app} not found: compile first (see README.md)')
parts = []
if what == 'all':
    boot_app0 = os.path.join(build, 'boot_app0.bin')
    if not os.path.exists(boot_app0):
        core = sorted(glob.glob(os.path.join(home, 'AppData', 'Local', 'Arduino15', 'packages', 'esp32', 'hardware', 'esp32', '*', 'tools', 'partitions', 'boot_app0.bin'))
                      + glob.glob(os.path.join(home, '.arduino15', 'packages', 'esp32', 'hardware', 'esp32', '*', 'tools', 'partitions', 'boot_app0.bin')))
        if not core:
            sys.exit('boot_app0.bin not found (build directory or ESP32 core)')
        boot_app0 = core[-1]
    parts += ['0x1000', os.path.join(build, 'SleepCYD.ino.bootloader.bin'), '0x8000', os.path.join(build, 'SleepCYD.ino.partitions.bin'), '0xe000', boot_app0]
elif what != 'app':
    sys.exit(__doc__)
parts += ['0x10000', app]
for p in parts[1::2]:
    if not os.path.exists(p):
        sys.exit(f'{p} not found')

cmd = tool + ['--chip', 'esp32', '--port', port, '--baud', '921600', 'write-flash', '-z', '--flash-mode', 'dio', '--flash-freq', '80m', '--flash-size', '4MB'] + parts
print(' '.join(cmd))
sys.exit(subprocess.call(cmd))
