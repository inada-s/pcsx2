// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/AuthSignIn.h"
#include "Zdxsv/Settings.h"
#include "Zdxsv/TestOptions.h"
#include "Config.h"
#include "Host.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/HTTPDownloader.h"
#include "common/Path.h"
#include "mbedtls/sha256.h"
#include "rapidjson/document.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#define AUTH_SOCKET_TYPE SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define AUTH_SOCKET_TYPE int
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace Zdxsv
{
	namespace
	{
		using AuthSocket = AUTH_SOCKET_TYPE;

		constexpr const char* kTokenFile = "zdxsv_auth_token.txt";
		constexpr size_t kMaxTokenLen = 128;
		constexpr size_t kMaxCarryLen = 16;
		constexpr auto kSignInTimeout = std::chrono::minutes(10);
		constexpr size_t kMaxRequestBytes = 4096;

		std::string TokenPath()
		{
			return Path::Combine(EmuFolders::Settings, kTokenFile);
		}

		bool ValidToken(const std::string& s)
		{
			if (s.empty() || s.size() > kMaxTokenLen)
				return false;
			for (unsigned char c : s)
				if (!std::isalnum(c) && c != '-' && c != '_')
					return false;
			return true;
		}

		std::string AuthUrl(const char* path)
		{
			std::string base;
			if (const char* env = TestEnv("ZDXSV_AUTH_URL"))
				base = env;
			else
				base = Host::GetStringSettingValue("DEV9/Eth", "ZdxsvAuthUrl", ServerApiUrl("/auth").c_str());
			return base.empty() ? base : base + path;
		}

		std::string RandomUrlSafe(size_t len)
		{
			static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
			std::random_device rd;
			std::string s(len, '\0');
			for (char& c : s)
				c = kChars[rd() % 64];
			return s;
		}

		std::string Base64Url(const unsigned char* data, size_t len)
		{
			static constexpr char kChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
			std::string out;
			u32 acc = 0;
			int bits = 0;
			for (size_t i = 0; i < len; i++)
			{
				acc = (acc << 8) | data[i];
				bits += 8;
				while (bits >= 6)
				{
					bits -= 6;
					out.push_back(kChars[(acc >> bits) & 63]);
				}
			}
			if (bits > 0)
				out.push_back(kChars[(acc << (6 - bits)) & 63]);
			return out;
		}

		std::string Challenge(const std::string& verifier)
		{
			std::array<unsigned char, 32> sum;
			mbedtls_sha256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), sum.data(), 0);
			return Base64Url(sum.data(), sum.size());
		}

		std::string QueryValue(const std::string& query, const std::string& key)
		{
			size_t pos = 0;
			while (pos <= query.size())
			{
				const size_t end = std::min(query.find('&', pos), query.size());
				const std::string pair = query.substr(pos, end - pos);
				if (pair.starts_with(key + "="))
					return pair.substr(key.size() + 1);
				pos = end + 1;
			}
			return {};
		}

		std::string Post(const std::string& url, const std::string& form, s32* status)
		{
			std::string body;
			*status = 0;
			std::unique_ptr<HTTPDownloader> http = HTTPDownloader::Create(Host::GetHTTPUserAgent());
			if (!http)
				return body;
			http->SetTimeout(10.0f);
			http->CreatePostRequest(url, form,
				[&body, status](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
					*status = code;
					body.assign(data.begin(), data.end());
				},
				nullptr, "application/x-www-form-urlencoded");
			http->WaitForAllRequests();
			return body;
		}

		struct Listener
		{
			AuthSocket sock = INVALID_SOCKET;
			~Listener()
			{
				if (sock != INVALID_SOCKET)
					closesocket(sock);
			}
		};

		void Respond(AuthSocket client, bool ok)
		{
			const char* page = ok ?
				"<!doctype html><meta charset=utf-8><p>Signed in. You can close this window.</p>"
				"<p>サインインしました。このウィンドウを閉じてください。</p><p>已登入。可以關閉此視窗。</p>" :
				"<!doctype html><meta charset=utf-8><p>Sign-in failed.</p><p>サインインに失敗しました。</p><p>登入失敗。</p>";
			const std::string resp = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
												 "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: ") +
									 std::to_string(std::strlen(page)) + "\r\n\r\n" + page;
			send(client, resp.data(), static_cast<int>(resp.size()), 0);
		}

		// The request line of one connection, or empty. Bounded in size and time.
		std::string ReadRequestLine(AuthSocket client)
		{
			std::string req;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (req.find("\r\n") == std::string::npos && req.size() < kMaxRequestBytes &&
				   std::chrono::steady_clock::now() < deadline)
			{
				fd_set fds;
				FD_ZERO(&fds);
				FD_SET(client, &fds);
				timeval tv{0, 200000};
				if (select(static_cast<int>(client) + 1, &fds, nullptr, nullptr, &tv) <= 0)
					continue;
				char buf[512];
				const int n = recv(client, buf, sizeof(buf), 0);
				if (n <= 0)
					break;
				req.append(buf, n);
			}
			const size_t eol = req.find("\r\n");
			return eol == std::string::npos ? std::string() : req.substr(0, eol);
		}
	} // namespace

	std::optional<std::string> AuthTokenRead()
	{
		std::optional<std::string> s = FileSystem::ReadFileToString(TokenPath().c_str());
		if (!s)
			return std::nullopt;
		while (!s->empty() && std::isspace(static_cast<unsigned char>(s->back())))
			s->pop_back();
		if (!ValidToken(*s))
			return std::nullopt;
		return s;
	}

	std::string AuthSignIn(const std::string& carry, const std::function<void(const std::string&)>& open_url,
		const std::atomic<bool>& cancel)
	{
		std::string carry_code;
		for (unsigned char c : carry)
		{
			if (std::isspace(c))
				continue;
			if (!std::isalnum(c) || carry_code.size() >= kMaxCarryLen)
				return "invalid carry-over code";
			carry_code.push_back(static_cast<char>(std::toupper(c)));
		}
		const std::string start = AuthUrl("/start");
		if (start.empty())
			return "no sign-in server";
#ifdef _WIN32
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
			return "socket error";
		struct WsaCleanup
		{
			~WsaCleanup() { WSACleanup(); }
		} wsa_cleanup;
#endif
		Listener listener;
		listener.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t addr_len = sizeof(addr);
		if (listener.sock == INVALID_SOCKET || bind(listener.sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
			listen(listener.sock, 4) != 0 || getsockname(listener.sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0)
			return "cannot listen on 127.0.0.1";
		const int port = ntohs(addr.sin_port);

		const std::string verifier = RandomUrlSafe(64);
		const std::string state = RandomUrlSafe(32);
		std::string url = start + "?redirect_port=" + std::to_string(port) + "&state=" + state +
						  "&code_challenge=" + Challenge(verifier);
		if (!carry_code.empty())
			url += "&carry=" + carry_code;
		Console.WriteLn("Zdxsv: auth: sign-in started, callback port %d", port);
		open_url(url);

		std::string code;
		const auto deadline = std::chrono::steady_clock::now() + kSignInTimeout;
		while (code.empty())
		{
			if (cancel.load())
				return "cancelled";
			if (std::chrono::steady_clock::now() >= deadline)
				return "timed out";
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(listener.sock, &fds);
			timeval tv{0, 200000};
			if (select(static_cast<int>(listener.sock) + 1, &fds, nullptr, nullptr, &tv) <= 0)
				continue;
			Listener client;
			client.sock = accept(listener.sock, nullptr, nullptr);
			if (client.sock == INVALID_SOCKET)
				continue;
			const std::string line = ReadRequestLine(client.sock);
			// Other requests (favicon, a stray tab) are answered and ignored; only the matching state ends the wait.
			const std::string prefix = "GET /callback?";
			if (!line.starts_with(prefix))
			{
				Respond(client.sock, false);
				continue;
			}
			const size_t sp = line.find(' ', prefix.size());
			const std::string query = line.substr(prefix.size(), sp == std::string::npos ? std::string::npos : sp - prefix.size());
			const std::string got = QueryValue(query, "code");
			const bool ok = QueryValue(query, "state") == state && ValidToken(got);
			Respond(client.sock, ok);
			if (ok)
				code = got;
		}

		s32 status;
		const std::string body = Post(AuthUrl("/token"), "code=" + code + "&code_verifier=" + verifier, &status);
		if (status != HTTPDownloader::HTTP_STATUS_OK)
			return "token request failed (" + std::to_string(status) + ")";
		rapidjson::Document doc;
		doc.Parse(body.data(), body.size());
		const auto it = doc.IsObject() ? doc.FindMember("token") : doc.MemberEnd();
		if (!doc.IsObject() || it == doc.MemberEnd() || !it->value.IsString())
			return "bad token response";
		const std::string token(it->value.GetString(), it->value.GetStringLength());
		if (!ValidToken(token))
			return "bad token response";
		if (!FileSystem::WriteStringToFile(TokenPath().c_str(), token))
			return "cannot write the token file";
		Console.WriteLn("Zdxsv: auth: signed in");
		return {};
	}

	std::string AuthSignOut()
	{
		std::string error;
		if (const std::optional<std::string> token = AuthTokenRead())
		{
			s32 status;
			Post(AuthUrl("/revoke"), "token=" + *token, &status);
			if (status < 200 || status > 299)
				error = "the server did not revoke the token (" + std::to_string(status) + ")";
		}
		if (FileSystem::FileExists(TokenPath().c_str()) && !FileSystem::DeleteFilePath(TokenPath().c_str()))
			return "cannot delete the token file";
		Console.WriteLn("Zdxsv: auth: signed out%s%s", error.empty() ? "" : ", ", error.c_str());
		return error;
	}
} // namespace Zdxsv
