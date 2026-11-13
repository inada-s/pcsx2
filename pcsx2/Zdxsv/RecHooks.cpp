// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// EE recompiler: the calls the zdxsv hooks need before an instruction (see CpuHooks.h).

#include "Zdxsv/CpuHooks.h"
#include "Zdxsv/RecHooks.h"

#include "x86/iR5900.h"

using namespace x86Emitter;

namespace Zdxsv
{
	void RecEmitHooks(u32 pc, const void* dispatcher)
	{
		if (g_ee_probe)
			RecEmitProbes(pc);
		if (g_net_hook && pc == NetRpcPc())
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnNetCall);
			// Everything is flushed: on true leave the block, the dispatcher continues at cpuRegs.pc.
			xTEST(al, al);
			xForwardJZ32 run_wrapper;
			xJMP(dispatcher);
			run_wrapper.SetTarget();
		}
		if (g_net_hook && g_z_game && pc == GgpoPcs().net_recv_ret)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnNetRecv);
		}
		if (g_zd_hook && pc == GgpoPcs().step_copy)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnStepCopy);
		}
		if (g_ps_hook && (pc == GgpoPcs().load_step || pc == GgpoPcs().ms_step))
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall(pc == GgpoPcs().load_step ? (void*)OnLoadStep : (void*)OnMsStep);
			xTEST(al, al);
			xForwardJZ32 run_step;
			xJMP(dispatcher);
			run_step.SetTarget();
		}
		if (g_ps_hook && (pc == GgpoPcs().draw_run || pc == GgpoPcs().draw_run2))
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall(pc == GgpoPcs().draw_run ? (void*)OnDrawRun : (void*)OnDrawRun2);
			xTEST(al, al);
			xForwardJZ32 run_draw;
			xJMP(dispatcher);
			run_draw.SetTarget();
		}
		if (g_ps_hook && pc == GgpoPcs().obj_call)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnObjCall);
			xTEST(al, al);
			xForwardJZ32 run_obj;
			xJMP(dispatcher);
			run_obj.SetTarget();
		}
	}
} // namespace Zdxsv
