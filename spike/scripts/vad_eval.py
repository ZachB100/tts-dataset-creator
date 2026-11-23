"""Compare Silero VAD (centisecond) speech segments against RMS-energy silence gaps.
usage: python3 -I vad_eval.py AUDIO.wav VAD.txt"""
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import rms_db, gaps_from
wav, vt = sys.argv[1:3]
segs = [(float(a)/100, float(b)/100) for a, b in re.findall(r"start = ([\d.]+), end = ([\d.]+)", open(vt).read())]
vgaps = [(segs[i][1], segs[i+1][0]) for i in range(len(segs)-1)]
db = rms_db(wav); s = sorted(db); floor = s[int(.1*len(s))]; pk = s[int(.98*len(s))]
thr = floor + .25*(pk-floor)
gaps = [g for g in gaps_from(db, thr) if g[1]-g[0] >= 0.25]   # pauses long enough to be sentence gaps
hit, so, eo, contained = 0, [], [], 0
for g in gaps:
    m = [v for v in vgaps if v[0] < g[1] and v[1] > g[0]]
    if m:
        hit += 1; v = max(m, key=lambda v: min(v[1], g[1]) - max(v[0], g[0]))
        so.append(v[0]-g[0]); eo.append(v[1]-g[1])
        if g[0]-0.05 <= v[0] and v[1] <= g[1]+0.05: contained += 1
med = lambda x: round(sorted(x)[len(x)//2]*1000) if x else None
p90 = lambda x: round(sorted(abs(v) for v in x)[int(.9*len(x))]*1000) if x else None
print(f"{wav}: rms_gaps>=250ms={len(gaps)} vad_gaps={len(vgaps)} matched={hit} ({100*hit/len(gaps):.0f}%) "
      f"vad_gap_within_rms_gap(+-50ms)={contained} | speech-end offset median={med(so)}ms p90|.|={p90(so)}ms "
      f"| speech-start offset median={med(eo)}ms p90|.|={p90(eo)}ms")
