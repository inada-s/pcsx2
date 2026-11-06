// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/AuthHooks.h"
#include "Zdxsv/AuthSignIn.h"
#include "Zdxsv/TestOptions.h"
#include "common/Console.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace Zdxsv
{
	void AuthSignInFromEnv()
	{
		const char* env = TestEnv("ZDXSV_AUTH_SIGNIN_TEST");
		if (!env || !env[0])
			return;
		if (std::strcmp(env, "signout") == 0)
		{
			std::thread([]() { AuthSignOut(); }).detach();
			return;
		}
		std::thread([]() {
			static const std::atomic<bool> never{false};
			const std::string error = AuthSignIn([](const std::string& url) {
				Console.WriteLn("Zdxsv: auth: open %s", url.c_str());
			}, never);
			if (!error.empty())
				Console.Error("Zdxsv: auth: sign-in failed: %s", error.c_str());
		}).detach();
	}
} // namespace Zdxsv
