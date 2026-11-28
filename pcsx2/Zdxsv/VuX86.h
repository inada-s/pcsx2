// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "VU.h"

#include <bit>
#include <cmath>

// VU ops computed as x86 microVU does, so the ARM64 interpreter matches x86 peers. Values are raw
// register words; a VECTOR lane 0 is x.
namespace Zdxsv
{
#ifdef ARCH_ARM64
	// x86 microVU keeps the D and I flags of the last DIV/SQRT/RSQRT through FMAC ops.
	inline constexpr u32 Vu0MacroStatusKeep = 0xFF0;
#else
	inline constexpr u32 Vu0MacroStatusKeep = 0xFC0;
#endif

#ifdef ARCH_ARM64
	// Set while an EE COP2 macro op runs: x86 microVU clamps more operands of MADDw and MSUB there.
	inline bool VuCop2Op = false;

	// Operands x86 microVU clamps for the upper op in code (clampType in microVU_Upper.inl), in the
	// default clamp mode (overflow only): bit 0 Ft (also a broadcast lane, Q or I), bit 1 Fs,
	// bit 2 ACC. Its clamp (MINPS +max, MAXPS -max) turns a NaN of either sign into +max; vuDouble
	// gives the rest of the values the same result.
	inline u32 VuX86ClampOperands(u32 code, int vu, bool cop2)
	{
		if (!CHECK_VU_OVERFLOW(vu) || CHECK_VU_SIGN_OVERFLOW(vu) || CHECK_VU_EXTRA_OVERFLOW(vu))
			return 0;
		const bool ps = ((code >> 21) & 0xf) == 0xf;
		const u32 funct = code & 0x3f;
		if (funct >= 0x3c)
		{
			switch ((code >> 6) & 0x1f)
			{
				case 2: return 2; // MADDAx..w
				case 6: return (ps && (code & 3) == 3) ? 3 : 2; // MULAx..w
				default: return 0;
			}
		}
		switch (funct)
		{
			case 0x04: case 0x05: case 0x06: case 0x07: case 0x24: case 0x26: case 0x2c: // SUBx..w, SUBq, SUBi, SUB
				return ps ? 3 : 0;
			case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1e: case 0x2a: // MULx..w, MULq, MULi, MUL
				return ps ? 3 : 2;
			case 0x08: case 0x09: case 0x0a: // MADDx..z
				return 2;
			case 0x0b: // MADDw
				return cop2 ? 7 : 2;
			case 0x2d: // MSUB
				return cop2 ? 2 : 0;
			default:
				return 0;
		}
	}

	inline u32 VuClampNaN(u32 f, bool on)
	{
		return (on && (f & 0x7fffffff) > 0x7f800000) ? 0x7f7fffff : f;
	}

	inline VECTOR VuClampNaN(const VECTOR& v, bool on)
	{
		VECTOR r;
		for (int i = 0; i < 4; i++)
			r.UL[i] = VuClampNaN(v.UL[i], on);
		return r;
	}

	// FMAC ops as x86 microVU computes them in the default clamp mode: SSE single precision with
	// DAZ/FTZ on the operands VuX86ClampOperands leaves alone. A NaN operand passes through quieted
	// (the first operand's wins, signaling or not), an invalid op gives the x86 default NaN, Inf and
	// NaN results stay unclamped. MAC flags as mVUupdateFlags: S = sign bit, Z = +-0, never O or U.
	enum class VuX86Op { None, Add, Sub, Mul, Madd, Msub };

	inline bool VuX86Fmac(int vu)
	{
		return CHECK_VU_OVERFLOW(vu) && !CHECK_VU_SIGN_OVERFLOW(vu) && !CHECK_VU_EXTRA_OVERFLOW(vu);
	}

	inline bool VuIsNaN(u32 f)
	{
		return (f & 0x7fffffff) > 0x7f800000;
	}

	// An operand: clamped as x86 mVUclamp2 (NaN -> +max, Inf -> signed max), else as read; DAZ.
	inline u32 VuX86In(u32 f, bool clamp)
	{
		if (clamp && VuIsNaN(f))
			return 0x7f7fffff;
		if ((f & 0x7f800000) == 0)
			return f & 0x80000000;
		if (clamp && (f & 0x7f800000) == 0x7f800000)
			return (f & 0x80000000) | 0x7f7fffff;
		return f;
	}

	inline VECTOR VuX86In(const VECTOR& v, bool clamp)
	{
		VECTOR r;
		for (int i = 0; i < 4; i++)
			r.UL[i] = VuX86In(v.UL[i], clamp);
		return r;
	}

	template <VuX86Op Op>
	inline u32 VuX86Arith(u32 a, u32 b)
	{
		if (VuIsNaN(a))
			return a | 0x400000;
		if (VuIsNaN(b))
			return b | 0x400000;
		const float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
		const float r = Op == VuX86Op::Add ? fa + fb : Op == VuX86Op::Sub ? fa - fb : fa * fb;
		const u32 v = std::bit_cast<u32>(r);
		if (VuIsNaN(v))
			return 0xffc00000;
		return (v & 0x7f800000) ? v : (v & 0x80000000);
	}

	// MADD = product + ACC, MADDA = ACC + product: the operand order picks which NaN passes.
	template <VuX86Op Op, bool ToAcc>
	inline u32 VuX86Lane(u32 acc, u32 fs, u32 ft)
	{
		if constexpr (Op == VuX86Op::Madd || Op == VuX86Op::Msub)
		{
			const u32 p = VuX86Arith<VuX86Op::Mul>(fs, ft);
			if constexpr (Op == VuX86Op::Msub)
				return VuX86Arith<VuX86Op::Sub>(acc, p);
			else if constexpr (!ToAcc)
				return VuX86Arith<VuX86Op::Add>(p, acc);
			else
				return VuX86Arith<VuX86Op::Add>(acc, p);
		}
		else
			return VuX86Arith<Op>(fs, ft);
	}

	inline u32 VuX86Mac(u32& macflag, int lane, u32 v)
	{
		const int shift = 3 - lane;
		macflag = (macflag & ~(0x1111 << shift)) | ((v >> 31) << (4 + shift)) |
				  ((v & 0x7fffffff) == 0 ? 1u << shift : 0);
		return v;
	}

	// The lanes the dest field of code selects; the MAC flags of the others cleared.
	template <VuX86Op Op, bool ToAcc>
	inline void VuX86Apply(u32 code, u32& macflag, VECTOR* dst, const VECTOR& acc, const VECTOR& fs, const VECTOR& ft)
	{
		for (int lane = 0; lane < 4; lane++)
		{
			if ((code >> (24 - lane)) & 1)
				dst->UL[lane] = VuX86Mac(macflag, lane, VuX86Lane<Op, ToAcc>(acc.UL[lane], fs.UL[lane], ft.UL[lane]));
			else
				macflag &= ~(0x1111 << (3 - lane));
		}
	}

	// OPMULA/OPMSUB: no operand clamp, ACC.xyz (+)= Fs.yzx * Ft.zxy.
	template <VuX86Op Op>
	inline void VuX86Outer(u32& macflag, VECTOR* dst, const VECTOR& acc, const VECTOR& fs, const VECTOR& ft)
	{
		const VECTOR xa = VuX86In(acc, false), xs = VuX86In(fs, false), xt = VuX86In(ft, false);
		const u32 x = VuX86Lane<Op, false>(xa.i.x, xs.i.y, xt.i.z);
		const u32 y = VuX86Lane<Op, false>(xa.i.y, xs.i.z, xt.i.x);
		const u32 z = VuX86Lane<Op, false>(xa.i.z, xs.i.x, xt.i.y);
		dst->i.x = VuX86Mac(macflag, 0, x);
		dst->i.y = VuX86Mac(macflag, 1, y);
		dst->i.z = VuX86Mac(macflag, 2, z);
	}

	// DIV/SQRT/RSQRT (x86/microVU_Lower.inl): the register bits unclamped, zero = exponent 0 (DAZ),
	// negative = sign bit (also -0), I/D as microVU sets them, the quotient clamped by MINSS/MAXSS
	// (a NaN becomes +max) unless Fs is VF0: microVU never clamps a register loaded from VF0, so a NaN
	// divisor passes quieted. Each returns Q and sets the I/D bits of status.
	inline bool VuFdivZero(u32 v)
	{
		return (v & 0x7f800000) == 0;
	}

	inline float VuFdivClamp(int vu, float r)
	{
		if (!CHECK_VU_OVERFLOW(vu))
			return r;
		const float max = std::bit_cast<float>(0x7f7fffffu), min = std::bit_cast<float>(0xff7fffffu);
		r = r < max ? r : max;
		return r > min ? r : min;
	}

	inline u32 VuDivX86(int vu, u32& status, u32 fs, u32 ft, bool fs_vf0)
	{
		status &= ~0x30;
		if (VuFdivZero(ft))
		{
			status |= VuFdivZero(fs) ? 0x10 : 0x20;
			return ((fs ^ ft) & 0x80000000) | 0x7f7fffff;
		}
		if (fs_vf0 && VuIsNaN(ft))
			return ft | 0x400000;
		return std::bit_cast<u32>(VuFdivClamp(vu, std::bit_cast<float>(fs) / std::bit_cast<float>(ft)));
	}

	inline u32 VuSqrtX86(int vu, u32& status, u32 ft)
	{
		status = (status & ~0x30) | (ft >> 31 ? 0x10 : 0);
		float t = std::bit_cast<float>(ft & 0x7fffffff);
		if (CHECK_VU_OVERFLOW(vu))
		{
			const float max = std::bit_cast<float>(0x7f7fffffu);
			t = t < max ? t : max;
		}
		return std::bit_cast<u32>(std::sqrt(t));
	}

	inline u32 VuRsqrtX86(int vu, u32& status, u32 fs, u32 ft, bool fs_vf0)
	{
		const float s = std::sqrt(std::bit_cast<float>(ft & 0x7fffffff));
		status &= ~0x30;
		if (VuFdivZero(std::bit_cast<u32>(s)))
		{
			status |= VuFdivZero(fs) ? 0x10 : 0x20;
			return (fs & 0x80000000) | 0x7f7fffff;
		}
		status |= ft >> 31 ? 0x10 : 0;
		if (fs_vf0 && VuIsNaN(ft))
			return (ft & 0x7fffffff) | 0x400000;
		return std::bit_cast<u32>(VuFdivClamp(vu, std::bit_cast<float>(fs) / s));
	}

	// EFU ops (x86/microVU_Lower.inl, default clamp mode): the register bits unclamped, the same
	// single precision ops in the same order, no zero checks, so ARM64 and x86 peers get the same P.
	// Each returns P; a NaN made here gets the x86 default NaN (sign set).
	inline float VuEfuF(u32 bits)
	{
		return std::bit_cast<float>(bits);
	}

	inline u32 VuEfuP(float p)
	{
		return std::isnan(p) && std::bit_cast<u32>(p) == 0x7fc00000 ? 0xffc00000 : std::bit_cast<u32>(p);
	}

	// a + b with the operands in this order (a NaN result depends on it, and the compiler may swap
	// the operands of a plain +), as the JIT's FADD.
	inline float VuEfuAdd(float a, float b)
	{
		float r;
		asm("fadd %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b));
		return r;
	}

	// A register operand with a NaN quieted. With two NaN operands SSE returns the first one, AArch64
	// a signaling one first; once both are quiet AArch64 also returns the first.
	inline float VuEfuR(u32 bits)
	{
		return VuEfuF((bits & 0x7fffffff) > 0x7f800000 ? bits | 0x400000 : bits);
	}

	// x*x + y*y + z*z as DPPS: (x*x + y*y) + (z*z + 0)
	inline float VuEfuSum3(const VECTOR& fs)
	{
		const float x = VuEfuF(fs.i.x), y = VuEfuF(fs.i.y), z = VuEfuF(fs.i.z);
		return VuEfuAdd(VuEfuAdd(x * x, y * y), z * z);
	}

	inline u32 VuEsaddX86(const VECTOR& fs) { return VuEfuP(VuEfuSum3(fs)); }
	inline u32 VuErsaddX86(const VECTOR& fs) { return VuEfuP(1.0f / VuEfuSum3(fs)); }
	inline u32 VuElengX86(const VECTOR& fs) { return VuEfuP(std::sqrt(VuEfuSum3(fs))); }
	inline u32 VuErlengX86(const VECTOR& fs) { return VuEfuP(1.0f / std::sqrt(VuEfuSum3(fs))); }
	inline u32 VuErcprX86(u32 f) { return VuEfuP(1.0f / VuEfuF(f)); }
	inline u32 VuEsqrtX86(u32 f) { return VuEfuP(std::sqrt(VuEfuF(f & 0x7fffffff))); }
	inline u32 VuErsqrtX86(u32 f) { return VuEfuP(1.0f / std::sqrt(VuEfuF(f & 0x7fffffff))); }

	inline u32 VuEsumX86(const VECTOR& fs)
	{
		const float x = VuEfuR(fs.i.x), y = VuEfuR(fs.i.y);
		const float z = VuEfuR(fs.i.z), w = VuEfuR(fs.i.w);
		return VuEfuP(VuEfuAdd(VuEfuAdd(x, w), VuEfuAdd(y, z)));
	}

	// atan(u) - pi/4 series on u = (a - b) / (a + b), then + pi/4.
	inline float VuEfuAtan(float u)
	{
		static constexpr u32 t[7] = {0xbeaaa61c, 0x3e4c40a6, 0xbe0e6c63, 0x3dc577df, 0xbd6501c4, 0x3cb31652, 0xbb84d7e7};
		float p = u * VuEfuF(0x3f7ffff5), un = u;
		for (const u32 c : t)
		{
			un = un * u;
			un = un * u;
			p = p + un * VuEfuF(c);
		}
		return p + VuEfuF(0x3f490fdb);
	}

	inline u32 VuEatanX86(u32 f)
	{
		const float x = VuEfuF(f);
		return VuEfuP(VuEfuAtan((x - 1.0f) / (x + 1.0f)));
	}

	inline u32 VuEatanxyX86(const VECTOR& fs)
	{
		const float x = VuEfuR(fs.i.x), y = VuEfuR(fs.i.y);
		return VuEfuP(VuEfuAtan((y - x) / VuEfuAdd(x, y)));
	}

	inline u32 VuEatanxzX86(const VECTOR& fs)
	{
		const float x = VuEfuR(fs.i.x), z = VuEfuR(fs.i.z);
		return VuEfuP(VuEfuAtan((z - x) / VuEfuAdd(x, z)));
	}

	inline u32 VuEsinX86(u32 f)
	{
		const float x = VuEfuF(f);
		const float x2 = x * x;
		float xn = x2 * x;
		float p = x + xn * VuEfuF(0xbe2aaaa4);
		for (const u32 c : {0x3c08873eu, 0xb94fb21fu, 0x362e9c14u})
		{
			xn = xn * x2;
			p = p + xn * VuEfuF(c);
		}
		return VuEfuP(p);
	}

	inline u32 VuEexpX86(u32 f)
	{
		const float x = VuEfuF(f);
		float p = x * VuEfuF(0x3e7fffa8) + 1.0f;
		float xn = x * x;
		p = p + xn * VuEfuF(0x3d0007f4);
		for (const u32 c : {0x3b29d3ffu, 0x3933e553u, 0x36b63510u, 0x353961acu})
		{
			xn = xn * x;
			p = p + xn * VuEfuF(c);
		}
		p = p * p;
		p = p * p;
		return VuEfuP(1.0f / p);
	}
#endif
} // namespace Zdxsv
