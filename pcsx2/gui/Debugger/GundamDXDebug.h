#pragma once

#include "DebugTools/DebugInterface.h"

void gdx_initialize();
void gdx_reload_breakpoints();
bool gdx_on_hit_breakpoint(DebugInterface *cpu);