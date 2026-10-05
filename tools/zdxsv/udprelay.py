# UDP relay between 2 GGPO peers on localhost with delay, jitter and loss per packet.
# Socket A (port --a) stands in for peer 2 towards peer 1, socket B (--b) for peer 1 towards peer 2:
# peer 1 sends to A -> relayed from B to peer 2 (--p2); peer 2 sends to B -> relayed from A to peer 1 (--p1).
# Each peer sees packets from the address it sends to, so GGPO accepts them.
# Usage: udprelay.py --delay 30 --jitter 10 --loss 0.05 --seconds 600  (one-way ms; loss 0..1)
import argparse, heapq, random, select, socket, time

ap = argparse.ArgumentParser()
ap.add_argument('--a', type=int, default=7101)
ap.add_argument('--b', type=int, default=7102)
ap.add_argument('--p1', type=int, default=7001)
ap.add_argument('--p2', type=int, default=7002)
ap.add_argument('--delay', type=float, default=0)
ap.add_argument('--jitter', type=float, default=0)
ap.add_argument('--loss', type=float, default=0)
ap.add_argument('--seed', type=int, default=1)
ap.add_argument('--seconds', type=float, default=600)
ap.add_argument('--idle', type=float, default=5)  # ends this many s after the last packet
ap.add_argument('--every', type=float, default=30)  # s between stats lines
o = ap.parse_args()
rng = random.Random(o.seed)
sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sa.bind(('127.0.0.1', o.a))
sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sb.bind(('127.0.0.1', o.b))
route = {sa: (sb, ('127.0.0.1', o.p2)), sb: (sa, ('127.0.0.1', o.p1))}
q = []  # (due, seq, out socket, dest, data)
seq = 0; got = 0; lost = 0; sent = 0; last = 0.0
end = time.monotonic() + o.seconds
print(f'relay {o.a}<->{o.b} delay {o.delay} jitter {o.jitter} loss {o.loss}', flush=True)
# m4relay.sh kills relays (TerminateProcess: no end line), so counts are also logged every --every s
tick = time.monotonic() + o.every
while time.monotonic() < end and not (got and not q and time.monotonic() - last > o.idle):
    now = time.monotonic()
    if now >= tick:
        tick = now + o.every
        print(f'relay stats: packets in {got} lost {lost} out {sent}', flush=True)
    while q and q[0][0] <= now:
        _, _, s, d, data = heapq.heappop(q)
        try:
            s.sendto(data, d); sent += 1
        except OSError:
            pass
    wait = max(0.0, min(0.05, q[0][0] - now)) if q else 0.05
    r, _, _ = select.select([sa, sb], [], [], wait)
    for s in r:
        try:
            data, _ = s.recvfrom(65536)
        except OSError:  # 10054: peer port closed
            continue
        got += 1; last = time.monotonic()
        if rng.random() < o.loss:
            lost += 1; continue
        ms = max(0.0, o.delay + rng.uniform(-o.jitter, o.jitter))
        out, dest = route[s]
        seq += 1
        heapq.heappush(q, (time.monotonic() + ms / 1000, seq, out, dest, data))
print(f'relay end: packets in {got} lost {lost} out {sent}', flush=True)
