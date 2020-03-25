#pragma once

#include "DebugTools/DebugInterface.h"

void gdx_initialize();
void gdx_in_vsync();
void gdx_reload_breakpoints();
bool gdx_check_breakpoint();
bool gdx_break();