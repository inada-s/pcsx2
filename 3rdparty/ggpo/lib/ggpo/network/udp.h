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
#include <thread>

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
      // zdxsv: called as soon as a packet is read, on the network thread or in OnLoopPoll, with NetMutex held.
      // Returns true if the packet was handled there (relay forwarding); otherwise it is queued for OnMsg.
      virtual bool TryForward(sockaddr_storage &from, UdpMsg *msg, int len) { return false; }
      // zdxsv: then, for a packet not forwarded, before it is queued: replies sent here do not wait for OnMsg.
      virtual void OnReceived(sockaddr_storage &from, UdpMsg *msg, int len) { }
      // zdxsv: with GGPO_NETWORK_DELAY set, every 1 ms on the network thread, NetMutex held: delayed sends
      // leave when due, not at the next poll of the game thread.
      virtual void PumpSendQueues() { }
   };

   Udp();

   void Init(uint16 port, Poll *p, Callbacks *callbacks);
   // zdxsv: read the sockets on a network thread so that packets are read and relayed while the
   // game thread is busy (a rollback). OnMsg still runs only from OnLoopPoll, on the game thread.
   void StartNetThread();
   void StopNetThread();
   // zdxsv: held while reading the sockets and dispatching queued packets. Guards what TryForward reads.
   std::mutex &NetMutex() { return _net_mutex; }
   
   void SendTo(char *buffer, int len, int flags, struct sockaddr *dst, int destlen);

   virtual bool OnLoopPoll(void *cookie);

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

   void ReadSockets();
   void NetThreadLoop();

   struct RecvPacket {
      sockaddr_storage from;
      int              len;
      uint8            data[MAX_UDP_PACKET_SIZE];
   };
   std::mutex              _net_mutex;
   std::deque<RecvPacket>  _recv_queue;
   std::thread             _net_thread;
   std::atomic<bool>       _net_stop;
   int                    _recv_dropped;
};

#endif
