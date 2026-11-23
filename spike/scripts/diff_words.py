"""Word-level diff of two transcripts. usage: python3 -I diff_words.py A.words.json B.words.json"""
import json, re, sys, difflib
norm = lambda w: re.sub(r"[^\w']", "", w.lower())
a = [norm(w["word"]) for w in json.load(open(sys.argv[1]))]; b = [norm(w["word"]) for w in json.load(open(sys.argv[2]))]
a = [x for x in a if x]; b = [x for x in b if x]
sm = difflib.SequenceMatcher(a=a, b=b, autojunk=False)
ops = [o for o in sm.get_opcodes() if o[0] != "equal"]
print(f"A={len(a)} B={len(b)} word-match-ratio={sm.ratio():.3f} diff-ops={len(ops)}")
for tag, i1, i2, j1, j2 in ops:
    print(f"  {tag:8s} A:{' '.join(a[i1:i2])!r:40s} B:{' '.join(b[j1:j2])!r}")
