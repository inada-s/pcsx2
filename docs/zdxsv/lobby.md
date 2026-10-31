# Lobby connection and GGPO lobby battles

What the emulator adds to the connection with the zdxsv lobby, and how a
battle that the lobby matched runs over GGPO. Option names and defaults are in
[options.md](options.md). The GGPO session itself is in
[rollback.md](rollback.md).

## Network defaults

A new configuration points at zdxsv: adapter on, Sockets API, `Auto` device,
DHCP intercepted, internal DNS, no host entries. The internal DNS server looks
up the game's server hosts as `lbs.zdxsv.net` (`Zdxsv/ServerHosts.cpp`), so a
server move needs no new release. A host entry for one of these names wins;
entries that map one to the stale address `153.121.44.150` are dropped
when the config loads.

## Battle server connection

The game always talks TCP to the battle server, as on a PS2. When a battle
runs over GGPO, the emulator answers the game's battle socket itself.

## TLS to the zdxsv server

The emulator carries the game's TCP connections to the zdxsv server through
TLS (`Zdxsv/TlsTunnel.cpp`); the game still sees plain TCP. A connection is
wrapped when its port is in the table below and its address is a DNS answer
for a game host, or the battle server of the last battle info.

| Game port | TLS port | Server |
|---|---|---|
| 443 | 8220 | DNAS |
| 8200 | 8221 | lobby |
| 8210 | 8222 | battle (TCP) |

- The connection goes to a local port; a thread there connects to the
  server's TLS port and copies the bytes both ways.
- Trust: public-key pins (SHA-256 of the SubjectPublicKeyInfo) of the zdxsv
  CA and the backup CA. A pinned server key is accepted as is; a pinned key
  higher in the chain the server sends must have signed the server
  certificate. No hostname or expiry check. Making and rotating the keys: the
  zdxsv repository, `docs/TLS.md`.
- No plain-text fallback. A failed connect, handshake or pin check resets
  the game's connection and shows an OSD message.
- TLS is off while the client has no pins. Rigs: `ZDXSV_TLS_TEST_PIN=1`
  trusts the zdxsv test CA, `ZDXSV_TLS=0` turns TLS off.
- Not covered: the login pages (port 80), UDP (GGPO, relays, live streams),
  real PS2s and zproxy.

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
| `pcsx2` | built from a `zdxsv-X.Y.Z` release tag or a commit after one | release version: `zdxsv-X.Y.Z` on the tag, `zdxsv-X.Y.(Z+1)-dev.N` N commits after it. The lobby refuses a login below its minimum client version (zdxsv master data); PCSX2 then shows an OSD message |
| `machine_id` | Windows, macOS | 16 hex digits: XXH64 (seed 37) of the host id (Windows `MachineGuid`, the macOS serial number), never the raw id; the same on every install and portable folder of a PC. Not sent on Linux (as flycast) |
| `udp=1` | lobby GGPO on | the client wants battle infos |
| `udp_addr`, `udp_local` | lobby GGPO on | the GGPO port's public IPv4 address from the lobby's STUN (asked from that port on every lobby connection, as flycast's `NotifyWanPort`), and the local one |
| `udp_addr6` | lobby GGPO on, with a global IPv6 address | global IPv6 address |
| `ggpo=P` | lobby GGPO on | GGPO UDP port |
| `relay_server=1` | lobby GGPO on | the client can route GGPO through relay servers |
| `nat=` | lobby GGPO on, after the connectivity test | result of the connectivity test |
| `upnp_result=`, `upnp_local_ip=` | with `nat=`, when the port was not open and `ZdxsvUPnP` is on | UPnP mapping of the GGPO port: `Success` or the error; the LAN address the router maps to |
| `<region>=ms` | after the HTTPS latency test | HTTPS round trip to that cloud region (gdxsv's keys) |

"Lobby GGPO on" means `ZDXSV_GGPO` with `net=1,lobby=1` (the default).

### Battle info `0x9951`

A `P2PMatching` protobuf message (zdxsv `pkg/proto/zdxsv.proto`). Field numbers
follow gdxsv's `P2PMatching` where the meaning is the same; zdxsv's own fields
start at 40. The client decodes it with the codec in `Proto.h` and rejects a
message with more than 8 users, 64 candidates or 4 relays, a string over 256
bytes, or a `peer_id` outside `users`.

| Field | Meaning |
|---|---|
| `battle_code` (1) | names the battle in the match report and the replay |
| `session_id` (2) | id of the battle in the ping test packets (zdxsv: random per battle). 0: no GGPO. |
| `peer_id` (4) | own index in `users` |
| `ping_test_duration` (6) | length of the ping test in ms, at most 10000 |
| `users` (9) | every player in battle order: `user_id`, `user_name`, `pilot_name` (OSD, replay), `team` (9, entry side 1 / 2: the ping test diagram rows), `ggpo_port` (40), `sync` (41, settings fingerprint) |
| `candidates` (10) | UDP addresses of the other players: `user_id`, `peer_id`, `ip`, `port` |
| `relays` (11) | relay servers: `ip`, `port`, `token`, `ip6`, `port6` (40) |
| `spectator_uplink` (12) | this client streams the battle for live spectating ([replay.md](replay.md)) |
| `lobby_session_id` (40), `battle_server` (41) | the lobby session and the battle server `ip:port` |

### Match report `0x9952`

How the battle ran: a `P2PMatchingReport` (field numbers as gdxsv's where the
meaning is the same). The client collects `key=value` lines during the battle
and encodes them when it sends the report; the lobby logs the report as one
`p2p matching report:` line of `field="value"`.

| Field | Sent | Meaning |
|---|---|---|
| `battle_code` (1), `user_id` (40) | always | from the battle info |
| `result` (41) | always | `ggpo` or `cut`; a cut before the ping test adds `cut_reason` (42) |
| `peer_id` (4), `player_count` (3) | always | own battle position and the player count |
| `remote_peers` (31) | ping test ran | per peer: `peer_id`, `rtt_ms` (40, -1 = no answer), the picked `addr` (41) and `route` (7) |
| `relay_count` (46), `ping_wait_ms` (48), `input_delay` (21), `min_delay` (49), `fixed_delay` (50) | ping test ran | relay server count, time waited for the test, GGPO input delay |
| `rollback_budget` (56), `rollback_budget_used` (57) | ping test ran | learned and used rollback budget (frames, float) |
| `ping_error` (47) | the ping test could not start | why, e.g. the GGPO port could not be bound |
| `close_reason` (5), `frame_count` (6), `rollback_frames` (52), `mismatches` (53), `disconnected` (54) | GGPO battle | why the session ended and its counters |
| `disconnected_peer_ids` (55) | GGPO battle | positions whose peer disconnected |
| `round_data` (20) | GGPO battle with a replay recorded (saved, uploaded or live) | per round: `win_team` (1) and `used_ms` (2: per position, the MS id + 1); the lobby keeps each player's mobile suits for the replay search |
| `remote_peers`: `ping_min_ms` (4), `ping_median_ms` (5), `ping_max_ms` (6) | GGPO battle | that peer's GGPO ping in ms, sampled once a second while connected; unset with no sample |
| `remote_peers`: `recv_loss` (42), `send_loss` (43) | GGPO battle | GGPO packets from that peer never received, and to it that it reported lost; totals at the last once-a-second sample |
| `total_timesync` (58), `input_block_count` (59) | GGPO battle | frames slept so a peer could catch up; frames that waited for a peer's input (GGPO prediction limit) |
| `fps_history` (60) | GGPO battle, after 1 s | frames run in each second of the session (at most 3600 entries) |
| `answered` (45), `cut_sends` (51), `sync` (43), `sync_differ` (44) | cut | peers that answered, sends dropped during the cut, differing settings |

## How a lobby battle starts

1. The platform info announces the GGPO port and the addresses of the client.
2. The battle info lists the players. The ping test starts on the GGPO port.
3. At the first key message of the battle: if the battle info is incomplete
   or a peer answered the ping test on no address, the battle connection is
   cut. Otherwise the battle runs over GGPO with the addresses and the input
   delay the ping test picked. A player without a GGPO port or address still
   joins the ping test (as flycast): it is reached at the sources of its pings
   or through a relay; with `delay=` (no ping test) the battle is cut.

No battle runs on the battle server.

## Ping test

Runs on the GGPO UDP port for `ggpo_ping_ms`, from an IPv4 and an IPv6
socket, with flycast's `UdpPingPong` packet. Packets with another session or
position are dropped. A peer's ping or pong from a source that is not one of
its candidates (its NAT mapping, or a peer listed without an address) adds
that source as a candidate (as flycast; at most 4 per peer). Every ping is
answered, and a pong counts for the candidate it names, whatever its source.
A relay pong with the battle's relay token counts from any
source (a multi-homed or NATed relay); up to 2 such sources per relay path
are kept, and GGPO accepts that relay's packets from them too.

Address candidates of a peer, in order: its public IPv4 address (its local
one when behind the same public IP), its other IPv4 address, its IPv6
address. Their ports are the ones it sent when one of them is its GGPO port (it asked STUN from that port), else its GGPO port (a client that asked from another port, or found the GGPO port taken). Each peer is reached at the candidate with the best score: lowest
RTT, plus 100 for loopback, 50 for a private address, 20 for IPv6, 1 for a
source the peer sends from. A candidate's RTT is the median of its first 256
pongs (the lower middle one for an even count), so a few late pongs change
neither the delay nor the route.

The input delay is max(`mindelay`, ceil(RTT of the slowest peer / 2 /
16 ms - the rollback budget)), the budget in `rollback.md`. `delay=` fixes it
and skips the test.

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

## Addresses in the log

Player addresses (own, peers, ping test candidates, the connectivity test's
public address, the match report's
`addr_<pos>`) are logged masked, as flycast's `mask_ip_address` but one `x` per part: `1.x.x.x:7001`,
`[2001:db8:1:x::x]:7001`. Relay server addresses stay whole. The lobby
messages carry them whole.

## P2P connectivity test

On the first lobby connection the emulator tests the GGPO port against the
lobby's STUN and its test socket at the STUN port + 1, as flycast's
feasibility test: `open`, `cone NAT`, `symmetric NAT` or `unknown`. A closed
port is also asked from Google's and Cloudflare's STUN, as flycast: the
server's two sockets share one IP, so a NAT that maps per destination IP
looks like a cone to them; different mappings there = `symmetric NAT`. Shown on
the OSD in words only (no address or port: streams), logged with the
addresses and sent as `nat=`. It runs on its own thread; a ping test
cancels it.

When the test socket reaches the port and a udptest URL is set
(`ZdxsvUdpTestUrl`), the emulator also posts `addr=<public ip:port>` to it
(zdxsv `infra/udptest`, a Cloud Function on another network) and waits 3 s
for its `Hello`. The test socket shares the lobby's IP, which an
address-restricted NAT lets through; without the `Hello` the result is
`cone NAT (open to the server only)` (`nat=cone`). A failed request keeps the
server's result. flycast does the same check against gdxsv's function.

When the result is not `open` and `ZdxsvUPnP` is on, the emulator asks the
router (UPnP IGD, miniupnpc in `3rdparty/miniupnpc`) to forward the GGPO UDP
port to this PC, as flycast's `EnableUPnP`, and runs the test again. The
retest's result replaces the first, unless only the retest got no answer.
`upnp_result=` (`Success` or the error) and `upnp_local_ip=` go with `nat=`.
The mapping has a 1-day lease and is removed when PCSX2 exits normally.

The "UPnP Now" button on Settings → zdxsv maps the `ZdxsvGgpoPort` port at
once (as flycast's button) and shows `opened` or the error next to it; with
the port at 0 it asks for one, since the random port is picked only at game
start. The same exit cleanup removes it. A router whose IGD
reports no routable WAN address (`0.0.0.0`, as on a shared IPv4 behind
MAP-E) counts as no gateway (`error 2`): such a router accepts mappings but
opens nothing (measured on a ZTE F2886Q from an outside host: 0 packets
through a mapped port, 184 through a punched one). `ZDXSV_UPNP_ANY_IGD=1`
accepts it, for tests.

## HTTPS latency test

When the game goes online, the emulator measures the HTTPS latency to the
cloud regions, as flycast's ping test (gcping hosts,
`Zdxsv/HttpsLatency.cpp`): the fastest of 3 `HEAD /api/ping` on a warm
connection per region. The best region goes to the log; every later platform
info carries `<region>=ms`. Once per run.

## Network status OSD

During a `net=1` battle the left edge shows, as flycast's `drawNetworkStat`:
the input delay, rollback and wait frames, and per opponent the position,
user id, name, pilot name, ping (or `Interrupted`, `Disconnected`) and
predicted frames.

With the OSD off (`ZdxsvNetOsd`, `osd=0`) it shows only while an opponent is
interrupted or disconnected, as flycast. A battle that ends on a disconnect
keeps its last lines up 10 s (`osd held 10 s` in the log), so the player sees
who left.

## Save states while online

A save state does not hold the network adapter, as upstream, except:

- `ZDXSV_LOBBY_STATE=1` (debugging): save states keep the adapter, and after
  a load the emulator takes over the TCP connections the PS2 had opened.
- `ZDXSV_GGPO` or `ZDXSV_REPLAY` set: save states keep the adapter without
  taking over connections, so the PS2's network driver and the adapter agree
  after a load.

The adapter state holds no host pointer. A load checks its buffer and
descriptor indices and fails on a damaged state.
