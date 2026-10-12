// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// ZDXSV_VU1_OPTEST=<file>[,<n>[,finite]] (docs/zdxsv/options.md): at the first VU1 micro program start, each
// op below runs as a short micro program on <n> generated inputs through the VU1 core in use
// (x86 microVU, ARM64 traces, interpreter). Inputs and outputs go to <file>, so two builds
// (x86_64 and ARM64) can be compared case by case (tests/zdxsv/vu1_optest_cmp.py).

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/TestOptions.h"

#include "Common.h"
#include "VUmicro.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
	struct VuTestOp
	{
		const char* name;
		u32 upper0, lower0, upper1, lower1;
	};

	constexpr u32 NOP_U = 0x000002ff, NOP_L = 0x8000033c, EBIT = 0x40000000;
	// Lower special op <idx> of table T3_<t>; fs = VF1, ft = VF2; dest = the dest bits or ftf << 2 | fsf.
	constexpr u32 L(u32 t, u32 idx, u32 dest) { return 0x80000000 | (dest << 21) | (2 << 16) | (1 << 11) | (idx << 6) | (0x3c + t); }
	// Upper op <fn>, dest xyzw, fs = VF1, ft = VF2, fd = VF3; special forms: fn = 0x3c + t, fd = idx.
	constexpr u32 U(u32 fn, u32 fd = 3, u32 dest = 0xf) { return (dest << 21) | (2 << 16) | (1 << 11) | (fd << 6) | fn; }
	constexpr u32 US(u32 t, u32 idx, u32 dest = 0xf) { return U(0x3c + t, idx, dest); }

	const VuTestOp s_ops[] = {
		{"DIV", NOP_U, L(0, 0x0e, 0x2), NOP_U, NOP_L},
		{"SQRT", NOP_U, L(1, 0x0e, 0x2), NOP_U, NOP_L},
		{"RSQRT", NOP_U, L(2, 0x0e, 0x2), NOP_U, NOP_L},
		{"ESADD", NOP_U, L(0, 0x1c, 0xe), NOP_U, NOP_L},
		{"ERSADD", NOP_U, L(1, 0x1c, 0xe), NOP_U, NOP_L},
		{"ELENG", NOP_U, L(2, 0x1c, 0xe), NOP_U, NOP_L},
		{"ERLENG", NOP_U, L(3, 0x1c, 0xe), NOP_U, NOP_L},
		{"EATANxy", NOP_U, L(0, 0x1d, 0xc), NOP_U, NOP_L},
		{"EATANxz", NOP_U, L(1, 0x1d, 0xa), NOP_U, NOP_L},
		{"ESUM", NOP_U, L(2, 0x1d, 0xf), NOP_U, NOP_L},
		{"ESQRT", NOP_U, L(0, 0x1e, 0x0), NOP_U, NOP_L},
		{"ERSQRT", NOP_U, L(1, 0x1e, 0x0), NOP_U, NOP_L},
		{"ERCPR", NOP_U, L(2, 0x1e, 0x0), NOP_U, NOP_L},
		{"ESIN", NOP_U, L(0, 0x1f, 0x0), NOP_U, NOP_L},
		{"EATAN", NOP_U, L(1, 0x1f, 0x0), NOP_U, NOP_L},
		{"EEXP", NOP_U, L(2, 0x1f, 0x0), NOP_U, NOP_L},
		{"ADD", U(0x28), NOP_L, NOP_U, NOP_L},
		{"SUB", U(0x2c), NOP_L, NOP_U, NOP_L},
		{"MUL", U(0x2a), NOP_L, NOP_U, NOP_L},
		{"MADD", U(0x29), NOP_L, NOP_U, NOP_L},
		{"MSUB", U(0x2d), NOP_L, NOP_U, NOP_L},
		{"MAX", U(0x2b), NOP_L, NOP_U, NOP_L},
		{"MINI", U(0x2f), NOP_L, NOP_U, NOP_L},
		{"ADDx", U(0x00), NOP_L, NOP_U, NOP_L},
		{"MADDy", U(0x09), NOP_L, NOP_U, NOP_L},
		{"MSUBw", U(0x0f), NOP_L, NOP_U, NOP_L},
		{"MULq", U(0x1c), NOP_L, NOP_U, NOP_L},
		{"MAXi", U(0x1d), NOP_L, NOP_U, NOP_L},
		{"MULi", U(0x1e), NOP_L, NOP_U, NOP_L},
		{"MINIi", U(0x1f), NOP_L, NOP_U, NOP_L},
		{"ADDq", U(0x20), NOP_L, NOP_U, NOP_L},
		{"ADDi", U(0x22), NOP_L, NOP_U, NOP_L},
		{"MADDi", U(0x23), NOP_L, NOP_U, NOP_L},
		{"SUBi", U(0x26), NOP_L, NOP_U, NOP_L},
		{"ADDA", US(0, 0x0a), NOP_L, NOP_U, NOP_L},
		{"MADDA", US(1, 0x0a), NOP_L, NOP_U, NOP_L},
		{"MULA", US(2, 0x0a), NOP_L, NOP_U, NOP_L},
		{"MSUBA", US(1, 0x0b), NOP_L, NOP_U, NOP_L},
		{"OPMULA+OPMSUB", US(2, 0x0b, 0xe), NOP_L, U(0x2e, 3, 0xe), NOP_L},
		{"ITOF0", US(0, 0x04), NOP_L, NOP_U, NOP_L},
		{"ITOF4", US(1, 0x04), NOP_L, NOP_U, NOP_L},
		{"ITOF12", US(2, 0x04), NOP_L, NOP_U, NOP_L},
		{"ITOF15", US(3, 0x04), NOP_L, NOP_U, NOP_L},
		{"FTOI0", US(0, 0x05), NOP_L, NOP_U, NOP_L},
		{"FTOI4", US(1, 0x05), NOP_L, NOP_U, NOP_L},
		{"FTOI12", US(2, 0x05), NOP_L, NOP_U, NOP_L},
		{"FTOI15", US(3, 0x05), NOP_L, NOP_U, NOP_L},
		{"ABS", US(1, 0x07), NOP_L, NOP_U, NOP_L},
		{"CLIP", US(3, 0x07, 0xe), NOP_L, NOP_U, NOP_L},
	};

	u32 s_rng = 0x12345678;
	u32 Next()
	{
		s_rng ^= s_rng << 13;
		s_rng ^= s_rng >> 17;
		s_rng ^= s_rng << 5;
		return s_rng;
	}

	bool s_finite = false;

	// Float bit patterns weighted to the edges: denormals, Inf/NaN exponents, zeros, extremes.
	u32 Edge()
	{
		const u32 r = Next(), m = Next();
		switch (r & 7)
		{
			case 0: return m & 0x807fffff;
			case 1: return m | 0x7f800000;
			case 2: return m & 0x80000000;
			case 3: return (m & 0x807fffff) | ((r >> 3 & 1) ? 0x7f000000 : 0x00800000);
			case 4: return (m & 0x807fffff) | 0x3f800000;
			default: return m;
		}
	}

	// finite: exponent 255 as 254, denormals as zero (values a PS2 FPU result can hold).
	u32 Value()
	{
		const u32 v = Edge();
		if (!s_finite)
			return v;
		const u32 e = v & 0x7f800000;
		return e == 0x7f800000 ? v & ~0x00800000 : e == 0 ? v & 0x80000000 : v;
	}

	void RunProgram()
	{
		VU1.VI[REG_TPC].UL = 0;
		VU0.VI[REG_VPU_STAT].UL |= 0x100;
		CpuVU1->SetStartPC(0);
		for (int i = 0; i < 16 && (VU0.VI[REG_VPU_STAT].UL & 0x100); i++)
			CpuVU1->Execute(vu1RunCycles);
	}
} // namespace

void Zdxsv::VuOpTest()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	const char* env = TestEnv("ZDXSV_VU1_OPTEST");
	if (!env || !*env)
		return;
	std::string path = env;
	u32 count = 2048;
	if (const size_t comma = path.find(','); comma != std::string::npos)
	{
		count = std::strtoul(path.c_str() + comma + 1, nullptr, 10);
		s_finite = path.find(",finite", comma) != std::string::npos;
		path.resize(comma);
	}
	std::FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp)
	{
		Console.Error("VU1 optest: cannot write %s", path.c_str());
		return;
	}

	const std::unique_ptr<VURegs> saved = std::make_unique<VURegs>(VU1);
	const u32 vpu_stat = VU0.VI[REG_VPU_STAT].UL;
	std::vector<u8> micro(VU1.Micro, VU1.Micro + VU1_PROGSIZE), mem(VU1.Mem, VU1.Mem + VU1_MEMSIZE);

	for (const VuTestOp& op : s_ops)
	{
		const u32 prog[] = {op.lower0, op.upper0, op.lower1, op.upper1, 0x800003bf /* WAITQ */, NOP_U,
			0x800007bf /* WAITP */, NOP_U, 0x81e8067c /* MFP.xyzw VF8 */, NOP_U, NOP_L, NOP_U,
			0x2c2107ff /* FSAND VI1, 0xfff */, NOP_U, 0x36020000 /* FMOR VI2, VI0 */, NOP_U, 0x38030000 /* FCGET VI3 */,
			NOP_U | EBIT, NOP_L, NOP_U};
		std::memset(VU1.Micro, 0, VU1_PROGSIZE);
		std::memcpy(VU1.Micro, prog, sizeof(prog));
		CpuVU1->Clear(0, VU1_PROGSIZE);
		u32 hash = 0x811c9dc5;
		for (u32 i = 0; i < count; i++)
		{
			// in: VF1..VF7, ACC, I, Q, P; out: VF1..VF8, ACC, Q, P, VI1..VI3 = status, MAC, clip as
			// the program reads them (microVU keeps no flags in VI[REG_STATUS_FLAG..] at exit).
			u32 in[35], out[41];
			for (u32& v : in)
				v = Value();
			std::memcpy(&VU1.VF[1], in, 7 * 16);
			std::memcpy(&VU1.ACC, &in[28], 16);
			VU1.VF[8] = {};
			VU1.VI[REG_I].UL = in[32];
			VU1.VI[REG_Q].UL = in[33];
			VU1.VI[REG_P].UL = in[34];
			VU1.q.UL = in[33];
			VU1.p.UL = in[34];
			VU1.VI[REG_STATUS_FLAG].UL = VU1.VI[REG_MAC_FLAG].UL = VU1.VI[REG_CLIP_FLAG].UL = 0;
			VU1.statusflag = VU1.macflag = VU1.clipflag = 0;
			VU1.VI[1].UL = VU1.VI[2].UL = VU1.VI[3].UL = 0;
			VU1.cycle = cpuRegs.cycle;
			RunProgram();
			std::memcpy(out, &VU1.VF[1], 8 * 16);
			std::memcpy(&out[32], &VU1.ACC, 16);
			out[36] = VU1.VI[REG_Q].UL;
			out[37] = VU1.VI[REG_P].UL;
			out[38] = VU1.VI[1].UL;
			out[39] = VU1.VI[2].UL;
			out[40] = VU1.VI[3].UL;
			for (const u32 v : out)
				hash = (hash ^ v) * 0x01000193;
			std::fwrite(in, sizeof(in), 1, fp);
			std::fwrite(out, sizeof(out), 1, fp);
		}
		Console.WriteLn("VU1 optest: %s %u cases, hash %08x", op.name, count, hash);
	}
	std::fclose(fp);

	VU1 = *saved;
	VU0.VI[REG_VPU_STAT].UL = vpu_stat;
	std::memcpy(VU1.Micro, micro.data(), micro.size());
	std::memcpy(VU1.Mem, mem.data(), mem.size());
	CpuVU1->Clear(0, VU1_PROGSIZE);
}
