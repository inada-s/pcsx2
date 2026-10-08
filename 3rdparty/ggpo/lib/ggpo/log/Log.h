#pragma once

// Stand-in for flycast's log/Log.h (included by ggpo_types.h).

#include "ggpo_log.h"

namespace LogTypes
{
	enum LOG_LEVELS
	{
		LERROR = GGPO_LOG_ERROR,
		LWARNING = GGPO_LOG_WARNING,
		LNOTICE = GGPO_LOG_NOTICE,
		LINFO = GGPO_LOG_INFO,
		LDEBUG = GGPO_LOG_DEBUG,
	};
}

// As in flycast release builds: debug messages are compiled out.
#ifndef MAX_LOGLEVEL
#define MAX_LOGLEVEL LogTypes::LINFO
#endif

#define GGPO_LOG_AT(level, ...) \
	do \
	{ \
		if (level <= MAX_LOGLEVEL) \
			ggpo_log(level, __VA_ARGS__); \
	} while (0)

#define ERROR_LOG(t, ...) GGPO_LOG_AT(LogTypes::LERROR, __VA_ARGS__)
#define WARN_LOG(t, ...) GGPO_LOG_AT(LogTypes::LWARNING, __VA_ARGS__)
#define NOTICE_LOG(t, ...) GGPO_LOG_AT(LogTypes::LNOTICE, __VA_ARGS__)
#define INFO_LOG(t, ...) GGPO_LOG_AT(LogTypes::LINFO, __VA_ARGS__)
#define DEBUG_LOG(t, ...) GGPO_LOG_AT(LogTypes::LDEBUG, __VA_ARGS__)
