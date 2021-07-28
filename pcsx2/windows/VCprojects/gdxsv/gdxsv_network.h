#pragma once

#include <string>
#include <mutex>
#include <deque>
#include "types.h"
#include "gdxsv.pb.h"

#ifdef _WIN32
typedef SOCKET sock_t;
typedef int socklen_t;
#define INET_ADDRSTRLEN 16
#define VALID(s) ((s) != INVALID_SOCKET)
#define L_EWOULDBLOCK WSAEWOULDBLOCK
#define L_EAGAIN WSAEWOULDBLOCK
#define get_last_error() (WSAGetLastError())
#define perror(s) do { printf("%s: Winsock error: %d\n", (s) != NULL ? (s) : "", WSAGetLastError()); } while (false)

#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#endif

static inline void set_non_blocking(sock_t fd)
{
#ifndef _WIN32
	fcntl(fd, F_SETFL, O_NONBLOCK);
#else
	u_long optl = 1;
	ioctlsocket(fd, FIONBIO, &optl);
#endif
}

static inline void set_tcp_nodelay(sock_t fd)
{
	int optval = 1;
	socklen_t optlen = sizeof(optval);
#if defined(_WIN32)
	struct protoent* tcp_proto = getprotobyname("TCP");
	setsockopt(fd, tcp_proto->p_proto, TCP_NODELAY, (const char*)&optval, optlen);
#elif !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__)
	setsockopt(fd, SOL_TCP, TCP_NODELAY, (const void*)&optval, optlen);
#else
	struct protoent* tcp_proto = getprotobyname("TCP");
	setsockopt(fd, tcp_proto->p_proto, TCP_NODELAY, &optval, optlen);
#endif
}

static inline bool set_recv_timeout(sock_t fd, int delayms)
{
#ifdef _WIN32
	const DWORD dwDelay = delayms;
	return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&dwDelay, sizeof(DWORD)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = delayms / 1000;
	tv.tv_usec = (delayms % 1000) * 1000;
	return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

static inline bool set_send_timeout(sock_t fd, int delayms) {
#ifdef _WIN32
	const DWORD dwDelay = delayms;
	return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&dwDelay, sizeof(DWORD)) == 0;
#else
	struct timeval tv;
	tv.tv_sec = delayms / 1000;
	tv.tv_usec = (delayms % 1000) * 1000;
	return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

#if defined(_WIN32) && _WIN32_WINNT < 0x0600
static inline const char* inet_ntop(int af, const void* src, char* dst, int cnt)
{
	struct sockaddr_in srcaddr;

	memset(&srcaddr, 0, sizeof(struct sockaddr_in));
	memcpy(&srcaddr.sin_addr, src, sizeof(srcaddr.sin_addr));

	srcaddr.sin_family = af;
	if (WSAAddressToString((struct sockaddr*) & srcaddr, sizeof(struct sockaddr_in), 0, dst, (LPDWORD)&cnt) != 0)
		return nullptr;
	else
		return dst;
}
#endif


class TcpClient {
public:
    ~TcpClient() {
        Close();
    }

    bool Connect(const char *host, int port);

    void SetNonBlocking();

    int IsConnected() const;

    int Recv(char *buf, int len);

    int Send(const char *buf, int len);

    void Close();

    u32 ReadableSize() const;

    const std::string &host() { return host_; }

    const std::string &local_ip() const { return local_ip_; }

    int port() const { return port_; }

private:
    sock_t sock_ = INVALID_SOCKET;
    std::string host_;
    std::string local_ip_;
    int port_;
};

class MessageBuffer {
public:
    static const int kBufSize = 50;

    MessageBuffer();

    void SessionId(const std::string &session_id);

    bool CanPush() const;

    bool PushBattleMessage(const std::string &user_id, u8 *body, u32 body_length);

    const proto::Packet &Packet();

    void ApplySeqAck(u32 seq, u32 ack);

    void Clear();

private:
    u32 msg_seq_;
    u32 snd_seq_;
    proto::Packet packet_;
};

class MessageFilter {
public:
    bool IsNextMessage(const proto::BattleMessage &msg);

    void Clear();

private:
    std::map<std::string, u32> recv_seq;
};


class UdpRemote {
public:
    bool Open(const char *host, int port);

    bool Open(const std::string &addr);

    void Close();

    bool is_open() const;

    const std::string &str_addr() const;

    const sockaddr_in &net_addr() const;

private:
    bool is_open_;
    std::string str_addr_;
    sockaddr_in net_addr_;
};

class UdpClient {
public:
    bool Bind(int port);

    bool Initialized() const;

    int RecvFrom(char *buf, int len, std::string &sender);

    int SendTo(const char *buf, int len, const UdpRemote &remote);

    u32 ReadableSize() const;

    void Close();

    int bind_port() const { return bind_port_; }

private:
    sock_t sock_ = INVALID_SOCKET;
    int bind_port_;
    std::string bind_ip_;
};
