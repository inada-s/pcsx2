// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

// EE interpreter: the hook calls of RecEmitHooks (RecHooks.cpp), made before an instruction runs (see CpuHooks.h).

#include "Zdxsv/CpuHooks.h"

namespace Zdxsv
{
	bool IntRunHooks(u32 pc)
	{
		if (g_net_hook && pc == NetRpcPc())
			return OnNetCall();
		if (g_net_hook && g_z_game && pc == GgpoPcs().net_recv_ret)
			OnNetRecv();
		if (g_zd_hook && pc == GgpoPcs().step_copy)
			OnStepCopy();
		if (g_ps_hook && (pc == GgpoPcs().load_step || pc == GgpoPcs().ms_step))
			return pc == GgpoPcs().load_step ? OnLoadStep() : OnMsStep();
		if (g_ps_hook && (pc == GgpoPcs().draw_run || pc == GgpoPcs().draw_run2))
			return pc == GgpoPcs().draw_run ? OnDrawRun() : OnDrawRun2();
		if (g_ps_hook && pc == GgpoPcs().obj_call)
			return OnObjCall();
		return false;
	}
} // namespace Zdxsv
