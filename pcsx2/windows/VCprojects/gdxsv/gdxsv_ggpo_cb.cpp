#include "PrecompiledHeader.h"
#include "gdxsv_ggpo_cb.h"
#include "gdxsv_backend_rollback.h"

extern GdxsvBackendRollback* ptr_GdxsvBackendRollback;

bool ggpo_cb_begin_game(const char* game) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_begin_game(game);
}

bool ggpo_cb_advance_frame(int frame) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_advance_frame(frame);
}

bool ggpo_cb_load_game_state(unsigned char* buffer, int len) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_load_game_state(buffer, len);
}

bool ggpo_cb_save_game_state(unsigned char** buffer, int* len, int* checksum, int frame) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_save_game_state(buffer, len, checksum, frame);
}

void ggpo_cb_free_buffer(void* buffer) {
	if (ptr_GdxsvBackendRollback == nullptr) return;
	ptr_GdxsvBackendRollback->ggpo_cb_free_buffer(buffer);
}

bool ggpo_cb_on_event(GGPOEvent* info) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_on_event(info);
}

bool ggpo_cb_log_game_state(char* filename, unsigned char* buffer, int len) {
	if (ptr_GdxsvBackendRollback == nullptr) return false;
	return ptr_GdxsvBackendRollback->ggpo_cb_log_game_state(filename, buffer, len);
}

