# PCSX2 for zdxsv: setup (Windows)

This build of PCSX2 is set up to play *Mobile Suit Gundam: Gundam vs. Zeta Gundam*
(PS2, 2004, Japan) online on the zdxsv server (`zdxsv.net`).

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

## Network settings (already set)

A new setup already has these settings, under Settings → Network & HDD:

| Setting | Value |
|---|---|
| Ethernet enabled | on |
| Ethernet device type | Sockets |
| Ethernet device | Auto |
| Intercept DHCP | on |
| DNS1 / DNS2 | Internal |
| Hosts | `www01.kddi-mmbb.jp`, `gate1.jp.dnas.playstation.org`, `ca1202.mmcp6`, `ca1203.mmcp6` → `153.121.44.150` (zdxsv.net) |
| zdxsv: Rollback netcode (GGPO) for online battles | on (takes effect at the next game start) |

Do not change them. If you copy a `PCSX2.ini` from another PCSX2 installation, it
replaces these settings. In that case, enter the values from the table yourself, or
delete `inis\PCSX2.ini` to get a new one with these defaults.

If Windows Firewall asks whether PCSX2 can use the network, allow it.

## First time in the game

If your memory card is empty, the game makes the files it needs by itself:

1. Main menu → 通信対戦. When the game cannot find the NET file, choose 新規作成 → はい,
   memory card slot 1, then はい to format the card.
2. Connection menu → ブロードバンド接続 → ネットワーク接続. Accept the terms.
3. There are no network settings yet. Choose ネットワーク設定の編集 → 新規作成 → slot 1.
   Choose these options: Ethernet, PPPoE 必要ではない, DHCP automatic, DNS automatic.
   Save the settings (for example as せってい1). Skip the connection test, then choose 終了する.
   The game then restarts.
4. 通信対戦 again → choose your network setting → connect. On the top screen, choose
   新規登録 to make an ID. The game saves the ID to the memory card.
5. ログイン, then enter a handle name. You then see 戦場選択 (the lobby).

Later sessions only need 通信対戦 → ログイン.

## Updates

When PCSX2 starts and a newer zdxsv release exists on
https://github.com/inada-s/pcsx2/releases, it shows an update window.
Press "Download and Install...": PCSX2 downloads the new version, closes, replaces its
files and starts again. Your settings, memory cards and save states are kept.
"Remind Me Later" asks again on the next start; "Skip This Update" waits for the next version.
You can also check by hand from the Help menu (Check for Updates).
