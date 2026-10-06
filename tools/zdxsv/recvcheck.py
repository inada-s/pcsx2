"""python recvcheck.py nettrace.txt (ZDXSV_NET_TRACE with R lines, pcsx2 9e93cd13+): R slots the local k table
flagged (? unknown, x X differs): frames until the local game sent that counter, k/X equal then?, long holds."""
import collections
import sys
S, R = [], []
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) < 3 or p[1] not in 'SR':
        continue
    (S if p[1] == 'S' else R).append((int(p[0]), p[3:]))
sends = collections.defaultdict(list)  # c -> frames
xs = []  # (frame, X) of own records
for f, sl in S:
    for s in sl:
        q = s.split(':')
        if len(q) >= 2 and q[0] != 'bad':
            sends[int(q[0], 16)].append(f)
        if len(q) == 4:
            xs.append((f, q[2]))
dl, xl = collections.Counter(), []
for f, sl in R:
    for s in sl:
        if s.endswith('?'):
            c = int(s.split(':')[0], 16)
            nxt = [g for g in sends[c] if g >= f]
            dl[(nxt[0] - f) if nxt else None] += 1
        if s.endswith('x'):
            xl.append((f, s))
print('unknown: frames until own send of c', sorted(dl.items(), key=lambda t: (t[0] is None, t[0] or 0))[:15])
print('first R frame', R[0][0], 'first S frame', S[0][0], 'unknown frames range', )
print('xbad', xl[:8])
xch = [(f, x) for i, (f, x) in enumerate(xs) if i == 0 or xs[i - 1][1] != x]
print('own X changes', len(xch), xch[:12])
# deferred check: '?' k vs the own k sent later for that counter; 'x' X vs the next own record's X
own = collections.defaultdict(list)  # c -> [(frame, k)]
for f, sl in S:
    for s in sl:
        q = s.split(':')
        if len(q) >= 2 and q[0] != 'bad':
            own[int(q[0], 16)].append((f, q[1]))
ok = bad = 0
for f, sl in R:
    for s in sl:
        if s.endswith('?'):
            q = s[:-1].split(':')
            nxt = [k for g, k in own[int(q[0], 16)] if g >= f]
            if nxt:
                ok, bad = ok + (nxt[0] == q[1]), bad + (nxt[0] != q[1])
xok = sum(1 for f, s in xl if [x for g, x in xs if g >= f][:1] == [s.split(':')[2]])
print('unknown k vs later own k: equal', ok, 'differ', bad, '| xbad X == next own X', xok, 'of', len(xl))
long = sorted({f for f, sl in R for s in sl if s.endswith('?') and
               ([g for g in sends[int(s.split(':')[0], 16)] if g >= f][:1] or [10**9])[0] - f >= 50})
print('long holds at frames', long[:20], 'last S frame', S[-1][0], 'last R frame', R[-1][0])
