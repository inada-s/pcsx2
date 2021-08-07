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
	printf("\n");

	va_end(args);
}

static inline void WARN_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);
	printf("\n");

	va_end(args);
}

static inline void NOTICE_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);
	printf("\n");

	va_end(args);
}

static inline void INFO_LOG(LogCategory cat, const char* format, ...) {
	va_list args;
	va_start(args, format);

	vprintf(format, args);
	printf("\n");

	va_end(args);
}

#define gdxsv_ReadMem32 r5900Debug.read32
#define gdxsv_ReadMem16 r5900Debug.read16
#define gdxsv_ReadMem8 r5900Debug.read8
#define gdxsv_WriteMem32 r5900Debug.write32
#define gdxsv_WriteMem8 r5900Debug.write8

static inline void gdxsv_WriteMem16(u32 addr, u16 value) {
	gdxsv_WriteMem8(addr, u8((value >> 8) & 0xff));
	gdxsv_WriteMem8(addr + 1, u8(value & 0xff)); // TODO check endian
}

static inline void gdxsv_ReadMemBlock(u8* dst, u32 addr, u32 size) {
	// TODO: Optimize
	for (int i = 0; i < size; ++i) {
		*(dst + i) = gdxsv_ReadMem8(addr + i);
	}
}

static inline void gdxsv_WriteMemBlock(u32 dst, const u8* src, u32 size) {
	// TODO: Optimize
	for (int i = 0; i < size; ++i) {
		gdxsv_WriteMem8(dst + i, *(src + i));
	}
}
