#!/bin/sh
# tests/menu/title_update_then_new_game.toml: Check for Updates answered 12 s later (9.9.9), by when NEW GAME has
# freed the title list (the 0.9.0-rc.1 Windows crash: the answer relabelled the freed list's row).
sleep 12
case "$*" in
  *check*) echo '{"current": "0.7.0", "latest": "9.9.9", "update": true, "message": "update available: 9.9.9", "exit": 0}' ;;
  *) echo '{"error": "unknown command", "exit": 2}'; exit 2 ;;
esac
