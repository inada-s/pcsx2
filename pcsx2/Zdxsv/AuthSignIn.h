// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Zdxsv
{
	// The server-issued emulator token in the settings folder, if present and well formed.
	std::optional<std::string> AuthTokenRead();

	// Logs the URL and opens it in the browser (not with ZDXSV_AUTH_NO_BROWSER).
	void AuthOpenBrowser(const std::string& url);

	// Blocking, off the UI thread. open_url gets the server's sign-in page URL; the browser comes back to a
	// 127.0.0.1 listener. carry is an optional carry-over code. Saves the token. Returns an empty string or an error.
	std::string AuthSignIn(const std::string& carry, const std::function<void(const std::string&)>& open_url,
		const std::atomic<bool>& cancel);

	// Blocking: revokes the token on the server and deletes the file (the file goes even if the server is unreachable).
	std::string AuthSignOut();

	struct AuthAccountList
	{
		// Per account, numbered from 1: the names of its users.
		std::vector<std::vector<std::string>> users;
		int default_account = 0;
		int max_accounts = 0;
		int added = 0;
	};

	// Blocking: the accounts of the signed-in Discord account; with add, creates one more first.
	// Returns an empty string or an error.
	std::string AuthAccounts(bool add, AuthAccountList* list);
} // namespace Zdxsv
