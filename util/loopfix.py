#!/usr/bin/env python3
# loopfix.py — crossfade de loop em PCM s16le estereo 44100 Hz
# uso: python3 loopfix.py entrada.pcm saida.pcm [ms_crossfade]
import sys, struct, math

entrada, saida = sys.argv[1], sys.argv[2]
ms = int(sys.argv[3]) if len(sys.argv) > 3 else 100
N = int(44100 * ms / 1000)          # frames de crossfade

raw = open(entrada, 'rb').read()
raw = raw[:len(raw) - (len(raw) % 4)]          # multiplo de 4 bytes
x = list(struct.unpack('<%dh' % (len(raw)//2), raw))
L = len(x) // 2                                 # total de frames
assert L > 3*N, "arquivo curto demais para esse crossfade"

def frame(i):  return (x[2*i], x[2*i+1])

out = []
# corpo: do frame N ate L-N (pulamos o inicio que sera fundido no fim)
for i in range(N, L - N):
    out += [x[2*i], x[2*i+1]]
# costura: fim (fade-out) + inicio (fade-in), equal-power
for k in range(N):
    g_out = math.cos(0.5 * math.pi * k / N)     # 1 -> 0
    g_in  = math.sin(0.5 * math.pi * k / N)     # 0 -> 1
    tl, tr = frame(L - N + k)                   # cauda
    hl, hr = frame(k)                           # cabeca
    for t, h in ((tl, hl), (tr, hr)):
        v = int(t * g_out + h * g_in)
        out.append(max(-32768, min(32767, v)))

open(saida, 'wb').write(struct.pack('<%dh' % len(out), *out))
print(f"OK: {L} -> {len(out)//2} frames, crossfade {ms} ms")
