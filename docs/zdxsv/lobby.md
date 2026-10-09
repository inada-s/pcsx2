# Lobby connection and GGPO lobby battles

What the emulator adds to the connection with the zdxsv lobby, and how a
battle that the lobby matched runs over GGPO. Option names and defaults are in
[options.md](options.md). The GGPO session itself is in
[rollback.md](rollback.md).

## Network defaults

A new configuration points at zdxsv: adapter on, Sockets API, `Auto` device,
DHCP intercepted, internal DNS, no host entries. The internal DNS server looks
up the game's server hosts as `zdxsv.net` (`Zdxsv/ServerHosts.cpp`), so a
server move needs no new release. A host entry for one of these names wins;
entries that map one to the stale address `153.121.44.150` are dropped
when the config loads.

## Battle server connection

The game always talks TCP to the battle server, as on a PS2. When a battle
runs over GGPO, the emulator answers the game's battle socket itself.

## Messages added to the lobby connection

Three custom messages; the game never sees them. Each body is a list of
`key=value` lines.

| Command | Direction | Name | When |
|---|---|---|---|
| `0x9950` | emulator to lobby | platform info | on the first key question of the lobby |
| `0x9951` | lobby to emulator | battle info | when the lobby matched a battle, to clients that sent `udp=1` |
| `0x9952` | emulator to lobby | match report | on the next lobby connection after a battle, right after the platform info |

### Platform info `0x9950`

Lets the server tell emulators from real PS2s.

| Key | Sent | Meaning |
|---|---|---|
| `emulator`, `version`, `os`, `cpu` | always | what runs the game |
| `udp=1` | lobby GGPO on | the client wants battle infos |
| `udp_addr`, `udp_local` | lobby GGPO on | public IPv4 address from the lobby's STUN, and the local one |
| `udp_addr6` | lobby GGPO on, with a global IPv6 address | global IPv6 address |
| `ggpo=P` | lobby GGPO on | GGPO UDP port |
| `relay_server=1` | lobby GGPO on | the client can route GGPO through relay servers |
| `nat=` | lobby GGPO on, after the connectivity test | result of the connectivity test |
| `<region>=ms` | after the HTTPS latency test | HTTPS round trip to that cloud region (gdxsv's keys) |

"Lobby GGPO on" means `ZDXSV_GGPO` with `net=1,lobby=1` (the default).

### Battle info `0x9951`

| Key | Meaning |
|---|---|
| `ggpo_<user>=port` | GGPO port of each player that announced one |
| `p2p_<user>=addr,...` | UDP addresses of each player |
| `ggpo_session=` | id of the battle (gdxsv: FNV-1 32 of the battle code). Without it there is no GGPO. |
| `ggpo_ping_ms=` | length of the ping test, at most 10000 |
| `name_<user>=`, `pilot_<user>=` | player name and pilot name, for the OSD and the replay |
| `relay_<k>=<hex token>,<ip:port>[,<[ip6]:port>]` | relay servers of the battle, k = 0 to 3 |
| `battle_code=`, `user_id=` | name the battle in the match report and the replay |
| `live_uplink=1` | this client streams the battle for live spectating ([replay.md](replay.md)) |

### Match report `0x9952`

How the battle ran, as flycast's `lbsP2PMatchingReport`.

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
3. At the first key message of the battle: if a player has no GGPO port, the
   battle info is incomplete, or a peer answered the ping test on no address,
   the battle connection is cut. Otherwise the battle runs over GGPO with the
   addresses and the input delay the ping test picked.

No battle runs on the battle server.

## Ping test

Runs on the GGPO UDP port for `ggpo_ping_ms`, from an IPv4 and an IPv6
socket, with flycast's `UdpPingPong` packet. Packets with another session,
position or source address are dropped.

Address candidates of a peer, in order: its public IPv4 address (its local
one when behind the same public IP), its other IPv4 address, its IPv6
address. Each peer is reached at the candidate with the best score: lowest
RTT, plus 100 for loopback, 50 for a private address, 20 for IPv6.

The input delay is max(`mindelay`, ceil(mean RTT of the slowest peer / 2 /
16 ms)), as flycast's rollback backend. `delay=` fixes it and skips the test.

### Connection cut

When the GGPO session cannot start, the battle connection is cut, as a
connection failure; there is no fallback to the battle server. Sends on the
battle socket are dropped and its poll fails, so the game sets its connection
error at once and goes back to the lobby.

## Relay servers

As flycast with gdxsv's `relay`. The ping test also pings each relay server of
the battle info (IPv4 and IPv6, gdxsv's relay ping) and shares the RTT and
relay-RTT matrices with the peers (flycast's `PacketWithRelays`), so a peer
reached only through another peer still gets a relay path.

A peer is reached through:

- relay server k, when that path is 16 ms faster than the direct one
- another peer, when that path is 32 ms faster, or the direct path got no
  answer

A relayed peer shows `(R)` on the OSD.

## P2P connectivity test

On the first lobby connection the emulator tests the GGPO port against the
lobby's STUN and its test socket at the STUN port + 1, as flycast's
feasibility test: `open`, `cone NAT`, `symmetric NAT` or `unknown`. Shown on
the OSD, logged and sent as `nat=`. It runs on its own thread; a ping test
cancels it. A missing STUN answer is asked again on the next connection.

## HTTPS latency test

When the game goes online, the emulator measures the HTTPS latency to the
cloud regions, as flycast's ping test (gcping hosts,
`Zdxsv/HttpsLatency.cpp`): the fastest of 3 `HEAD /api/ping` on a warm
connection per region. The best region goes to the OSD; every later platform
info carries `<region>=ms`. Once per run.

## Network status OSD

During a `net=1` battle the left edge shows, as flycast's `drawNetworkStat`:
the input delay, rollback and wait frames, and per opponent the position,
user id, name, pilot name, ping (or `Interrupted`, `Disconnected`) and
predicted frames.

## Save states while online

A save state does not hold the network adapter, as upstream, except:

- `ZDXSV_LOBBY_STATE=1` (debugging): save states keep the adapter, and after
  a load the emulator takes over the TCP connections the PS2 had opened.
- `ZDXSV_GGPO` or `ZDXSV_REPLAY` set: save states keep the adapter without
  taking over connections, so the PS2's network driver and the adapter agree
  after a load.

The adapter state holds no host pointer. A load checks its buffer and
descriptor indices and fails on a damaged state.
