# SPDX-License-Identifier: GPL-2.0-only
from io import BytesIO

from PIL import Image


def emit(name, data):
    print(f"static const u8 {name}[] = {{")
    for i in range(0, len(data), 12):
        print("\t" + ", ".join(f"0x{x:02x}" for x in data[i:i + 12]) + ",")
    print("};\n")


for f, (w, h) in enumerate(((16, 16), (32, 24))):
    pattern = bytes((17 + 47 * (x // 8) + 71 * (y // 8) + 83 * f) % 256
                    for y in range(h) for x in range(w))
    image = Image.frombytes("L", (w, h), pattern)
    jpeg = BytesIO()
    image.save(jpeg, format="JPEG", qtables=[[1] * 64])
    data = jpeg.getvalue()
    assert data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"
    assert Image.open(BytesIO(data)).tobytes() == pattern
    i = 2
    tables = {}
    while data[i + 1] != 0xda:
        assert data[i] == 0xff
        size = int.from_bytes(data[i + 2:i + 4], "big")
        if data[i + 1] == 0xc4:
            huff = data[i + 4:i + 2 + size]
            assert len(huff) == 17 + sum(huff[1:17])
            tables[huff[0]] = huff[1:]
        if data[i + 1] == 0xdb:
            assert data[i + 4:i + 2 + size] == b"\x00" + b"\x01" * 64
        i += 2 + size
    if not f:
        emit("gray_dc0", tables[0x00])
        emit("gray_ac0", tables[0x10])
        original_tables = tables
    else:
        assert tables == original_tables
    emit(f"gray_scan{f}", data[i:])
