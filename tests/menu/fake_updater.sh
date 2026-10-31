#!/bin/sh
# A stand-in for the installed kuf-update (tests/menu/title_rows.toml, M7 feature 4): answers Check for Updates with
# an update 9.9.9 and installs it, without a network or files. Arguments as the game passes them: --json check |
# --json update --yes.
case "$*" in
  *check*) echo '{"current": "0.7.0", "latest": "9.9.9", "update": true, "message": "update available: 9.9.9", "exit": 0}' ;;
  *update*) echo '{"current": "9.9.9", "previous": "0.7.0", "message": "updated 0.7.0 -> 9.9.9", "exit": 0}' ;;
  *) echo '{"error": "unknown command", "exit": 2}'; exit 2 ;;
esac
