#pragma once

#include "DebugTools/DebugInterface.h"

void gdx_initialize();
void gdx_in_vsync();
void gdx_reload_breakpoints();
bool gdx_check_breakpoint();
bool gdx_break();
void gdx_on_load_irx(std::string name, u32 import_table, u16 index);