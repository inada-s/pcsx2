# GGPO

Rollback netcode library, imported for zdxsv from inada-s/flycast `gdxsv-master`
(5c539de1e, `core/deps/ggpo`). Upstream: https://github.com/pond3r/ggpo (MIT, `LICENSE`).

PCSX2-side changes (the flycast sources are otherwise unchanged):
- `CMakeLists.txt`, `ggpo.vcxproj`: build files.
- `include/ggpo_log.h`, `lib/ggpo/log/`: log sink replacing flycast's `log/Log.h`.
- `lib/ggpo/sleep.h`: `sleep_us` replacing flycast's `sleep.h`.
- `backends/synctest.cpp`: sync errors also go to the log sink (error level).
- `game_input.h`, `bitvector.h`, `network/udp_msg.h`: input size constants for
  up to 4 players with 32-byte inputs (`GAMEINPUT_MAX_BYTES` 32,
  `GAMEINPUT_MAX_PLAYERS` 4, `BITVECTOR_NIBBLE_SIZE` 16, `MAX_COMPRESSED_BITS` 32768).
- `ggpo_get_last_confirmed_frame` (`include/ggponet.h`, `main.cpp`,
  `backends/backend.h`, `backends/p2p.h`, `sync.h`): last frame with every
  player's input received.
- `network/udp.cpp` `CreateSocket`: `IPV6_V6ONLY` on the IPv6 socket (the
  IPv4 socket has the same port); on Windows `SIO_UDP_CONNRESET` off, so an ICMP
  port unreachable does not end a poll's reads.
- `network/udp.cpp` `Udp::OnLoopPoll`: a socket that could not be created
  (no IPv6) is skipped instead of looping forever.
- `network/udp_proto.cpp` `UdpProtocol::HandlesMsg`: a packet is matched to an
  endpoint by its peer's source address, not by the `remote_endpoint` header
  byte alone, so a third party who knows a player's address can no longer inject
  inputs or a disconnect.
- `platform_windows.h` `AssertFailed`: prints to stderr instead of a MessageBox
  (it blocked the emulator CPU thread).
