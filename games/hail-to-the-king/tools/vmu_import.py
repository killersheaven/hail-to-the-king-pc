#!/usr/bin/env python3
"""Copy a Dreamcast save in VMI/VMS form (as saved by emulators and the
Planetweb browser, or found on save-sharing sites) onto one of the game's
memory card files.

    python vmu_import.py SAVE.vmi CARD.bin       (the .vms next to the .vmi is used)
    python vmu_import.py SAVE.vms CARD.bin       (file name EVILDEAD.000 is assumed)

CARD.bin is vmu_a1.bin (Save Slot 1), vmu_a2.bin (Slot 2), vmu_b1.bin (Slot 3)
... vmu_c2.bin (Slot 6) in the pc-build folder. A missing card is created and
formatted. A file of the same name already on the card is replaced. The card
layout is the standard one the game's own memory card emulation uses (maple.c).
"""
import os
import struct
import sys
import time

BLOCK = 512
NBLOCKS = 256
ROOT, FAT, DIR_LAST, DIR_BLOCKS, USER_BLOCKS = 255, 254, 253, 13, 200
FREE, LAST = 0xFFFC, 0xFFFA


def bcd(v):
    return (v // 10) << 4 | v % 10


def formatted_card():
    """Same layout as maple.c vmu_format()."""
    img = bytearray(NBLOCKS * BLOCK)
    root = ROOT * BLOCK
    img[root:root + 16] = b'\x55' * 16
    img[root + 0x30:root + 0x34] = bytes([0x20, 0x00, 0x01, 0x01])
    struct.pack_into('<7H', img, root + 0x44, ROOT, FAT, 1, DIR_LAST, DIR_BLOCKS, 0, USER_BLOCKS)
    struct.pack_into('<H', img, root + 0x52, 31)
    fat = FAT * BLOCK
    for b in range(NBLOCKS):
        struct.pack_into('<H', img, fat + 2 * b, FREE)
    struct.pack_into('<H', img, fat + 2 * ROOT, LAST)
    struct.pack_into('<H', img, fat + 2 * FAT, LAST)
    for b in range(DIR_LAST, DIR_LAST - DIR_BLOCKS + 1, -1):
        struct.pack_into('<H', img, fat + 2 * b, b - 1)
    struct.pack_into('<H', img, fat + 2 * (DIR_LAST - DIR_BLOCKS + 1), LAST)
    return img


def dir_slots():
    """Byte offsets of the 32-byte directory entries, in directory order."""
    for b in range(DIR_LAST, DIR_LAST - DIR_BLOCKS, -1):
        for e in range(BLOCK // 32):
            yield b * BLOCK + e * 32


def fat_get(img, b):
    return struct.unpack_from('<H', img, FAT * BLOCK + 2 * b)[0]


def fat_set(img, b, v):
    struct.pack_into('<H', img, FAT * BLOCK + 2 * b, v)


def read_save(path):
    """Returns (file name, data bytes, is_game, copy_protected, timestamp tuple)."""
    base, ext = os.path.splitext(path)
    if ext.lower() == '.vmi':
        vmi = open(path, 'rb').read()
        if len(vmi) < 108:
            sys.exit(f'{path}: too short for a VMI file')
        year, month, day, hour, minute, second, _wd = struct.unpack_from('<HBBBBBB', vmi, 0x44)
        res = vmi[0x50:0x58].split(b'\0')[0].decode('ascii', 'replace')
        name = vmi[0x58:0x64].split(b'\0')[0].decode('ascii', 'replace')
        mode, = struct.unpack_from('<H', vmi, 0x64)
        size, = struct.unpack_from('<I', vmi, 0x68)
        vms = None
        folder = os.path.dirname(path) or '.'
        for f in os.listdir(folder):                       # the .vms is named by the VMI's resource name
            stem, e = os.path.splitext(f)
            if e.lower() == '.vms' and stem.lower() in (res.lower(), os.path.basename(base).lower()):
                vms = os.path.join(folder, f)
                break
        if not vms:
            sys.exit(f'{path}: cannot find {res}.vms next to it')
        data = open(vms, 'rb').read()
        if size and len(data) != size:
            print(f'note: the VMI says {size} bytes, {os.path.basename(vms)} has {len(data)}')
        return name, data, bool(mode & 2), bool(mode & 1), (year, month, day, hour, minute, second)
    data = open(path, 'rb').read()
    t = time.localtime()
    return 'EVILDEAD.000', data, False, False, t[:6]


def import_save(card_path, name, data, is_game=False, protected=False, stamp=None):
    img = bytearray(open(card_path, 'rb').read()) if os.path.exists(card_path) else formatted_card()
    if len(img) != NBLOCKS * BLOCK or img[ROOT * BLOCK:ROOT * BLOCK + 16] != b'\x55' * 16:
        sys.exit(f'{card_path}: not a formatted 128 KB memory card image')
    if is_game:
        sys.exit('this is a mini-game (VMU game) file, not a save; it is not supported')
    nblk = (len(data) + BLOCK - 1) // BLOCK
    data = data + b'\0' * (nblk * BLOCK - len(data))
    want = name.encode('ascii')[:12].ljust(12, b'\0')

    # drop a file of the same name
    free_entry = None
    for off in dir_slots():
        t = img[off]
        if t in (0x33, 0xCC) and bytes(img[off + 4:off + 16]) == want:
            b = struct.unpack_from('<H', img, off + 2)[0]
            for _ in range(NBLOCKS):
                nxt = fat_get(img, b)
                fat_set(img, b, FREE)
                if nxt == LAST or nxt >= NBLOCKS:
                    break
                b = nxt
            img[off:off + 32] = b'\0' * 32
            print(f'replaced the old {name} on the card')
        if img[off] == 0 and free_entry is None:
            free_entry = off
    if free_entry is None:
        sys.exit(f'{card_path}: the card directory is full')

    # data files are placed from the top of the user area downwards
    blocks = [b for b in range(USER_BLOCKS - 1, -1, -1) if fat_get(img, b) == FREE][:nblk]
    if len(blocks) < nblk:
        sys.exit(f'{card_path}: not enough free blocks ({nblk} needed)')
    for i, b in enumerate(blocks):
        img[b * BLOCK:(b + 1) * BLOCK] = data[i * BLOCK:(i + 1) * BLOCK]
        fat_set(img, b, blocks[i + 1] if i + 1 < nblk else LAST)

    y, mo, d, h, mi, s = stamp or time.localtime()[:6]
    wd = time.struct_time((y, mo, d, h, mi, s, 0, 1, -1)).tm_wday if 1 <= mo <= 12 else 0
    e = bytearray(32)
    e[0] = 0x33
    e[1] = 0xFF if protected else 0x00
    struct.pack_into('<H', e, 2, blocks[0])
    e[4:16] = want
    e[16:24] = bytes([bcd(y // 100), bcd(y % 100), bcd(mo), bcd(d), bcd(h), bcd(mi), bcd(s), bcd(wd)])
    struct.pack_into('<HH', e, 24, nblk, 0)
    img[free_entry:free_entry + 32] = e
    open(card_path, 'wb').write(img)
    print(f'{name}: {nblk} blocks copied to {card_path}')


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    name, data, is_game, protected, stamp = read_save(sys.argv[1])
    import_save(sys.argv[2], name, data, is_game, protected, stamp)


if __name__ == '__main__':
    main()
