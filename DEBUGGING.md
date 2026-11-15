# Debugging

Two ways to run NextUI while developing:

- **Desktop** (`PLATFORM=desktop`): the apps run natively on your Mac or Linux machine, in a
  window, with the keyboard as the controller. Builds of a single app take well under a minute, there is nothing to deploy
  and no input to inject. Use this for anything UI or logic related. Start here.
- **On a device** over `adb`: needed for anything hardware specific (WiFi, Bluetooth, audio,
  input quirks, performance, the real installer/boot flow) and as a final check before a PR.

Written against the TrimUI Brick (`tg5040`) for the device part; other devices differ in the
details noted below.

## Desktop

The desktop platform fakes an SD card at `/var/tmp/nextui/sdcard` and builds the apps with your
host compiler. Hardware that does not exist on a PC is stubbed, so WiFi and Bluetooth menus
are hidden (Settings checks the platform, see `DeviceInfo::hasWifi()`). The exception is
About > OTA Update: it is shown on desktop and the host counts as online, so the whole flow
(release scan, version picker, notes, download, extraction) can be tried locally. Updates are
only staged in the fake card, nothing is installed and nothing reboots.

### One time setup

macOS (Homebrew):

```sh
brew install sdl2_image sdl2_ttf gcc make libsamplerate sqlite libzip
```

Linux: the equivalent SDL2, SDL2_image, SDL2_ttf, libsamplerate, sqlite and libzip dev packages.

The makefiles call the compiler as `gcc`/`g++`, but Homebrew installs it with a version suffix
(`gcc-16`). The repo ships a script that links them into `/usr/local/bin`
(`sudo ./workspace/desktop/macos_create_gcc_symlinks.sh`). If you would rather not use `sudo`,
make the links somewhere else and point `CROSS_COMPILE` at it:

```sh
mkdir -p /tmp/gccbin
ln -sf $(ls /opt/homebrew/bin/gcc-[0-9]* | head -1) /tmp/gccbin/gcc
ln -sf $(ls /opt/homebrew/bin/g++-[0-9]* | head -1) /tmp/gccbin/g++
```

Create the fake SD card (it does nothing if it exists, delete `/var/tmp/nextui/sdcard` to start
over):

```sh
./workspace/desktop/prepare_fake_sd_root.sh
```

### Build and run a single app

Example for Settings, takes about 30 seconds the first time:

```sh
cd workspace/all/settings
env UNAME_S=Darwin PLATFORM=desktop UNION_PLATFORM=desktop CROSS_COMPILE=/tmp/gccbin/ \
    PREFIX=/opt/homebrew PREFIX_LOCAL=/var/tmp/nextui make
```

(On Linux use `UNAME_S=Linux CROSS_COMPILE=/usr/bin/ PREFIX=/usr`. With the `sudo` symlinks use
`CROSS_COMPILE=/usr/local/bin/`.) The result is `build/desktop/settings.elf`. Copy it into the
fake card and run it with the environment the launcher would set up
(`skeleton/SYSTEM/desktop/paks/MinUI.pak/launch.sh` has the full list):

```sh
PAK=/var/tmp/nextui/sdcard/Tools/desktop/Settings.pak
cp build/desktop/settings.elf $PAK/

export PLATFORM=desktop SDCARD_PATH=/var/tmp/nextui/sdcard IS_NEXT=yes
export SYSTEM_PATH=$SDCARD_PATH/.system/$PLATFORM
export USERDATA_PATH=$SDCARD_PATH/.userdata/$PLATFORM
export SHARED_USERDATA_PATH=$SDCARD_PATH/.userdata/shared
export BIOS_PATH=$SDCARD_PATH/Bios ROMS_PATH=$SDCARD_PATH/Roms SAVES_PATH=$SDCARD_PATH/Saves
export CHEATS_PATH=$SDCARD_PATH/Cheats CORES_PATH=$SYSTEM_PATH/cores
export LOGS_PATH=$USERDATA_PATH/logs HOOKS_PATH=$USERDATA_PATH/.hooks
mkdir -p $USERDATA_PATH $LOGS_PATH $SHARED_USERDATA_PATH/.minui
export DYLD_LIBRARY_PATH=$SYSTEM_PATH/lib:/var/tmp/nextui/lib   # LD_LIBRARY_PATH on Linux
export PATH=$SYSTEM_PATH/bin:$PATH

cd $PAK && ./settings.elf
```

Save the exports as a script if you do this often. Other apps (`nextui`, `minarch`, ...) build the
same way from their folder under `workspace/all/`. A full desktop build of everything is
`make setup common PLATFORM=desktop` from the repo root, see `makefile.native`.

Keyboard map (also printed in the app's log on start):

| Pad | Key | Pad | Key |
|---|---|---|---|
| D-pad | arrow keys | L1 | Tab |
| A | S | R1 | B |
| B | A | SELECT | ` |
| X | W | START | Return |
| Y | Q | MENU / POWER | Space / Backspace |

Screenshots are a normal window grab (macOS: Cmd+Shift+4, then Space and click the window).
Apps still write their log next to the binary, e.g. `Settings.pak/settings.log`.

Things to keep in mind:

- It is a different compiler, libc and screen size than the devices. Layout, input handling and
  logic carry over, performance and timing do not. A change that builds here can still fail with
  the device toolchain, so build for the device before you push.
- The fake card lives outside the repo. Anything an app writes (settings, saves) stays in
  `/var/tmp/nextui/sdcard` until you delete it.
- Hardware backed code paths (WiFi, Bluetooth, rumble, LEDs, deep sleep) do nothing or are
  skipped, `/dev/fb0: Operation not permitted` in the log is harmless.

## On a device

How to build, deploy, drive and screenshot NextUI on a handheld over `adb`.

Needs `adb` on the host and the device connected over USB (debugging enabled in the stock OS).
The SD card is mounted at `/mnt/SDCARD`.

## Build and deploy one app

Build in the platform toolchain container, then push the binary over the one on the card.
Example for Settings:

```sh
docker run --rm -v $(pwd)/workspace:/root/workspace \
  -e PLATFORM=tg5040 -e UNION_PLATFORM=tg5040 \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  /bin/bash -c '. ~/.bashrc && export PLATFORM=tg5040 UNION_PLATFORM=tg5040 && cd /root/workspace/all/settings && make'

adb push workspace/all/settings/build/tg5040/settings.elf /mnt/SDCARD/Tools/tg5040/Settings.pak/settings.elf
```

Back up the original first (`adb pull`) if you want to restore it. A full install (`MinUI.zip`)
overwrites it again on the next update.

## Launching an app (read this before killing anything)

`MinUI.pak/launch.sh` runs a loop: start `nextui.elf`, and if it left a command in `/tmp/next`,
run that, then go round again. The loop only continues while `/tmp/nextui_exec` exists.

**`killall nextui.elf` is a shutdown request.** SIGTERM becomes an SDL quit, nextui handles it as
power off, which creates `/tmp/poweroff` and deletes `/tmp/nextui_exec`. Whatever you launched
runs, and when it exits the device powers off. Since the device is on USB power it then boots
straight back up, which looks like a spontaneous reboot and drops adb for a minute.

To launch an app by hand, set the target and kill nextui with SIGKILL, which has no handler:

```sh
adb shell "echo \"'/mnt/SDCARD/Tools/tg5040/Settings.pak/launch.sh'\" > /tmp/next; killall -9 nextui.elf"
```

You can prefix environment variables, they are passed through `eval`:

```sh
adb shell "echo \"MY_VAR=1 '/mnt/SDCARD/Tools/tg5040/Settings.pak/launch.sh'\" > /tmp/next; killall -9 nextui.elf"
```

When the app exits, nextui comes back. If you did kill it with plain `killall` and the app is still
running, repair the loop before quitting the app:

```sh
adb shell 'rm -f /tmp/poweroff; touch /tmp/nextui_exec'
```

Check `ls /tmp/poweroff /tmp/reboot /tmp/nextui_exec` if the device does something unexpected.

Other things that look like a dead device: auto sleep (an idle app lets the screen sleep and USB
goes quiet, press power) and low battery. Keep it on a charger while testing.

App logs: `Tools/<platform>/<Name>.pak/*.log`, `.userdata/<platform>/logs/`.

## Screenshots

The framebuffer is read straight from `/dev/fb0` (1024x768, 32bpp BGRA on the Brick):

```sh
tools/device-debug/screenshot.py shot.png     # needs Pillow
```

The script reads the geometry from sysfs, runs `dd` on the device and pulls the result. Use
`screenshot.py shot.png 1` if you get a stale frame (the buffer is double paged). Overlays and
the main UI both end up in the framebuffer, so everything is visible. Screenshots taken while the
app is blocked in a syscall show the last presented frame.

## Pressing buttons

There is no input over adb, so `tools/device-debug/inject.c` creates a virtual gamepad that looks
like the built-in one and presses buttons for you.

```sh
# build for the device
docker run --rm -v $(pwd)/tools/device-debug:/inj ghcr.io/loveretro/tg5040-toolchain:latest \
  /bin/bash -c '. ~/.bashrc; aarch64-nextui-linux-gnu-gcc -o /inj/inject /inj/inject.c'
# do not link it statically, the device kernel is too old for the toolchain's static glibc
# if Docker refuses to mount the folder (file sharing is limited to some paths on macOS),
# copy inject.c somewhere it can, e.g. under workspace/, and build it there

adb push tools/device-debug/inject /tmp/inject
adb shell 'chmod +x /tmp/inject; mkfifo /tmp/inj'
adb shell '/tmp/inject /tmp/inj'      # keep this session open, e.g. in the background
```

Then, from another shell:

```sh
adb shell 'echo "down down a" > /tmp/inj'
```

Tokens: `up down left right a b x y l1 r1 sleep quit`. Each press takes about 0.3s, `sleep` waits
one second. Input sent while the app is blocked (for example during a network call) is
swallowed, so wait for the screen to settle and screenshot between steps.

Button numbers are the SDL joystick indices from `JOY_*` in `workspace/<platform>/platform/platform.h`.
The tool is written for the Brick; for another device check the real pad with
`cat /proc/bus/input/devices` and adjust the name, ids and button set to match.

The virtual pad vanishes when the injector exits (use `quit`), and apps see that as an unplugged
controller. Quit the app first.

## Handy

```sh
adb shell 'cat /sys/class/power_supply/*/status'     # charging?
adb shell 'ps | grep -E "nextui|settings"'           # who is running
adb shell 'uptime'                                   # did it reboot?
```
