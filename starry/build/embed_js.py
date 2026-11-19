#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""把一段要在网页里跑的 JS 编进 C++，生成一个返回它的函数。

去掉只有注释的行、空行和缩进，代码只能是 ASCII（注释写在单独的行上），同
starry/components/content_filter/procedural/generate_procedural_script.py。

用法：embed_js.py --out-cc <文件> --out-h <文件> --header-include <路径>
                  --namespace <名字空间> --function <函数名> <js>
"""

import argparse
import sys


def fail(message):
    sys.stderr.write('embed_js.py: %s\n' % message)
    sys.exit(1)


def minify(path):
    lines = []
    with open(path, encoding='utf-8') as f:
        for number, raw in enumerate(f, 1):
            line = raw.strip()
            if line == '' or line.startswith('//'):
                continue
            if any(ord(c) > 0x7e or (ord(c) < 0x20 and c != '\t') for c in line):
                fail('%s:%d: 代码里只能是 ASCII（注释写在单独的行上）' % (path, number))
            lines.append(line)
    if not lines:
        fail('%s: 没有代码' % path)
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-cc', required=True)
    parser.add_argument('--out-h', required=True)
    parser.add_argument('--header-include', required=True)
    parser.add_argument('--namespace', required=True)
    parser.add_argument('--function', required=True)
    parser.add_argument('script')
    args = parser.parse_args()

    data = minify(args.script).encode('ascii')
    guard = args.header_include.upper().replace('/', '_').replace('.', '_') + '_'
    source = args.script.replace('\\', '/')
    source = source[source.find('starry/'):] if 'starry/' in source else source
    generated = '// 由 starry/build/embed_js.py 从 %s 生成，不要手改。\n\n' % source
    with open(args.out_h, 'w', encoding='utf-8') as h:
        h.write(generated)
        h.write('#ifndef %s\n#define %s\n\n' % (guard, guard))
        h.write('#include <string_view>\n\n')
        h.write('namespace %s {\n\n' % args.namespace)
        h.write('std::string_view %s();\n\n' % args.function)
        h.write('}  // namespace %s\n\n' % args.namespace)
        h.write('#endif  // %s\n' % guard)
    with open(args.out_cc, 'w', encoding='utf-8') as cc:
        cc.write(generated)
        cc.write('#include "%s"\n\n' % args.header_include)
        cc.write('namespace %s {\n\nnamespace {\n\n' % args.namespace)
        # 字节数组而不是字符串字面量：MSVC 对字符串字面量的长度有限制。
        cc.write('constexpr char kScript[] = {\n')
        for i in range(0, len(data), 24):
            cc.write('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',\n')
        cc.write('};\n\n}  // namespace\n\n')
        cc.write('std::string_view %s() {\n' % args.function)
        cc.write('  return std::string_view(kScript, sizeof(kScript));\n}\n\n')
        cc.write('}  // namespace %s\n' % args.namespace)


if __name__ == '__main__':
    main()
