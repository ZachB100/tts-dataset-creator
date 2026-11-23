"""Gap-selection heuristic B: among Silero VAD gaps overlapping [end-0.2s, next.start+0.3s],
pick the longest; fall back to nearest. Reference = the pause turbo+DTW picked (DTW was 100% self-consistent),
so this measures agreement with stock turbo+DTW on which pause to cut in.
usage: python3 -I vad_snap2.py WORDS.json REF_WORDS.json VAD.txt"""
import json, re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import SENT_END
wp, rp, vt = sys.argv[1:4]
segs = [(float(a)/100, float(b)/100) for a, b in re.findall(r"start = ([\d.]+), end = ([\d.]+)", open(vt).read())]
vg = [(segs[i][1], segs[i+1][0]) for i in range(len(segs)-1)]
near = lambda t: min(vg, key=lambda g: 0 if g[0] <= t <= g[1] else min(abs(t-g[0]), abs(t-g[1])))
def picks(words, heur):
    out = []
    for k, w in enumerate(words[:-1]):
        if not SENT_END.search(w["word"].strip()): continue
        t, nx = w["end"], words[k+1]["start"]
        c = [g for g in vg if g[1] > t-0.2 and g[0] < max(t, nx)+0.3] if heur else []
        out.append((w["word"].lower().strip('.?!,"'), max(c, key=lambda g: g[1]-g[0]) if c else near(t)))
    return out
ref = picks(json.load(open(rp)), False)
for heur in (False, True):
    p = picks(json.load(open(wp)), heur); refset = {g for _, g in ref}
    agree = sum(g in refset for _, g in p)
    print(f"{os.path.basename(wp)} {'longest-gap' if heur else 'nearest-gap'}: {agree}/{len(p)} = {100*agree/len(p):.1f}% same pause as turbo+DTW")
