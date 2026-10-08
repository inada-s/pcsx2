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
		if (g_net_hook && pc == NET_RPC_PC)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnNetCall);
			// Everything is flushed: on true leave the block, the dispatcher continues at cpuRegs.pc.
			xTEST(al, al);
			xForwardJZ32 run_wrapper;
			xJMP(dispatcher);
			run_wrapper.SetTarget();
		}
		if (g_net_hook && pc == NET_RECV_RET_PC)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnNetRecv);
		}
		if (g_zd_hook && pc == STEP_COPY_PC)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnStepCopy);
		}
		if (g_ps_hook && pc == LOAD_STEP_PC)
		{
			iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
			xFastCall((void*)OnLoadStep);
			xTEST(al, al);
			xForwardJZ32 run_step;
			xJMP(dispatcher);
			run_step.SetTarget();
		}
	}
} // namespace Zdxsv
