#!/usr/bin/env python3
"""The memory image of an NSO, for duo_host_test: text, rodata and data at their load offsets, bss zero.

    make_fake_image.py <exefs/main> <image.bin>

The input is the player's own game file; nothing of it is kept in the repository or packed anywhere. With the real
1.6.0 main the test's "load-plan site ... original" lines and the module's build checks run against the real words.
"""
import struct
import sys


def lz4_block(src, size):
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]; i += 1
        n = token >> 4
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255: break
        out += src[i:i + n]; i += n
        if i >= len(src): break
        offset = src[i] | src[i + 1] << 8; i += 2
        n = token & 15
        if n == 15:
            while True:
                b = src[i]; i += 1; n += b
                if b != 255: break
        n += 4
        start = len(out) - offset
        if offset >= n:
            out += out[start:start + n]
        else:   # the match overlaps what it writes: a run
            for k in range(n):
                out.append(out[start + k])
    if len(out) != size:
        raise SystemExit(f'LZ4: {len(out)} bytes, expected {size}')
    return bytes(out)


def main(path, out):
    nso = open(path, 'rb').read()
    if nso[:4] != b'NSO0':
        raise SystemExit('not an NSO')
    flags = struct.unpack_from('<I', nso, 12)[0]
    segments = [struct.unpack_from('<III', nso, 0x10 + 0x10 * k) for k in range(3)]   # file offset, memory offset, size
    packed = struct.unpack_from('<III', nso, 0x60)
    end = max(mem + size for _, mem, size in segments)
    image = bytearray(end)
    for k, (off, mem, size) in enumerate(segments):
        raw = nso[off:off + packed[k]]
        image[mem:mem + size] = lz4_block(raw, size) if flags >> k & 1 else raw[:size]
    open(out, 'wb').write(image)
    print(f'{out}: {end:#x} bytes, build id {nso[0x40:0x50].hex().upper()}')


if __name__ == '__main__':
    main(*sys.argv[1:3])
