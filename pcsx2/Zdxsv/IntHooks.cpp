// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// EE interpreter: the hook calls of RecEmitHooks (RecHooks.cpp), made before an instruction runs (see CpuHooks.h).

#include "Zdxsv/CpuHooks.h"

namespace Zdxsv
{
	bool IntRunHooks(u32 pc)
	{
		if (g_net_hook && pc == NET_RPC_PC)
			return OnNetCall();
		if (g_net_hook && pc == NET_RECV_RET_PC)
			OnNetRecv();
		if (g_zd_hook && pc == STEP_COPY_PC)
			OnStepCopy();
		if (g_ps_hook && (pc == LOAD_STEP_PC || pc == MS_STEP_PC))
			return pc == LOAD_STEP_PC ? OnLoadStep() : OnMsStep();
		if (g_ps_hook && (pc == DRAW_RUN_PC || pc == DRAW_RUN2_PC))
			return OnDrawRun();
		return false;
	}
} // namespace Zdxsv
