# SPDX-License-Identifier: GPL-2.0-only
from io import BytesIO

from PIL import Image


def emit(name, data):
    print(f"static const unsigned char {name}[] = {{")
    for start in range(0, len(data), 16):
        print("    " + ", ".join(f"0x{x:02x}" for x in data[start:start + 16]) + ",")
    print("};")


for n, (w, h) in enumerate(((16, 16), (32, 24), (320, 240), (1024, 1024))):
    pattern = bytes((17 + 47 * (x // 8) + 71 * (y // 8) + 83 * n) % 256
                    for y in range(h) for x in range(w))
    im = Image.frombytes("L", (w, h), pattern)
    f = BytesIO()
    im.save(f, format="JPEG", qtables=[[4] + list(range(1, 64))])
    data = f.getvalue()
    if n == 1:
        data = data[:2] + b"\xff\xe2\x00\x03\x55" + data[2:]
    assert Image.open(BytesIO(data)).tobytes() == pattern
    print(f"/* SOS offset {data.index(bytes((255, 218)))} */")
    emit(f"frame_{n}", data)
    if not n:
        sof = data.index(bytes((255, 192)))
        assert data[sof + 4] == 8 and data[sof + 9] == 1
        bad = data[:sof + 5] + bytes((0, 24, 0, 32)) + data[sof + 9:]
        emit("bad_frame", bad)
