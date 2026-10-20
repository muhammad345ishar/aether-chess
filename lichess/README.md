# Running Aether on Lichess

Aether is a UCI engine, so it plays on Lichess through the official
[`lichess-bot`](https://github.com/lichess-bot-devs/lichess-bot) bridge:
Lichess talks to `lichess-bot` over the Bot API, and `lichess-bot` talks to
Aether over UCI on stdin/stdout.

```
Lichess  <--Bot API-->  lichess-bot  <--UCI-->  aether
```

The scripts in this folder wire that up for you. You still have to create the
bot account and token yourself — that part can only be done from your Lichess
account.

## 1. Create the bot account and token (do this on lichess.org)

1. Register a **brand-new** Lichess account for the bot. It must have **zero
   games played** — Lichess refuses to convert an account that has already
   played. Do not use your personal account.
2. Log in as that account and create a token at
   <https://lichess.org/account/oauth/token/create>.
   - Enable the **`bot:play`** scope ("Play games with the bot API").
   - Give it any description, e.g. `aether-bot`.
   - Copy the token (starts with `lip_`). You only see it once.

Converting an account to a BOT account is **irreversible**, and a bot account
can never play from the website — only through the API.

## 2. Set up the bridge (run in this folder)

```bash
./setup.sh lip_yourTokenHere
```

This will:
- find the built `aether` binary (or build it if missing),
- clone `lichess-bot` into `lichess/lichess-bot/`,
- create a Python virtualenv and install `lichess-bot`'s dependencies,
- copy the engine to `lichess-bot/engines/aether`,
- generate `lichess-bot/config.yml` preconfigured for Aether.

You can omit the token and paste it into `lichess-bot/config.yml` afterwards
(the `token:` line) instead.

## 3. Upgrade the account, then play

First run only — turn the account into a BOT account (irreversible):

```bash
./run.sh -u
```

After that, just start the bot:

```bash
./run.sh
```

The bot is now online. It accepts challenges according to `config.yml`. To play
it, open `https://lichess.org/@/<your-bot-username>` and send a challenge, or
enable matchmaking in the config so it seeks games on its own. Stop the bot with
`Ctrl-C`.

## What the generated config sets for Aether

`setup.sh` starts from `lichess-bot`'s own `config.yml.default` (so every
required field is present) and overrides only what Aether needs:

| Setting | Value | Why |
|---|---|---|
| `engine.name` | `aether` | the copied binary |
| `engine.dir` | `./engines/` | where it lives |
| `engine.protocol` | `uci` | Aether speaks UCI |
| `engine.ponder` | `false` | Aether rejects `go ponder`, so pondering must stay off |
| `engine.uci_options` | `{ Hash: 256 }` | Aether's only tunable UCI option |
| `challenge.variants` | `[ standard ]` | Aether plays standard chess only |

Everything else keeps `lichess-bot`'s defaults. Edit `lichess-bot/config.yml`
to change accepted time controls, rated vs. casual, concurrency, greetings,
matchmaking, etc. — see the comments in `config.yml.default`.

**Do not raise `Hash` above the engine's max (1024 MB), and do not add other
UCI options** — Aether only implements `Hash` (and the `Clear Hash` button).
It ignores unknown options, but there is no point sending them.

## Requirements and notes

- Python 3.9+ (you have a working `python3`). `lichess-bot`'s dependency wheels
  should install into the venv automatically. If `pip install` fails on a very
  new Python, install a 3.11–3.13 interpreter and re-run `./setup.sh`.
- A C++17 compiler and CMake 3.16+ are only needed if the engine has not been
  built yet (`../build/aether`).
- `lichess-bot/`, the venv, and your token are intentionally **not** committed
  (see `.gitignore`). Keep the token secret — anyone with it controls the bot
  account.
- Windows: run `lichess-bot` per its own README (the binary would be
  `aether.exe`); the config settings above are the same.

## Troubleshooting

- **"token is required" / 401 Unauthorized** — the `token:` line in
  `config.yml` is empty or wrong, or the token lacks the `bot:play` scope.
- **"This is not a bot account"** — run `./run.sh -u` once to upgrade it.
- **Account can't be upgraded** — it has already played a game; make a fresh
  account.
- **Engine not found** — build it: `cmake -S .. -B ../build -DCMAKE_BUILD_TYPE=Release && cmake --build ../build -j`, then re-run `./setup.sh`.
