#!/usr/bin/env python3
"""Assemble the Asteroids Deluxe CPU address space from a ROM set.

Produces `<set>_dump.bin`, a 64 KB image of what the 6502 sees, which is
the single input every other tool here reads.

The memory map is the one the original Atari source states in its header
(DSTRD0.MAC):

    VECTOR RAM   4000-47FF  (2K, written by the CPU, read by the DVG)
    VECTOR ROM   4800-57FF  (4K, DVG display-list subroutines)
    PROGRAM ROM  6000-7FFF  (8K, 6502 code)
    SCRATCH      page 0
    STACK        page 1
    PLAYER1      page 2      } bank-switched pair, see BNKSEL (3C04)
    PLAYER2      page 3      }

The 6502 fetches its vectors from FFFA-FFFF, which on this board is the
top ROM (7800-7FFF) selected again by an incompletely decoded address
line.  We mirror it so a tracing disassembler can read RESET/NMI/IRQ the
way the CPU does.

ROM files are matched by part number and revision, not by filename, so
any of these work and directory layout does not matter:

    036430.02                 Atari release name (source archive)
    036430-02.d1              MAME name
    astdelux.zip              a MAME romset zip - members are indexed too

Usage:
    python gen_from_roms.py <rom-directory-or-zip> [--set astdelux2]
"""

import argparse
import hashlib
import os
import re
import sys
import zipfile

# ---------------------------------------------------------------------------
# ROM sets.  Each entry: (part, revision, load address, length, sha1)
# sha1s are the MAME reference hashes - they are how we prove the image is
# the romset we think it is, independent of what the files are named.
# ---------------------------------------------------------------------------

ROM_SETS = {
    # Asteroids Deluxe (rev 3) - MAME's primary set
    "astdelux": [
        ("036800", "02", 0x4800, 0x800, "cebaa1b91b96e8b80f2b2c17c6fd31fa9f156386"),
        ("036799", "01", 0x5000, 0x800, "1956a12bccb5d3a84ce0c1cc10c6ad7f64e30b40"),
        ("036430", "02", 0x6000, 0x800, "abe262193ec8e1981be36928e9a89a8ac95cd0ad"),
        ("036431", "02", 0x6800, 0x800, "aa2099b8fc62a79879efeea70ea1e9ed77e3e6f0"),
        ("036432", "02", 0x7000, 0x800, "198218cd2f43f8b83e4463b1f3a8aa49da5015e4"),
        ("036433", "03", 0x7800, 0x800, "bf10ffb0c4870e777d6b509cbede35db8bb6b0b8"),
    ],
    # Asteroids Deluxe (rev 2) - THE DEFAULT.  This is the build the Atari
    # source archive documents: DSTRD0.MAP's NMI $785C / PWRON $7CD7 are
    # exactly the vectors in 036433-02, so its symbol names apply to this
    # revision and no other.
    "astdelux2": [
        ("036800", "01", 0x4800, 0x800, "344fea2e5d84acce365d76daed61e96b9b6b37cc"),
        ("036799", "01", 0x5000, 0x800, "1956a12bccb5d3a84ce0c1cc10c6ad7f64e30b40"),
        ("036430", "01", 0x6000, 0x800, "5d7543e19acab99ddb63c0ffd60f54d7a0f267f5"),
        ("036431", "01", 0x6800, 0x800, "9041d8c2369d004f198681e02b59a923fa8f70c9"),
        ("036432", "01", 0x7000, 0x800, "ded0138a20d80317d67add5bb2a64e6274e0e409"),
        ("036433", "02", 0x7800, 0x800, "52b64e867df98d14742eb1817b59931bb7f941d9"),
    ],
    # Asteroids Deluxe (rev 1)
    "astdelux1": [
        ("036800", "01", 0x4800, 0x800, "344fea2e5d84acce365d76daed61e96b9b6b37cc"),
        ("036799", "01", 0x5000, 0x800, "1956a12bccb5d3a84ce0c1cc10c6ad7f64e30b40"),
        ("036430", "01", 0x6000, 0x800, "5d7543e19acab99ddb63c0ffd60f54d7a0f267f5"),
        ("036431", "01", 0x6800, 0x800, "9041d8c2369d004f198681e02b59a923fa8f70c9"),
        ("036432", "01", 0x7000, 0x800, "ded0138a20d80317d67add5bb2a64e6274e0e409"),
        ("036433", "01", 0x7800, 0x800, "6a4b37dbfe4e6badc4e81036b1430da2e9cb8ca4"),
    ],
}

# DVG state PROM (034602-01.c8, 82S129, 256x4).  Not part of the CPU address
# space; exported separately for anyone modelling DVG timing.
DVG_PROM = ("034602", "01", 0x100, "8cbded64d1dd35b18c4d5cece00f77e7b2cab2ad")

VECTOR_MIRROR_SRC = 0x7800      # top ROM ...
VECTOR_MIRROR_DST = 0xF800      # ... also answers here, where the 6502 looks


def sha1(data):
    return hashlib.sha1(data).hexdigest()


# `036430.02`, `036430-02.d1`, `A34602.01` (the archive's PROM name) all
# reduce to (part, revision).  A leading letter is tolerated and dropped.
ROM_NAME_RE = re.compile(r"^[A-Za-z]?(\d{5,6})[.\-_](\d{2})")


def _key(name):
    m = ROM_NAME_RE.match(os.path.basename(name))
    if not m:
        return None
    part = m.group(1)
    return (part[-6:].zfill(6), m.group(2))


def index_roms(romdir):
    """Map (part, rev) -> (label, loader) for every ROM we can reach.

    Walks loose files and looks inside any .zip, so a directory of MAME
    romset zips indexes just as well as an unpacked source archive.
    `loader` is a no-argument callable returning the file's bytes.
    """
    found = {}

    def add(key, label, loader):
        if key is not None:
            found.setdefault(key, (label, loader))

    def scan_zip(path):
        try:
            zf = zipfile.ZipFile(path)
        except (zipfile.BadZipFile, OSError) as exc:
            print("  note: cannot read %s (%s); skipped" % (path, exc))
            return
        for member in sorted(zf.namelist()):
            if member.endswith("/"):
                continue
            add(_key(member), "%s:%s" % (os.path.basename(path), member),
                lambda m=member, z=zf: z.read(m))

    targets = [romdir] if os.path.isfile(romdir) else []
    if not targets:
        for root, _dirs, files in os.walk(romdir):
            targets.extend(os.path.join(root, n) for n in sorted(files))

    # Loose files win over zip members: an unpacked ROM is the more direct
    # evidence, and it keeps behaviour stable if both are present.
    for path in targets:
        if not path.lower().endswith(".zip"):
            add(_key(path), path, lambda p=path: open(p, "rb").read())
    for path in targets:
        if path.lower().endswith(".zip"):
            scan_zip(path)
    return found


def build(romdir, setname):
    roms = ROM_SETS[setname]
    index = index_roms(romdir)
    image = bytearray(0x10000)

    missing, bad = [], []
    for part, rev, addr, length, want in roms:
        entry = index.get((part, rev))
        if entry is None:
            missing.append("%s-%s" % (part, rev))
            continue
        label, loader = entry
        data = loader()
        if len(data) != length:
            bad.append("%s: expected %d bytes, got %d" % (label, length, len(data)))
            continue
        got = sha1(data)
        if got != want:
            bad.append("%s: sha1 %s, expected %s" % (label, got, want))
            continue
        image[addr:addr + length] = data
        print("  %-28s -> $%04X-$%04X  %s" % (os.path.basename(label), addr,
                                              addr + length - 1, got[:12]))

    if missing:
        sys.exit("missing ROMs for set '%s': %s" % (setname, ", ".join(missing)))
    if bad:
        sys.exit("bad ROMs:\n  " + "\n  ".join(bad))

    # Mirror the top ROM into F800-FFFF so FFFA/FFFC/FFFE resolve.
    image[VECTOR_MIRROR_DST:VECTOR_MIRROR_DST + 0x800] = \
        image[VECTOR_MIRROR_SRC:VECTOR_MIRROR_SRC + 0x800]
    return image


def find_prom(romdir):
    part, rev, length, want = DVG_PROM
    entry = index_roms(romdir).get((part, rev))
    if entry is None:
        return None, None
    label, loader = entry
    data = loader()
    # Some archive copies pad the 256x4 PROM out; take the leading 256 bytes
    # and check the hash before trusting either form.
    for cand in (data, data[:length]):
        if len(cand) == length and sha1(cand) == want:
            return label, cand
    return label, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("romdir",
                    help="directory holding the ROM files or romset zips "
                         "(a single .zip also works)")
    ap.add_argument("--set", default="astdelux2", choices=sorted(ROM_SETS),
                    help="which romset to assemble (default: astdelux2, rev 2 - "
                         "the revision the Atari source archive documents)")
    ap.add_argument("--listings", action="store_true",
                    help="also run mkdefines.py, m6502trace.py and dvgdasm.py "
                         "to regenerate every listing for this set")
    ap.add_argument("-o", "--out", default=None,
                    help="output image (default: <set>_dump.bin)")
    args = ap.parse_args()

    out = args.out or ("%s_dump.bin" % args.set)
    print("assembling '%s' from %s" % (args.set, args.romdir))
    image = build(args.romdir, args.set)
    with open(out, "wb") as f:
        f.write(image)
    print("wrote %s (%d bytes)" % (out, len(image)))

    vec = lambda a: image[a] | (image[a + 1] << 8)
    print("  NMI   $%04X" % vec(0xFFFA))
    print("  RESET $%04X" % vec(0xFFFC))
    print("  IRQ   $%04X" % vec(0xFFFE))

    path, prom = find_prom(args.romdir)
    if prom is not None:
        with open("astdelux_dvgprom.bin", "wb") as f:
            f.write(prom)
        print("wrote astdelux_dvgprom.bin (256 bytes) from %s" % os.path.basename(path))
    elif path is not None:
        print("note: found %s but it does not match the reference PROM hash; skipped"
              % os.path.basename(path))
    else:
        print("note: DVG state PROM (034602-01) not found; skipped")

    if args.listings:
        run_listings(args.set)


ARCHIVE = os.path.join("..", "asteroids-deluxe-main")


def run_listings(romset):
    """Regenerate every derived file for one romset, in dependency order."""
    import subprocess
    # Three steps read Atari's source archive (mkdefines, nameroutines,
    # vecnames).  It is not distributed with the repository, but what those
    # steps recover from it - astdelux_defines.py/.asm, astdelux_names.py,
    # astdelux_vecnames.py - is checked in, so without a copy at ARCHIVE
    # the chain skips them and uses the committed tables.
    have_archive = os.path.isdir(ARCHIVE)
    if not have_archive:
        print("note: %s not found - the symbol/name recovery steps are skipped\n"
              "      and the committed name tables are used; see README.md"
              % ARCHIVE)

    def needs_archive(cmd, what):
        return (cmd, what, True)

    # m6502trace runs twice on purpose.  nameroutines.py needs the code/data
    # map to know where instructions begin, and the trace needs the names
    # nameroutines recovers - but the map does not depend on the names, so
    # one extra pass closes the loop.
    steps = [
        needs_archive([sys.executable, "mkdefines.py"], "symbol tables"),
        # The C port's memory model.  Not per-romset: it comes from the
        # source archive's RAM layout, which documents rev 2, and the port
        # targets rev 2 whichever listing is being generated.  Running it
        # here keeps the committed header from drifting away from the
        # symbol tables it is derived from.
        ([sys.executable, "mkstate.py"], "C memory model"),
        ([sys.executable, "m6502trace.py", "--set", romset, "--quiet"],
         "6502 trace (first pass, for the code map)"),
        needs_archive([sys.executable, "nameroutines.py", "--set", romset],
                      "routine names from the Atari source"),
        ([sys.executable, "m6502trace.py", "--set", romset], "6502 listing"),
        needs_archive([sys.executable, "vecnames.py", "--set", romset],
                      "shape names from the vector source"),
        ([sys.executable, "dvgdasm.py", "--set", romset], "vector listing"),
        # The plain, assembler-style form (mkplain.py) needs the routine
        # and shape names nameroutines.py/vecnames.py just placed, so it
        # runs after both - right after the traced listing it mirrors.
        ([sys.executable, "mkplain.py", "--set", romset], "plain assembler-style listing"),
        ([sys.executable, "mkpreview.py", "--set", romset], "shape preview"),
        ([sys.executable, "mknotes.py", "--set", romset], "routine index"),
        # The ROM images as C data, so the port reads its tables at their
        # ROM addresses instead of carrying hand-typed copies.
        ([sys.executable, "mkrom.py", "--set", romset], "C ROM data"),
    ]
    for step in steps:
        cmd, what = step[0], step[1]
        if len(step) > 2 and not have_archive:
            print("\n--- %s: skipped (no source archive)" % what)
            continue
        print("\n--- %s: %s" % (what, " ".join(cmd[1:])))
        sys.stdout.flush()      # keep our log interleaved with the child's
        r = subprocess.run(cmd)
        if r.returncode:
            sys.exit("%s failed (exit %d)" % (what, r.returncode))


if __name__ == "__main__":
    main()
