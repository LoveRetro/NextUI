# About NextUI hooks

## The idea

Hooks are platform-specific, just like paks. The launcher reads them from:

```
$USERDATA_PATH/.hooks/
    boot.d/          # scripts run on boot
    pre-launch.d/    # scripts run before launch
    post-launch.d/   # scripts run after launch exits
    pre-sleep.d/     # scripts run before device goes to sleep
    post-resume.d/   # scripts run after device wakes from sleep
```

On device, `USERDATA_PATH` resolves to:

```
/mnt/SDCARD/.userdata/$PLATFORM
```

So the actual hook directories on device are:

```
/mnt/SDCARD/.userdata/<platform>/.hooks/pre-launch.d/
/mnt/SDCARD/.userdata/<platform>/.hooks/post-launch.d/

```

Example installed hook path:

```
/mnt/SDCARD/.userdata/tg5040/.hooks/post-launch.d/shortcuts-resume.sh

```

If these directories don't exist, nothing happens and there is no overhead.

## Environment variables

Hook scripts inherit all standard NextUI environment variables (`SDCARD_PATH`, `PLATFORM`, `USERDATA_PATH`, `SHARED_USERDATA_PATH`, etc.) plus these launch-specific ones:

| Variable | Description |
|---|---|
| `HOOK_PHASE` | `pre` or `post` |
| `HOOK_TYPE` | `rom` or `pak` |
| `HOOK_CMD` | The raw launch command |
| `HOOK_EMU_PATH` | Path to the emulator or pak `launch.sh` |
| `HOOK_ROM_PATH` | Path to the ROM file (empty for pak launches) |
| `HOOK_LAST` | Contents of `/tmp/last.txt` (the last selected menu entry) |

These can then be used by the underlying Pak to ingest information about the hook that just occurred.

## Writing a hook script

A hook script is any executable `.sh` file in one of the hook directories. Scripts run in alphabetical order.

```sh
#!/bin/sh
# my-hook.sh — log every ROM launch

[ "$HOOK_TYPE" = "rom" ] || exit 0
echo "$(date): launched $HOOK_ROM_PATH" >> "$LOGS_PATH/launches.log"
```

## Rules

- Each script runs in a subshell. A crash will not affect the launcher or other hooks.
- Script output (stdout/stderr) is suppressed. If you need logging, write to your own log file.
- A **synchronous** pre-launch hook (`*.sync.sh`) that exits non-zero **cancels the launch**: the rom or pak is never started, and `post-launch.d` is skipped. See "Vetoing a launch" below.
- Background hooks cannot cancel anything — their exit status is not recoverable from `wait` in POSIX sh, so only `.sync.sh` hooks get a vote. A non-zero exit from a background hook is ignored, as is any exit status outside `pre-launch.d`.
- Keep hooks fast. A slow hook delays the launch or the return to the menu.
- Unlike auto.sh, each pak should manage their own hook and use a descriptive filename to avoid collisions.


## Vetoing a launch

Name the hook `*.sync.sh` and exit non-zero:

```sh
#!/bin/sh
# no-roms-before-noon.sync.sh

[ "$HOOK_TYPE" = "rom" ] || exit 0
[ "$(date +%H)" -ge 12 ] && exit 0

show2.elf --mode=simple --text="Not before noon" --timeout=3
exit 1
```

Two things to know when you cancel a rom launch:

- The launcher has already run `gametimectl.elf start` for that rom by the time the
  hook runs, so a vetoing hook should call `gametimectl.elf stop_all` to avoid leaving
  an open play session behind.
- Nothing is displayed for you. If the user should know why nothing happened, say so —
  `show2.elf` is the usual way.


## Pak-scoped hooks (self-registering, no arming step)

The `.hooks/` directories above require a pak to actively install its own
script there (usually the first time it's opened), and nothing removes that
script when the pak is later deleted -- it's an orphan until someone opens
the pak again to tidy up after itself.

For a Tools pak that wants a **launch gate** and/or a **teardown step** every
single time, there's a second, self-registering mechanism: drop a
`pre-launch.sh` and/or `post-launch.sh` file right next to the pak's own
`launch.sh`:

```
Tools/tg5040/SomePak.pak/
    launch.sh
    pre-launch.sh    # optional
    post-launch.sh   # optional
```

`pak-hooks.sh` scans every installed Tools pak for these two filenames and
runs whichever exist, in alphabetical order by pak folder name, on every ROM
or pak launch -- same env vars as above (`HOOK_TYPE`, `HOOK_ROM_PATH`, etc.).
No copy step, no install step: the file's presence in the pak's own folder
*is* the registration, and removing the pak removes its hook with it.

Differences from `.hooks/*.d/`:

- No `*.sync.sh` naming trick: `pre-launch.sh` is always synchronous and
  always gets a vote. A non-zero exit from **any** installed pak's
  `pre-launch.sh` cancels the launch -- every pak that registers one is
  assumed to need to agree before a game runs.
- The list of paks with a registered hook is cached (`/tmp/pak_hooks_cache.txt`)
  and rebuilt when returning to the main menu, not on every single launch --
  see `nextui.c`. A pak installed or removed while sitting at the menu is
  picked up on the very next visit, not stuck until reboot.
- Because presence alone activates it, installing a pak with a
  `pre-launch.sh` is enough to give it veto power over every launch -- there
  is no separate "open once to arm" consent step like some paks used to
  implement by hand via `.hooks/`.

## Example: sync after ROM exit

```sh
#!/bin/sh
# shortcuts-resume.sh — one-shot resume metadata sync after a ROM exits

[ "$HOOK_TYPE" = "rom" ] || exit 0

SHORTCUTS_PAK="$SDCARD_PATH/Tools/$PLATFORM/Shortcuts.pak"
[ -x "$SHORTCUTS_PAK/shortcuts" ] || exit 0

"$SHORTCUTS_PAK/shortcuts" --resume-sync-hook >> "$LOGS_PATH/shortcuts-resume-sync.txt" 2>&1
```
