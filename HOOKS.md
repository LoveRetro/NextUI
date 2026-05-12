# About NextUI hooks

## The idea

Hooks are platform-specific, just like paks. The launcher reads them from:

```
$USERDATA_PATH/.hooks/
    pre-launch.d/    # scripts run before launch
    post-launch.d/   # scripts run after launch exits
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

- Each script runs in a subshell. A crash or non-zero exit will not affect the launcher or other hooks.
- Script output (stdout/stderr) is suppressed. If you need logging, write to your own log file.
- Pre-launch hooks cannot cancel the launch. They are for observation and setup only.
- Keep hooks fast. A slow hook delays the launch or the return to the menu.
- Unlike auto.sh, each pak should manage their own hook and use a descriptive filename to avoid collisions.


## Example: sync after ROM exit

```sh
#!/bin/sh
# shortcuts-resume.sh — one-shot resume metadata sync after a ROM exits

[ "$HOOK_TYPE" = "rom" ] || exit 0

SHORTCUTS_PAK="$SDCARD_PATH/Tools/$PLATFORM/Shortcuts.pak"
[ -x "$SHORTCUTS_PAK/shortcuts" ] || exit 0

"$SHORTCUTS_PAK/shortcuts" --resume-sync-hook >> "$LOGS_PATH/shortcuts-resume-sync.txt" 2>&1
```
