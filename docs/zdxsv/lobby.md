# Lobby connection and GGPO lobby battles

What the emulator adds to the connection with the zdxsv lobby, and how a
battle that the lobby matched runs over GGPO. Option names and defaults are in
[options.md](options.md). The GGPO session itself is in
[rollback.md](rollback.md).

## Network defaults

A new configuration points at zdxsv: adapter on, Sockets API, `Auto` device,
DHCP intercepted, internal DNS. The server hosts of the game resolve to the
zdxsv server:

- `www01.kddi-mmbb.jp`
- `gate1.jp.dnas.playstation.org`
- `ca1202.mmcp6`
- `ca1203.mmcp6`

## Battle server connection

The game always talks TCP to the battle server, as on a PS2. When a battle
runs over GGPO, the emulator answers the battle socket of the game itself and
the game still believes it talks to the battle server.

## Messages added to the lobby connection

The lobby connection carries three custom messages. The game never sees them.
Each body is a list of `key=value` lines.

| Command | Direction | Name | When |
|---|---|---|---|
| `0x9950` | emulator to lobby | platform info | on the first key question of the lobby |
| `0x9951` | lobby to emulator | battle info | when the lobby matched a battle, to clients that sent `udp=1` |
| `0x9952` | emulator to lobby | match report | on the next lobby connection after a battle, right after the platform info |

### Platform info `0x9950`

It lets the server tell emulators from real PS2s.

| Key | Sent | Meaning |
|---|---|---|
| `emulator`, `version`, `os`, `cpu` | always | what runs the game |
| `udp=1` | lobby GGPO on | the client wants battle infos |
| `udp_addr`, `udp_local` | lobby GGPO on | public IPv4 address from the STUN of the lobby, and the local one |
| `udp_addr6` | lobby GGPO on, with a global IPv6 address | global IPv6 address |
| `ggpo=P` | lobby GGPO on | GGPO UDP port |
| `relay_server=1` | lobby GGPO on | the client can route GGPO through relay servers |
| `nat=` | lobby GGPO on | result of the connectivity test |

"Lobby GGPO on" means `ZDXSV_GGPO` with `net=1,lobby=1`.

### Battle info `0x9951`

| Key | Meaning |
|---|---|
| `ggpo_<user>=port` | GGPO port of each player that announced one |
| `p2p_<user>=addr,...` | UDP addresses of each player |
| `ggpo_session=` | id of the battle. zdxsv sends the FNV-1 32 hash of the battle code, as gdxsv. Without it there is no GGPO. |
| `ggpo_ping_ms=` | length of the ping test. zdxsv sends 7500. |
| `name_<user>=` | player name, for the OSD |
| `relay_<k>=<hex token>,<ip:port>[,<[ip6]:port>]` | relay servers of the battle, k = 0 to 3 |
| `battle_code=`, `user_id=` | name the battle in the match report and the replay file |

### Match report `0x9952`

It tells the lobby how the battle ran, as the `lbsP2PMatchingReport` of
flycast. The emulator logs it as `DEV9: TCP: zdxsv matching report: ...`, and
the zdxsv lobby logs it as `p2p matching report: ...`.

| Key | Sent | Meaning |
|---|---|---|
| `battle_code`, `user_id` | always | from the battle info |
| `result` | always | `ggpo`, `cut` or `server`; `server` adds `reason` |
| `position`, `players` | always | own battle position and the player count |
| `rtt_<pos>`, `addr_<pos>`, `path_<pos>` | ping test ran | RTT per peer (-1 = no answer), the picked address and path |
| `relays`, `ping_wait_ms`, `delay` | ping test ran | relay server count, time waited for the test, GGPO input delay |
| `close`, `frames`, `rollback_frames`, `mismatches`, `disconnected` | GGPO battle | why the session ended and its counters |
| `answered`, `cut_sends` | cut | peers that answered, and sends dropped during the cut |

## How a lobby battle starts

1. The platform info announces the GGPO port and the addresses of the client.
2. The battle info lists the players. The ping test starts on the GGPO port.
3. At the first key message of the battle the emulator decides:
   - A player has no GGPO port: the battle stays on the battle server.
   - A peer answered the ping test on no address: the battle connection is
     cut. See [Ping test failure](#ping-test-failure).
   - Otherwise the battle runs over GGPO, with the address and the input delay
     the ping test picked.

One GGPO battle runs per emulator run. Later battles use the battle server.

## Ping test

The test runs on the GGPO UDP port for `ggpo_ping_ms`, from an IPv4 and an
IPv6 socket. It uses the `UdpPingPong` packet of flycast: magic, session id,
from and to battle position, candidate index, timestamps. Packets with another
session, position or source address are dropped.

Address candidates of a peer, in order:

1. its public IPv4 address, or its local IPv4 address when it is behind the
   same public IP
2. its other IPv4 address
3. its IPv6 address

The test pings every candidate of every peer. Each peer is then reached at the
candidate with the best score: lowest RTT, plus 100 for loopback, 50 for a
private address, 20 for IPv6.

The GGPO input delay is max(`mindelay`, ceil(mean RTT of the slowest peer / 2
/ 16 ms)), as the rollback backend of flycast. `delay=` keeps the delay fixed
and skips the test.

Log lines: `zdxsv: ping test: ...` (RTT per peer), `ZdxsvGgpo: lobby delay D`,
`ZdxsvGgpo: lobby path to position P: direct|peer K|relay K`.

### Ping test failure

When a peer answered on no candidate, the GGPO session does not start and the
battle connection is cut, as a connection failure. There is no fallback to the
battle server. Sends on the battle socket are dropped and nothing arrives, so
the game gets no response, closes the socket and reconnects to the lobby. Its
post-battle report has `battles=0`.

Log lines: `lobby battle connection cut: K of N peers answered the ping test`,
then `connection cut ended ... game RPC 0xd`.

## Relay servers

Relay servers work as in flycast with the `relay` of gdxsv. The ping test also
pings each relay server of the battle info over IPv4 and IPv6, with the 28-byte
relay ping of gdxsv, and shares the RTT and relay-RTT matrices with the peers
(`PacketWithRelays` of flycast).

A peer is reached through:

- relay server k, when that path (own plus peer relay RTT) is 16 ms faster
  than the direct one
- another peer, when that path is 32 ms faster, or when the direct path got no
  answer

The relay servers are registered with GGPO in the order of the battle info,
before the players. A relayed peer shows `(R)` on the OSD. The zdxsv lobby
offers relay servers when its `ZDXSV_LOBBY_RELAY_ADDR` is set.

## P2P connectivity test

On the first lobby connection the emulator tests the GGPO port against the
STUN of the lobby and its test socket at the STUN port + 1, as the feasibility
test of flycast. The result is `open`, `cone NAT`, `symmetric NAT` or
`unknown`. It is shown in an OSD message, written to the log and sent in the
platform info.

## Network status OSD

During any `net=1` battle the left edge shows, as `drawNetworkStat` of
flycast:

- `Delay Nfr`: yellow from 5, orange from 10, red from 13
- `Roll`: rollback frames. `Wait`: frames that waited for a peer
- per opponent: position and user id, name, `Ping` (GGPO RTT, in the colors of
  flycast) and `P` (frames predicted for it), or `Interrupted` /
  `Disconnected`

The lines are logged every 600 frames as `ZdxsvGgpo: osd frame F: ...`.

## Save states while online

Normally a save state does not hold the network adapter, as upstream. Two
cases keep it:

- `ZDXSV_LOBBY_STATE=1` (debugging): save states keep the DEV9 registers and
  SMAP buffers, and after a load the emulator takes over the TCP connections
  the PS2 had opened.
- `ZDXSV_GGPO` or `ZDXSV_REPLAY` set: save states keep the network adapter
  without taking over connections. This includes the frame 0 state and the seek
  keys of a replay. Without it the network driver of the PS2 and the adapter
  disagree on the next send buffer after a load, and the network calls of the
  game hang.

The adapter state holds no host pointer: an IOP DMA in progress is saved as an
offset in IOP RAM, and the EEPROM (MAC address) stays the one of the loading
process. A load checks the buffer and descriptor indices and fails on a damaged
state. States written before this format (marker `DEV9`) still load; an IOP DMA
in progress in them is dropped.
