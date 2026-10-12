// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_EE_OPTEST=<file>[,<n>[,finite]] (docs/zdxsv/options.md): at vsync 300, each COP1 op below runs on <n>
// generated inputs as a guest loop through the EE core in use (x86 or ARM64 recompiler, interpreter). Interrupts
// are off while it runs; at the vsync after it ends, inputs and outputs go to <file> and the borrowed RAM and
// registers are restored. Two builds (x86_64 and ARM64) compare case by case (tests/zdxsv/ee_optest_cmp.py).

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/TestOptions.h"

#include "Common.h"
#include "R5900.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
	enum Form : u8
	{
		FD_FS_FT, // fd = f0, fs = f1, ft = f2
		FD_FS, // ABS, MOV, NEG, CVT
		FD_FT, // SQRT
		ACC_FS_FT, // ADDA, SUBA, MULA, MADDA, MSUBA: ACC read back by MADD.S f0, f4, f4 (ACC + 0 * 0)
		CMP, // C.cond: result in FCR31 bit 23
	};

	struct EeTestOp
	{
		const char* name;
		u8 fmt, func;
		Form form;
		bool reads_acc; // ACC set first: ADDA.S f3, f4 (f3 + 0)
	};

	constexpr u8 S = 0x10, W = 0x14;
	const EeTestOp s_ops[] = {
		{"ADD", S, 0x00, FD_FS_FT, false},
		{"SUB", S, 0x01, FD_FS_FT, false},
		{"MUL", S, 0x02, FD_FS_FT, false},
		{"DIV", S, 0x03, FD_FS_FT, false},
		{"SQRT", S, 0x04, FD_FT, false},
		{"ABS", S, 0x05, FD_FS, false},
		{"MOV", S, 0x06, FD_FS, false},
		{"NEG", S, 0x07, FD_FS, false},
		{"RSQRT", S, 0x16, FD_FS_FT, false},
		{"ADDA", S, 0x18, ACC_FS_FT, false},
		{"SUBA", S, 0x19, ACC_FS_FT, false},
		{"MULA", S, 0x1a, ACC_FS_FT, false},
		{"MADD", S, 0x1c, FD_FS_FT, true},
		{"MSUB", S, 0x1d, FD_FS_FT, true},
		{"MADDA", S, 0x1e, ACC_FS_FT, true},
		{"MSUBA", S, 0x1f, ACC_FS_FT, true},
		{"CVT.W", S, 0x24, FD_FS, false},
		{"MAX", S, 0x28, FD_FS_FT, false},
		{"MIN", S, 0x29, FD_FS_FT, false},
		{"C.F", S, 0x30, CMP, false},
		{"C.EQ", S, 0x32, CMP, false},
		{"C.LT", S, 0x34, CMP, false},
		{"C.LE", S, 0x36, CMP, false},
		{"CVT.S", W, 0x20, FD_FS, false},
	};
	constexpr u32 NUM_OPS = std::size(s_ops);

	// Borrowed EE RAM (the last MB): code, inputs (4 words a case: fs, ft, ACC, f0), outputs (f0, FCR31).
	constexpr u32 BASE = 0x01f00000, SIZE = 0x100000, IN = BASE + 0x1000, OUT = BASE + 0x11000;
	constexpr u32 MAX_CASES = 4096;
	static_assert(OUT + NUM_OPS * MAX_CASES * 8 <= BASE + SIZE);

	constexpr u32 T0 = 8, T1 = 9, T2 = 10, T3 = 11;
	constexpr u32 I(u32 op, u32 rs, u32 rt, u32 imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff); }
	constexpr u32 COP1(u32 fmt, u32 ft, u32 fs, u32 fd, u32 func) { return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | func; }
	constexpr u32 CTC1(u32 rt, u32 fs) { return COP1(0x06, rt, fs, 0, 0); }
	constexpr u32 CFC1(u32 rt, u32 fs) { return COP1(0x02, rt, fs, 0, 0); }
	constexpr u32 MTC1(u32 rt, u32 fs) { return COP1(0x04, rt, fs, 0, 0); }

	u32 s_rng = 0x12345678;
	u32 Next()
	{
		s_rng ^= s_rng << 13;
		s_rng ^= s_rng >> 17;
		s_rng ^= s_rng << 5;
		return s_rng;
	}

	bool s_finite = false;

	// Float bit patterns weighted to the edges: denormals, Inf/NaN exponents, zeros, extremes (as VuOpTest.cpp).
	u32 Value()
	{
		const u32 r = Next(), m = Next();
		u32 v;
		switch (r & 7)
		{
			case 0: v = m & 0x807fffff; break;
			case 1: v = m | 0x7f800000; break;
			case 2: v = m & 0x80000000; break;
			case 3: v = (m & 0x807fffff) | ((r >> 3 & 1) ? 0x7f000000 : 0x00800000); break;
			case 4: v = (m & 0x807fffff) | 0x3f800000; break;
			default: v = m; break;
		}
		if (!s_finite)
			return v;
		const u32 e = v & 0x7f800000;
		return e == 0x7f800000 ? v & ~0x00800000 : e == 0 ? v & 0x80000000 : v;
	}

	std::vector<u32> BuildProgram(u32 count)
	{
		std::vector<u32> p;
		for (u32 k = 0; k < NUM_OPS; k++)
		{
			const EeTestOp& op = s_ops[k];
			const u32 out = OUT + k * count * 8;
			p.push_back(I(0x0f, 0, T0, IN >> 16)); // lui t0
			p.push_back(I(0x0d, T0, T0, IN)); // ori t0
			p.push_back(I(0x0f, 0, T1, out >> 16)); // lui t1
			p.push_back(I(0x0d, T1, T1, out)); // ori t1
			p.push_back(I(0x0d, 0, T2, count)); // ori t2, zero, count
			p.push_back(MTC1(0, 4)); // f4 = +0
			const size_t loop = p.size();
			for (u32 r = 0; r < 4; r++)
				p.push_back(I(0x31, T0, r == 3 ? 0 : r + 1, r * 4)); // lwc1 f1, f2, f3, f0
			if (op.reads_acc)
				p.push_back(COP1(S, 4, 3, 0, 0x18)); // adda.s f3, f4
			p.push_back(CTC1(0, 31));
			switch (op.form)
			{
				case FD_FS_FT: p.push_back(COP1(op.fmt, 2, 1, 0, op.func)); break;
				case FD_FS: p.push_back(COP1(op.fmt, 0, 1, 0, op.func)); break;
				case FD_FT: p.push_back(COP1(op.fmt, 2, 0, 0, op.func)); break;
				case ACC_FS_FT: p.push_back(COP1(op.fmt, 2, 1, 0, op.func)); break;
				case CMP: p.push_back(COP1(op.fmt, 2, 1, 0, op.func)); break;
			}
			p.push_back(CFC1(T3, 31));
			if (op.form == ACC_FS_FT)
				p.push_back(COP1(S, 4, 4, 0, 0x1c)); // madd.s f0, f4, f4
			p.push_back(I(0x39, T1, 0, 0)); // swc1 f0, 0(t1)
			p.push_back(I(0x2b, T1, T3, 4)); // sw t3, 4(t1)
			p.push_back(I(0x09, T0, T0, 16)); // addiu t0, 16
			p.push_back(I(0x09, T2, T2, 0xffff)); // addiu t2, -1
			p.push_back(I(0x05, T2, 0, static_cast<u32>(loop - (p.size() + 1)))); // bne t2, zero, loop
			p.push_back(I(0x09, T1, T1, 8)); // addiu t1, 8 (delay slot)
		}
		p.push_back(I(0x04, 0, 0, 0xffff)); // end: beq zero, zero, end
		p.push_back(0); // nop
		return p;
	}

	enum class State
	{
		Off,
		Waiting,
		Running,
		Done,
	};
	State s_state = State::Waiting;
	std::string s_path;
	u32 s_count = 2048;
	u32 s_vsync = 0;
	u32 s_end_pc = 0;
	std::vector<u8> s_ram;
	std::vector<u32> s_in;
	GPR_reg s_gpr[4];
	std::unique_ptr<fpuRegisters> s_fpu;
	u32 s_status = 0, s_pc = 0;

	void Start()
	{
		std::vector<u32> prog = BuildProgram(s_count);
		s_ram.assign(eeMem->Main + BASE, eeMem->Main + BASE + SIZE);
		std::memcpy(s_gpr, &cpuRegs.GPR.r[T0], sizeof(s_gpr));
		s_fpu = std::make_unique<fpuRegisters>(fpuRegs);
		s_status = cpuRegs.CP0.n.Status.val;
		s_pc = cpuRegs.pc;

		s_in.resize(s_count * 4);
		for (u32& v : s_in)
			v = Value();
		std::memset(eeMem->Main + BASE, 0, SIZE);
		std::memcpy(eeMem->Main + BASE, prog.data(), prog.size() * 4);
		std::memcpy(eeMem->Main + IN, s_in.data(), s_in.size() * 4);
		Cpu->Clear(BASE, SIZE / 4);
		s_end_pc = BASE + static_cast<u32>(prog.size() - 2) * 4;
		// COP1 usable, interrupts off (IE, EIE).
		cpuRegs.CP0.n.Status.val = (s_status | 0x20000000) & ~0x10001u;
		cpuRegs.pc = BASE;
		Console.WriteLn("EE optest: %u ops x %u cases started at vsync %u, pc %08x", NUM_OPS, s_count, s_vsync, s_pc);
	}

	void Finish()
	{
		std::FILE* fp = std::fopen(s_path.c_str(), "wb");
		if (!fp)
			Console.Error("EE optest: cannot write %s", s_path.c_str());
		const bool in_ok = std::memcmp(eeMem->Main + IN, s_in.data(), s_in.size() * 4) == 0;
		for (u32 k = 0; k < NUM_OPS; k++)
		{
			const u32* out = reinterpret_cast<const u32*>(eeMem->Main + OUT + k * s_count * 8);
			u32 hash = 0x811c9dc5;
			for (u32 i = 0; i < s_count; i++)
			{
				hash = (hash ^ out[i * 2]) * 0x01000193;
				hash = (hash ^ out[i * 2 + 1]) * 0x01000193;
				if (fp)
				{
					std::fwrite(&s_in[i * 4], 16, 1, fp);
					std::fwrite(&out[i * 2], 8, 1, fp);
				}
			}
			Console.WriteLn("EE optest: %s %u cases, hash %08x", s_ops[k].name, s_count, hash);
		}
		if (fp)
			std::fclose(fp);
		Console.WriteLn("EE optest: done at vsync %u, inputs %s", s_vsync, in_ok ? "intact" : "CHANGED (DMA into the borrowed RAM?)");

		std::memcpy(eeMem->Main + BASE, s_ram.data(), SIZE);
		Cpu->Clear(BASE, SIZE / 4);
		std::memcpy(&cpuRegs.GPR.r[T0], s_gpr, sizeof(s_gpr));
		fpuRegs = *s_fpu;
		cpuRegs.CP0.n.Status.val = s_status;
		cpuRegs.pc = s_pc;
	}
} // namespace

void Zdxsv::EeOpTestOnVsync()
{
	switch (s_state)
	{
		case State::Waiting:
		{
			const char* env = TestEnv("ZDXSV_EE_OPTEST");
			if (!env || !*env)
			{
				s_state = State::Off;
				return;
			}
			if (++s_vsync < 300)
				return;
			s_path = env;
			if (const size_t comma = s_path.find(','); comma != std::string::npos)
			{
				s_count = std::min<u32>(std::strtoul(s_path.c_str() + comma + 1, nullptr, 10), MAX_CASES);
				s_finite = s_path.find(",finite", comma) != std::string::npos;
				s_path.resize(comma);
			}
			Start();
			s_state = State::Running;
			return;
		}
		case State::Running:
			s_vsync++;
			if (cpuRegs.pc - s_end_pc < 8)
			{
				Finish();
				s_state = State::Done;
			}
			return;
		default:
			return;
	}
}
