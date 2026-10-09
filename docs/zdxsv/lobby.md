# Lobby connection and GGPO lobby battles

What the emulator adds to the connection with the zdxsv lobby, and how a
battle that the lobby matched runs over GGPO. Option names and defaults are in
[options.md](options.md). The GGPO session itself is in
[rollback.md](rollback.md).

## Network defaults

A new configuration points at zdxsv: adapter on, Sockets API, `Auto` device,
DHCP intercepted, internal DNS, no host entries. The internal DNS server looks
up the server hosts of the game as `zdxsv.net` on the host's resolver
(`Zdxsv/ServerHosts.cpp`), so a server move needs no new release:

- `www01.kddi-mmbb.jp`
- `gate1.jp.dnas.playstation.org`
- `ca1202.mmcp6`
- `ca1203.mmcp6`

A host entry for one of these names wins (local servers, tests). Entries that
map one of them to `153.121.44.150` are dropped when the config loads.

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
| `nat=` | lobby GGPO on, after the connectivity test ended | result of the connectivity test |
| `<region>=ms` (e.g. `asia-northeast1=12`) | after the HTTPS latency test ended | HTTPS round trip to that cloud region (gdxsv's keys) |

"Lobby GGPO on" means `ZDXSV_GGPO` with `net=1,lobby=1`.

### Battle info `0x9951`

| Key | Meaning |
|---|---|
| `ggpo_<user>=port` | GGPO port of each player that announced one |
| `p2p_<user>=addr,...` | UDP addresses of each player |
| `ggpo_session=` | id of the battle. zdxsv sends the FNV-1 32 hash of the battle code, as gdxsv. Without it there is no GGPO. |
| `ggpo_ping_ms=` | length of the ping test, at most 10000 (`MAX_PING_MS`). zdxsv sends 7500. |
| `name_<user>=` | player name (HN), for the OSD |
| `pilot_<user>=` | pilot name (first field of the game's user binary, 0x6143), for the OSD and replay header |
| `relay_<k>=<hex token>,<ip:port>[,<[ip6]:port>]` | relay servers of the battle, k = 0 to 3 |
| `battle_code=`, `user_id=` | name the battle in the match report and the replay file |

### Match report `0x9952`

It tells the lobby how the battle ran, as the `lbsP2PMatchingReport` of
flycast. The emulator logs it as `DEV9: TCP: zdxsv matching report: ...`, and
the zdxsv lobby logs it as `p2p matching report: ...`.

| Key | Sent | Meaning |
|---|---|---|
| `battle_code`, `user_id` | always | from the battle info |
| `result` | always | `ggpo` or `cut`; a cut before the ping test adds `reason` |
| `position`, `players` | always | own battle position and the player count |
| `rtt_<pos>`, `addr_<pos>`, `path_<pos>` | ping test ran | RTT per peer (-1 = no answer), the picked address and path |
| `relays`, `ping_wait_ms`, `delay` | ping test ran | relay server count, time waited for the test, GGPO input delay |
| `ping_error` | the ping test could not start | why, e.g. the GGPO port could not be bound |
| `close`, `frames`, `rollback_frames`, `mismatches`, `disconnected` | GGPO battle | why the session ended and its counters |
| `answered`, `cut_sends` | cut | peers that answered, and sends dropped during the cut |

## How a lobby battle starts

1. The platform info announces the GGPO port and the addresses of the client.
2. The battle info lists the players. The ping test starts on the GGPO port.
3. At the first key message of the battle the emulator decides:
   - A player has no GGPO port, or the battle info is incomplete: the battle
     connection is cut. See [Connection cut](#connection-cut).
   - A peer answered the ping test on no address: the battle connection is
     cut.
   - Otherwise the battle runs over GGPO, with the address and the input delay
     the ping test picked.

Every lobby battle starts a new GGPO session. No battle runs on the battle
server.

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
/ 16 ms)), as the rollback backend of flycast. `mindelay` defaults to the
setting `ZdxsvGgpoMinDelay` (2..6, default 2). `delay=` keeps the delay fixed
and skips the test.

Log lines: `zdxsv: ping test: ...` (RTT per peer), `ZdxsvGgpo: lobby delay D`,
`ZdxsvGgpo: lobby path to position P: direct|peer K|relay K`.

### Connection cut

When the GGPO session cannot start (a peer answered the ping test on no
candidate, a player has no GGPO port, the battle info is incomplete), the
battle connection is cut, as a connection failure. There is no fallback to the
battle server. Sends on the battle socket are dropped and the poll of the
socket fails, so the game sets its connection error at once, closes the socket
and goes back to the lobby: about 53 frames after the cut, where waiting for
its no-response timeout took about 590. Its post-battle report has
`battles=0`.

Log lines: `lobby battle connection cut: K of N peers answered the ping test`
or `lobby battle connection cut: <reason>`, then `connection cut ended ... game
RPC 0xd`.

## Relay servers

Relay servers work as in flycast with the `relay` of gdxsv. The ping test also
pings each relay server of the battle info over IPv4 and IPv6, with the 28-byte
relay ping of gdxsv, and shares the RTT and relay-RTT matrices with the peers
(`PacketWithRelays` of flycast). From each packet it takes every relay-RTT row
the sender knows, not only the sender's own, so a peer reached only through
another peer (symmetric NAT) still gets a relay server path.

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
platform info. It runs on its own thread (up to about 2 s), so `nat=` goes out
from the first lobby connection after it ended. A ping test cancels it (both
use the GGPO port). A missing STUN answer is not kept: the next lobby
connection asks again.

## HTTPS latency test

When the game goes online (the first lookup of a game host, else the first
lobby connection), the emulator measures the HTTPS latency to 13 cloud regions,
as the ping test of flycast (gcping hosts, `Zdxsv/HttpsLatency.cpp`): per
region a warm-up `HEAD /api/ping` opens the connection, then 3 more on it; the
fastest is the region's latency. 6 regions at a time, on their own threads.
Each region's attempts go to the log (`ZdxsvHttpsLatency:`), the best region to
an OSD message. Once every region ended, each lobby connection's platform info
carries `<region>=ms` for the regions that answered (gdxsv's lobby picks the
battle region from these keys; the zdxsv lobby keeps them in the peer's
platform info). Once per run. `ZDXSV_HTTPS_LATENCY=0` turns it off.

## Network status OSD

During any `net=1` battle the left edge shows, as `drawNetworkStat` of
flycast:

- `Delay Nfr`: yellow from 5, orange from 10, red from 13
- `Roll`: rollback frames. `Wait`: frames that waited for a peer
- per opponent: position and user id, name (HN), pilot name, `Ping` (GGPO RTT, in the colors of
  flycast) and `P` (frames predicted for it), `Interrupted` in place of `Ping`, or
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
