#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""把 Starry 整页界面的文件（HTML、CSS、JS）编进 C++：生成一个 .cc，里面是每个文件的路径和
字节。WebUI 的数据源按路径从这里取，不用 grit 和 .pak，也就不用改 Chrome 的打包。

用法：embed_resources.py --root <目录> --out-cc <文件> --out-h <文件> 文件…
路径是相对 --root 的，用 / 分隔。--name 是生成的函数和结构体的名字（默认 Page：
GetPageResources()、PageResource），同一个程序里嵌好几组文件时各用各的名字。
--gzip：存 gzip 压缩后的字节（取用的代码自己解压）；--flat：路径只留文件名。
"""

import argparse
import gzip
import os


def c_bytes(data):
    # 字节数组而不是字符串字面量：MSVC 风格的编译器对字符串字面量的长度有限制。
    lines = []
    for i in range(0, len(data), 24):
        chunk = data[i:i + 24]
        lines.append('    ' + ', '.join('0x%02x' % b for b in chunk) + ',')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', required=True)
    parser.add_argument('--out-cc', required=True)
    parser.add_argument('--out-h', required=True)
    parser.add_argument('--header-include', required=True)
    parser.add_argument('--name', default='Page')
    parser.add_argument('--comment', default='整页界面的所有文件，按路径排好。')
    parser.add_argument('--gzip', action='store_true')
    parser.add_argument('--flat', action='store_true')
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()

    entries = []
    for path in args.files:
        rel = (os.path.basename(path) if args.flat else
               os.path.relpath(path, args.root).replace(os.sep, '/'))
        with open(path, 'rb') as f:
            data = f.read()
        if args.gzip:
            # mtime=0：同样的输入生成同样的字节。
            data = gzip.compress(data, compresslevel=9, mtime=0)
        entries.append((rel, data))
    entries.sort()

    guard = args.header_include.upper().replace('/', '_').replace('.', '_') + '_'
    struct = '%sResource' % args.name
    function = 'Get%sResources' % args.name
    with open(args.out_h, 'w', encoding='utf-8') as h:
        h.write('// 由 starry/browser/resources/embed_resources.py 生成，不要手改。\n\n')
        h.write('#ifndef %s\n#define %s\n\n' % (guard, guard))
        h.write('#include <cstdint>\n#include <string_view>\n\n')
        h.write('#include "base/containers/span.h"\n\n')
        h.write('namespace starry {\n\n')
        h.write('struct %s {\n' % struct)
        h.write('  std::string_view path;\n')
        h.write('  base::span<const uint8_t> data;\n')
        h.write('};\n\n')
        h.write('// %s\n' % args.comment)
        h.write('base::span<const %s> %s();\n\n' % (struct, function))
        h.write('}  // namespace starry\n\n#endif  // %s\n' % guard)

    with open(args.out_cc, 'w', encoding='utf-8') as cc:
        cc.write('// 由 starry/browser/resources/embed_resources.py 生成，不要手改。\n\n')
        cc.write('#include "%s"\n\n' % args.header_include)
        cc.write('#include <array>\n\n')
        cc.write('namespace starry {\n\nnamespace {\n\n')
        for index, (_, data) in enumerate(entries):
            cc.write('constexpr uint8_t kData%d[] = {\n%s\n};\n\n' %
                     (index, c_bytes(data) if data else '    0x00,'))
        cc.write('constexpr auto kResources = std::to_array<%s>({\n' % struct)
        for index, (rel, data) in enumerate(entries):
            cc.write('    {"%s", base::span(kData%d).first(%dU)},\n' %
                     (rel, index, len(data)))
        cc.write('});\n\n}  // namespace\n\n')
        cc.write('base::span<const %s> %s() {\n' % (struct, function))
        cc.write('  return kResources;\n}\n\n}  // namespace starry\n')


if __name__ == '__main__':
    main()
