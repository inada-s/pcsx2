# PCSX2 for zdxsv: setup (Windows)

This build of PCSX2 is set up to play *Mobile Suit Gundam: Gundam vs. Zeta Gundam*
(PS2, 2004, Japan) online on the zdxsv server (`lbs.zdxsv.net`).

## What you need

- Windows 10 or 11, 64-bit.
- A PS2 BIOS dumped from your own console.
- The game disc, or a disc image (ISO) made from it.

## Install

1. Download `pcsx2-zdxsv-windows-x64.zip` from https://github.com/inada-s/pcsx2/releases and extract it to a folder you can write to (for example `C:\Games\pcsx2-zdxsv`).
   Do not put it in `Program Files`.
2. The zip includes a `portable.txt` file. Keep it. With this file, PCSX2 keeps all
   settings, memory cards and save states in this folder. It does not use or change
   the settings of another PCSX2 installation on the same PC.
3. Start `pcsx2-qt.exe`. The first-run wizard asks for the BIOS folder and the game folder.

## Network settings

Online play needs no settings under Settings → Network & HDD. The Ethernet
settings there are not used for online play, on or off.

If Windows Firewall asks whether PCSX2 can use the network, allow it.

Battles use one UDP port: Settings → zdxsv → "UDP port for battles". At the first game
start, PCSX2 picks a random port between 29700 and 29800 and saves it there. If you
cannot connect to opponents directly, forward this UDP port to your PC on your router,
or press "UPnP Now" on the same page to ask a UPnP router to do it (the result shows
next to the button; the forward is removed when PCSX2 exits).
Two players behind the same router need different ports.

When a battle cannot start, a message beginning with "No battle:" (Japanese: "対戦できません:") says why
(an opponent did not answer, the network port is in use, an opponent's build or
settings differ), and the game returns to the lobby.

During an online battle, pause, fast forward, slow motion, frame advance and
loading a save state are not available ("Not available during an online battle.").
In the lobby and in online battles, Escape (Open Pause Menu) does not open the pause
menu ("The pause menu is not available while online."); the game keeps running.

## Recommended settings

Settings → zdxsv has three buttons, for a low-, mid- and high-spec PC. Each sets
the internal resolution (Native, 2x or 3x) and the minimum input delay (3, 2 or 2).
If the game slows down in battles, use the low-spec button.

"Single draw list" (Settings → zdxsv) shows each frame's picture one frame
sooner after your input. It changes only your picture: you stay in sync with
players who have it off.

## First time in the game

If your memory card is empty, the game makes the files it needs by itself:

1. Main menu → 通信対戦. When the game cannot find the NET file, choose 新規作成 → はい,
   memory card slot 1, then はい to format the card.
2. Connection menu: choose any of its buttons and accept the terms. The game connects with no
   network settings.
3. On the top screen, choose 新規登録 to make an ID. The game saves the ID to the memory card.
4. ログイン, then enter a handle name. You then see 戦場選択 (the lobby).

Later sessions need no input: from game start, fast boot runs the game at full speed through the title, 通信対戦 and ブロードバンド接続, the emulator logs in by itself, and the game stops at the ID selection (DNAS and the top screen are skipped). If the memory card is not formatted or has no NET file (for example a new card, before the steps above), fast boot stops at once and returns the controls to you at normal speed. If the path does not fit for another reason, it gives up after 4 minutes of game time (well under a minute on most PCs).
To turn fast boot off (for example to play offline), uncheck Fast boot in Settings → zdxsv.

## Account

Settings → zdxsv → Account → Sign in with Discord opens the zdxsv sign-in page in your browser. The
first sign-in makes a new account; PCSX2 keeps a sign-in token for this PC and logs in with it. To
use the account you play on a PS2, type ｄｃ in the lobby chat on the PS2, enter the code it shows
in Carry-over code, then sign in (within 10 minutes). Sign Out revokes the token.

## Crash reports

When PCSX2 crashes, it sends a crash report to the zdxsv developers.
To send nothing, turn this off in Settings → zdxsv.

## Updates

When PCSX2 starts and a newer zdxsv release exists on
https://github.com/inada-s/pcsx2/releases, it shows an update window.
Press "Download and Install...": PCSX2 downloads the new version, closes, replaces its
files and starts again. Your settings, memory cards and save states are kept.
"Remind Me Later" asks again on the next start; "Skip This Update" waits for the next version.
You can also check by hand from the Help menu (Check for Updates).
If the game shows "PLEASE UPDATE PCSX2" when you connect (PCSX2 also shows "The lobby refused this PCSX2
as outdated"), the server no longer accepts your
version: update as above, then connect again.
