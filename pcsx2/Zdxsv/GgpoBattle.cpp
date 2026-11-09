// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// The game's battle socket over GGPO: sent msgs into the local input, synced msgs back to recv, the
// RPCs of the battle sock (OnNetCall) and the built-in battle start of a rig run (RbkCall).

#include "Zdxsv/GgpoShared.h"
#include "Zdxsv/LobbySocket.h"
#include "Zdxsv/OnlineLimits.h"
#include "Zdxsv/UiHooks.h"
#include "IconsFontAwesome.h"

namespace Zdxsv
{
	namespace
	{
		// Z net RPC request header (EE RAM): sock s16, len s16, then data. Result length s16 at 0xc22c98.
		constexpr u32 NET_REQ_SOCK = 0xc22c9c;
		constexpr u32 NET_REQ_LEN = 0xc22c9e;
		constexpr u32 NET_REQ_DATA = 0xc22ca0;
		// Own key table (from the game's sends): k per counter, frame it was sent, last record X.
		u8 s_own_k[64];
		u32 s_own_frame[64];
		int s_own_x = -1;
		struct RecvStats
		{
			u32 msgs, keymsgs, slots, recs, unknown, kbad, xbad, rebuilt_bad;
		} s_rs;

		// Key msg bytes from its slots (inverse of ParseKeySlots), sender p.
		std::vector<u8> BuildKeyMsg(u32 p, const std::vector<KeySlot>& slots)
		{
			std::vector<u8> m{0, static_cast<u8>(0x20 | p)};
			for (const KeySlot& s : slots)
			{
				if (s.rec)
					m.insert(m.end(), {0, s.c, static_cast<u8>(s.a >> 8), static_cast<u8>(s.a), s.x, s.k,
										  static_cast<u8>(s.b >> 8), static_cast<u8>(s.b)});
				else
					m.insert(m.end(), {static_cast<u8>(0x80 | s.c), s.k});
			}
			m[0] = static_cast<u8>(m.size());
			return m;
		}
		constexpr u32 NET_NOWAIT = 0xc22c10; // s16: the wrapper takes the nowait RPC path if nonzero
		constexpr u32 NET_RX_MAX = 0x384; // the battle recv's max

		void NetSend(std::vector<u8> m)
		{
			s_ns.msgs++;
			if (m.size() >= 2 && (m[1] >> 4) == 0xf && s_rb.end_frame < 0)
				s_rb.end_frame = s_battle.frame;
			const int kind = m.size() >= 2 && m[0] == m.size() ? m[1] >> 4 : -1;
			if (kind == 3)
				; // GGPO input (below), released by NetSyncAndApply
			else if (kind == 2 || kind == 7 || kind == 9 || kind == 0xf)
			{
				for (int q = 0; q < s_opt.players; q++)
					if (q != s_battle.me)
					{
						s_rb.net_rx.push_back(m[0]);
						s_rb.net_rx.push_back(static_cast<u8>((m[1] & 0xf0) | q));
						s_rb.net_rx.insert(s_rb.net_rx.end(), m.begin() + 2, m.end());
					}
				if (!g_ggpo_in_rollback)
					s_battle.zds_echo++;
			}
			else if (!g_ggpo_in_rollback && s_battle.zds_skip++ < 20)
				Console.Warning("ZdxsvGgpo: zds msg kind %d (%zu bytes) not echoed", kind, m.size());
			if (s_rb.net_pos < s_battle.sent.size())
			{
				if (s_battle.sent[s_rb.net_pos] != m && s_ns.senddiff++ < 40)
				{
					std::string a, b;
					char h[4];
					for (u8 v : s_battle.sent[s_rb.net_pos])
						std::snprintf(h, sizeof(h), "%02x", v), a += h;
					for (u8 v : m)
						std::snprintf(h, sizeof(h), "%02x", v), b += h;
					Console.Warning("ZdxsvGgpo: net rerun send %zu differs (frame %d) sent %s rerun %s", s_rb.net_pos, s_battle.frame, a.c_str(), b.c_str());
				}
				if (s_battle.sent_at[s_rb.net_pos] != s_battle.frame && m.size() >= 2 && (m[1] >> 4) == 3)
				{
					auto it = std::find_if(s_battle.out.begin(), s_battle.out.end(), [](const NetOut& o) { return o.idx == s_rb.net_pos; });
					if (it != s_battle.out.end())
						it->frame = s_battle.frame, s_ns.restamp++;
					else if (s_ns.late++ < 20)
						Console.Warning("ZdxsvGgpo: net rerun send %zu moved %d -> %d after it went into an input", s_rb.net_pos, s_battle.sent_at[s_rb.net_pos], s_battle.frame);
					if (s_net_trace)
						std::fprintf(s_net_trace, "%u OM %zu %d %d\n", g_FrameCount, s_rb.net_pos, s_battle.sent_at[s_rb.net_pos], s_battle.frame);
				}
				s_battle.sent_at[s_rb.net_pos] = s_battle.frame;
			}
			else
			{
				s_battle.sent.push_back(m);
				s_battle.sent_at.push_back(s_battle.frame);
				if (m.size() >= 2 && (m[1] >> 4) == 3)
					s_battle.out.push_back({s_battle.frame, s_rb.net_pos, std::move(m)});
				s_ns.maxq = std::max<u32>(s_ns.maxq, static_cast<u32>(s_battle.out.size()));
			}
			s_rb.net_pos++;
		}

		// McsMessage framing: byte 0 = length (>= 2), byte 1 = kind << 4 | sender.
		bool HasKeyMsg(const u8* d, s32 len)
		{
			for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
				if ((d[i + 1] >> 4) == 2)
					return true;
			return false;
		}
		// ZDXSV_RBK: answers of a real start (traced with 4 players and tests/zdxsv/fake_lobby.py): lobby frames are
		// `18 cat cmd size seq 00ffffff body` (BE, 12-byte header), the game's `81 01 ..`.
		std::vector<u8> s_rbk_rx; // what recv 0x13 / 0x14 and the poll's readable count see
		// lobby=1: the battle server answered on the EE until GGPO arms; players 0 = off
		struct BattleServerAnswer
		{
			std::vector<u8> rx;
			int me = -1, players = 0;
		} s_bs;
		bool s_rbk_started = false; // 0x6910 (battle start) queued
		u32 s_rbk_calls[0x50] = {};
		constexpr u32 RBK_FNO_RECV_LOBBY = 0x13;
		const char* const RBK_USERS[4] = {
			"010100064a3953584e4d000682a082a082a000000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814901",
			"02010006554a4239414d000682a282a282a200000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814902",
			"030200063856514b5043000682a482a482a400000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814903",
			"040200065a3537434e32000682a682a682a600000064834a837e815b838681458372835f839300000000000097b989f081490000000000000000000000000000000082bb82bf82e782cc94ed8a518ff38bb582cd8148000093478b408c82946a814900000000000000000000000089b482c9944382b982eb814904",
		};
		const char* const RBK_6917[4] = {
			"0100000000000000000010000000020000000d00000000",
			"0200000000000000000010000000020000000d00000000",
			"0300000000000000000010000000000000000f00000000",
			"0400000000000000000010000000000000000f00000000",
		};
		const char* const RBK_RULE = "006400000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff0000025802580064006400d20000000002000000000000000000000000000001";
		constexpr size_t RBK_RULE_TIME = 80; // u16 BE seconds (0x00d2 = 210)
		constexpr size_t RBK_RULE_GAUGE = 72; // u16 BE x2 (72, 74), 戦力ゲージ (0x0258 = 600)
		constexpr size_t RBK_RULE_COUNT = 87; // u8 連続対戦数, 0 = 任意 (rematch picked by input)

		std::vector<u8> Unhex(std::string_view s)
		{
			std::vector<u8> b;
			for (size_t i = 0; i + 1 < s.size(); i += 2)
				b.push_back(static_cast<u8>(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
			return b;
		}

		void RbkQueue(u8 cat, u16 cmd, u16 seq, const std::vector<u8>& body)
		{
			const u8 h[12] = {0x18, cat, static_cast<u8>(cmd >> 8), static_cast<u8>(cmd), static_cast<u8>(body.size() >> 8),
				static_cast<u8>(body.size()), static_cast<u8>(seq >> 8), static_cast<u8>(seq), 0, 0xff, 0xff, 0xff};
			s_rbk_rx.insert(s_rbk_rx.end(), h, h + 12);
			s_rbk_rx.insert(s_rbk_rx.end(), body.begin(), body.end());
			Zdxsv::LobbyNoteFrame(&*(s_rbk_rx.end() - 12 - body.size()), 12 + body.size());
		}

		// Position p (1-based) of N plays the recorded 4-player position of its side:
		// p <= ceil(N/2) side 1 (recorded 1, 2), else side 2 (recorded 3, 4).
		int RbkRecordedIndex(int p)
		{
			const int half = (s_rbk_n + 1) / 2;
			return p <= half ? p - 1 : 2 + p - 1 - half;
		}

		// RBK_USERS' user ids, names and pilot name, UTF-8, for the network OSD.
		std::vector<LobbyPlayer> RbkPlayers()
		{
			static constexpr const char* PILOT = "カミーユ・ビダン";
			const LobbyPlayer recorded[4] = {{"J9SXNM", "あああ", PILOT}, {"UJB9AM", "いいい", PILOT}, {"8VQKPC", "ううう", PILOT}, {"Z57CN2", "えええ", PILOT}};
			std::vector<LobbyPlayer> players;
			for (int p = 1; p <= s_rbk_n; p++)
				players.push_back(recorded[RbkRecordedIndex(p)]);
			return players;
		}

		// Answer body of lobby Q cmd (body q).
		std::vector<u8> RbkBody(u16 cmd, const u8* q, u32 qn)
		{
			const int p = qn ? q[0] : 0;
			// Common start: the file's answer to cmd (0x6913 / 0x6917: the one of position p, its body's first
			// byte); the side 0x6912 stays the point of view
			if (s_play_common && cmd >= 0x6911 && cmd <= 0x6917 && cmd != 0x6912)
			{
				for (const std::vector<u8>& a : s_play_answers)
					if (a[2] == (cmd >> 8) && a[3] == (cmd & 0xff) && ((cmd != 0x6913 && cmd != 0x6917) || (a.size() > 12 && a[12] == p)))
						return std::vector<u8>(a.begin() + 12, a.end());
				Console.Error("ZdxsvGgpo: replay: common start: the file has no answer to 0x%04x (position %d)", cmd, p);
			}
			switch (cmd)
			{
				case 0x6911: return {static_cast<u8>(s_rbk_n)};
				case 0x6912: return {static_cast<u8>(s_rbk_me + 1)};
				case 0x6913:
				case 0x6917:
				{
					if (p < 1 || p > s_rbk_n)
						break;
					std::vector<u8> b = Unhex((cmd == 0x6913 ? RBK_USERS : RBK_6917)[RbkRecordedIndex(p)]);
					b.front() = static_cast<u8>(p);
					if (cmd == 0x6913)
						b.back() = static_cast<u8>(p);
					return b;
				}
				case 0x6915: return Unhex("000d31373931303831323634393138");
				case 0x6914:
				{
					std::vector<u8> b = Unhex(RBK_RULE);
					if (const char* t = Zdxsv::TestEnv("ZDXSV_RBK_TIME"))
					{
						const int s = std::atoi(t);
						b[RBK_RULE_TIME] = static_cast<u8>(s >> 8);
						b[RBK_RULE_TIME + 1] = static_cast<u8>(s);
					}
					if (const char* g = Zdxsv::TestEnv("ZDXSV_RBK_GAUGE"))
					{
						const int v = std::atoi(g);
						for (size_t o : {RBK_RULE_GAUGE, RBK_RULE_GAUGE + 2})
						{
							b[o] = static_cast<u8>(v >> 8);
							b[o + 1] = static_cast<u8>(v);
						}
					}
					if (const char* c = Zdxsv::TestEnv("ZDXSV_RBK_COUNT"))
						b[RBK_RULE_COUNT] = static_cast<u8>(std::atoi(c));
					return b;
				}
				case 0x6916: return Unhex("0004c0a8010800022012");
			}
			return {};
		}

		bool NetAnswer(s32 result)
		{
			*reinterpret_cast<s32*>(eeMem->Main + NET_RES_LEN) = result;
			cpuRegs.GPR.n.v0.SD[0] = result;
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			return true;
		}

		// The battle server's part of a battle before GGPO arms: its greet on connect, then each battle msg
		// sent back once per remote position with the sender nibble rewritten. rx is what recv 0x13 / 0x14
		// and the poll's readable count see. Poll as the battle poll, getopt 4 (0x2008 -> 0, 0x2001 -> 1 at
		// data+2), setopt 0. Returns false for other fnos.
		bool BattleServerCall(std::vector<u8>& rx, int me, int players, u32 fno, s16 len, u8* d, s32& result)
		{
			u8* ram = eeMem->Main;
			result = 0;
			switch (fno)
			{
				case NET_FNO_POLL:
					*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
					*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
					*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = static_cast<u16>(std::min<size_t>(rx.size(), NET_RX_MAX));
					return true;
				case RBK_FNO_RECV_LOBBY: // header, then body
				case NET_FNO_RECV:
				{
					const u32 max = static_cast<u32>(std::clamp<s32>(len, 0, NET_RX_MAX));
					u32 n = 0;
					if (fno == NET_FNO_RECV) // whole McsMessages
						while (n + 1 < rx.size() && rx[n] >= 2 && n + rx[n] <= std::min<size_t>(rx.size(), max))
							n += rx[n];
					else
						n = std::min<u32>(max, static_cast<u32>(rx.size()));
					std::memcpy(d, rx.data(), n);
					rx.erase(rx.begin(), rx.begin() + n);
					result = static_cast<s32>(n);
					return true;
				}
				case NET_FNO_SEND:
				{
					const s32 n = std::clamp<s32>(len, 0, 0x3ca);
					if (n >= 12 && d[0] == 0x82) // battle conn msg: the greet needs no answer
						return true;
					for (s32 i = 0; i + 1 < n && d[i] >= 2 && i + d[i] <= n; i += d[i])
						for (int q = 0; q < players; q++)
							if (q != me)
							{
								const size_t at = rx.size();
								rx.insert(rx.end(), d + i, d + i + d[i]);
								rx[at + 1] = static_cast<u8>((d[i + 1] & 0xf0) | q);
							}
					return true;
				}
				case NET_FNO_CONNECT:
				{
					static constexpr u8 greet[12] = {0x28, 0x01, 0x10, 0x31, 0, 0, 0, 1, 0, 0xff, 0xff, 0xff};
					rx.insert(rx.end(), greet, greet + 12);
					return true;
				}
				case 4:
				{
					const u32 v = *reinterpret_cast<const u16*>(ram + NET_REQ_LEN) == 0x2001;
					std::memcpy(ram + NET_REQ_DATA + 2, &v, 4);
					result = 4;
					return true;
				}
				case 3:
					return true;
			}
			return false;
		}

		// Sock-0 RPC fno before GGPO arms. Results as recorded in that trace: the battle server's
		// (BattleServerCall), 0x38 3, close / socket / link up 0. Other fnos go to the IOP or the no-DEV9 sockets.
		bool RbkCall(u32 fno, s16 len, u8* d)
		{
			u8* ram = eeMem->Main;
			if (fno < std::size(s_rbk_calls) && s_rbk_calls[fno]++ < 3)
				Console.WriteLn("ZdxsvGgpo: rbk fno %x len %d at vsync %u", fno, len, g_FrameCount);
			s32 result = 0;
			switch (fno)
			{
				case NET_FNO_POLL:
					if (!std::exchange(s_rbk_started, true))
					{
						RbkQueue(0x10, 0x6910, 0x1000, {});
						Console.WriteLn("ZdxsvGgpo: rbk position %d of %d, battle start at vsync %u", s_rbk_me + 1, s_rbk_n, g_FrameCount);
					}
					BattleServerCall(s_rbk_rx, s_rbk_me, s_rbk_n, fno, len, d, result);
					break;
				case NET_FNO_SEND:
				{
					const s32 n = std::clamp<s32>(len, 0, 0x3ca);
					if (n >= 12 && d[0] == 0x81) // lobby Qs
					{
						for (s32 o = 0; o + 12 <= n;)
						{
							const u16 cmd = static_cast<u16>(d[o + 2] << 8 | d[o + 3]);
							const u32 size = std::min<u32>(d[o + 4] << 8 | d[o + 5], n - o - 12);
							const u16 seq = static_cast<u16>(d[o + 6] << 8 | d[o + 7]);
							const std::vector<u8> body = RbkBody(cmd, d + o + 12, size);
							RbkQueue(0x02, cmd, seq, body);
							Console.WriteLn("ZdxsvGgpo: rbk Q %04x -> %zu B", cmd, body.size());
							o += 12 + size;
						}
					}
					else
						BattleServerCall(s_rbk_rx, s_rbk_me, s_rbk_n, fno, len, d, result);
					break;
				}
				case 0x38:
					result = 3;
					break;
				case 0xd: // lobby close
					s_rbk_rx.clear();
					break;
				case 0x16:
				case 0x46: // link up: 1 makes the game close the lobby it resumed with
					break;
				default:
					if (!BattleServerCall(s_rbk_rx, s_rbk_me, s_rbk_n, fno, len, d, result))
						return false;
			}
			*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
			cpuRegs.GPR.n.v0.SD[0] = result;
			cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
			return true;
		}
	} // namespace

	u32 s_game_gp = 0; // game gp, latched in OnNetRpc
	std::FILE* s_net_trace = [] {
		const char* p = Zdxsv::TestEnv("ZDXSV_NET_TRACE");
		std::FILE* f = p ? std::fopen(p, "w") : nullptr;
		if (f) // unbuffered: the rig kills pcsx2, buffered lines would be lost (zdxsv/probelint.py)
			std::setvbuf(f, nullptr, _IONBF, 0);
		return f;
	}();

	void OnNetRpc()
	{
		s_game_gp = cpuRegs.GPR.n.gp.UL[0];
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		const u8* ram = eeMem->Main;
		const s16 sock = *reinterpret_cast<const s16*>(ram + NET_REQ_SOCK);
		const s16 len = *reinterpret_cast<const s16*>(ram + NET_REQ_LEN);
		if (fno != NET_FNO_SEND || sock != NET_BATTLE_SOCK || len <= 0 || len > 0x3ca)
			return;
		const u8* d = ram + NET_REQ_DATA;
		if (!s_net_trace)
			return;
		std::string hex;
		for (int i = 0; i < len; i++)
			hex += fmt::format("{:02x}", d[i]);
		std::fprintf(s_net_trace, "%u S %s", g_FrameCount, hex.c_str());
		// McsMessage framing: byte 0 = length, byte 1 = kind << 4 | sender.
		for (int i = 0; i + 1 < len && d[i] >= 2; i += d[i])
		{
			if ((d[i + 1] >> 4) != 2 || i + d[i] > len)
				continue;
			std::vector<KeySlot> slots;
			if (!ParseKeySlots(d + i, d[i], slots))
			{
				std::fputs(" bad", s_net_trace);
				continue;
			}
			for (const KeySlot& s : slots)
				NoteOwnSend(s), std::fprintf(s_net_trace, s.rec ? " %02x:%02x:%02x:%04x/%04x" : " %02x:%02x", s.c, s.k, s.x, s.a, s.b);
		}
		std::fputc('\n', s_net_trace);
		std::fflush(s_net_trace);
	}

	void NoteOwnSend(const KeySlot& s)
	{
		s_own_k[s.c] = s.k;
		s_own_frame[s.c] = g_FrameCount;
		if (s.rec)
			s_own_x = s.x;
	}

	// EE rec hook at NET_RECV_RET_PC (fno 0x14 recv, after the wait RPC returned): s4 = sock,
	// result length at 0xc22c98, data at 0xc22ca0. Logs R lines and checks that every remote key
	// msg = BuildKeyMsg(its input fields + the local game's own k for that counter).
	void OnNetRecv()
	{
		u8* ram = eeMem->Main;
		const s32 sock = cpuRegs.GPR.n.s4.SL[0];
		const s32 len = *reinterpret_cast<const s32*>(ram + NET_RES_LEN);
		if (sock == NET_BATTLE_SOCK && s_net_trace && (cpuRegs.GPR.n.v0.SL[0] < 0 || len < 0 || len > 0x3ca))
			std::fprintf(s_net_trace, "%u E recv v0 %d len %d\n", g_FrameCount, cpuRegs.GPR.n.v0.SL[0], len);
		if (sock != NET_BATTLE_SOCK || cpuRegs.GPR.n.v0.SL[0] < 0 || len < 0 || len > 0x3ca || !s_net_trace)
			return;
		u8* d = ram + NET_REQ_DATA;
		if (len == 0)
			return;
		std::string hex;
		for (int i = 0; i < len; i++)
			hex += fmt::format("{:02x}", d[i]);
		std::fprintf(s_net_trace, "%u R %s", g_FrameCount, hex.c_str());
		for (int i = 0; i + 1 < len && d[i] >= 2; i += d[i])
		{
			s_rs.msgs++;
			if ((d[i + 1] >> 4) != 2 || i + d[i] > len)
				continue;
			s_rs.keymsgs++;
			std::vector<KeySlot> slots;
			if (!ParseKeySlots(d + i, d[i], slots))
			{
				std::fputs(" bad", s_net_trace);
				continue;
			}
			for (KeySlot& s : slots)
			{
				s_rs.slots++;
				s_rs.recs += s.rec;
				// local k for c must be from a send within the last 32 frames (the counter wraps at 64)
				const bool known = s_own_frame[s.c] != 0 && g_FrameCount - s_own_frame[s.c] < 32;
				char flag = ' ';
				if (!known)
					s_rs.unknown++, flag = '?';
				else if (s_own_k[s.c] != s.k)
					s_rs.kbad++, flag = '!';
				if (s.rec && s.x != s_own_x)
					s_rs.xbad++, flag = flag == ' ' ? 'x' : flag;
				if (s.rec)
					std::fprintf(s_net_trace, " %02x:%02x:%02x:%04x/%04x%c", s.c, s.k, s.x, s.a, s.b, flag);
				else
					std::fprintf(s_net_trace, " %02x:%02x%c", s.c, s.k, flag);
				if (known)
					s.k = s_own_k[s.c];
				if (s.rec && s_own_x >= 0)
					s.x = static_cast<u8>(s_own_x);
			}
			const std::vector<u8> m = BuildKeyMsg(d[i + 1] & 0xf, slots);
			if (m.size() != d[i] || std::memcmp(m.data(), d + i, m.size()) != 0)
				s_rs.rebuilt_bad++, std::fputs(" REBUILT_DIFF", s_net_trace);
		}
		std::fputc('\n', s_net_trace);
		if (s_rs.keymsgs % 2000 == 1)
			std::fprintf(s_net_trace, "STATS msgs=%u keymsgs=%u slots=%u recs=%u unknown=%u kbad=%u xbad=%u rebuilt_bad=%u\n",
				s_rs.msgs, s_rs.keymsgs, s_rs.slots, s_rs.recs, s_rs.unknown, s_rs.kbad, s_rs.xbad, s_rs.rebuilt_bad);
		std::fflush(s_net_trace);
	}

	// Own input of frame (live: GGPO adds the delay; replay takeover: PlayFrame): pad 0 undelayed to
	// the game, its (A, B) + the own kind-3 msgs due since the last input.
	NetInput NetPack(const Input& pad, int frame)
	{
		NetInput in = s_battle.local;
		in.pad = pad;
		s_rings.zd_pad[frame & 127] = in.pad;
		u16 ab[2];
		ZdPadAB(in.pad, ab[0], ab[1]);
		if (s_net_trace)
			std::fprintf(s_net_trace, "%u Q %d %04x %04x\n", g_FrameCount, frame, ab[0], ab[1]);
		in.pad = {};
		std::memcpy(&in.pad, ab, sizeof(ab));
		in.pad.unused[0] = s_rb.ms.n;
		in.pad.unused[1] = s_rb.ps.n;
		std::vector<u8> data;
		while (!s_battle.out.empty() && (s_k3_lag == 0 || s_battle.out.front().frame + s_k3_lag <= frame))
		{
			const std::vector<u8>& m = s_battle.out.front().m;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u O %d %zu %d\n", g_FrameCount, frame, s_battle.out.front().idx, s_battle.out.front().frame);
			if (m.size() > sizeof(in.data))
			{
				if (s_ns.toolong++ < 20)
					Console.Error("ZdxsvGgpo: net msg of %zu bytes dropped", m.size());
			}
			else if (data.size() + m.size() > sizeof(in.data))
				break;
			else
				data.insert(data.end(), m.begin(), m.end());
			s_battle.out.pop_front();
		}
		if (!data.empty())
		{
			in.seq++;
			in.len = static_cast<u8>(data.size());
			std::memset(in.data, 0, sizeof(in.data));
			std::memcpy(in.data, data.data(), data.size());
		}
		s_battle.local = in;
		return in;
	}

	// The synced inputs of frame s_battle.frame (live: GGPO, replay: the file) to the game.
	void NetApply(const NetInput* in)
	{
		const int f = s_battle.frame;
		ReplayLog(f, in);
		ApplyPad(0, s_rings.zd_pad[f & 127]);
		for (int p = 0; p < s_opt.players; p++)
			std::memcpy(s_rings.zd_hist[f & 127][p], &in[p].pad, sizeof(s_rings.zd_hist[f & 127][p]));
		// a predicted input repeats its seq, so entries come only from real inputs: no rollback state
		for (int p = 0; p < s_opt.players; p++)
		{
			const bool fresh = in[p].seq != s_rings.net_seq_at[(f - 1) & 63][p];
			const u8* d = in[p].data;
			const u32 len = std::min<u32>(in[p].len, sizeof(in[p].data));
			if (fresh)
				for (u32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
					if ((d[i + 1] >> 4) == 3)
					{
						std::vector<std::vector<u8>>& v = s_battle.zds_k3[p];
						if (v.size() <= static_cast<size_t>(s_rb.zds_seen[p]))
							v.resize(s_rb.zds_seen[p] + 1);
						v[s_rb.zds_seen[p]++].assign(d + i, d + i + d[i]);
					}
			s_rings.net_seq_at[f & 63][p] = in[p].seq;
		}
		for (;;)
		{
			bool all = true;
			for (int p = 0; p < s_opt.players; p++)
				all = all && s_rb.zds_seen[p] > s_rb.zds_rel;
			if (!all)
				break;
			for (int p = 0; p < s_opt.players; p++)
				if (p != s_battle.me)
					s_rb.net_rx.insert(s_rb.net_rx.end(), s_battle.zds_k3[p][s_rb.zds_rel].begin(), s_battle.zds_k3[p][s_rb.zds_rel].end());
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u K3%s %d %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "", f, s_rb.zds_rel);
			if (!g_ggpo_in_rollback)
				s_battle.zds_k3rel++;
			s_rb.zds_rel++;
		}
		const auto release = [&](PS& b, int k, char tag) {
			if (!b.hold || b.go)
				return;
			for (int p = 0; p < s_opt.players; p++)
				if (static_cast<u8>(in[p].pad.unused[k] - b.rel) < 1 || static_cast<u8>(in[p].pad.unused[k] - b.rel) >= 128)
					return;
			b.go = true;
			b.rel++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u %cS%s %d %d\n", g_FrameCount, tag, g_ggpo_in_rollback ? "r" : "", f, b.rel);
		};
		release(s_rb.ps, 1, 'P');
		if (s_ms_on)
			release(s_rb.ms, 0, 'M');
	}

	// A reset VM (live auto-next) starts its battle from the lobby phase again.
	void RbkReset()
	{
		s_rbk_rx.clear();
		s_bs.rx.clear();
		s_bs.players = 0;
		s_rbk_started = false;
		std::fill(std::begin(s_rbk_calls), std::end(s_rbk_calls), 0u);
	}

	// net mode: the battle sock's RPCs. Returns true when answered (v0 = result, pc = ra).
	static bool GgpoNetCall()
	{
		if (!s_net_env)
			return false;
		const u32 fno = cpuRegs.GPR.n.a0.UL[0];
		u8* ram = eeMem->Main;
		const s16 sock = *reinterpret_cast<const s16*>(ram + NET_REQ_SOCK);
		const s16 len = *reinterpret_cast<const s16*>(ram + NET_REQ_LEN);
		u8* d = ram + NET_REQ_DATA;
		const bool key = fno == NET_FNO_SEND && sock == NET_BATTLE_SOCK && len > 0 && len <= 0x3ca && HasKeyMsg(d, len);
		// lobby=1: the battle server is answered on the EE (BattleServerCall); the game never reaches it
		if (s_opt.lobby && !s_play_env && !s_rbk && fno == NET_FNO_CONNECT && (!s_battle.armed || s_battle.over))
		{
			u32 ip;
			std::memcpy(&ip, ram + NET_REQ_SOCK, 4);
			int me, players;
			if (IsBattleServer(ip, static_cast<u16>(d[0] << 8 | d[1])) && LobbyPendingBattle(me, players))
			{
				if (s_battle.lobby_cut)
					LobbyCutCall(fno, sock, len);
				if (s_battle.over)
					NetReset();
				s_bs.rx.clear();
				s_bs.me = me;
				s_bs.players = players;
				s32 result;
				BattleServerCall(s_bs.rx, me, players, fno, len, d, result);
				Console.WriteLn("ZdxsvGgpo: battle server answered on the EE, position %d of %d, vsync %u", me, players, g_FrameCount);
				return NetAnswer(result);
			}
		}
		if (s_bs.players && !s_battle.armed && sock == NET_BATTLE_SOCK)
		{
			if (fno == NET_FNO_CLOSE)
			{
				if (s_battle.lobby_cut)
					LobbyCutCall(fno, sock, len);
				s_bs.rx.clear();
				s_bs.players = 0;
				return NetAnswer(0);
			}
			s32 result;
			if (!s_battle.lobby_cut && !key && BattleServerCall(s_bs.rx, s_bs.me, s_bs.players, fno, len, d, result))
				return NetAnswer(result);
		}
		if (s_battle.over && s_battle.local_server && (fno == NET_FNO_CONNECT || (fno == NET_FNO_CLOSE && sock == NET_BATTLE_SOCK)))
		{
			s_battle.local_server = s_battle.closed = false;
			Console.WriteLn("ZdxsvGgpo: battle sock released at vsync %u: game RPC 0x%x", g_FrameCount, fno);
			if (fno == NET_FNO_CLOSE)
				return NetAnswer(0);
		}
		if (s_battle.over)
		{
			// lobby=1: the first key msg of a battle with a new battle info starts the next GGPO battle; the
			// sends of the ended battle (same battle info) stay with the IOP
			if (s_opt.lobby && !s_play_env && !s_rbk && key && LobbyNewBattle())
			{
				NetReset();
				Console.WriteLn("ZdxsvGgpo: next lobby battle at vsync %u", g_FrameCount);
			}
			else if (!(s_battle.closed || s_battle.local_server) || sock != NET_BATTLE_SOCK)
				return false;
			else
			{
				// The battle server closes the connection on the game's end msg; the game waits ~100 s for
				// that before it gives up. A closed sock: sends dropped, nothing to recv, the poll fails.
				s32 result = 0;
				if (fno == NET_FNO_SEND)
					result = std::clamp<s32>(len, 0, 0x3ca);
				else if (fno == NET_FNO_POLL)
					result = -1;
				else if (s_battle.local_server && fno != NET_FNO_RECV)
					return false;
				else if (fno != NET_FNO_RECV)
				{
					s_battle.closed = false;
					Console.WriteLn("ZdxsvGgpo: battle sock released at vsync %u: game RPC 0x%x", g_FrameCount, fno);
					return false;
				}
				*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
				cpuRegs.GPR.n.v0.SD[0] = result;
				cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
				return true;
			}
		}
		if (s_rbk && !s_battle.armed && !key) // connect (fno 7) has the address in the sock field
			return RbkCall(fno, len, d);
		if (s_battle.lobby_cut)
			return LobbyCutCall(fno, sock, len);
		if (sock != NET_BATTLE_SOCK)
			return false;
		if (!s_battle.armed)
		{
			// sock 0 is the lobby TCP too: arm at the battle's first key msg
			if (!key)
				return false;
			int me = -1;
			for (s32 i = 0; i + 1 < len && d[i] >= 2 && i + d[i] <= len; i += d[i])
				if ((d[i + 1] >> 4) == 2)
					me = d[i + 1] & 0xf;
			if (s_opt.lobby && !LobbyArm(me))
				return s_battle.lobby_cut && LobbyCutCall(fno, sock, len);
			s_battle.armed = true;
			if (s_rbk)
			{
				s_rb.net_rx.insert(s_rb.net_rx.end(), s_rbk_rx.begin(), s_rbk_rx.end());
				s_rbk_rx.clear();
				if (!s_play_env)
					s_net_players = RbkPlayers();
			}
			if (s_bs.players)
			{
				s_rb.net_rx.insert(s_rb.net_rx.end(), s_bs.rx.begin(), s_bs.rx.end());
				s_bs.rx.clear();
				s_bs.players = 0;
				s_battle.local_server = true;
			}
			// ZDXSV_RBK_TURBO=1: the battle runs turbo too (GGPO paces the peers by frame)
			if (!s_rbk || !Zdxsv::TestEnv("ZDXSV_RBK_TURBO"))
				VMManager::SetLimiterMode(LimiterModeType::Nominal);
			if (me >= 0)
				s_battle.me = me;
			Console.WriteLn("ZdxsvGgpo: net armed at vsync %u, position %d", g_FrameCount, s_battle.me);
		}
		if (*reinterpret_cast<const s16*>(ram + NET_NOWAIT) != 0)
			s_ns.nowait++;
		s32 result = 0;
		if (fno == NET_FNO_SEND)
		{
			s_ns.sends++;
			const s32 n = std::clamp<s32>(len, 0, 0x3ca);
			s32 i = 0;
			for (; i + 1 < n && d[i] >= 2 && i + d[i] <= n; i += d[i])
				NetSend(std::vector<u8>(d + i, d + i + d[i]));
			if (i < n) // not McsMessage framed: one msg
				NetSend(std::vector<u8>(d + i, d + n));
			result = n;
		}
		else if (fno == NET_FNO_RECV)
		{
			s_ns.recvs++;
			u32 n = 0;
			while (n + 1 < s_rb.net_rx.size() && s_rb.net_rx[n] >= 2 && n + s_rb.net_rx[n] <= s_rb.net_rx.size() && n + s_rb.net_rx[n] <= NET_RX_MAX)
				n += s_rb.net_rx[n], s_ns.rxmsgs++;
			if (n == 0 && !s_rb.net_rx.empty() && s_rb.net_rx.size() <= NET_RX_MAX) // unframed rest
				n = static_cast<u32>(s_rb.net_rx.size());
			std::memcpy(d, s_rb.net_rx.data(), n);
			s_rb.net_rx.erase(s_rb.net_rx.begin(), s_rb.net_rx.begin() + n);
			s_ns.rxbytes += n;
			result = static_cast<s32>(n);
		}
		else if (fno == NET_FNO_POLL)
		{
			// As a real battle poll returns: state 4, 0x2000 send space, readable bytes
			s_ns.polls++;
			*reinterpret_cast<u16*>(ram + NET_REQ_LEN) = 4;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 2) = 0x2000;
			*reinterpret_cast<u16*>(ram + NET_REQ_DATA + 4) = static_cast<u16>(std::min<size_t>(s_rb.net_rx.size(), NET_RX_MAX));
		}
		else
		{
			s_ns.other++;
			return false;
		}
		*reinterpret_cast<s32*>(ram + NET_RES_LEN) = result;
		cpuRegs.GPR.n.v0.SD[0] = result;
		cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0];
		return true;
	}

	// EE rec hook at NET_RPC_PC (the net RPC wrapper's entry): trace, GGPO first; what it leaves goes to
	// the no-DEV9 host sockets, else to the IOP. Returns true when answered.
	bool OnNetCall()
	{
		OnNetRpc();
		if (GgpoNetCall())
			return true;
		return g_no_dev9 && NoDev9OnNetCall();
	}

	bool GgpoBattleRefuses(Error* error)
	{
		if (!g_ggpo_active || !s_opt.net || s_play_env)
			return false;
		const std::string_view msg = TRANSLATE_SV("Zdxsv", "Not available during an online battle.");
		Console.WriteLn("ZdxsvGgpo: refused during the battle (pause, speed or state load)");
		if (error)
			Error::SetString(error, std::string(msg));
		else
			Host::AddIconOSDMessage("ZdxsvBattleRefuses", ICON_FA_TRIANGLE_EXCLAMATION, std::string(msg), Host::OSD_WARNING_DURATION);
		return true;
	}

	bool OnlineBattleNow()
	{
		return g_ggpo_active && s_opt.net && !s_play_env;
	}

	bool OnlineNow()
	{
		return OnlineBattleNow() || LobbyConnectionOpen();
	}

	bool OnlinePauseMenuRefuses()
	{
		if (!OnlineNow())
			return false;
		Console.WriteLn("Zdxsv: pause menu refused (%s)", OnlineBattleNow() ? "battle" : "lobby");
		Host::AddIconOSDMessage("ZdxsvPauseMenuRefuses", ICON_FA_TRIANGLE_EXCLAMATION,
			TRANSLATE_STR("Zdxsv", "The pause menu is not available while online."), Host::OSD_WARNING_DURATION);
		return true;
	}

	namespace
	{
		// The game's button settings for pad port 0 (system data): u16[8], per assignable button
		// (□ △ L1 × ○ R1 L2 R2) an index into PAD_ACTIONS, the game's action bits of the A word.
		constexpr u32 PAD_BIND_SLOTS = 0x7ade6c;
		constexpr u16 PAD_ACTIONS[8] = {0x0200, 0x0100, 0x0040, 0x0080, 0x0300, 0x0180, 0x0280, 0x0020};
		constexpr u16 PAD_DEFAULT_SLOTS[8] = {0, 1, 7, 3, 2, 4, 6, 5};
	} // namespace

	// The game's input record (A, B) of a host pad, A with the game's button settings (OR-linear; B bit 0 = game
	// state, left 0). A stick counts as a direction at axis <= 0x40 / >= 0xc0: the left stick sets the D-pad's B
	// bits, the right stick the same bits in A.
	void ZdPadAB(const Input& in, u16& a, u16& b)
	{
		using I = PadDualshock2::Inputs;
		static constexpr struct { I i; u16 a, b; s8 slot; } bind[] = {
			{I::PAD_L3, 0, 0x0002, -1}, {I::PAD_R2, 0, 0x0004, 7}, {I::PAD_L2, 0, 0x0008, 6},
			{I::PAD_R1, 0, 0x0010, 5}, {I::PAD_CIRCLE, 0, 0x0020, 4}, {I::PAD_CROSS, 0, 0x0040, 3},
			{I::PAD_L1, 0, 0x0080, 2}, {I::PAD_TRIANGLE, 0, 0x0100, 1}, {I::PAD_SQUARE, 0, 0x0200, 0},
			{I::PAD_RIGHT, 0, 0x0400, -1}, {I::PAD_LEFT, 0, 0x0800, -1}, {I::PAD_DOWN, 0, 0x1000, -1},
			{I::PAD_UP, 0, 0x2000, -1}, {I::PAD_SELECT, 0, 0x4000, -1}, {I::PAD_START, 0x8000, 0x8000, -1}};
		u16 slots[8];
		std::memcpy(slots, PAD_DEFAULT_SLOTS, sizeof(slots));
		if (eeMem)
		{
			u16 cfg[8];
			std::memcpy(cfg, eeMem->Main + PAD_BIND_SLOTS, sizeof(cfg));
			bool valid = true;
			for (const u16 s : cfg)
				valid &= s < std::size(PAD_ACTIONS);
			if (valid)
				std::memcpy(slots, cfg, sizeof(slots));
		}
		a = b = 0;
		for (const auto& e : bind)
		{
			if (!((in.buttons >> e.i) & 1))
				continue;
			a |= e.a;
			b |= e.b;
			if (e.slot >= 0)
				a |= PAD_ACTIONS[slots[e.slot]];
		}
		const auto dirs = [](u8 x, u8 y) {
			return static_cast<u16>((x >= 0xc0 ? 0x0400 : 0) | (x <= 0x40 ? 0x0800 : 0) | (y >= 0xc0 ? 0x1000 : 0) | (y <= 0x40 ? 0x2000 : 0));
		};
		b |= dirs(in.lx, in.ly);
		a |= dirs(in.rx, in.ry);
	}

	bool s_ms_on = true;

	// A load step's pass: count the wish, then hold until NetApply sets go. true = held: v0 = 0 (step not done,
	// the scene retries it next frame), pc = the step's epilogue. Trace `<tag>H frame n`.
	static bool BarrierHold(PS& b, char tag, u32 epilogue)
	{
		if (!g_ggpo_active || !s_battle.armed || s_battle.over)
			return false;
		if (!b.hold)
		{
			b.hold = true;
			b.n++;
			if (s_net_trace)
				std::fprintf(s_net_trace, "%u %cH%s %d %d\n", g_FrameCount, tag, g_ggpo_in_rollback ? "r" : "", s_battle.frame, b.n);
		}
		if (b.go)
		{
			b.hold = b.go = false;
			return false;
		}
		cpuRegs.GPR.n.v0.UD[0] = 0;
		cpuRegs.pc = epilogue;
		return true;
	}

	// ps: rec hook at LOAD_STEP_PC (0x2b1d80, battle load step past its load-busy check), epilogue 0x2b1fa8.
	bool OnLoadStep()
	{
		return BarrierHold(s_rb.ps, 'P', 0x2b1fa8);
	}

	// ms: rec hook at MS_STEP_PC (0x2b8698, MS-select load step past its load-busy check), epilogue 0x2b8ab8.
	bool OnMsStep()
	{
		return s_ms_on && BarrierHold(s_rb.ms, 'M', 0x2b8ab8);
	}

	// zd: rec hook at the lockstep step's ring read (0x312bf4, per active position: s0 = position,
	// a1 = ring entry: +2 A, +6 B as u16; bit 0 of B is game-wide state, kept). Trace `Z frame p slot A B`.
	void OnStepCopy()
	{
		if (!g_ggpo_active || !s_battle.armed)
			return;
		const int p = static_cast<s8>(cpuRegs.GPR.n.s0.UL[0]);
		const u32 e = cpuRegs.GPR.n.a1.UL[0] & 0x1ffffff;
		if (p < 0 || p >= s_opt.players || e + 8 > Ps2MemSize::MainRam)
			return;
		u8* ram = eeMem->Main;
		u16 a, b;
		std::memcpy(&a, ram + e + 2, 2);
		std::memcpy(&b, ram + e + 6, 2);
		const int c = ram[e] & 63;
		const int k = s_battle.frame;
		const u16* ab = s_rings.zd_hist[k & 127][p];
		const u16 na = ab[0];
		const u16 nb = static_cast<u16>((ab[1] & ~1u) | (b & 1u));
		s_battle.zd_steps++;
		if (a != na || b != nb)
			s_battle.zd_changed++;
		std::memcpy(ram + e + 2, &na, 2);
		std::memcpy(ram + e + 6, &nb, 2);
		if (s_net_trace)
			std::fprintf(s_net_trace, "%u Z%s %d %d %02x %04x %04x %04x %04x %02x %d\n", g_FrameCount, g_ggpo_in_rollback ? "r" : "",
				s_battle.frame, p, ((e - 0xc61f40) / 8) & 63, na, nb, a, b, c, k);
	}
} // namespace Zdxsv
