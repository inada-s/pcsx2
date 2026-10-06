# zdxsv rig tests

Scripts that run one or more PCSX2 instances with the game (and, for the lobby
tests, a local zdxsv server) and check the result from logs. Which test covers
which feature: `docs/zdxsv/features.md`. Each script's usage and knobs are in
its header comment.

## Setup

- Windows, Git Bash (`bash`), PowerShell 5.1, Python 3 with Pillow (windowed
  snapshots only).
- A PCSX2 build of this repository (`bin/pcsx2-qtx64.exe`, or `PCSX2_EXE`).
- The game image (`ISO`). It is not part of this repository.
- The lobby tests (`m4*.sh`, `stack.sh`, `rbkprep.sh`) need a zdxsv checkout
  with its binaries built in `$ZDXSV/bin` (`zdxsv.exe`, `zproxy.exe`) and Go to
  build `battlereg/`.
- Lobby save states (`STATES`) and the rbk start states (`RBKSTATES`, made by
  `rbkprep.sh`) are made on the machine; they are not part of this repository.

## Settings

Each setting comes from the environment, else from `tests/zdxsv/local.env`
(`KEY=value` lines, ignored by git). A script that needs a missing setting
fails with exit 2 and names it.

| Setting | Used by | Value |
|---|---|---|
| `RUN` | all | work root: instance data dirs `$RUN/pN/PCSX2` |
| `ISO` | `launch.ps1` | the game image |
| `PCSX2_EXE` | `launch.ps1` | PCSX2 to test (default `bin/pcsx2-qtx64.exe` of this tree) |
| `ZDXSV` | lobby tests | zdxsv checkout (`bin/zdxsv.exe`, `bin/zproxy.exe`) |
| `IP` | lobby tests | this machine's LAN IPv4 address, given to the game as the server |
| `STATES` | `m4.sh`, `rbkprep.sh` | dir of the lobby save states of p1..p4 |
| `RBKSTATES` | `rbk.sh` | dir of `rbk-p1..p4.p2s` from `rbkprep.sh` |
| `CARDS` | `m4z.sh` | memory card names of p1..p4, space separated |
| `USERS` | `m4z.sh` | zdxsv.db user ids of those cards, same order |
| `DZ` | `stack.sh` | zdxsv binary for login + dnas (default the lobby's) |
| `ZDXSV_PY` | all | python with Pillow (default `python`) |
| `GOEXE` | `m4.sh`, `zbincheck.sh` | go (default `go`) |

Example `local.env`:

```
RUN=G:/zdxsv-run
ISO=G:/rom/game.iso
ZDXSV=G:/src/zdxsv
IP=192.168.1.8
STATES=G:/zdxsv-run/states
RBKSTATES=G:/zdxsv-run/prep
CARDS=card-p1 card-p2 card-p3 card-p4
USERS=AAAAAA BBBBBB CCCCCC DDDDDD
```

## Rules

- One rig at a time: every rig takes the lock in `riglock.sh` / `riglock.ps1`
  (the instances share fixed ports).
- `launch.ps1` runs `probelint.py` first and refuses a PCSX2 build older than
  its sources.
- Run from the repository root, e.g. `OUT=G:/zdxsv-run/out bash tests/zdxsv/rbk.sh 2 1`.
  Exit 0 = every check passed; a failed check prints `FAIL` and its reason.
