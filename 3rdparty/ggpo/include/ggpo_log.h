#pragma once

// GGPO log sink. GGPO sources come from inada-s/flycast, which logs through
// flycast's log/Log.h; lib/ggpo/log/Log.h maps those macros here.

#ifdef __cplusplus
extern "C" {
#endif

enum GGPOLogLevel
{
	GGPO_LOG_ERROR = 1,
	GGPO_LOG_WARNING = 2,
	GGPO_LOG_NOTICE = 3,
	GGPO_LOG_INFO = 4,
	GGPO_LOG_DEBUG = 5,
};

typedef void (*GGPOLogFunction)(int level, const char* msg);

// nullptr (default) drops all messages.
void ggpo_set_log_function(GGPOLogFunction func);

void ggpo_log(int level, const char* fmt, ...);

#ifdef __cplusplus
}
#endif
