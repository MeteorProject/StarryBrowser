#!/usr/bin/env python3
"""界面文字的多语言工具，核对语言文件与检查未本地化文案。

  scripts/l10n.py check            核对语言文件和代码：键是否齐全、占位符是否一致、代码里用到的键是否都有
      [--with <目录>]…              先把目录里的 <语言>.json 叠加上去再核对（合并之前检查一批新文字）
      [--unused]                    列出代码里没用到的键
  scripts/l10n.py hardcoded        列出代码里还写死的中文（字符串字面量里的）
  scripts/l10n.py format           把语言文件按键排序、统一缩进（编辑之后跑一下）
  scripts/l10n.py merge <目录>...  把每个目录里的 <语言>.json 合并进语言文件（同一个键的值不同会报错）
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "starry/browser/resources/l10n")
SOURCE_DIR = os.path.join(ROOT, "starry")
BASE = "zh-Hans"
LANGUAGES = ["zh-Hans", "zh-Hant", "en", "ja", "ko"]
PLURAL_FORMS = {"one", "other"}
PLACEHOLDER = re.compile(r"\{([A-Za-z0-9_]+)\}")
# 代码里的 L("键") / L("键", …)，网页里还有 L('键')，页面的 HTML 里是 $L{键}。
USAGE = re.compile(r"""\bL\(\s*(?:"([^"\\]+)"|'([^'\\]+)')|\$L\{([^}]+)\}""")
SOURCE_EXTENSIONS = (".cc", ".h", ".mm", ".js", ".html")
# 写死中文不算问题的文件：语言自己的名字与构建脚本。
HARDCODED_ALLOWED = (
    "starry/browser/l10n/l10n.cc",
    "starry/browser/resources/embed_resources.py",
    "starry/browser/resources/pages/common/l10n.js",
)
CJK = re.compile(r"[぀-ヿ㐀-鿿가-힯＀-￯　-〿]")
LITERAL = re.compile(r'''u?"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'|`((?:[^`\\]|\\.)*)`''')


def path(language):
    return os.path.join(LANG_DIR, f"{language}.json")


def load(language):
    with open(path(language), encoding="utf-8") as f:
        return json.load(f)


def save(language, table):
    with open(path(language), "w", encoding="utf-8") as f:
        json.dump(dict(sorted(table.items())), f, ensure_ascii=False, indent=2)
        f.write("\n")


def placeholders(value):
    texts = value.values() if isinstance(value, dict) else [value]
    return {name for text in texts for name in PLACEHOLDER.findall(text)}


def source_files():
    for dirpath, _, filenames in os.walk(SOURCE_DIR):
        for filename in sorted(filenames):
            if filename.endswith(SOURCE_EXTENSIONS):
                yield os.path.join(dirpath, filename)


def code_usages():
    usages = {}
    for file in source_files():
        with open(file, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                if line.lstrip().startswith("//"):
                    continue
                for groups in USAGE.findall(line):
                    usages.setdefault(next(g for g in groups if g), f"{os.path.relpath(file, ROOT)}:{number}")
    return usages


def check(overlays=(), list_unused=False):
    errors, warnings = [], []
    tables = {}
    for language in LANGUAGES:
        try:
            tables[language] = load(language)
            for directory in overlays:
                with open(os.path.join(directory, f"{language}.json"), encoding="utf-8") as f:
                    tables[language].update(json.load(f))
        except (OSError, json.JSONDecodeError) as error:
            errors.append(f"{language}.json：读不了（{error}）")
    if errors:
        return report(errors, warnings)
    base = tables[BASE]
    for language, table in tables.items():
        for key, value in table.items():
            if key.startswith("//"):
                continue
            if isinstance(value, dict):
                if "other" not in value or not set(value) <= PLURAL_FORMS or not all(isinstance(v, str) for v in value.values()):
                    errors.append(f"{language}.json「{key}」：复数写法只能有 one、other 两项，而且必须有 other")
            elif not isinstance(value, str):
                errors.append(f"{language}.json「{key}」：值要是字符串或复数对象")
        if language == BASE:
            continue
        missing = sorted(k for k in base if k not in table and not k.startswith("//"))
        extra = sorted(k for k in table if k not in base and not k.startswith("//"))
        for key in missing:
            errors.append(f"{language}.json：缺少「{key}」")
        for key in extra:
            errors.append(f"{language}.json：多出「{key}」（{BASE}.json 里没有）")
        for key, value in table.items():
            if key in base and not key.startswith("//"):
                unknown = placeholders(value) - placeholders(base[key])
                if unknown:
                    errors.append(f"{language}.json「{key}」：{BASE} 里没有的占位符 {sorted(unknown)}")
                lost = placeholders(base[key]) - placeholders(value) - {"count"}
                if lost:
                    warnings.append(f"{language}.json「{key}」：没用到占位符 {sorted(lost)}")
    usages = code_usages()
    for key, location in sorted(usages.items()):
        if key not in base:
            errors.append(f"{location}：「{key}」不在 {BASE}.json 里")
    unused = sorted(k for k in base if k not in usages and not k.startswith("//"))
    if list_unused:
        for key in unused:
            warnings.append(f"代码里没用到「{key}」")
    print(f"{len(base)} 个键，代码里用到 {len(usages)} 个，没用到的 {len(unused)} 个（--unused 列出）")
    return report(errors, warnings)


def hardcoded():
    found = 0
    for file in source_files():
        relative = os.path.relpath(file, ROOT)
        if any(relative.startswith(prefix) for prefix in HARDCODED_ALLOWED):
            continue
        with open(file, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                stripped = line.strip()
                if stripped.startswith(("//", "*", "/*", "#")):
                    continue
                code = re.sub(r"\s//.*$", "", line)
                if not CJK.search(code):
                    continue
                literals = [m.group(1) or m.group(2) or m.group(3) or "" for m in LITERAL.finditer(code)]
                outside = LITERAL.sub("", code)
                if any(CJK.search(text) for text in literals) or (relative.endswith(".js") and CJK.search(outside)):
                    found += 1
                    print(f"{relative}:{number}: {stripped[:120]}")
    print(f"{found} 处")
    return 1 if found else 0


def report(errors, warnings):
    for warning in warnings:
        print(f"warning: {warning}")
    for error in errors:
        print(f"error: {error}")
    return 1 if errors else 0


def format_files():
    for language in LANGUAGES:
        save(language, load(language))
    return 0


def merge(directories):
    tables = {language: load(language) for language in LANGUAGES}
    conflicts = []
    for directory in directories:
        for language in LANGUAGES:
            fragment_path = os.path.join(directory, f"{language}.json")
            if not os.path.exists(fragment_path):
                conflicts.append(f"{fragment_path}：不存在")
                continue
            with open(fragment_path, encoding="utf-8") as f:
                fragment = json.load(f)
            for key, value in fragment.items():
                current = tables[language].get(key)
                if current is not None and current != value:
                    conflicts.append(f"{language}「{key}」：{current!r} ≠ {value!r}（{directory}）")
                else:
                    tables[language][key] = value
    if conflicts:
        for conflict in conflicts:
            print(f"error: {conflict}")
        return 1
    for language in LANGUAGES:
        save(language, tables[language])
    print(f"已合并，{len(tables[BASE])} 个键")
    return 0


def main(argv):
    command = argv[1] if len(argv) > 1 else "check"
    if command == "check":
        overlays, list_unused, rest = [], False, argv[2:]
        while rest:
            if rest[0] == "--unused":
                list_unused, rest = True, rest[1:]
            elif rest[0] == "--with" and len(rest) > 1:
                overlays.append(rest[1])
                rest = rest[2:]
            else:
                print(__doc__)
                return 2
        return check(overlays, list_unused)
    if command == "hardcoded":
        return hardcoded()
    if command == "format":
        return format_files()
    if command == "merge" and len(argv) > 2:
        return merge(argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
