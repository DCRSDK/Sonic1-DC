#!/usr/bin/env python3
# dtex_convert.py — RSDKv4 Dreamcast native-UI texture pre-converter.
#
# Converts the menu / model PNGs (Data/Game/Menu, Data/Game/Models) into small
# ".dtex" files that the Dreamcast reads straight into VRAM — no runtime PNG
# decode (stbi_load fails on the DC's fragmented heap for anything bigger than
# ~256x256, which is why most menu/model textures were invisible).
#
# .dtex layout (little-endian):
#   0  char[4]  magic  = 'DTEX'
#   4  u8       version = 1
#   5  u8       format  = 1 (ARGB4444) | 2 (ARGB1555)
#   6  u16      origW           (source image width)
#   8  u16      origH
#   10 u16      potW            (power-of-two, padded)
#   12 u16      potH
#   14 u16      reserved = 0
#   16 ...      potW*potH*2 bytes, 16bpp, NON-twiddled (linear, row-major),
#               top-left = source; padded region = 0.
#
# Format is chosen per image: ARGB1555 (crisp 5-bit colour, 1-bit alpha) when
# the alpha channel is effectively binary, else ARGB4444 (4-bit colour, smooth
# alpha) for soft/glowing edges.
#
# The Dreamcast identifies the format by the 'DTEX' magic header, NOT the file
# extension, so the output may be named either "X.dtex" or kept as "X.png"
# (holding .dtex data — the same trick as the downsampled .ogg audio).
#
# Usage:
#   python3 dtex_convert.py [--out DIR] [--ext dtex|png] [--vq] [--maxdim N] <dir-or-file> ...
#
#   # simplest: write X.dtex next to each source PNG
#   python3 dtex_convert.py Data/Game/Menu Data/Game/Models
#
#   # write into a separate tree, named .png (drop this 'Data' folder onto the
#   # CD next to Data.rsdk); never touches your source PNGs:
#   python3 dtex_convert.py --out disc --ext png Data/Game/Menu Data/Game/Models
#   #   -> disc/Data/Game/Menu/CWLogo.png  (holding .dtex data)
#
#   # RECOMMENDED for the DC: VQ compression keeps the ORIGINAL resolution at
#   # ~1/8 the VRAM, so the whole menu set (incl. the character portraits) fits:
#   python3 dtex_convert.py --vq --out disc --ext png Data/Game/Menu Data/Game/Models
#
# --vq     native Dreamcast VQ (vector quantisation): a 256-entry codebook of
#          2x2 texel blocks + a 1-byte-per-block twiddled index. ~1/8 the VRAM of
#          plain 16bpp, full resolution preserved. Best for menu/portrait art
#          (flat colour + gradients compress cleanly; only photographic noise
#          suffers). VQ textures are padded to a SQUARE power-of-two.
# --maxdim N  cap source dimension before POT rounding (default 1024 = PVR max).
#          Only needed WITHOUT --vq, where the full set won't fit (use 512/256).

import sys, os, struct
from PIL import Image

OUT_DIR = None      # mirror sources under this root when set
OUT_EXT = ".dtex"   # ".dtex" or ".png"
MAX_DIM = 1024      # cap source width/height before power-of-two rounding.
                    # With VQ (--vq) each texture costs ~1/8 of 16bpp, so we can
                    # afford to keep the ORIGINAL resolution (1024 = PVR max). In
                    # plain 16bpp mode the whole menu set won't fit at 1024; use
                    # --maxdim 512 (or lower) there.
USE_VQ  = False     # --vq : vector-quantised textures (native DC VQ, ~1/8 VRAM,
                    # keeps resolution). Off by default (plain 16bpp, no clustering).
TOTAL_VRAM = 0      # running estimate of VRAM the converted set will occupy

MAGIC = b"DTEX"
FMT_ARGB4444    = 1  # plain 16bpp, non-twiddled linear
FMT_ARGB1555    = 2  # plain 16bpp, non-twiddled linear
FMT_VQ_ARGB4444 = 3  # VQ codebook (ARGB4444 texels) + twiddled index array
FMT_VQ_ARGB1555 = 4  # VQ codebook (ARGB1555 texels) + twiddled index array


def next_pot(v, lo=8, hi=1024):
    p = lo
    while p < v:
        p <<= 1
    return min(p, hi)


def pick_format(img):
    # Binary alpha -> 1555 (better colour); graded alpha -> 4444.
    a = img.getchannel("A")
    extrema = a.getextrema()  # (min, max)
    # sample: if every alpha is 0 or 255, treat as binary
    hist = a.histogram()
    mid = sum(hist[8:248])  # count of "partial" alpha pixels
    if mid == 0:
        return FMT_ARGB1555
    return FMT_ARGB4444


def to16(r, g, b, a, fmt):
    if fmt in (FMT_ARGB1555, FMT_VQ_ARGB1555):
        return ((1 if a >= 128 else 0) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    else:  # ARGB4444
        return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def _out_path(png_path):
    if OUT_DIR:
        # mirror the source path (e.g. Data/Game/Menu/X.png) under OUT_DIR
        rel = os.path.splitext(png_path)[0] + OUT_EXT
        rel = rel.lstrip("/\\")
        out = os.path.join(OUT_DIR, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
    else:
        out = os.path.splitext(png_path)[0] + OUT_EXT
    return out


def _to16_arr(px, fmt):
    # px: numpy uint array (..., 4) RGBA -> uint16 packed for the chosen format.
    import numpy as np
    r = px[..., 0].astype(np.uint16)
    g = px[..., 1].astype(np.uint16)
    b = px[..., 2].astype(np.uint16)
    a = px[..., 3].astype(np.uint16)
    if fmt in (FMT_ARGB1555, FMT_VQ_ARGB1555):
        return ((a >= 128).astype(np.uint16) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
    return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)


def _spread_bits(v):
    # Spread each bit i of v to bit 2i (the PVR twiddle interleave). v: numpy int array.
    import numpy as np
    r = np.zeros_like(v)
    for i in range(11):  # covers index grids up to 2048 wide (POT 1024 texture)
        r |= ((v >> i) & 1) << (2 * i)
    return r


def _kmeans_np(data, k, iters=16, seed=0):
    # Lloyd's k-means in pure numpy (no scipy). data: (n, dim) float32.
    # Vectorised nearest-centroid via ||x-c||^2 = ||x||^2 - 2 x.c + ||c||^2,
    # computed in row-chunks to bound memory. Centroid update via bincount.
    import numpy as np
    n, dim = data.shape
    rng = np.random.default_rng(seed)
    cent = data[rng.choice(n, size=k, replace=False)].astype(np.float32).copy()
    labels = np.zeros(n, dtype=np.int64)
    for _ in range(iters):
        c2 = (cent * cent).sum(1)[None, :]              # (1, k)
        for s in range(0, n, 8192):
            e = min(n, s + 8192)
            d = -2.0 * (data[s:e] @ cent.T) + c2        # (chunk, k); +||x||^2 is const per row
            labels[s:e] = d.argmin(1)
        counts = np.bincount(labels, minlength=k).astype(np.float32)
        sums = np.empty((k, dim), dtype=np.float32)
        for c in range(dim):
            sums[:, c] = np.bincount(labels, weights=data[:, c], minlength=k)
        nz = counts > 0
        newc = cent.copy()
        newc[nz] = sums[nz] / counts[nz, None]
        if np.array_equal(newc, cent):
            break
        cent = newc
    return cent, labels


def convert_vq(png_path, img, w, h, fmt16):
    # Native Dreamcast VQ: a 256-entry codebook of 2x2 texel blocks + a twiddled
    # index array (1 byte/block). ~1/8 the VRAM of plain 16bpp at full resolution.
    #
    # Layout written (streamed straight to VRAM on the DC):
    #   [ 2048-byte codebook ][ (P/2 * P/2) index bytes, twiddled ]
    # The texture is forced to a SQUARE power-of-two P (P = next_pot(max(w,h))) so
    # the index-grid twiddle is the simple bit-interleave (no rectangular tiling).
    import numpy as np

    global TOTAL_VRAM
    P = next_pot(max(w, h))            # square POT
    N = P // 2                         # blocks per side

    # Pad to PxP (transparent), RGBA uint8.
    canvas = Image.new("RGBA", (P, P), (0, 0, 0, 0))
    canvas.paste(img, (0, 0))
    px = np.asarray(canvas, dtype=np.uint8)   # shape (P, P, 4), [y, x]

    # Gather the 4 texels of each 2x2 block in DC codebook slot order:
    #   slot0=(x0,y0) slot1=(x0,y1) slot2=(x1,y0) slot3=(x1,y1)   (y fastest)
    p0 = px[0::2, 0::2]   # (N, N, 4)  (x0,y0)
    p1 = px[1::2, 0::2]   #            (x0,y1)
    p2 = px[0::2, 1::2]   #            (x1,y0)
    p3 = px[1::2, 1::2]   #            (x1,y1)
    blocks = np.concatenate([p0, p1, p2, p3], axis=2).reshape(N * N, 16).astype(np.float32)

    # Codebook via k-means. If the image has <=256 distinct blocks, use them
    # exactly (lossless index) instead of clustering.
    uniq = np.unique(blocks, axis=0)
    if len(uniq) <= 256:
        centroids = uniq.astype(np.float32)
        # map each block to its exact unique row
        lut = {row.tobytes(): i for i, row in enumerate(uniq.astype(np.float32))}
        labels = np.fromiter((lut[b.tobytes()] for b in blocks.astype(np.float32)),
                             dtype=np.int64, count=N * N)
    else:
        centroids, labels = _kmeans_np(blocks, 256, iters=16, seed=0)

    # Pad the codebook to a full 256 entries (unused entries are transparent).
    cb = np.zeros((256, 16), dtype=np.float64)
    cb[: centroids.shape[0]] = centroids
    cb = np.clip(np.rint(cb), 0, 255).astype(np.uint16).reshape(256, 4, 4)  # (256, 4 texels, RGBA)
    cb16 = _to16_arr(cb, fmt16)  # (256, 4) uint16, texels already in slot order

    codebook = bytearray(2048)
    for e in range(256):
        for s in range(4):
            v = int(cb16[e, s])
            o = e * 8 + s * 2
            codebook[o] = v & 0xFF
            codebook[o + 1] = (v >> 8) & 0xFF

    # Twiddle the index array: block (bx, by) -> offset TWIDOUT(bx, by), i.e.
    # spread(by) | (spread(bx) << 1) — x bits in the odd positions (KOS convention).
    twid = _spread_bits(np.arange(N))
    off = (twid[None, :] << 1) | twid[:, None]     # off[by, bx] = (spread(bx)<<1)|spread(by)
    lab2d = labels.reshape(N, N)                    # lab2d[by, bx]
    index = np.zeros(N * N, dtype=np.uint8)
    index[off.ravel()] = lab2d.ravel().astype(np.uint8)

    payload = bytes(codebook) + index.tobytes()
    # fmt byte: VQ variant; potW=potH=P so the DC computes 2048 + (P/2)*(P/2).
    fmt_out = FMT_VQ_ARGB1555 if fmt16 in (FMT_ARGB1555, FMT_VQ_ARGB1555) else FMT_VQ_ARGB4444
    header = struct.pack("<4sBBHHHHH", MAGIC, 1, fmt_out, w, h, P, P, 0)

    out = _out_path(png_path)
    with open(out, "wb") as f:
        f.write(header)
        f.write(payload)
    vram = len(payload)
    TOTAL_VRAM += vram
    fmtname = "VQ/1555" if fmt_out == FMT_VQ_ARGB1555 else "VQ/4444"
    print(f"  {os.path.basename(png_path):28s} {w}x{h} -> {P}x{P} {fmtname}  (VRAM {vram//1024} KB) -> {out}")


def convert(png_path):
    global TOTAL_VRAM
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    cap = min(MAX_DIM, 1024)  # PVR hard max is 1024
    if w > cap or h > cap:
        scale = min(cap / float(w), cap / float(h))
        img = img.resize((max(1, int(w * scale)), max(1, int(h * scale))), Image.LANCZOS)
        w, h = img.size
    fmt = pick_format(img)

    if USE_VQ:
        convert_vq(png_path, img, w, h, fmt)
        return

    potw, poth = next_pot(w), next_pot(h)

    px = img.load()
    # Linear, non-twiddled, row-major; padded region is 0 (transparent).
    buf = bytearray(potw * poth * 2)
    for y in range(h):
        row = (y * potw) * 2
        for x in range(w):
            r, g, b, a = px[x, y]
            v = to16(r, g, b, a, fmt)
            o = row + x * 2
            buf[o] = v & 0xFF
            buf[o + 1] = (v >> 8) & 0xFF

    header = struct.pack("<4sBBHHHHH", MAGIC, 1, fmt, w, h, potw, poth, 0)
    out = _out_path(png_path)
    with open(out, "wb") as f:
        f.write(header)
        f.write(buf)
    TOTAL_VRAM += potw * poth * 2
    fmtname = "ARGB1555" if fmt == FMT_ARGB1555 else "ARGB4444"
    print(f"  {os.path.basename(png_path):28s} {w}x{h} -> {potw}x{poth} {fmtname}  (VRAM {potw*poth*2//1024} KB) -> {out}")


def walk(path):
    if os.path.isfile(path) and path.lower().endswith(".png"):
        convert(path)
        return
    for root, _dirs, files in os.walk(path):
        for fn in files:
            if fn.lower().endswith(".png"):
                convert(os.path.join(root, fn))


if __name__ == "__main__":
    args = sys.argv[1:]
    inputs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--out":
            OUT_DIR = args[i + 1]
            i += 2
        elif a == "--ext":
            e = args[i + 1].lstrip(".").lower()
            OUT_EXT = ".png" if e == "png" else ".dtex"
            i += 2
        elif a == "--maxdim":
            MAX_DIM = int(args[i + 1])
            i += 2
        elif a == "--vq":
            USE_VQ = True
            i += 1
        elif a in ("-h", "--help"):
            print(__doc__)
            sys.exit(0)
        else:
            inputs.append(a)
            i += 1
    if not inputs:
        print(__doc__)
        sys.exit(1)
    if USE_VQ:
        try:
            import numpy  # noqa: F401  (VQ clustering needs numpy; plain mode does not)
        except ImportError:
            print("[dtex] ERROR: --vq needs numpy (scipy is NOT required).\n"
                  "       Install it with:  pip3 install numpy\n"
                  "       (or:  sudo apt install python3-numpy )\n"
                  "       Or drop --vq to use plain 16bpp (add --maxdim 512 so the set fits).")
            sys.exit(1)
    print(f"[dtex] ext={OUT_EXT}  maxdim={MAX_DIM}  vq={'on' if USE_VQ else 'off'}  out={'in-place' if not OUT_DIR else OUT_DIR}")
    for p in inputs:
        print(f"[dtex] {p}")
        walk(p)
    print(f"[dtex] done — total VRAM if ALL resident at once: {TOTAL_VRAM // 1024} KB "
          f"(DC has ~4000 KB free for menu textures; the menu doesn't need every one at once)")
