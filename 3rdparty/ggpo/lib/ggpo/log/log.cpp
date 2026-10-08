// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ggpo_log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

static std::atomic<GGPOLogFunction> s_log_function{nullptr};

void ggpo_set_log_function(GGPOLogFunction func)
{
	s_log_function.store(func);
}

void ggpo_log(int level, const char* fmt, ...)
{
	const GGPOLogFunction func = s_log_function.load();
	if (!func)
		return;

	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	func(level, buf);
}
