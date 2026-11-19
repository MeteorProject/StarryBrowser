#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""把几张方形 PNG 合成一个 Windows 的 .ico（每个尺寸存原样的 PNG，Windows Vista 以后都认）。

用法：make_ico.py <输出.ico> <图.png>…   尺寸从 PNG 头里读，最大 256。
"""

import struct
import sys


def png_size(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n' or data[12:16] != b'IHDR':
        raise ValueError('不是 PNG')
    return struct.unpack('>II', data[16:24])


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    images = []
    for path in sys.argv[2:]:
        with open(path, 'rb') as f:
            data = f.read()
        width, height = png_size(data)
        if width != height or width > 256:
            raise ValueError('%s：要是不大于 256 的正方形（%dx%d）' % (path, width, height))
        images.append((width, data))
    images.sort(key=lambda item: item[0])
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = len(header) + 16 * len(images)
    entries = b''
    for size, data in images:
        # 宽高字段是一个字节，256 写成 0。
        entries += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32,
                               len(data), offset)
        offset += len(data)
    with open(sys.argv[1], 'wb') as f:
        f.write(header + entries + b''.join(data for _, data in images))
    return 0


if __name__ == '__main__':
    sys.exit(main())
