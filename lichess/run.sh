#!/usr/bin/env bash
#
# Start the Aether bot on Lichess. Run setup.sh first.
# Pass -u the very first time to upgrade the account to a BOT account.
#
#   ./run.sh        # start playing
#   ./run.sh -u     # upgrade account to BOT, then start playing

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOT_DIR="$HERE/lichess-bot"

if [ ! -x "$BOT_DIR/venv/bin/python" ]; then
  echo "lichess-bot is not set up yet. Run ./setup.sh first." >&2
  exit 1
fi

cd "$BOT_DIR"
exec ./venv/bin/python lichess-bot.py "$@"
