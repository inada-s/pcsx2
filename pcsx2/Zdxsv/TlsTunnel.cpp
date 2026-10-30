// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#include "Zdxsv/Dev9Hooks.h"
#include "Zdxsv/ServerHosts.h"
#include "Zdxsv/TestOptions.h"

#include "Host.h"
#include "IconsFontAwesome.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace Zdxsv
{
	namespace
	{
		// Public-key pins (SHA-256 of the SubjectPublicKeyInfo, hex) of the zdxsv CA and the backup CA.
		constexpr std::array<std::string_view, 2> kPins{
			"59c321a216b77fa3428869e87a47abba9587972998cef63f943a461f64778d94", // zdxsv ca
			"c784e5d5659c3190cb1d867c5d97e7acde181c6b076f650534c43c4f70c9dca5", // zdxsv backup-ca
		};

		// The zdxsv test CA (zdxsv repository, pkg/tlsconf/testdata/ca.crt). Trusted only with ZDXSV_TLS_TEST_PIN=1.
		constexpr std::string_view kTestPin = "0af329b315307efed87292e042cc0726ce5d1e37cd52301aac53d5c72674b6b7";

		struct TlsPort
		{
			u16 plain;
			u16 tls;
		};
		constexpr TlsPort kPorts[] = {
			{443, 8220}, // DNAS and login pages
			{8200, 8221}, // lobby
			{8210, 8222}, // battle
		};

		constexpr int kTimeoutMs = 10000;
		constexpr size_t kBufferSize = 16384;

		struct TlsOptions
		{
			bool enabled = false;
			std::vector<std::string> pins;
		};

		const TlsOptions& Options()
		{
			static const TlsOptions options = [] {
				TlsOptions o;
				o.pins.assign(kPins.begin(), kPins.end());
				const char* test_pin = TestEnv("ZDXSV_TLS_TEST_PIN");
				if (test_pin && std::strcmp(test_pin, "1") == 0)
					o.pins.emplace_back(kTestPin);
				const char* tls = TestEnv("ZDXSV_TLS");
				o.enabled = !o.pins.empty() && !(tls && std::strcmp(tls, "0") == 0);
				Console.WriteLn("Zdxsv: TLS: %s (%zu pins)", o.enabled ? "on" : "off", o.pins.size());
				return o;
			}();
			return options;
		}

		std::string ErrorText(int ret)
		{
			char text[128];
			mbedtls_strerror(ret, text, sizeof(text));
			return StringUtil::StdStringFromFormat("%s (-0x%04x)", text, static_cast<unsigned>(-ret));
		}

		std::string KeyPin(mbedtls_pk_context* pk)
		{
			std::array<unsigned char, 1024> der;
			const int len = mbedtls_pk_write_pubkey_der(pk, der.data(), der.size());
			if (len <= 0)
				return {};
			std::array<unsigned char, 32> hash;
			mbedtls_sha256(der.data() + der.size() - len, len, hash.data(), 0);
			std::string hex;
			for (unsigned char b : hash)
				hex += StringUtil::StdStringFromFormat("%02x", b);
			return hex;
		}

		bool IsPinned(const std::string& pin)
		{
			const std::vector<std::string>& pins = Options().pins;
			return !pin.empty() && std::find(pins.begin(), pins.end(), pin) != pins.end();
		}

		// A pinned leaf key is accepted as is. A pinned key further up the chain must have signed the
		// leaf (through the chain the server sent): the CA certificate is public, so sending it is no proof.
		// Expiry is not checked.
		bool ChainMatchesPin(const mbedtls_x509_crt* chain, std::string* why)
		{
			if (!chain)
			{
				*why = "no certificate";
				return false;
			}
			if (IsPinned(KeyPin(const_cast<mbedtls_pk_context*>(&chain->pk))))
				return true;
			for (const mbedtls_x509_crt* c = chain->next; c; c = c->next)
			{
				if (!IsPinned(KeyPin(const_cast<mbedtls_pk_context*>(&c->pk))))
					continue;
				mbedtls_x509_crt anchor;
				mbedtls_x509_crt_init(&anchor);
				u32 flags = 0;
				int ret = mbedtls_x509_crt_parse_der(&anchor, c->raw.p, c->raw.len);
				if (ret == 0)
					ret = mbedtls_x509_crt_verify(const_cast<mbedtls_x509_crt*>(chain), &anchor, nullptr, nullptr, &flags, nullptr, nullptr);
				mbedtls_x509_crt_free(&anchor);
				if ((flags & ~(MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) == 0 && (ret == 0 || flags != 0))
					return true;
				*why = StringUtil::StdStringFromFormat("pinned certificate did not sign the server certificate (flags 0x%x)", flags);
				return false;
			}
			*why = "no pinned key in the server's certificates";
			return false;
		}

		void SetNoDelay(int fd)
		{
			constexpr int no_delay = 1;
			setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
		}

		// Ends the emulator side with a RST, so the game sees a refused connection, not an orderly close.
		void ResetLocal(mbedtls_net_context* local)
		{
			if (local->fd < 0)
				return;
			linger l{};
			l.l_onoff = 1;
			l.l_linger = 0;
			setsockopt(local->fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof(l));
			mbedtls_net_free(local);
		}

		struct Tunnel
		{
			std::string server_ip;
			u16 plain_port = 0;
			u16 tls_port = 0;
			mbedtls_net_context listener;
			mbedtls_net_context local;
			mbedtls_net_context server;
			mbedtls_entropy_context entropy;
			mbedtls_ctr_drbg_context drbg;
			mbedtls_ssl_config conf;
			mbedtls_ssl_context ssl;

			Tunnel()
			{
				mbedtls_net_init(&listener);
				mbedtls_net_init(&local);
				mbedtls_net_init(&server);
				mbedtls_entropy_init(&entropy);
				mbedtls_ctr_drbg_init(&drbg);
				mbedtls_ssl_config_init(&conf);
				mbedtls_ssl_init(&ssl);
			}
			~Tunnel()
			{
				mbedtls_ssl_free(&ssl);
				mbedtls_ssl_config_free(&conf);
				mbedtls_ctr_drbg_free(&drbg);
				mbedtls_entropy_free(&entropy);
				mbedtls_net_free(&server);
				mbedtls_net_free(&local);
				mbedtls_net_free(&listener);
			}
			Tunnel(const Tunnel&) = delete;
			Tunnel& operator=(const Tunnel&) = delete;

			void Fail(const std::string& why)
			{
				Console.Error("Zdxsv: TLS: %s:%u (TLS port %u): %s", server_ip.c_str(), plain_port, tls_port, why.c_str());
				Host::AddIconOSDMessage("ZdxsvTls", ICON_FA_TRIANGLE_EXCLAMATION,
					TRANSLATE_STR("Zdxsv", "Secure connection to the zdxsv server failed:") + " " + why, Host::OSD_ERROR_DURATION);
				ResetLocal(&local);
			}

			bool Connect()
			{
				int ret = mbedtls_net_poll(&listener, MBEDTLS_NET_POLL_READ, kTimeoutMs);
				if (ret <= 0 || mbedtls_net_accept(&listener, &local, nullptr, 0, nullptr) != 0)
				{
					Console.Error("Zdxsv: TLS: the emulator side did not connect");
					return false;
				}
				mbedtls_net_free(&listener);

				const std::string port = std::to_string(tls_port);
				if ((ret = mbedtls_net_connect(&server, server_ip.c_str(), port.c_str(), MBEDTLS_NET_PROTO_TCP)) != 0)
				{
					Fail("connect: " + ErrorText(ret));
					return false;
				}
				SetNoDelay(server.fd);
				static constexpr char kPersonalization[] = "zdxsv tls";
				if ((ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
						 reinterpret_cast<const unsigned char*>(kPersonalization), sizeof(kPersonalization))) != 0 ||
					(ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
						 MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
				{
					Fail("setup: " + ErrorText(ret));
					return false;
				}
				// The pin check after the handshake is the only certificate check.
				mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
				mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
				mbedtls_ssl_conf_read_timeout(&conf, kTimeoutMs);
				if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
				{
					Fail("setup: " + ErrorText(ret));
					return false;
				}
				mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
				if ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
				{
					Fail("handshake: " + ErrorText(ret));
					return false;
				}
				std::string why;
				if (!ChainMatchesPin(mbedtls_ssl_get_peer_cert(&ssl), &why))
				{
					Fail(why);
					return false;
				}
				Console.WriteLn("Zdxsv: TLS: %s:%u through TLS port %u (%s)", server_ip.c_str(), plain_port, tls_port,
					mbedtls_ssl_get_version(&ssl));
				// A record that carries no data (a TLS 1.3 session ticket) makes the server readable, and a blocking
				// read would then wait for the next record while the emulator's bytes go unsent.
				mbedtls_net_set_nonblock(&server);
				mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, nullptr);
				return true;
			}

			bool WriteServer(const unsigned char* data, size_t len)
			{
				while (len > 0)
				{
					const int ret = mbedtls_ssl_write(&ssl, data, len);
					if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
					{
						const u32 wait = ret == MBEDTLS_ERR_SSL_WANT_READ ? MBEDTLS_NET_POLL_READ : MBEDTLS_NET_POLL_WRITE;
						if (mbedtls_net_poll(&server, wait, kTimeoutMs) <= 0)
							return false;
						continue;
					}
					if (ret <= 0)
						return false;
					data += ret;
					len -= static_cast<size_t>(ret);
				}
				return true;
			}

			bool WriteLocal(const unsigned char* data, size_t len)
			{
				while (len > 0)
				{
					const int ret = mbedtls_net_send(&local, data, len);
					if (ret <= 0)
						return false;
					data += ret;
					len -= static_cast<size_t>(ret);
				}
				return true;
			}

			// Returns when either side closes or fails; a server failure resets the emulator side.
			void Pump()
			{
				std::vector<unsigned char> buffer(kBufferSize);
				for (;;)
				{
					bool server_ready = mbedtls_ssl_get_bytes_avail(&ssl) > 0;
					bool local_ready = false;
					if (!server_ready)
					{
						fd_set reads;
						FD_ZERO(&reads);
						FD_SET(local.fd, &reads);
						FD_SET(server.fd, &reads);
						timeval wait{1, 0};
						const int n = select(std::max(local.fd, server.fd) + 1, &reads, nullptr, nullptr, &wait);
						if (n < 0)
							break;
						local_ready = FD_ISSET(local.fd, &reads);
						server_ready = FD_ISSET(server.fd, &reads);
					}
					if (local_ready)
					{
						const int got = mbedtls_net_recv(&local, buffer.data(), buffer.size());
						if (got <= 0)
						{
							mbedtls_ssl_close_notify(&ssl);
							return;
						}
						if (!WriteServer(buffer.data(), static_cast<size_t>(got)))
						{
							ResetLocal(&local);
							return;
						}
					}
					if (server_ready)
					{
						const int got = mbedtls_ssl_read(&ssl, buffer.data(), buffer.size());
						if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE ||
							got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
							continue;
						if (got == 0 || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
							return;
						if (got < 0)
						{
							Console.Error("Zdxsv: TLS: %s:%u read: %s", server_ip.c_str(), plain_port, ErrorText(got).c_str());
							ResetLocal(&local);
							return;
						}
						if (!WriteLocal(buffer.data(), static_cast<size_t>(got)))
							return;
					}
				}
			}

			void Run()
			{
				if (Connect())
					Pump();
			}
		};

		const TlsPort* FindPort(u16 port)
		{
			for (const TlsPort& p : kPorts)
				if (p.plain == port)
					return &p;
			return nullptr;
		}
	} // namespace

	void TlsOnConnect(void* endpoint)
	{
		sockaddr_in* addr = static_cast<sockaddr_in*>(endpoint);
		const u32 ip = std::bit_cast<u32>(addr->sin_addr);
		const u16 port = ntohs(addr->sin_port);
		const TlsPort* tls_port = FindPort(port);
		if (!tls_port || !Options().enabled || !(IsServerAddress(ip) || IsBattleServer(ip, port)))
			return;

		static std::once_flag psa_once;
		std::call_once(psa_once, [] { psa_crypto_init(); });

		auto tunnel = std::make_unique<Tunnel>();
		char ip_text[INET_ADDRSTRLEN] = {};
		inet_ntop(AF_INET, &addr->sin_addr, ip_text, sizeof(ip_text));
		tunnel->server_ip = ip_text;
		tunnel->plain_port = port;
		tunnel->tls_port = tls_port->tls;

		int ret = mbedtls_net_bind(&tunnel->listener, "127.0.0.1", "0", MBEDTLS_NET_PROTO_TCP);
		sockaddr_in bound{};
		socklen_t bound_len = sizeof(bound);
		if (ret == 0)
			ret = getsockname(tunnel->listener.fd, reinterpret_cast<sockaddr*>(&bound), &bound_len);
		if (ret != 0)
		{
			// No plain-text fallback: the connection goes to a closed local port.
			Console.Error("Zdxsv: TLS: no local tunnel port (%d)", ret);
			addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr->sin_port = 0;
			return;
		}
		addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr->sin_port = bound.sin_port;
		std::thread([tunnel = std::move(tunnel)]() { tunnel->Run(); }).detach();
	}
} // namespace Zdxsv
