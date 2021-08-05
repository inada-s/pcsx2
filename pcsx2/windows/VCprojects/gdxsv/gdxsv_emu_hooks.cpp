#include "PrecompiledHeader.h"
#include "gdxsv_emu_hooks.h"
#include "gdxsv_emu_debug.h"
#include "gdxsv.h"

#include <map>

std::map<std::string, std::string> emu_args;

void gdxsv_emu_arg(const char* name, const char* value) {
	emu_args[name] = value;
}

void gdxsv_emu_reset() {
	gdxsv.Reset();

	// gdxsv_emu_debug_initialize();
}

void gdxsv_emu_update() {
	gdxsv.Update();
}

void gdxsv_emu_rpc() {
	gdxsv.HandleRPC();
}

void gdxsv_emu_loadstate(int slot) {
	printf("gdxsv_emu_loadstate");
	if (slot == 1) {
		if (emu_args.find("replay") != emu_args.end()) {
			if (emu_args.find("rollback-test") != emu_args.end()) {
				printf("rollback-replay-test");
				gdxsv.StartRollbackReplayTest(emu_args["replay"].c_str());
			} else {
				printf("replay");
				gdxsv.StartReplayFile(emu_args["replay"].c_str());
			}
		}
	}
}
