#pragma once

#include "ggpo/src/include/ggponet.h"

bool ggpo_cb_begin_game(const char*);

bool ggpo_cb_advance_frame(int);

bool ggpo_cb_load_game_state(unsigned char* buffer, int len);

bool ggpo_cb_save_game_state(unsigned char** buffer, int* len, int* checksum, int frame);

void ggpo_cb_free_buffer(void* buffer);

bool ggpo_cb_on_event(GGPOEvent* info);

bool ggpo_cb_log_game_state(char* filename, unsigned char* buffer, int len);
