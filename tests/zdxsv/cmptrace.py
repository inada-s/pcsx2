"""python cmptrace.py nettrace.txt bridge-<user>.txt: the game's battle sends seen at the EE RPC hook
(ZDXSV_NET_TRACE) vs the UDP bridge dump (ZDXSV_UDP_DUMP, S lines): byte streams + frame offsets."""
import collections
import sys

hook = [(int(l.split()[0]), l.split()[2]) for l in open(sys.argv[1]) if ' S ' in l]
br = [(int(l.split()[0]), l.split()[-1]) for l in open(sys.argv[2]) if l.split()[2] == 'S']
hs, bs = ''.join(h for _, h in hook), ''.join(h for _, h in br)
at = hs.find(bs[:200])
print('hook sends', len(hook), 'bridge sends', len(br), 'bridge stream at hook offset', at // 2)
tail = hs[at:]
print('bridge stream == hook stream from there:', tail[:len(bs)] == bs, 'lens', len(tail) // 2, len(bs) // 2)
# frame of each bridge send vs the hook send with the same bytes (in order)
d, j = collections.Counter(), 0
for f, h in br:
    while j < len(hook) and hook[j][1] != h:
        j += 1
    if j < len(hook):
        d[f - hook[j][0]] += 1
        j += 1
print('bridge frame - hook frame', d.most_common(5))
bad = sum(1 for l in open(sys.argv[1]) if ' bad' in l)
print('unparsed key msgs', bad)
