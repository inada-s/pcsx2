/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _UDP_H
#define _UDP_H

#include "ggpo_poll.h"
#include "udp_msg.h"
#include "ggponet.h"
#include "ring_buffer.h"

#include <atomic>
#include <deque>
#include <mutex>

#define MAX_UDP_ENDPOINTS     16

constexpr size_t MAX_UDP_PACKET_SIZE = sizeof(UdpMsg);

class Udp : public IPollSink
{
public:
   struct Stats {
      int      bytes_sent;
      int      packets_sent;
      float    kbps_sent;
   };

   struct Callbacks {
      virtual ~Callbacks() { }
      virtual void OnMsg(sockaddr_storage &from, UdpMsg *msg, int len) = 0;
      // zdxsv: called as soon as a packet is read, with RecvMutex held, also from ForwardRelayed on
      // another thread. True: handled there (a relayed packet); otherwise it is queued for OnMsg.
      virtual bool TryForward(sockaddr_storage &from, UdpMsg *msg, int len) { return false; }
   };

   Udp();

   void Init(uint16 port, Poll *p, Callbacks *callbacks);
   
   void SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen);

   virtual bool OnLoopPoll(void *cookie);

   // zdxsv: reads the sockets now and forwards relayed packets; the rest wait for OnLoopPoll.
   // Safe while another thread is in a GGPO call. Returns the packets TryForward handled.
   int ForwardRelayed();
   // zdxsv: held while reading the sockets and handling packets. Guards what TryForward reads.
   std::mutex &RecvMutex() { return _recv_mutex; }
   // zdxsv (tests): drops every packet sent and received from now on, as a network that went silent.
   void SetMute(bool mute) { _mute = mute; }

public:
   ~Udp(void);

protected:
   // Network transmission information
   SOCKET         _socket_v4;
   SOCKET         _socket_v6;

   // state management
   Callbacks      *_callbacks;
   std::atomic<bool> _mute{false};

   int ReadSockets();

   struct RecvPacket {
      sockaddr_storage from;
      int              len;
      uint8            data[MAX_UDP_PACKET_SIZE];
   };
   std::mutex             _recv_mutex;
   std::deque<RecvPacket> _recv_queue;
   int                    _recv_dropped;
};

#endif
