"""Normalise backend outputs to [{word,start,end}] (seconds).

usage:
  python3 -I convert.py crisper IN.json OUT.json
  python3 -I convert.py wcli    IN.json OUT.json        # whisper-cli -ojf -ml 1 -sow, segment offsets
  python3 -I convert.py wcli-dtw IN.json OUT.json       # same file, DTW token times (t_dtw)
"""
import json, sys

kind, src, dst = sys.argv[1:4]
d = json.load(open(src))
words = []
if kind == "crisper":
    words = [{"word": w["word"], "start": w["start"], "end": w["end"]} for w in d["words"]]
else:
    segs = [s for s in d["transcription"] if s["text"].strip()]
    for s in segs:
        toks = [t for t in s.get("tokens", []) if not t["text"].startswith("[_")]
        w = {"word": s["text"].strip(),
             "start": s["offsets"]["from"] / 1000, "end": s["offsets"]["to"] / 1000}
        if kind == "wcli-dtw" and toks and all("t_dtw" in t for t in toks):
            w["start"] = toks[0]["t_dtw"] / 100
            # t_dtw is a token onset; the onset of the trailing punctuation token
            # is the best in-segment estimate of the word's end.
            w["end"] = toks[-1]["t_dtw"] / 100 if len(toks) > 1 else None
        words.append(w)
    if kind == "wcli-dtw":
        for i, w in enumerate(words):
            if w["end"] is None or w["end"] <= w["start"]:
                w["end"] = words[i + 1]["start"] if i + 1 < len(words) else w["start"] + 0.3
json.dump(words, open(dst, "w"), indent=0)
print(f"{len(words)} words -> {dst}")
