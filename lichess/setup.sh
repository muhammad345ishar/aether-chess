#!/usr/bin/env bash
#
# Set up the lichess-bot bridge so the Aether UCI engine can play on Lichess.
#
# What it does:
#   1. Finds (or builds) the Aether engine binary.
#   2. Clones lichess-bot next to this script.
#   3. Creates a Python venv and installs lichess-bot's requirements.
#   4. Copies the engine into lichess-bot/engines/.
#   5. Generates lichess-bot/config.yml preconfigured for Aether.
#
# Usage:
#   ./setup.sh [LICHESS_TOKEN]
#   LICHESS_BOT_TOKEN=lip_xxx ./setup.sh
#
# The token is optional here; you can also paste it into config.yml afterwards.
# See README.md for how to create the bot account and token.

set -euo pipefail

LICHESS_BOT_REPO="${LICHESS_BOT_REPO:-https://github.com/lichess-bot-devs/lichess-bot.git}"
LICHESS_BOT_REF="${LICHESS_BOT_REF:-}"   # empty = clone the default branch

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$HERE/.." && pwd)"
BOT_DIR="$HERE/lichess-bot"
TOKEN="${1:-${LICHESS_BOT_TOKEN:-}}"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------------------
# 1. Locate or build the Aether engine binary.
# ---------------------------------------------------------------------------
ENGINE_SRC=""
for candidate in "$PROJECT_ROOT/build/aether" "$PROJECT_ROOT/build-ci/aether"; do
  if [ -x "$candidate" ]; then ENGINE_SRC="$candidate"; break; fi
done

if [ -z "$ENGINE_SRC" ]; then
  log "No prebuilt engine found; building Aether (Release)..."
  cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$PROJECT_ROOT/build" -j
  ENGINE_SRC="$PROJECT_ROOT/build/aether"
fi
log "Using engine binary: $ENGINE_SRC"

# ---------------------------------------------------------------------------
# 2. Clone lichess-bot.
# ---------------------------------------------------------------------------
if [ ! -d "$BOT_DIR/.git" ]; then
  log "Cloning lichess-bot (${LICHESS_BOT_REF:-default branch})..."
  if [ -n "$LICHESS_BOT_REF" ]; then
    git clone --branch "$LICHESS_BOT_REF" "$LICHESS_BOT_REPO" "$BOT_DIR"
  else
    git clone "$LICHESS_BOT_REPO" "$BOT_DIR"
  fi
else
  log "lichess-bot already cloned; leaving it as-is (git pull to update)."
fi

# ---------------------------------------------------------------------------
# 3. Python venv + dependencies.
# ---------------------------------------------------------------------------
if [ ! -d "$BOT_DIR/venv" ]; then
  log "Creating Python venv..."
  python3 -m venv "$BOT_DIR/venv"
fi
log "Installing lichess-bot requirements..."
"$BOT_DIR/venv/bin/python" -m pip install --upgrade pip >/dev/null
"$BOT_DIR/venv/bin/python" -m pip install -r "$BOT_DIR/requirements.txt"

# ---------------------------------------------------------------------------
# 4. Copy the engine into lichess-bot/engines/.
# ---------------------------------------------------------------------------
mkdir -p "$BOT_DIR/engines"
cp "$ENGINE_SRC" "$BOT_DIR/engines/aether"
chmod +x "$BOT_DIR/engines/aether"
log "Installed engine at $BOT_DIR/engines/aether"

# ---------------------------------------------------------------------------
# 5. Generate config.yml preconfigured for Aether.
# ---------------------------------------------------------------------------
log "Writing config.yml..."
CONFIG_TOKEN="$TOKEN" "$BOT_DIR/venv/bin/python" - "$BOT_DIR" <<'PY'
import os, sys
import yaml

bot_dir = sys.argv[1]
cfg_path = os.path.join(bot_dir, "config.yml")
default_path = os.path.join(bot_dir, "config.yml.default")

# Start from an existing config (to preserve a token you already set),
# otherwise from the upstream default so every required key is present.
src = cfg_path if os.path.exists(cfg_path) else default_path
with open(src) as f:
    cfg = yaml.safe_load(f)

token = os.environ.get("CONFIG_TOKEN", "").strip()
if token:
    cfg["token"] = token
elif not cfg.get("token") or cfg.get("token") == "xxxxxxxxxxxxxxxx":
    cfg["token"] = ""  # fill this in before running

eng = cfg.setdefault("engine", {})
eng["dir"] = "./engines/"
eng["name"] = "aether"
eng["protocol"] = "uci"
eng["ponder"] = False  # Aether rejects "go ponder"; must stay off
# Aether only understands the "Hash" UCI option; do not send others.
eng["uci_options"] = {"Hash": 256}

# Aether plays standard chess only (no variants / Chess960).
ch = cfg.setdefault("challenge", {})
ch["variants"] = ["standard"]

with open(cfg_path, "w") as f:
    yaml.safe_dump(cfg, f, sort_keys=False, default_flow_style=False, allow_unicode=True)

print("  token set:" , "yes" if cfg.get("token") else "NO - edit config.yml")
PY

echo
log "Done. Next steps:"
cat <<EOF

  1. If you have not already, put your Lichess bot token in:
       $BOT_DIR/config.yml   (the 'token:' line)

  2. First run only, upgrade the account to a BOT account (irreversible):
       cd "$BOT_DIR" && ./venv/bin/python lichess-bot.py -u

  3. On later runs just start the bot:
       cd "$BOT_DIR" && ./venv/bin/python lichess-bot.py

  Or use the helper:  $HERE/run.sh
EOF
