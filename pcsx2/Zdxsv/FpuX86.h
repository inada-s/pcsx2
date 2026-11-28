// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"

#include <algorithm>
#include <bit>
#include <utility>

// EE FPU ops computed as the x86 recompiler (x86/iFPU.cpp) does, so the interpreter matches it: the add
// cut on every host, the rest for the interpreter on ARM64. Values are raw FPR words.
namespace Zdxsv
{
	inline constexpr u32 FpuPosFmax = 0x7F7FFFFF;
	inline constexpr u32 FpuNegFmax = 0xFF7FFFFF;

	// The PS2 FPU's add: the smaller operand's mantissa cut to the bits the larger exponent keeps
	// (FPU_ADD_SUB in x86/iFPU.cpp).
	inline void FpuAddSubCut(u32& a, u32& b)
	{
		const s32 d = static_cast<s32>((a >> 23) & 0xff) - static_cast<s32>((b >> 23) & 0xff);
		if (d >= 25)
			b &= 0x80000000;
		else if (d > 0)
			b &= 0xffffffffu << (d - 1);
		else if (d <= -25)
			a &= 0x80000000;
		else if (d < 0)
			a &= 0xffffffffu << (-d - 1);
	}

	// The default clamp mode: operands reach SSE unclamped, denormals as zero; a NaN result becomes
	// +Fmax and an infinity +/-Fmax (ClampValues); O and U are never written.
	inline bool FpuX86Mode()
	{
		return CHECK_FPU_OVERFLOW && !CHECK_FPU_EXTRA_OVERFLOW && !CHECK_FPU_FULL;
	}

	// fpuFloat2: +/-NaN, +/-Inf -> +/-Fmax
	inline u32 FpuClampX86(u32 v)
	{
		if (!CHECK_FPU_OVERFLOW)
			return v;
		v = static_cast<u32>(std::min<s32>(static_cast<s32>(v), static_cast<s32>(FpuPosFmax)));
		return std::min<u32>(v, FpuNegFmax);
	}

	// MAXSS/MINSS on clamped operands with denormals as zero: on equal operands (+0 vs -0) SSE returns
	// the second one, which recCommutativeOp makes Ft for the Z game.
	inline u32 FpuMaxMinX86(u32 fs, u32 ft, bool max)
	{
		u32 a = FpuClampX86(fs);
		u32 b = FpuClampX86(ft);
		if (!(a & 0x7f800000))
			a &= 0x80000000;
		if (!(b & 0x7f800000))
			b &= 0x80000000;
		const float fa = std::bit_cast<float>(a);
		const float fb = std::bit_cast<float>(b);
		return (max ? fa > fb : fa < fb) ? a : b;
	}

	inline float FpuDaz(u32 f)
	{
		if (!(f & 0x7f800000))
			f &= 0x80000000;
		return std::bit_cast<float>(f);
	}

	inline u32 FpuResultX86(float r)
	{
		u32 v = std::bit_cast<u32>(r);
		if ((v & 0x7f800000) == 0x7f800000)
			return (v & 0x007fffff) ? FpuPosFmax : (v & 0x80000000) | FpuPosFmax;
		if (!(v & 0x7f800000))
			v &= 0x80000000;
		return v;
	}

	inline u32 FpuAddSubX86(u32 a, u32 b, bool sub)
	{
		FpuAddSubCut(a, b);
		return FpuResultX86(sub ? FpuDaz(a) - FpuDaz(b) : FpuDaz(a) + FpuDaz(b));
	}

	// MADD/MSUB: the product rounded on its own, then added to ACC as ADD.S
	inline u32 FpuMulAddX86(u32 acc, u32 fs, u32 ft, bool sub)
	{
		const float p = FpuDaz(fs) * FpuDaz(ft);
		return FpuAddSubX86(acc, std::bit_cast<u32>(p), sub);
	}

	// recABS_S_xmm: MINSS with +Fmax, so NaN and Inf give +Fmax and a denormal 0
	inline u32 FpuAbsX86(u32 v)
	{
		v &= 0x7fffffff;
		return v >= 0x7f800000 ? FpuPosFmax : v < 0x00800000 ? 0 : v;
	}

	// recNEG_S_xmm: fpuFloat3 on the result, denormals kept
	inline u32 FpuNegX86(u32 v)
	{
		v ^= 0x80000000;
		return std::min<u32>(static_cast<u32>(std::min<s32>(static_cast<s32>(v), static_cast<s32>(FpuPosFmax))), FpuNegFmax);
	}
} // namespace Zdxsv
