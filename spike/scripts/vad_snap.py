"""Snap each sentence-end to the nearest Silero VAD gap; check the gap is the right pause
(lies after the sentence-final word's start and before the next word's end) and is silent.
usage: python3 -I vad_snap.py AUDIO.wav WORDS.json VAD.txt"""
import json, re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import rms_db, SENT_END
wav, wp, vt = sys.argv[1:4]
words = json.load(open(wp))
segs = [(float(a)/100, float(b)/100) for a, b in re.findall(r"start = ([\d.]+), end = ([\d.]+)", open(vt).read())]
vg = [(segs[i][1], segs[i+1][0]) for i in range(len(segs)-1)]
db = rms_db(wav)
n = ok = silent = 0
for k, w in enumerate(words[:-1]):
    if not SENT_END.search(w["word"].strip()): continue
    n += 1; t = w["end"]; nx = words[k+1]
    g = min(vg, key=lambda g: 0 if g[0] <= t <= g[1] else min(abs(t-g[0]), abs(t-g[1])))
    if g[0] >= w["start"] - 0.05 and g[1] <= nx["end"] + 0.05: ok += 1
    q = min(db[int(g[0]/0.01):int(g[1]/0.01)+1] or [0])
    if q < -45: silent += 1
print(f"{os.path.basename(wp)}: sentence_ends={n} nearest_vad_gap_is_right_pause={100*ok/n:.1f}% quietest_point_in_gap<-45dBFS={100*silent/n:.1f}%")
