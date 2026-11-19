#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""把扩展选择器的 JS（procedural/procedural.js）编进 C++（网页进程用）。

去掉只有注释的行、空行和缩进，只能是 ASCII（同 scriptlets/generate_scriptlets.py）。

用法：generate_procedural_script.py --out-cc <文件> --out-h <文件> --header-include <路径> <js>
"""

import argparse
import sys


def fail(message):
    sys.stderr.write('generate_procedural_script.py: %s\n' % message)
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
    code = '\n'.join(lines)
    if not code.startswith('(function(') or not code.endswith('})'):
        fail('%s: 要是一个函数表达式 (function(…) {…})' % path)
    return code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-cc', required=True)
    parser.add_argument('--out-h', required=True)
    parser.add_argument('--header-include', required=True)
    parser.add_argument('script')
    args = parser.parse_args()

    data = minify(args.script).encode('ascii')
    guard = args.header_include.upper().replace('/', '_').replace('.', '_') + '_'
    generated = ('// 由 starry/components/content_filter/procedural/'
                 'generate_procedural_script.py 生成，不要手改。\n\n')
    with open(args.out_h, 'w', encoding='utf-8') as h:
        h.write(generated)
        h.write('#ifndef %s\n#define %s\n\n' % (guard, guard))
        h.write('#include <string_view>\n\n')
        h.write('namespace starry::content_filter {\n\n')
        h.write('// procedural/procedural.js：函数表达式 `(function(filters, attr) {…})`，后面接上\n')
        h.write('// `(规则的 JSON, "属性名");` 就是要跑的脚本。\n')
        h.write('std::string_view ProceduralScript();\n\n')
        h.write('}  // namespace starry::content_filter\n\n')
        h.write('#endif  // %s\n' % guard)
    with open(args.out_cc, 'w', encoding='utf-8') as cc:
        cc.write(generated)
        cc.write('#include "%s"\n\n' % args.header_include)
        cc.write('namespace starry::content_filter {\n\nnamespace {\n\n')
        # 字节数组而不是字符串字面量：MSVC 对字符串字面量的长度有限制。
        cc.write('constexpr char kScript[] = {\n')
        for i in range(0, len(data), 24):
            cc.write('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',\n')
        cc.write('};\n\n}  // namespace\n\n')
        cc.write('std::string_view ProceduralScript() {\n')
        cc.write('  return std::string_view(kScript, sizeof(kScript));\n}\n\n')
        cc.write('}  // namespace starry::content_filter\n')


if __name__ == '__main__':
    main()
