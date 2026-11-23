import wave, sys
src, dst, start, dur = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
with wave.open(src) as w:
    sr = w.getframerate(); w.setpos(int(start*sr)); data = w.readframes(int(dur*sr)); p = w.getparams()
with wave.open(dst, 'wb') as o:
    o.setparams(p); o.writeframes(data)
