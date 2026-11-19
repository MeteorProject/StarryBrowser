#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""把脚本片段的 JS（scriptlets/*.js）编进 C++。

每个文件是一个模块。文件开头 `// @` 的行说明它：
  // @requires <模块>               用到另一个模块里的函数（core 不用写，都带）
  // @scriptlet <名字> <函数> [trusted]
  // @alias <别名> <名字>
生成的代码去掉只有注释的行、空行和缩进（注入网页的脚本小一点），只能是 ASCII。

用法：generate_scriptlets.py --out-cc <文件> --out-h <文件> --header-include <路径> 文件…
"""

import argparse
import os
import re
import sys

DIRECTIVE = re.compile(r'^// @(\w+)\s+(.*)$')


class Module:
    def __init__(self, name):
        self.name = name
        self.code = ''
        self.requires = []
        self.scriptlets = []
        self.aliases = []


def fail(message):
    sys.stderr.write('generate_scriptlets.py: %s\n' % message)
    sys.exit(1)


def parse(path):
    module = Module(os.path.splitext(os.path.basename(path))[0])
    lines = []
    with open(path, encoding='utf-8') as f:
        for number, raw in enumerate(f, 1):
            line = raw.strip()
            match = DIRECTIVE.match(line)
            if match:
                kind, rest = match.group(1), match.group(2).split()
                if kind == 'requires' and len(rest) == 1:
                    module.requires.append(rest[0])
                elif kind == 'scriptlet' and len(rest) in (2, 3):
                    if len(rest) == 3 and rest[2] != 'trusted':
                        fail('%s:%d: 只能写 trusted' % (path, number))
                    module.scriptlets.append((rest[0], rest[1], len(rest) == 3))
                elif kind == 'alias' and len(rest) == 2:
                    module.aliases.append((rest[0], rest[1]))
                else:
                    fail('%s:%d: 看不懂 %s' % (path, number, line))
                continue
            if line == '' or line.startswith('//'):
                continue
            if any(ord(c) > 0x7e or (ord(c) < 0x20 and c != '\t') for c in line):
                fail('%s:%d: 代码里只能是 ASCII（注释写在单独的行上）' % (path, number))
            lines.append(line)
    module.code = '\n'.join(lines) + '\n'
    for _, function, _ in module.scriptlets:
        if not re.search(r'^function %s\(' % re.escape(function), module.code, re.M):
            fail('%s: 没有函数 %s' % (path, function))
    return module


def c_chars(data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out-cc', required=True)
    parser.add_argument('--out-h', required=True)
    parser.add_argument('--header-include', required=True)
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()

    modules = {}
    for path in args.files:
        module = parse(path)
        modules[module.name] = module
    if 'core' not in modules:
        fail('没有 core.js')

    # 模块的顺序：core 第一个，之后按名字；每个模块要先放的（不含 core 和自己），按依赖排好。
    order = ['core'] + sorted(name for name in modules if name != 'core')
    index = {name: i for i, name in enumerate(order)}

    def closure(name, stack):
        if name in stack:
            fail('循环依赖：%s' % ' -> '.join(stack + [name]))
        result = []
        for required in modules[name].requires:
            if required not in modules:
                fail('%s 要的 %s 不存在' % (name, required))
            if required == 'core':
                continue
            for item in closure(required, stack + [name]) + [required]:
                if item not in result:
                    result.append(item)
        return result

    scriptlets = {}
    for name in order:
        for scriptlet, function, trusted in modules[name].scriptlets:
            if scriptlet in scriptlets:
                fail('脚本片段 %s 写了两次' % scriptlet)
            scriptlets[scriptlet] = (function, index[name], trusted)
    names = sorted(scriptlets)
    scriptlet_index = {name: i for i, name in enumerate(names)}
    aliases = {}
    for name in order:
        for alias, target in modules[name].aliases:
            if target not in scriptlets:
                fail('别名 %s 指向不存在的 %s' % (alias, target))
            if alias in scriptlets or alias in aliases:
                fail('别名 %s 重复' % alias)
            aliases[alias] = target

    guard = args.header_include.upper().replace('/', '_').replace('.', '_') + '_'
    generated = '// 由 starry/components/content_filter/scriptlets/generate_scriptlets.py 生成，不要手改。\n\n'
    with open(args.out_h, 'w', encoding='utf-8') as h:
        h.write(generated)
        h.write('#ifndef %s\n#define %s\n\n' % (guard, guard))
        h.write('#include <cstdint>\n#include <string_view>\n\n')
        h.write('#include "base/containers/span.h"\n\n')
        h.write('namespace starry::content_filter::scriptlet_library {\n\n')
        h.write('struct Module {\n')
        h.write('  std::string_view name;\n')
        h.write('  std::string_view code;\n')
        h.write('  // 要先放进去的模块（按顺序，不含 core 和自己）。\n')
        h.write('  base::span<const uint8_t> dependencies;\n')
        h.write('};\n\n')
        h.write('struct Scriptlet {\n')
        h.write('  std::string_view name;\n')
        h.write('  std::string_view function;\n')
        h.write('  uint8_t module;\n')
        h.write('  bool trusted;\n')
        h.write('};\n\n')
        h.write('struct Alias {\n')
        h.write('  std::string_view alias;\n')
        h.write('  uint16_t scriptlet;\n')
        h.write('};\n\n')
        h.write('// 所有模块，第 0 个是 core。\n')
        h.write('base::span<const Module> Modules();\n')
        h.write('// 所有脚本片段，按名字排好。\n')
        h.write('base::span<const Scriptlet> Scriptlets();\n')
        h.write('// 别名，按别名排好。\n')
        h.write('base::span<const Alias> Aliases();\n\n')
        h.write('}  // namespace starry::content_filter::scriptlet_library\n\n')
        h.write('#endif  // %s\n' % guard)

    with open(args.out_cc, 'w', encoding='utf-8') as cc:
        cc.write(generated)
        cc.write('#include "%s"\n\n' % args.header_include)
        cc.write('#include <array>\n\n')
        cc.write('namespace starry::content_filter::scriptlet_library {\n\nnamespace {\n\n')
        for i, name in enumerate(order):
            data = modules[name].code.encode('ascii')
            # 字节数组而不是字符串字面量：MSVC 对字符串字面量的长度有限制。
            cc.write('constexpr char kCode%d[] = {\n%s\n};\n\n' % (i, c_chars(data)))
            deps = [index[d] for d in closure(name, [])]
            if deps:
                cc.write('constexpr uint8_t kDependencies%d[] = {%s};\n\n' %
                         (i, ', '.join(str(d) for d in deps)))
        cc.write('constexpr auto kModules = std::to_array<Module>({\n')
        for i, name in enumerate(order):
            deps = closure(name, [])
            cc.write('    {"%s", std::string_view(kCode%d, sizeof(kCode%d)), %s},\n' %
                     (name, i, i, 'base::span<const uint8_t>(kDependencies%d)' % i
                      if deps else 'base::span<const uint8_t>()'))
        cc.write('});\n\n')
        cc.write('constexpr auto kScriptlets = std::to_array<Scriptlet>({\n')
        for name in names:
            function, module, trusted = scriptlets[name]
            cc.write('    {"%s", "%s", %d, %s},\n' %
                     (name, function, module, 'true' if trusted else 'false'))
        cc.write('});\n\n')
        cc.write('constexpr auto kAliases = std::to_array<Alias>({\n')
        for alias in sorted(aliases):
            cc.write('    {"%s", %d},\n' % (alias, scriptlet_index[aliases[alias]]))
        cc.write('});\n\n')
        cc.write('}  // namespace\n\n')
        cc.write('base::span<const Module> Modules() {\n  return kModules;\n}\n\n')
        cc.write('base::span<const Scriptlet> Scriptlets() {\n  return kScriptlets;\n}\n\n')
        cc.write('base::span<const Alias> Aliases() {\n  return kAliases;\n}\n\n')
        cc.write('}  // namespace starry::content_filter::scriptlet_library\n')


if __name__ == '__main__':
    main()
