"""Spike analysis: sentence-end word timestamp quality vs RMS energy, plus filler stats.

usage: python3 -I analyze.py AUDIO.wav WORDS.json [LABEL]
WORDS.json = [{"word": str, "start": s, "end": s}, ...]  (see convert.py)
Prints one JSON line of metrics to stdout.
"""
import json, math, os, re, sys, wave, array

FRAME = 0.010  # 10 ms hop
WIN = 0.020    # 20 ms window
MIN_GAP = 0.10 # silence run that counts as a pause
TOL = float(os.environ.get("TOL", "0.050"))  # tolerance at gap edges
SENT_END = re.compile(r"[.?!][\"'”’)\]]*$")


def rms_db(path):
    with wave.open(path) as w:
        sr = w.getframerate(); n = w.getnframes()
        a = array.array("h"); a.frombytes(w.readframes(n))
    hop = int(sr * FRAME); win = int(sr * WIN)
    out = []
    for i in range(0, len(a) - win, hop):
        s = 0
        for v in a[i:i + win:2]:  # decimate by 2 for speed, fine for RMS
            s += v * v
        r = math.sqrt(s / (win / 2)) / 32768.0
        out.append(20 * math.log10(r + 1e-9))
    return out


def gaps_from(db, thr):
    gaps, start = [], None
    for i, v in enumerate(db + [999]):
        if v < thr and start is None:
            start = i
        elif v >= thr and start is not None:
            if (i - start) * FRAME >= MIN_GAP:
                gaps.append((start * FRAME + WIN / 2, i * FRAME + WIN / 2))
            start = None
    return gaps


def main():
    wav, words_path = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 else words_path
    words = json.load(open(words_path))
    db = rms_db(wav)
    s = sorted(db)
    floor = s[int(0.10 * len(s))]
    peakish = s[int(0.98 * len(s))]
    # THR=strict: 25% of floor..speech range (counts low decay tails as speech)
    # THR=<number>: absolute dBFS threshold
    mode = os.environ.get("THR", "strict")
    thr = floor + 0.25 * (peakish - floor) if mode == "strict" else float(mode)
    gaps = gaps_from(db, thr)

    def frame_db(t):
        i = min(max(int(t / FRAME), 0), len(db) - 1)
        return db[i]

    res = {"in_gap": 0, "early": 0, "late": 0, "no_gap": 0, "cuttable": 0}
    early_err, late_err, offsets = [], [], []
    n = 0
    for k, w in enumerate(words[:-1]):
        if not SENT_END.search(w["word"].strip()):
            continue
        n += 1
        t, nxt = w["end"], words[k + 1]["start"]
        # the real pause that belongs to this boundary: nearest gap to t within 1 s
        cand = [g for g in gaps if g[1] > t - 1.0 and g[0] < max(t, nxt) + 1.0]
        if not cand:
            res["no_gap"] += 1
            continue
        g = min(cand, key=lambda g: 0 if g[0] <= t <= g[1] else min(abs(t - g[0]), abs(t - g[1])))
        offsets.append(t - g[0])
        if g[0] - TOL <= t <= g[1] + 0.020:
            res["in_gap"] += 1
        elif t < g[0]:
            res["early"] += 1; early_err.append(g[0] - t)
        else:
            res["late"] += 1; late_err.append(t - g[1])
        # would a "quietest point between end and next start" search land in silence?
        lo, hi = min(t, nxt), max(t, nxt)
        if hi - lo < FRAME:
            lo, hi = lo - 0.05, hi + 0.05
        q = min(frame_db(lo + i * FRAME) for i in range(int((hi - lo) / FRAME) + 1))
        if q < thr:
            res["cuttable"] += 1

    def med(x):
        x = sorted(x); return round(x[len(x) // 2] * 1000) if x else None

    toks = [re.sub(r"[^\w'-]", "", w["word"].lower()) for w in words]
    fillers = sum(t in {"uh", "um", "uhm", "er", "erm", "ah", "eh", "hmm", "mm", "mhm", "uh-huh"} for t in toks)
    reps = sum(1 for a, b in zip(toks, toks[1:]) if a and a == b)
    frags = sum(1 for w in words if w["word"].strip().endswith("-"))
    out = {
        "label": label, "words": len(words), "sentence_ends": n,
        "pct_end_in_silence": round(100 * res["in_gap"] / n, 1) if n else None,
        "pct_end_inside_speech_early": round(100 * res["early"] / n, 1) if n else None,
        "pct_end_past_gap_late": round(100 * res["late"] / n, 1) if n else None,
        "no_gap_found": res["no_gap"],
        "pct_quietest_point_is_silent": round(100 * res["cuttable"] / n, 1) if n else None,
        "median_early_ms": med(early_err), "median_late_ms": med(late_err),
        "median_end_minus_gapstart_ms": med(offsets),
        "fillers": fillers, "repeats": reps, "fragments": frags,
        "silence_thr_db": round(thr, 1), "gaps": len(gaps),
    }
    print(json.dumps(out))


if __name__ == "__main__":
    main()
