"""vu1_image.py - rebuild the game's VU1 micro memory from the owner's disc (build time, nothing is committed).

The game uploads its VU1 programs once at boot with a VIF1 DMA chain at EE address 0x286980 (RhBase_Init). This
reads the boot ELF from the ISO, loads its segments, walks that chain (tags with TTE) and applies the VIF MPG
commands to a 16 KB micro memory image.

    python vu1_image.py <game.iso> <out.bin>
"""
import mmap
import struct
import sys

SECTOR = 2048
CHAIN = 0x00286980


def iso_files(iso):
    """{UPPER-CASE PATH: (lba, size)} for every file on the disc (ISO 9660, no Joliet needed)."""
    pvd = iso[16 * SECTOR:17 * SECTOR]
    root = pvd[156:156 + 34]
    out = {}

    def walk(lba, size, prefix):
        data = iso[lba * SECTOR:lba * SECTOR + size]
        o = 0
        while o < len(data):
            n = data[o]
            if n == 0:
                o = (o // SECTOR + 1) * SECTOR
                continue
            r_lba, r_size = struct.unpack_from("<I", data, o + 2)[0], struct.unpack_from("<I", data, o + 10)[0]
            flags, nlen = data[o + 25], data[o + 32]
            name = data[o + 33:o + 33 + nlen].decode("latin-1")
            if name not in ("\x00", "\x01"):
                name = name.split(";")[0]
                path = prefix + "/" + name.upper()
                if flags & 2:
                    walk(r_lba, r_size, path)
                else:
                    out[path] = (r_lba, r_size)
            o += n

    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "")
    return out


def boot_elf(iso):
    files = iso_files(iso)
    lba, size = files["/SYSTEM.CNF"]
    cnf = iso[lba * SECTOR:lba * SECTOR + size].decode("latin-1")
    for line in cnf.splitlines():
        if line.strip().upper().startswith("BOOT2"):
            name = line.split("\\")[-1].split(";")[0].strip().upper()
            lba, size = files["/" + name]
            return iso[lba * SECTOR:lba * SECTOR + size]
    raise SystemExit("no BOOT2 in SYSTEM.CNF")


def load_elf(elf):
    """32 MB EE RAM image with the ELF's PT_LOAD segments."""
    ram = bytearray(32 << 20)
    phoff, = struct.unpack_from("<I", elf, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", elf, 0x2A)
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _ = struct.unpack_from("<IIIIII", elf, phoff + i * phentsize)
        if p_type == 1:
            a = p_vaddr & 0x01FFFFFF
            ram[a:a + p_filesz] = elf[p_offset:p_offset + p_filesz]
    return ram


def vif_mpg(micro, words):
    """Apply the MPG commands of a VIF code stream; other commands are skipped by their sizes."""
    i = 0
    while i < len(words):
        c = words[i]
        cmd, num, imm = (c >> 24) & 0x7F, (c >> 16) & 0xFF, c & 0xFFFF
        if cmd == 0x4A:   # MPG: num pairs (0 = 256) to micro address imm (in pairs)
            n = num or 256
            body = words[i + 1:i + 1 + 2 * n]
            for k in range(n):
                a = ((imm + k) * 8) & 0x3FFF
                struct.pack_into("<II", micro, a, body[2 * k], body[2 * k + 1])
            i += 1 + 2 * n
        elif cmd in (0x50, 0x51):
            i += 1 + (imm or 65536) * 4
        elif cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            i += 1 + ((32 >> vl) * (vn + 1) * (num or 256) + 31) // 32
        elif cmd == 0x20:
            i += 2
        elif cmd in (0x30, 0x31):
            i += 5
        else:
            i += 1


def walk_chain(ram, tadr, micro):
    """VIF1 source chain with TTE: each tag's upper 64 bits are VIF codes."""
    stack, guard = [], 0
    stream = []
    while guard < 100000:
        guard += 1
        lo, hi, w2, w3 = struct.unpack_from("<IIII", ram, tadr & 0x01FFFFF0)
        qwc, tid, addr = lo & 0xFFFF, (lo >> 28) & 7, hi & 0x01FFFFF0
        stream += [w2, w3]
        def data(a, n):
            return list(struct.unpack_from("<%dI" % (n * 4), ram, a & 0x01FFFFF0)) if n else []
        if tid == 0:   # refe
            stream += data(addr, qwc); break
        elif tid == 1:   # cnt
            stream += data(tadr + 16, qwc); tadr += 16 + qwc * 16
        elif tid == 2:   # next
            stream += data(tadr + 16, qwc); tadr = addr
        elif tid in (3, 4):   # ref, refs
            stream += data(addr, qwc); tadr += 16
        elif tid == 5:   # call
            stream += data(tadr + 16, qwc); stack.append(tadr + 16 + qwc * 16); tadr = addr
        elif tid == 6:   # ret
            stream += data(tadr + 16, qwc)
            if not stack: break
            tadr = stack.pop()
        else:   # end
            stream += data(tadr + 16, qwc); break
    vif_mpg(micro, stream)


def build(iso_path):
    f = open(iso_path, "rb")
    iso = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    ram = load_elf(boot_elf(iso))
    micro = bytearray(0x4000)
    walk_chain(ram, CHAIN, micro)
    return bytes(micro)


if __name__ == "__main__":
    img = build(sys.argv[1])
    open(sys.argv[2], "wb").write(img)
    print("micro image: %d bytes used" % len(img.rstrip(b"\0")))
