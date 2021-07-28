#pragma once

#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include "../DebugTools/DebugInterface.h"

#define verify assert

enum LogCategory {
	COMMON = 0,
};

static inline void ERROR_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);

	va_end(args);
}

static inline void WARN_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);

	va_end(args);
}

static inline void NOTICE_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);

	va_end(args);
}

static inline void INFO_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);

	va_end(args);
}

#define gdxsv_ReadMem32 r5900Debug.read32
#define gdxsv_ReadMem16 r5900Debug.read16
#define gdxsv_ReadMem8 r5900Debug.read8
#define gdxsv_WriteMem32 r5900Debug.write32
#define gdxsv_WriteMem16 r5900Debug.write16
#define gdxsv_WriteMem8 r5900Debug.write8
