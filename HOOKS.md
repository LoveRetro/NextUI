# About NextUI hooks

## The idea

Hooks are pak-scoped and self-registering. A Tools pak that wants to react to
a point in the system lifecycle drops one or more of these files right next
to its own `launch.sh`:

```
Tools/<platform>/SomePak.pak/
    launch.sh
    boot.sh          # optional -- run once at boot
    pre-launch.sh     # optional -- run before a launch, can cancel it
    post-launch.sh    # optional -- run after a launch exits
    pre-sleep.sh       # optional -- run before the device sleeps or powers off
    post-resume.sh     # optional -- run after the device wakes from sleep
```

`pak-hooks.sh` scans every installed Tools pak for these filenames and runs
whichever exist at the matching point in the lifecycle, in alphabetical
order by pak folder name. The file's presence *is* the registration: there
is no install step, no arming (no need to open the pak first), and removing
the pak removes its hooks with it.

If no pak registers a given phase, nothing happens and there is no overhead.

## Environment variables

Hook scripts inherit all standard NextUI environment variables (`SDCARD_PATH`, `PLATFORM`, `USERDATA_PATH`, `SHARED_USERDATA_PATH`, etc.) plus these launch-specific ones, for `pre-launch.sh`/`post-launch.sh`:

| Variable | Description |
|---|---|
| `HOOK_TYPE` | `rom` or `pak` |
| `HOOK_CMD` | The raw launch command |
| `HOOK_EMU_PATH` | Path to the emulator or pak `launch.sh` |
| `HOOK_ROM_PATH` | Path to the ROM file (empty for pak launches) |
| `HOOK_LAST` | Contents of `/tmp/last.txt` (the last selected menu entry) |

`boot.sh`/`pre-sleep.sh`/`post-resume.sh` get none of these -- there's no
launch command to describe at those points. There's no `HOOK_PHASE` either:
your script's own filename (`pre-launch.sh`, `post-resume.sh`, ...) already
says which phase it's running for.

## Writing a hook script

```sh
#!/bin/sh
# pre-launch.sh — log every ROM launch

[ "$HOOK_TYPE" = "rom" ] || exit 0
echo "$(date): launched $HOOK_ROM_PATH" >> "$LOGS_PATH/launches.log"
```

This is how the built-in `Game Tracker.pak` hangs `gametimectl.elf
start/stop/stop_all/resume` off the lifecycle -- see its own
`pre-launch.sh`/`post-launch.sh`/`pre-sleep.sh`/`post-resume.sh`. Core code
(`api.c`, `nextui.c`) has no direct knowledge of gametimectl any more, it
only fires the generic `pak-hooks.sh` events.

## Rules

- Each script runs in a subshell. A crash will not affect the launcher or other paks' hooks.
- Script output (stdout/stderr) is suppressed. If you need logging, write to your own log file.
- Only `pre-launch.sh` can cancel anything: a non-zero exit from **any**
  registered pak's `pre-launch.sh` cancels the launch -- every pak that
  registers one is assumed to need to agree before a game runs. `boot.sh`,
  `post-launch.sh`, `pre-sleep.sh` and `post-resume.sh` are fire-and-forget;
  their exit code is ignored. Sleep in particular isn't cancellable.
- Every registered `pre-launch.sh` runs in the same pass, in alphabetical
  order by pak folder name, with **no ordering guarantee relative to
  another pak's veto**. If your hook does something that must be undone
  when a *different* pak vetoes the launch, that vetoing pak is responsible
  for the cleanup itself -- see `Game Tracker.pak/pre-launch.sh`, which
  always starts tracking unconditionally because it cannot know in advance
  whether another pak will refuse the launch.
- Keep hooks fast. A slow hook delays the launch, sleep, or the return to the menu.
- Use a descriptive filename inside your own pak folder; collisions across
  paks aren't possible since each hook lives inside its owning pak.
- If several of your phases share logic, don't merge them into one script
  dispatched by an argument or env var -- that would force `pak-hooks.sh` to
  invoke it for every phase just to find out which ones it actually handles,
  losing the whole point of presence-based registration. Put the shared part
  in a plain file next to your hooks and `source` it from each one instead:
  `. "$(dirname "$0")/common.sh"`.

## Caching

The list of paks with at least one registered hook is cached
(`/tmp/pak_hooks_cache.txt`) to avoid re-scanning every Tools pak on every
single launch or sleep. It's built fresh on the very first call of the
session -- in practice `pak-hooks.sh boot`, so it's always current right
after a reboot.

After that, it's only rebuilt when a **Tools pak** (not a rom) exits --
see `MinUI.pak/launch.sh`, right after `eval $CMD` when `$HOOK_TYPE` is
`pak`. A rom can't change what's under `Tools/`, so the overwhelming
majority of returns to the menu (finishing a game) skip the rescan
entirely; a pak that plausibly did change something there (Pak Store,
Files.pak, ...) triggers a refresh as soon as it closes, so a newly
installed/removed pak is picked up on the very next launch instead of
staying stale until the next reboot.

## Legacy hooks (deprecated)

Before `pak-hooks.sh`, hooks were plain scripts a pak copied into
`$USERDATA_PATH/.hooks/{boot,pre-launch,post-launch,pre-sleep,post-resume}.d/`
(usually the first time it was opened), run by `run_hooks.sh`. This
mechanism is **deprecated** and kept only so a pak that already dropped
scripts there doesn't silently break:

- It requires that install/arming step, and nothing removes the script if
  the pak is later deleted -- it becomes an orphan.
- `pre-launch.d`/`post-launch.d` scripts here **cannot veto a launch**; only
  a pak-scoped `pre-launch.sh` can.
- `run_hooks.sh` logs a warning to `$LOGS_PATH/hooks-deprecated.txt`
  whenever it actually finds a script to run in one of these directories,
  so a stale legacy hook doesn't go unnoticed.

New hooks should always be pak-scoped (`pak-hooks.sh`), never dropped into
`.hooks/`. Migrating an existing legacy hook just means moving the script
into your pak's own folder under the matching name (`pre-launch.sh`,
`post-launch.sh`, `pre-sleep.sh`, `post-resume.sh`, or `boot.sh`).
