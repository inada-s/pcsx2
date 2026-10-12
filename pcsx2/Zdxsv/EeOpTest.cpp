// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_EE_OPTEST=<file>[,<n>[,finite]] (docs/zdxsv/options.md): from vsync 300, each COP1 op and COP2 macro op
// below runs on <n> generated inputs as a guest loop through the EE core in use (x86 or ARM64 recompiler,
// interpreter), one op per vsync. Interrupts are off meanwhile; after the last op, inputs and outputs go to
// <file> and the borrowed RAM and registers are restored. Two builds (x86_64 and ARM64) compare case by case
// (tests/zdxsv/ee_optest_cmp.py).

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/TestOptions.h"

#include "Common.h"
#include "R5900.h"
#include "VUmicro.h"

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
		V_FD, // COP2: fd = VF3, fs = VF1, ft = VF2
		V_ACC, // COP2, writes ACC: read back by VMADDx VF3, VF0, VF0 (ACC + 0)
		V_Q, // COP2 DIV, SQRT, RSQRT: VWAITQ, result in Q
	};

	struct EeTestOp
	{
		const char* name;
		u8 fmt, func;
		Form form;
		bool reads_acc; // ACC set first: ADDA.S f3, f4 (f3 + 0)
		u32 cop2[2]; // COP2 macro op words (second one 0 if none)
	};

	// Macro op from a VU upper/lower op word (as VuOpTest.cpp builds them): COP2, CO bit, then its low 25 bits.
	constexpr u32 V(u32 vu) { return 0x4a000000 | (vu & 0x01ffffff); }
	constexpr u32 U(u32 fn, u32 fd = 3, u32 dest = 0xf, u32 fs = 1, u32 ft = 2) { return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn; }
	constexpr u32 US(u32 t, u32 idx, u32 dest = 0xf) { return U(0x3c + t, idx, dest); }
	constexpr u32 L(u32 t, u32 idx, u32 dest) { return (dest << 21) | (2 << 16) | (1 << 11) | (idx << 6) | (0x3c + t); }

	constexpr u8 S = 0x10, W = 0x14, C2 = 0;
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
		{"VDIV", C2, 0, V_Q, false, {V(L(0, 0x0e, 0x2))}},
		{"VSQRT", C2, 0, V_Q, false, {V(L(1, 0x0e, 0x2))}},
		{"VRSQRT", C2, 0, V_Q, false, {V(L(2, 0x0e, 0x2))}},
		{"VADD", C2, 0, V_FD, false, {V(U(0x28))}},
		{"VSUB", C2, 0, V_FD, false, {V(U(0x2c))}},
		{"VMUL", C2, 0, V_FD, false, {V(U(0x2a))}},
		{"VMADD", C2, 0, V_FD, false, {V(U(0x29))}},
		{"VMSUB", C2, 0, V_FD, false, {V(U(0x2d))}},
		{"VMAX", C2, 0, V_FD, false, {V(U(0x2b))}},
		{"VMINI", C2, 0, V_FD, false, {V(U(0x2f))}},
		{"VADDx", C2, 0, V_FD, false, {V(U(0x00))}},
		{"VMADDy", C2, 0, V_FD, false, {V(U(0x09))}},
		{"VMSUBw", C2, 0, V_FD, false, {V(U(0x0f))}},
		{"VMULq", C2, 0, V_FD, false, {V(U(0x1c))}},
		{"VMAXi", C2, 0, V_FD, false, {V(U(0x1d))}},
		{"VMULi", C2, 0, V_FD, false, {V(U(0x1e))}},
		{"VMINIi", C2, 0, V_FD, false, {V(U(0x1f))}},
		{"VADDq", C2, 0, V_FD, false, {V(U(0x20))}},
		{"VADDi", C2, 0, V_FD, false, {V(U(0x22))}},
		{"VMADDi", C2, 0, V_FD, false, {V(U(0x23))}},
		{"VSUBi", C2, 0, V_FD, false, {V(U(0x26))}},
		{"VADDA", C2, 0, V_ACC, false, {V(US(0, 0x0a))}},
		{"VMADDA", C2, 0, V_ACC, false, {V(US(1, 0x0a))}},
		{"VMULA", C2, 0, V_ACC, false, {V(US(2, 0x0a))}},
		{"VMSUBA", C2, 0, V_ACC, false, {V(US(1, 0x0b))}},
		{"VOPMULA+VOPMSUB", C2, 0, V_FD, false, {V(US(2, 0x0b, 0xe)), V(U(0x2e, 3, 0xe))}},
		{"VITOF0", C2, 0, V_FD, false, {V(US(0, 0x04))}},
		{"VITOF4", C2, 0, V_FD, false, {V(US(1, 0x04))}},
		{"VITOF12", C2, 0, V_FD, false, {V(US(2, 0x04))}},
		{"VITOF15", C2, 0, V_FD, false, {V(US(3, 0x04))}},
		{"VFTOI0", C2, 0, V_FD, false, {V(US(0, 0x05))}},
		{"VFTOI4", C2, 0, V_FD, false, {V(US(1, 0x05))}},
		{"VFTOI12", C2, 0, V_FD, false, {V(US(2, 0x05))}},
		{"VFTOI15", C2, 0, V_FD, false, {V(US(3, 0x05))}},
		{"VABS", C2, 0, V_FD, false, {V(US(1, 0x07))}},
		{"VCLIP", C2, 0, V_FD, false, {V(US(3, 0x07, 0xe))}},
	};
	constexpr u32 NUM_OPS = std::size(s_ops);

	// A case: COP1 reads words 0-3 (fs, ft, ACC, f0); COP2 reads VF1, VF2, ACC, VF3 (words 0-15), I, Q.
	constexpr u32 IN_WORDS = 20;
	// Output of a case: COP1 f0, FCR31; COP2 VF3, status, MAC, clip, Q.
	constexpr u32 OutWords(const EeTestOp& op) { return op.fmt == C2 ? 8 : 2; }
	constexpr u32 FileInWords(const EeTestOp& op) { return op.fmt == C2 ? 18 : 4; }

	// Borrowed EE RAM (the last MB): code, inputs, the outputs of the op that runs.
	constexpr u32 BASE = 0x01f00000, SIZE = 0x100000, IN = BASE + 0x1000, OUT = BASE + 0x51000;
	constexpr u32 MAX_CASES = 4096;
	static_assert(IN + MAX_CASES * IN_WORDS * 4 <= OUT && OUT + MAX_CASES * 8 * 4 <= BASE + SIZE);

	constexpr u32 T0 = 8, T1 = 9, T2 = 10, T3 = 11;
	constexpr u32 I(u32 op, u32 rs, u32 rt, u32 imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff); }
	constexpr u32 COP1(u32 fmt, u32 ft, u32 fs, u32 fd, u32 func) { return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | func; }
	constexpr u32 CTC1(u32 rt, u32 fs) { return COP1(0x06, rt, fs, 0, 0); }
	constexpr u32 CFC1(u32 rt, u32 fs) { return COP1(0x02, rt, fs, 0, 0); }
	constexpr u32 MTC1(u32 rt, u32 fs) { return COP1(0x04, rt, fs, 0, 0); }
	constexpr u32 CFC2(u32 rt, u32 vi) { return 0x48400000 | (rt << 16) | (vi << 11); }
	constexpr u32 CTC2(u32 rt, u32 vi) { return 0x48c00000 | (rt << 16) | (vi << 11); }
	constexpr u32 VMULAW_ACC_VF4 = V(U(0x3f, 0x06, 0xf, 4, 0)); // ACC = VF4 * VF0w (1.0)
	constexpr u32 VADDW_X_VF5 = V(U(0x03, 5, 0x8, 0, 0)); // VF5x = 0 + 1: MAC and status 0
	constexpr u32 VMADDX_VF3_ACC = V(U(0x08, 3, 0xf, 0, 0)); // VF3 = ACC + VF0 * 0
	constexpr u32 VWAITQ = V(L(3, 0x0e, 0) & ~0x001ff800u);

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

	std::vector<u32> BuildCop1(const EeTestOp& op, u32 count)
	{
		std::vector<u32> p;
		p.push_back(I(0x0f, 0, T0, IN >> 16)); // lui t0
		p.push_back(I(0x0d, T0, T0, IN)); // ori t0
		p.push_back(I(0x0f, 0, T1, OUT >> 16)); // lui t1
		p.push_back(I(0x0d, T1, T1, OUT)); // ori t1
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
			case FD_FT: p.push_back(COP1(op.fmt, 2, 0, 0, op.func)); break;
			case FD_FS: p.push_back(COP1(op.fmt, 0, 1, 0, op.func)); break;
			default: p.push_back(COP1(op.fmt, 2, 1, 0, op.func)); break;
		}
		p.push_back(CFC1(T3, 31));
		if (op.form == ACC_FS_FT)
			p.push_back(COP1(S, 4, 4, 0, 0x1c)); // madd.s f0, f4, f4
		p.push_back(I(0x39, T1, 0, 0)); // swc1 f0, 0(t1)
		p.push_back(I(0x2b, T1, T3, 4)); // sw t3, 4(t1)
		p.push_back(I(0x09, T0, T0, IN_WORDS * 4)); // addiu t0
		p.push_back(I(0x09, T2, T2, 0xffff)); // addiu t2, -1
		p.push_back(I(0x05, T2, 0, static_cast<u32>(loop - (p.size() + 1)))); // bne t2, zero, loop
		p.push_back(I(0x09, T1, T1, 8)); // addiu t1, 8 (delay slot)
		return p;
	}

	std::vector<u32> BuildCop2(const EeTestOp& op, u32 count)
	{
		std::vector<u32> p;
		p.push_back(I(0x0f, 0, T0, IN >> 16)); // lui t0
		p.push_back(I(0x0d, T0, T0, IN)); // ori t0
		p.push_back(I(0x0f, 0, T1, OUT >> 16)); // lui t1
		p.push_back(I(0x0d, T1, T1, OUT)); // ori t1
		p.push_back(I(0x0d, 0, T2, count)); // ori t2, zero, count
		const size_t loop = p.size();
		for (u32 r = 0; r < 4; r++)
			p.push_back(I(0x36, T0, r == 2 ? 4 : r == 3 ? 3 : r + 1, r * 16)); // lqc2 vf1, vf2, vf4, vf3
		p.push_back(I(0x23, T0, T3, 64)); // lw t3, 64(t0)
		p.push_back(CTC2(T3, 21)); // I
		p.push_back(I(0x23, T0, T3, 68)); // lw t3, 68(t0)
		p.push_back(CTC2(T3, 22)); // Q
		p.push_back(VMULAW_ACC_VF4);
		p.push_back(VADDW_X_VF5);
		p.push_back(CTC2(0, 16)); // status
		p.push_back(CTC2(0, 18)); // clip
		for (u32 w : op.cop2)
			if (w)
				p.push_back(w);
		if (op.form == V_Q)
			p.push_back(VWAITQ);
		for (u32 r = 0; r < 4; r++)
		{
			p.push_back(CFC2(T3, r == 3 ? 22 : 16 + r)); // status, MAC, clip, Q
			p.push_back(I(0x2b, T1, T3, 16 + r * 4)); // sw t3
		}
		if (op.form == V_ACC)
			p.push_back(VMADDX_VF3_ACC);
		p.push_back(I(0x3e, T1, 3, 0)); // sqc2 vf3, 0(t1)
		p.push_back(I(0x09, T0, T0, IN_WORDS * 4)); // addiu t0
		p.push_back(I(0x09, T2, T2, 0xffff)); // addiu t2, -1
		p.push_back(I(0x05, T2, 0, static_cast<u32>(loop - (p.size() + 1)))); // bne t2, zero, loop
		p.push_back(I(0x09, T1, T1, 32)); // addiu t1, 32 (delay slot)
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
	u32 s_op = 0;
	u32 s_end_pc = 0;
	std::vector<u8> s_ram;
	std::vector<u32> s_in;
	std::vector<std::vector<u32>> s_out;
	GPR_reg s_gpr[4];
	std::unique_ptr<fpuRegisters> s_fpu;
	struct Vu0Saved
	{
		VECTOR VF[32];
		REG_VI VI[32];
		VECTOR ACC;
		REG_VI q, p;
		u32 macflag, statusflag, clipflag;
	} s_vu0;
	u32 s_status = 0, s_pc = 0;

	void LoadOp(u32 k)
	{
		const EeTestOp& op = s_ops[k];
		std::vector<u32> prog = op.fmt == C2 ? BuildCop2(op, s_count) : BuildCop1(op, s_count);
		prog.push_back(I(0x04, 0, 0, 0xffff)); // end: beq zero, zero, end
		prog.push_back(0); // nop
		std::memset(eeMem->Main + BASE, 0, IN - BASE);
		std::memcpy(eeMem->Main + BASE, prog.data(), prog.size() * 4);
		Cpu->Clear(BASE, (IN - BASE) / 4);
		s_end_pc = BASE + static_cast<u32>(prog.size() - 2) * 4;
		cpuRegs.pc = BASE;
	}

	void Start()
	{
		s_ram.assign(eeMem->Main + BASE, eeMem->Main + BASE + SIZE);
		std::memcpy(s_gpr, &cpuRegs.GPR.r[T0], sizeof(s_gpr));
		s_fpu = std::make_unique<fpuRegisters>(fpuRegs);
		std::memcpy(s_vu0.VF, VU0.VF, sizeof(s_vu0.VF));
		std::memcpy(s_vu0.VI, VU0.VI, sizeof(s_vu0.VI));
		s_vu0.ACC = VU0.ACC;
		s_vu0.q = VU0.q;
		s_vu0.p = VU0.p;
		s_vu0.macflag = VU0.macflag;
		s_vu0.statusflag = VU0.statusflag;
		s_vu0.clipflag = VU0.clipflag;
		s_status = cpuRegs.CP0.n.Status.val;
		s_pc = cpuRegs.pc;

		s_in.resize(s_count * IN_WORDS);
		for (u32& v : s_in)
			v = Value();
		s_out.assign(NUM_OPS, {});
		std::memset(eeMem->Main + BASE, 0, SIZE);
		std::memcpy(eeMem->Main + IN, s_in.data(), s_in.size() * 4);
		// COP1 and COP2 usable, interrupts off (IE, EIE).
		cpuRegs.CP0.n.Status.val = (s_status | 0x60000000) & ~0x10001u;
		s_op = 0;
		LoadOp(0);
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
			const u32 ow = OutWords(s_ops[k]), iw = FileInWords(s_ops[k]);
			const u32* out = s_out[k].data();
			u32 hash = 0x811c9dc5;
			for (u32 i = 0; i < s_count; i++)
			{
				for (u32 j = 0; j < ow; j++)
					hash = (hash ^ out[i * ow + j]) * 0x01000193;
				if (fp)
				{
					std::fwrite(&s_in[i * IN_WORDS], iw * 4, 1, fp);
					std::fwrite(&out[i * ow], ow * 4, 1, fp);
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
		std::memcpy(VU0.VF, s_vu0.VF, sizeof(s_vu0.VF));
		std::memcpy(VU0.VI, s_vu0.VI, sizeof(s_vu0.VI));
		VU0.ACC = s_vu0.ACC;
		VU0.q = s_vu0.q;
		VU0.p = s_vu0.p;
		VU0.macflag = s_vu0.macflag;
		VU0.statusflag = s_vu0.statusflag;
		VU0.clipflag = s_vu0.clipflag;
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
				const u32* out = reinterpret_cast<const u32*>(eeMem->Main + OUT);
				s_out[s_op].assign(out, out + s_count * OutWords(s_ops[s_op]));
				if (++s_op < NUM_OPS)
					return LoadOp(s_op);
				Finish();
				s_state = State::Done;
			}
			return;
		default:
			return;
	}
}
