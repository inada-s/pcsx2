// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Minimal protobuf wire codec for zdxsv's messages (zdxsv.proto: lobby Ping / Pong; replay.proto: BattleLogFile).
// No generated code: the writers put fields in field order, the reader hands each field to a callback.
namespace Zdxsv::Pb
{
	inline void PutVarint(std::vector<uint8_t>& o, uint64_t v)
	{
		while (v >= 0x80)
		{
			o.push_back(static_cast<uint8_t>(v | 0x80));
			v >>= 7;
		}
		o.push_back(static_cast<uint8_t>(v));
	}

	inline void PutBytes(std::vector<uint8_t>& o, uint32_t field, const void* p, size_t n)
	{
		PutVarint(o, (field << 3) | 2);
		PutVarint(o, n);
		o.insert(o.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
	}

	inline void PutString(std::vector<uint8_t>& o, uint32_t field, std::string_view s)
	{
		PutBytes(o, field, s.data(), s.size());
	}

	// Varint field (int32 / int64 / uint / bool). Negative int32 / int64 go as 10-byte two's complement, as protobuf.
	inline void PutUint(std::vector<uint8_t>& o, uint32_t field, uint64_t v)
	{
		PutVarint(o, field << 3);
		PutVarint(o, v);
	}

	inline void PutInt(std::vector<uint8_t>& o, uint32_t field, int64_t v)
	{
		PutUint(o, field, static_cast<uint64_t>(v));
	}

	// Packed repeated varint (proto3 default for repeated int32 / uint64).
	inline void PutPackedInts(std::vector<uint8_t>& o, uint32_t field, const std::vector<int64_t>& vs)
	{
		std::vector<uint8_t> m;
		for (const int64_t v : vs)
			PutVarint(m, static_cast<uint64_t>(v));
		PutBytes(o, field, m.data(), m.size());
	}

	struct Reader
	{
		const uint8_t* p;
		const uint8_t* end;

		bool Varint(uint64_t& v)
		{
			v = 0;
			for (int shift = 0; shift < 64 && p < end; shift += 7)
			{
				const uint8_t b = *p++;
				v |= uint64_t{b & 0x7Fu} << shift;
				if (!(b & 0x80))
					return true;
			}
			return false;
		}

		// Calls f(field, wiretype, value, bytes, len) per field: value = the varint, fixed64 or fixed32 (wire types 0, 1,
		// 5), bytes / len = a length-delimited field (wire type 2). f returns false to stop with a failure.
		template <typename F>
		bool Fields(F f)
		{
			while (p < end)
			{
				uint64_t tag, v = 0;
				if (!Varint(tag))
					return false;
				const uint32_t wt = tag & 7;
				const uint8_t* bytes = nullptr;
				size_t n = 0;
				if (wt == 0)
				{
					if (!Varint(v))
						return false;
				}
				else if (wt == 2)
				{
					if (!Varint(v) || v > static_cast<uint64_t>(end - p))
						return false;
					bytes = p;
					n = static_cast<size_t>(v);
					p += n;
				}
				else if (wt == 1 || wt == 5)
				{
					const size_t size = wt == 1 ? 8 : 4;
					if (static_cast<size_t>(end - p) < size)
						return false;
					for (size_t i = 0; i < size; i++) // little endian
						v |= uint64_t{p[i]} << (8 * i);
					p += size;
				}
				else
					return false;
				if (!f(static_cast<uint32_t>(tag >> 3), wt, v, bytes, n))
					return false;
			}
			return true;
		}
	};

	// A repeated varint field, packed (wire type 2) or not (0), appended to out.
	inline bool ReadInts(uint32_t wt, uint64_t v, const uint8_t* bytes, size_t n, std::vector<int64_t>& out)
	{
		if (wt == 0)
		{
			out.push_back(static_cast<int64_t>(v));
			return true;
		}
		if (wt != 2)
			return false;
		Reader r{bytes, bytes + n};
		while (r.p < r.end)
		{
			if (!r.Varint(v))
				return false;
			out.push_back(static_cast<int64_t>(v));
		}
		return true;
	}
} // namespace Zdxsv::Pb
