// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The lobby's TCP message frame, shared by the lobby files (Lobby*.cpp); included by no other component.
// Header (12 bytes, big-endian): direction, category, command (u16), body size (u16), sequence (u16), status (u32).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Zdxsv
{
	constexpr size_t LOBBY_FRAME_HEADER = 12;
	constexpr size_t LOBBY_FRAME_BODY_MAX = 0xFFFF;
	constexpr uint16_t LOBBY_CUSTOM_COMMANDS = 0x9900; // 0x99xx: commands only the emulator and the zdxsv lobby know

	enum LobbyFrameDirection : uint8_t
	{
		LobbyToClient = 0x18,
		LobbyToServer = 0x81,
	};

	enum LobbyFrameCategory : uint8_t
	{
		LobbyQuestion = 0x01,
		LobbyAnswer = 0x02,
		LobbyNotice = 0x10,
		LobbyCustom = 0xFF,
	};

	// Commands the emulator reads or writes; the game handles all others.
	enum LobbyFrameCommand : uint16_t
	{
		LobbyLineCheck = 0x6001,
		LobbyShutdownNotice = 0x6003,
		LobbyKeyPair = 0x6101,
		LobbyMatchStart = 0x6910, // first of the match start exchange, which ends at LobbyMatchLast
		LobbyMatchLast = 0x6917,
		LobbyPlatformInfo = 0x9950,
		LobbyBattleInfo = 0x9951,
		LobbyMatchingReport = 0x9952,
		LobbyGamePatches = 0x9953,
	};

	// A frame whose header the caller has; Body() needs Size() bytes.
	struct LobbyFrameView
	{
		const uint8_t* p;

		uint8_t Direction() const { return p[0]; }
		uint8_t Category() const { return p[1]; }
		uint16_t Command() const { return static_cast<uint16_t>((p[2] << 8) | p[3]); }
		uint16_t Sequence() const { return static_cast<uint16_t>((p[6] << 8) | p[7]); }
		size_t BodySize() const { return (size_t{p[4]} << 8) | p[5]; }
		size_t Size() const { return LOBBY_FRAME_HEADER + BodySize(); }
		std::string_view Body() const { return {reinterpret_cast<const char*>(p + LOBBY_FRAME_HEADER), BodySize()}; }
		bool Is(uint8_t direction, uint8_t category, uint16_t command) const
		{
			return Direction() == direction && Category() == category && Command() == command;
		}
	};

	// Appends a client-to-server frame with a success status. False (nothing appended) when the body does not fit.
	inline bool LobbyFrameAppend(std::vector<uint8_t>& out, uint8_t category, uint16_t command, uint16_t sequence,
		std::string_view body = {})
	{
		if (body.size() > LOBBY_FRAME_BODY_MAX)
			return false;
		const uint8_t header[LOBBY_FRAME_HEADER] = {LobbyToServer, category, static_cast<uint8_t>(command >> 8),
			static_cast<uint8_t>(command), static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size()),
			static_cast<uint8_t>(sequence >> 8), static_cast<uint8_t>(sequence), 0x00, 0xFF, 0xFF, 0xFF};
		out.insert(out.end(), header, header + LOBBY_FRAME_HEADER);
		out.insert(out.end(), body.begin(), body.end());
		return true;
	}
} // namespace Zdxsv
