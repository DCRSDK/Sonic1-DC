#!/usr/bin/env python3
"""Extract assets from an RSDKv4 (Sonic 1 / Sonic 2 2013) Data.rsdk.

RSDKv3 packs store plaintext paths, so Sonic CD's extractor can walk a
directory table. RSDKv4 stores only an MD5 of each (lowercased) path, so there
is nothing to enumerate -- a name has to be known before it can be looked up.

So this bootstraps instead. Data/Game/GameConfig.bin is at a path we know, and
it names the global sound effects and lists every stage folder; each stage's
StageConfig.bin then names that stage's sound effects. Music and the 2013 menu
textures are the classes the pack cannot tell us about -- RSDKv4 sets tracks
from script bytecode and the native UI loads its textures from string literals
in C++ -- so those names come from a candidate list, and anything absent is
simply skipped.

By default it writes exactly what the Dreamcast build needs loose beside
Data.rsdk:

    Music/                  streamed as raw PCM (no Vorbis decoder on-console)
    SoundFX/Global/         raw PCM, software-mixed
    Game/Menu/              native UI textures, converted to .dtex
    Game/Models/            ditto, the About-screen box art

Everything else -- sprites, tilesets, scripts, palettes, stage sfx -- stays in
the pack and is read from there.

Usage:
    rsdkv4_extract.py <Data.rsdk> <outdir> [--all] [--quiet]

    --all            extract every candidate name, not just the four classes
                     above. Useful for looking around; the disc does not want
                     it, and note that it writes a loose Game/GameConfig.bin,
                     which on the disc would override the packed one.
    --names=<file>   candidate paths, one per line. Defaults to
                     rsdkv4_filelist.txt beside this script.
"""
import hashlib, os, struct, sys

# ---------------------------------------------------------------- pack format
MAGIC = b"RSDKvB"

def md5_words(path):
    """The four uint32s CheckFileInfo compares, from GenerateMD5FromString.

    That function returns MD5's a/b/c/d state words, i.e. each 4-byte group of
    the digest read LITTLE-endian. The pack stores the same value BIG-endian,
    so a group in the file is the byte-reverse of the digest's.
    """
    d = hashlib.md5(path.lower().encode("ascii")).digest()
    return tuple(int.from_bytes(d[i:i + 4], "little") for i in range(0, 16, 4))

class Pack:
    def __init__(self, path):
        self.f = open(path, "rb")
        if self.f.read(6) != MAGIC:
            raise SystemExit(f"{path} is not an RSDKv4 datapack (no RSDKvB magic).")
        count, = struct.unpack("<H", self.f.read(2))
        self.entries = {}
        for _ in range(count):
            h = struct.unpack(">4I", self.f.read(16))          # stored big-endian
            offset, size = struct.unpack("<II", self.f.read(8))
            self.entries[h] = (offset, size & 0x7FFFFFFF, bool(size & 0x80000000))
        self.count = count

    def has(self, path):
        return md5_words(path) in self.entries

    def read(self, path):
        e = self.entries.get(md5_words(path))
        if e is None:
            return None
        offset, size, enc = e
        self.f.seek(offset)
        buf = self.f.read(size)
        if len(buf) != size:
            raise SystemExit(f"{path}: pack is truncated ({len(buf)} of {size} bytes).")
        return decrypt(buf, size) if enc else buf

# ---------------------------------------------------------------- decryption
# Transcribed from FileRead() in RSDKv4/Reader.cpp. The key schedule branches on
# its own position counters, so it cannot be expressed as a simple keystream --
# it has to be stepped a byte at a time exactly as the engine does.
ENC_KEY_1 = 0xAAAAAAAB
ENC_KEY_2 = 0x24924925

def _mul_hi(a, b):
    return (a * b) >> 32

def _load_keys(key1, key2):
    """GenerateELoadKeys: MD5 of the DECIMAL STRING of each key, each 4-byte
    group of the digest emitted big-endian."""
    out = []
    for k in (key1, key2):
        d = hashlib.md5(str(k).encode("ascii")).digest()
        s = bytearray(16)
        for i in range(4):
            w = int.from_bytes(d[i * 4:i * 4 + 4], "little")
            for j in range(4):
                s[i * 4 + j] = (w >> (8 * (j ^ 3))) & 0xFF
        out.append(bytes(s))
    return out

def decrypt(buf, size):
    a, b = _load_keys(size, (size >> 1) + 1)
    no, pa, pb, swap = (size & 0x1FC) >> 2, 0, 8, 0
    out = bytearray(len(buf))
    for i, c in enumerate(buf):
        v = b[pb] ^ no ^ c
        if swap:
            v = ((v << 4) + (v >> 4)) & 0xFF
        v ^= a[pa]
        out[i] = v
        pa += 1
        pb += 1
        if pa <= 0x0F:
            if pb > 0x0C:
                pb = 0
                swap ^= 1
        elif pb <= 0x08:
            pa = 0
            swap ^= 1
        else:
            no = (no + 2) & 0x7F
            k1 = _mul_hi(ENC_KEY_1, no)
            k2 = _mul_hi(ENC_KEY_2, no)
            t1 = k2 + (no - k2) // 2
            t2 = k1 // 8 * 3
            if swap:
                swap = 0
                pa = no - t1 // 4 * 7
                pb = no - t2 * 4 + 2
            else:
                swap = 1
                pb = no - t1 // 4 * 7
                pa = no - t2 * 4 + 3
    return bytes(out)

# ------------------------------------------------------------ config parsing
class Reader:
    """Just enough of a cursor to walk the length-prefixed config formats."""
    def __init__(self, data):
        self.d, self.p = data, 0
    def u8(self):
        v = self.d[self.p]; self.p += 1; return v
    def skip(self, n):
        self.p += n
    def pstr(self):
        n = self.u8()
        s = self.d[self.p:self.p + n].decode("ascii", "replace")
        self.p += n
        return s

def parse_gameconfig(data):
    """Layout from RetroEngine::LoadGameConfig."""
    r = Reader(data)
    title, desc = r.pstr(), r.pstr()
    r.skip(0x60 * 3)                                  # palette
    nobj = r.u8()
    for _ in range(nobj): r.pstr()                    # object names
    for _ in range(nobj): r.pstr()                    # script paths
    nvar = r.u8()
    for _ in range(nvar):
        r.pstr(); r.skip(4)                           # name, value
    nsfx = r.u8()
    for _ in range(nsfx): r.pstr()                    # sfx names
    sfx = [r.pstr() for _ in range(nsfx)]             # sfx paths
    nplr = r.u8()
    for _ in range(nplr): r.pstr()                    # player names
    stages = []
    for _ in range(4):                                # four stage categories
        n = r.u8()
        for _ in range(n):
            folder, sid, name = r.pstr(), r.pstr(), r.pstr()
            r.u8()                                    # highlighted
            stages.append(folder)
    return title, desc, sfx, stages

def parse_stageconfig(data):
    """Layout from LoadStageFiles' StageConfig read."""
    r = Reader(data)
    r.u8()                                            # load globals
    r.skip(0x20 * 3)                                  # palette rows 0x60..0x80
    n = r.u8()
    for _ in range(n): r.pstr()                       # sfx names
    return [r.pstr() for _ in range(n)]               # sfx paths

# ------------------------------------------------------------------ main
def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if len(args) != 2:
        raise SystemExit(__doc__)
    rsdk, outdir = args
    quiet = "--quiet" in flags
    names_file = None
    for a in flags:
        if a.startswith("--names="):
            names_file = a.split("=", 1)[1]
    if names_file is None:
        here = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "rsdkv4_filelist.txt")
        if os.path.isfile(here):
            names_file = here
    def say(*a):
        if not quiet: print(*a)

    pack = Pack(rsdk)
    say(f"-- {os.path.basename(rsdk)}: {pack.count} entries")

    gc = pack.read("Data/Game/GameConfig.bin")
    if gc is None:
        raise SystemExit("Data/Game/GameConfig.bin is not in this pack -- is it an "
                         "RSDKv4 datafile from Sonic 1 or Sonic 2 (2013)?")
    title, desc, gsfx, stages = parse_gameconfig(gc)
    say(f"-- {title.strip()}")
    say(f"-- {len(gsfx)} global sfx, {len(stages)} stages listed")

    # Candidates come from two independent places and are unioned, because
    # neither alone is complete: GameConfig names the sfx this pack actually
    # uses (authoritative, and works for a pack nobody has listed), while the
    # names file covers music, which RSDKv4 sets from script bytecode and so
    # names nowhere a reader can reach.
    cand = []
    for p in gsfx:
        cand.append("Data/SoundFX/" + p)
    if names_file:
        with open(names_file) as f:
            listed = [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
        say(f"-- {len(listed)} candidate names from {os.path.basename(names_file)}")
        cand += listed
    else:
        say("-- no names file; music cannot be found without one")

    keep_all = "--all" in flags
    LOOSE = ("Data/Music/", "Data/SoundFX/Global/",
             "Data/Game/Menu/", "Data/Game/Models/")
    def wanted_here(path):
        if keep_all:
            return path.startswith("Data/")
        # Data/Game/ is deliberately NOT a prefix here: it would pull
        # GameConfig.bin out too, and a loose copy of that beside Data.rsdk
        # overrides the packed one.
        return path.startswith(LOOSE)

    seen_c, wanted = set(), []
    for path in cand:
        if path in seen_c or not wanted_here(path):
            continue
        seen_c.add(path)
        wanted.append((path, path[len("Data/"):]))

    if keep_all:
        extra = 0
        for folder in stages:
            sc = pack.read(f"Data/Stages/{folder}/StageConfig.bin")
            if sc is None:
                continue
            for p in parse_stageconfig(sc):
                full = "Data/SoundFX/" + p
                if full not in seen_c:
                    seen_c.add(full)
                    wanted.append((full, "SoundFX/" + p))
                    extra += 1
        say(f"-- {extra} further stage sfx named by the StageConfigs")

    got = miss = 0
    for src, dst in wanted:
        data = pack.read(src)
        if data is None:
            miss += 1
            continue
        full = os.path.join(outdir, dst)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "wb") as f:
            f.write(data)
        got += 1

    say(f"-- extracted {got} files to {outdir}  ({miss} of the candidates not in this pack)")
    if got == 0:
        raise SystemExit("Nothing was extracted -- the pack parsed but no asset matched.")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
