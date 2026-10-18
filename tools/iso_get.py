"""Copy one file out of the owner's disc image (read-only), for windowless checks such as gfxboot.
usage: py -I tools/iso_get.py <disc.iso> <path on disc, e.g. SLUS_205.15> <out file>"""
import struct, sys

iso_path, want, out = sys.argv[1], sys.argv[2].strip('\\/').upper(), sys.argv[3]
f = open(iso_path, 'rb')


def read(lba, n):
    f.seek(lba * 2048)
    return f.read(n)


pvd = read(16, 2048)
assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
for part in want.replace('\\', '/').split('/'):
    data = read(lba, size)
    i, found = 0, None
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048
            continue
        name = data[i + 33:i + 33 + data[i + 32]].decode('ascii', 'replace').split(';')[0].upper()
        if name == part:
            found = (struct.unpack_from('<I', data, i + 2)[0], struct.unpack_from('<I', data, i + 10)[0])
            break
        i += n
    if not found:
        sys.exit('%s not on the disc' % want)
    lba, size = found
open(out, 'wb').write(read(lba, size))
print('%s: %d bytes -> %s' % (want, size, out))
