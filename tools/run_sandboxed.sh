#!/usr/bin/env bash
# Run the recompiled game inside a bubblewrap sandbox. This is the ONLY supported way to start kuf_cod
# (even for --help): see AGENTS.md "Running the game".
#
#   - game files: read-only bind of $KUF_GAME_DIR (default: the original disc folder)
#   - build output: read-only
#   - writable: only build/runs/<run-id>/ (user data, cache, log); an existing directory of that name is
#     renamed to <run-id>.prev-<time> first, so every run starts empty
#   - no network, no X11/Wayland/PulseAudio sockets, SDL offscreen video
#   - audio: the SDK builds SDL without the dummy driver, so SDL uses ALSA with a sandbox-local ~/.asoundrc
#     whose default PCM is ALSA's `null` sink (the host's audio setup is never touched). The null sink does
#     not pace playback, so these runs pass --kuf_audio_pace=true (KNOWN_ISSUES K14)
#   - --audio (opt-in): real sound through the session's PulseAudio-compatible socket
#     ($XDG_RUNTIME_DIR/pulse/native, served by PipeWire) and nothing else; no autospawn, no host config
#   - --pad <dev> (opt-in): one game controller. <dev> must be a /dev/input/by-id/*-event-joystick link that
#     resolves to an evdev node registered only as a joystick (no keyboard or mouse handler); only that
#     node is bound. Its identity is printed. No other input devices, no hotplug
#   - saves (the game's content packages) live in their own directory, build/runs/<run-id>/saves
#     (--content_root), never in the game folder; every run starts without saves unless
#     --saves-from <dir> copies existing ones in (a saves directory or a run directory; the source is only
#     read), or --save-dir <dir> binds a persistent save directory read-write (opt-in; if it already holds
#     saves they are first backed up to build/save-backups/, and overwriting them needs confirmation: an
#     interactive yes, or --save-dir-confirm)
#   - --cache-from <dir>: start with a copy of a shader/pipeline cache (a cache directory or a run directory;
#     the source is only read), e.g. to measure with a warm cache as a player has after the first session
#   - --input-script <file>: scripted input from a file (copied into the run directory)
#   - --copy-in <file>: a test helper copied to /run-data/in/<name> (M7: a fake updater for Check for Updates)
#   - --pc-settings-from <file or run dir>: start with a copy of a pc_settings.toml (M6B; a file, or a run
#     directory's user/pc_settings.toml; the source is only read)
#   - --window (only with your explicit permission to use the display): binds the Wayland socket and
#     nothing else (no X11, no audio, no other runtime files), SDL's wayland driver, --fullscreen=false (the
#     window then takes the display mode of pc_settings.toml, M6C). Offscreen runs draw for a simulated
#     1280x720 output (--kuf_display_size, overridable)
#   - hard wall-clock limit (default 60 s)
#   - bounded logs: 64 MB x 2 files unless overridden with --log_max_file_size_mb/--log_max_files
#   - on a crash (exit status 70) the report is symbolized to guest instruction addresses
#
# Usage: tools/run_sandboxed.sh [-p preset] [-t seconds] [-n run-id] [--gpu] [--window] [--audio]
#                               [--pad /dev/input/by-id/...-event-joystick] [--gdb script.gdb]
#                               [--saves-from dir | --save-dir dir [--save-dir-confirm]]
#                               [--cache-from dir] [--input-script file] [--pc-settings-from file|dir]
#                               [-- extra kuf_cod args]
#   --gdb runs the game under `gdb -batch -x script` inside the same sandbox (output in gdb.txt).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRESET=linux-amd64-relwithdebinfo
TIMEOUT=60
RUN_ID="run-$(date +%Y%m%d-%H%M%S)"
GPU=""
GDB_SCRIPT=""
WINDOW=""
AUDIO=""
PAD=""
SAVES_FROM=""
SAVE_DIR=""
SAVE_DIR_CONFIRM=""
CACHE_FROM=""
INPUT_SCRIPT=""
PC_SETTINGS_FROM=""
COPY_IN=()
while [ $# -gt 0 ]; do
  case "$1" in
    -p) PRESET="$2"; shift 2 ;;
    -t) TIMEOUT="$2"; shift 2 ;;
    -n) RUN_ID="$2"; shift 2 ;;
    --gpu) GPU="--gpu_plugin=xenos"; shift ;;
    --window) WINDOW=1; shift ;;
    --audio) AUDIO=1; shift ;;
    --pad) PAD="$2"; shift 2 ;;
    --gdb) GDB_SCRIPT="$(realpath "$2")"; shift 2 ;;
    --saves-from) SAVES_FROM="$(realpath -e "$2")"; shift 2 ;;
    --save-dir) SAVE_DIR="$(realpath -e "$2")"; shift 2 ;;
    --save-dir-confirm) SAVE_DIR_CONFIRM=1; shift ;;
    --cache-from) CACHE_FROM="$(realpath -e "$2")"; shift 2 ;;
    --input-script) INPUT_SCRIPT="$(realpath -e "$2")"; shift 2 ;;
    --pc-settings-from) PC_SETTINGS_FROM="$(realpath -e "$2")"; shift 2 ;;
    --copy-in) COPY_IN+=("$(realpath -e "$2")"); shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
GAME_DIR="${KUF_GAME_DIR:-$(cat "$ROOT/.game_dir" 2>/dev/null || true)}"  # your game folder (.game_dir: not committed)
[ -n "$GAME_DIR" ] || { echo "set KUF_GAME_DIR, or write your game folder's path into $ROOT/.game_dir" >&2; exit 1; }
BIN="$ROOT/build/$PRESET/bin"
RUN="$ROOT/build/runs/$RUN_ID"
[ -x "$BIN/kuf_cod" ] || { echo "no build at $BIN (build first)" >&2; exit 1; }
[ -d "$GAME_DIR" ] || { echo "game dir not found: $GAME_DIR" >&2; exit 1; }
GAME_REAL="$(realpath -e "$GAME_DIR")"
inside() { [ "$1" = "$2" ] || [[ "$1" == "$2"/* ]]; }
[ -z "$SAVES_FROM" ] || [ -z "$SAVE_DIR" ] || { echo "--saves-from and --save-dir exclude each other" >&2; exit 2; }
for d in "$SAVES_FROM" "$SAVE_DIR"; do
  [ -z "$d" ] && continue
  [ -d "$d" ] || { echo "not a directory: $d" >&2; exit 1; }
  if inside "$d" "$GAME_REAL" || inside "$GAME_REAL" "$d"; then
    echo "save directories never overlap the game folder: $d" >&2; exit 1
  fi
done
if [ -n "$SAVES_FROM" ] && [ -d "$SAVES_FROM/saves" ]; then
  SAVES_FROM="$SAVES_FROM/saves"  # a run directory
fi
if [ -n "$CACHE_FROM" ]; then
  [ -d "$CACHE_FROM/cache" ] && CACHE_FROM="$CACHE_FROM/cache"  # a run directory
  [ -d "$CACHE_FROM" ] || { echo "not a directory: $CACHE_FROM" >&2; exit 1; }
  if inside "$CACHE_FROM" "$RUN" || inside "$CACHE_FROM" "$GAME_REAL" || inside "$GAME_REAL" "$CACHE_FROM"; then
    echo "--cache-from must be outside this run's directory and the game folder: $CACHE_FROM" >&2; exit 1
  fi
fi
if [ -n "$PC_SETTINGS_FROM" ]; then
  [ -d "$PC_SETTINGS_FROM" ] && PC_SETTINGS_FROM="$PC_SETTINGS_FROM/user/pc_settings.toml"  # a run directory
  [ -f "$PC_SETTINGS_FROM" ] || { echo "no settings file: $PC_SETTINGS_FROM" >&2; exit 1; }
  if inside "$PC_SETTINGS_FROM" "$RUN" || inside "$PC_SETTINGS_FROM" "$GAME_REAL"; then
    echo "--pc-settings-from must be outside this run's directory and the game folder: $PC_SETTINGS_FROM" >&2; exit 1
  fi
fi
if [ -n "$SAVE_DIR" ]; then
  # Personal saves must not end up in Git: inside the repository only below build/ (ignored).
  if inside "$SAVE_DIR" "$ROOT" && ! inside "$SAVE_DIR" "$ROOT/build"; then
    echo "--save-dir inside the repository must be below build/ (never tracked): $SAVE_DIR" >&2; exit 1
  fi
  if [ -n "$(ls -A "$SAVE_DIR")" ]; then
    BACKUP="$ROOT/build/save-backups/$(basename "$SAVE_DIR")-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$(dirname "$BACKUP")"
    cp -a "$SAVE_DIR" "$BACKUP"
    echo "saves in $SAVE_DIR backed up to $BACKUP"
    if [ -z "$SAVE_DIR_CONFIRM" ]; then
      if [ -t 0 ]; then
        read -r -p "The game may overwrite the saves in $SAVE_DIR. Continue? [y/N] " answer
        [[ "$answer" =~ ^[yY] ]] || { echo "cancelled; nothing changed" >&2; exit 1; }
      else
        echo "$SAVE_DIR already holds saves: pass --save-dir-confirm to let the game overwrite them" >&2; exit 1
      fi
    fi
  fi
fi

PAD_ARGS=()
if [ -n "$PAD" ]; then
  case "$PAD" in
    /dev/input/by-id/*-event-joystick) ;;
    *) echo "--pad needs a /dev/input/by-id/*-event-joystick link, got $PAD" >&2; exit 1 ;;
  esac
  NODE="$(readlink -f "$PAD")"
  [[ "$NODE" =~ ^/dev/input/event[0-9]+$ ]] && [ -c "$NODE" ] || { echo "--pad: $PAD -> $NODE is not an evdev node" >&2; exit 1; }
  SYS="/sys/class/input/$(basename "$NODE")/device"
  # The kernel's handler list for this device: a joystick (js*) and nothing that reads keys or a pointer.
  HANDLERS="$(awk -v ev="$(basename "$NODE")" '/^H: Handlers=/ { sub(/^H: Handlers=/, ""); for (i = 1; i <= NF; i++) if ($i == ev) { print; exit } }' /proc/bus/input/devices)"
  if ! [[ " $HANDLERS " =~ \ js[0-9]+\  ]] || [[ " $HANDLERS " =~ \ (kbd|mouse[0-9]*)\  ]]; then
    echo "--pad: $NODE is not a joystick-only device (handlers: $HANDLERS)" >&2; exit 1
  fi
  echo "controller: $PAD -> $NODE \"$(cat "$SYS/name")\" usb $(cat "$SYS/id/vendor"):$(cat "$SYS/id/product") handlers: $HANDLERS"
  PAD_ARGS=(--dev-bind "$NODE" "$NODE")
fi

# A run directory belongs to one run: logs and the capture index append, so a reused run id would mix runs
# (and could satisfy a test with an older run's log lines). Keep the old one under a new name; nothing is
# deleted.
if [ -e "$RUN" ]; then
  mv "$RUN" "$RUN.prev-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$RUN/home" "$RUN/user" "$RUN/cache" "$RUN/saves"
if [ -n "$CACHE_FROM" ]; then
  cp -a "$CACHE_FROM/." "$RUN/cache/"
  chmod -R u+w "$RUN/cache"
  echo "cache copied from $CACHE_FROM"
fi
for f in "${COPY_IN[@]}"; do  # test helpers (e.g. a fake updater): /run-data/in/<name>, executable bits kept
  [ -f "$f" ] && ! inside "$f" "$GAME_REAL" || { echo "--copy-in must be a file outside the game folder: $f" >&2; exit 1; }
  mkdir -p "$RUN/in" && cp -p "$f" "$RUN/in/"
done
if [ -n "$PC_SETTINGS_FROM" ]; then
  cp "$PC_SETTINGS_FROM" "$RUN/user/pc_settings.toml"
  chmod u+w "$RUN/user/pc_settings.toml"
  echo "pc_settings.toml copied from $PC_SETTINGS_FROM"
fi
SAVE_ARGS=()
if [ -n "$SAVES_FROM" ]; then
  cp -a "$SAVES_FROM/." "$RUN/saves/"
  chmod -R u+w "$RUN/saves"
  echo "saves copied from $SAVES_FROM"
elif [ -n "$SAVE_DIR" ]; then
  SAVE_ARGS=(--bind "$SAVE_DIR" /run-data/saves)
  echo "persistent saves: $SAVE_DIR"
fi
SCRIPT_ARGS=()
if [ -n "$INPUT_SCRIPT" ]; then
  cp "$INPUT_SCRIPT" "$RUN/input-script.txt"
  SCRIPT_ARGS=(--kuf_input_script_file=/run-data/input-script.txt)
fi
cat > "$RUN/home/.asoundrc" <<'ASOUND'
pcm.!default { type null }
ctl.!default { type hw card 0 }
ASOUND

LAUNCH=(/app/kuf_cod)
GDB_BIND=()
if [ -n "$GDB_SCRIPT" ]; then
  cp "$GDB_SCRIPT" "$RUN/script.gdb"
  LAUNCH=(gdb -nx -batch -x /run-data/script.gdb --args /app/kuf_cod)
fi

DRI=()
[ -n "$GPU" ] && [ -d /dev/dri ] && DRI=(--dev-bind /dev/dri /dev/dri --ro-bind /sys /sys)

VIDEO=(--setenv SDL_VIDEO_DRIVER offscreen)
# Offscreen there is no real display (SDL's offscreen one is 1024x768): the game draws for a simulated 1280x720
# output, as the original (M6C), unless the run gives --kuf_display_size itself.
WINDOW_ARGS=()
[[ " $* " == *" --kuf_display_size="* ]] || WINDOW_ARGS=(--kuf_display_size=1280x720)
if [ -n "$WINDOW" ]; then
  SOCK="${XDG_RUNTIME_DIR:-}/${WAYLAND_DISPLAY:-}"
  [ -n "${WAYLAND_DISPLAY:-}" ] && [ -S "$SOCK" ] || { echo "--window needs a Wayland session socket" >&2; exit 1; }
  VIDEO=(--ro-bind "$SOCK" "/tmp/$WAYLAND_DISPLAY" --setenv WAYLAND_DISPLAY "$WAYLAND_DISPLAY"
         --setenv SDL_VIDEO_DRIVER wayland)
  WINDOW_ARGS=(--fullscreen=false)
fi

AUDIO_ARGS=(--setenv SDL_AUDIO_DRIVER alsa)
GAME_AUDIO_ARGS=(--kuf_audio_pace=true)
if [ -n "$AUDIO" ]; then
  PULSE_SOCK="${XDG_RUNTIME_DIR:-}/pulse/native"
  [ -S "$PULSE_SOCK" ] || { echo "--audio needs the session's PulseAudio socket ($PULSE_SOCK)" >&2; exit 1; }
  printf 'autospawn = no\n' > "$RUN/home/pulse-client.conf"
  AUDIO_ARGS=(--ro-bind "$PULSE_SOCK" /tmp/pulse/native --setenv PULSE_SERVER unix:/tmp/pulse/native
              --setenv PULSE_CLIENTCONFIG /run-data/home/pulse-client.conf --setenv SDL_AUDIO_DRIVER pulseaudio)
  GAME_AUDIO_ARGS=()
fi


CONTENT_ARGS=(--content_root=/run-data/saves)
[[ " $* " == *" --content_root="* ]] && CONTENT_ARGS=()
LOG_LIMITS=()
[[ " $* " == *" --log_max_file_size_mb="* ]] || LOG_LIMITS+=(--log_max_file_size_mb=64)
[[ " $* " == *" --log_max_files="* ]] || LOG_LIMITS+=(--log_max_files=2)

set +e
timeout --kill-after=5 "$TIMEOUT" bwrap \
  --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin \
  --ro-bind /etc /etc --proc /proc --dev /dev --tmpfs /tmp "${DRI[@]}" \
  --ro-bind "$GAME_DIR" /game \
  --ro-bind "$BIN" /app \
  --bind "$RUN" /run-data "${SAVE_ARGS[@]}" \
  --unshare-all --die-with-parent --new-session --clearenv \
  --setenv HOME /run-data/home --setenv XDG_RUNTIME_DIR /tmp \
  "${VIDEO[@]}" "${AUDIO_ARGS[@]}" "${PAD_ARGS[@]}" \
  "${LAUNCH[@]}" --game_data_root=/game --user_data_root=/run-data/user --cache_root=/run-data/cache \
    "${CONTENT_ARGS[@]}" "${SCRIPT_ARGS[@]}" \
    --log_file=/run-data/kuf_cod.log "${LOG_LIMITS[@]}" \
    --headless=true $GPU "${WINDOW_ARGS[@]}" "${GAME_AUDIO_ARGS[@]}" "$@" \
  > "$RUN/stdout.txt" 2>&1
status=$?
set -e
echo "$status" > "$RUN/exit_status"
echo "exit status $status (124 = timeout, 70 = crash report) -- logs in $RUN"
SAVES_SHOWN="${SAVE_DIR:-$RUN/saves}"
if [ -n "$(find "$SAVES_SHOWN" -type f -not -name '.rex-*' -print -quit)" ]; then
  echo "saves in $SAVES_SHOWN:"
  (cd "$SAVES_SHOWN" && find . -type f -not -name '.rex-*' -printf '  %10s  %p\n' | sort -k2)
fi
if [ -f "$RUN/user/crash-report.txt" ]; then
  python3 -I "$ROOT/tools/symbolize_crash.py" "$RUN/user/crash-report.txt" "$BIN" "$ROOT/generated/default" \
    -o "$RUN/crash-report.symbolized.txt"
fi
exit 0
