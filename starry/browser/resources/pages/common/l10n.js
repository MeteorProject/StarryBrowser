// Copyright 2026 The Starry Browser Authors
//
// 界面文字的多语言（同 Views 界面的 starry/browser/l10n/l10n.h，同一份语言文件）。
// /strings.js 是浏览器按界面语言生成的文字表，后备语言已经并进去了。
//
//   L('downloads.page.title')
//   L('downloads.quit.paused', {count: 3})
//
// 键一律写成字面量，scripts/l10n.py check 才能核对。

import data from '../strings.js';

// 界面语言：zh-Hans、zh-Hant、en、ja、ko。
export const language = data.language;
// 日期、数字的格式用的区域（Intl 的 locale）。
export const locale = data.locale;
// mac、win、linux：「在访达中显示」这类各平台叫法不同的文字。
export const platform = data.platform;

document.documentElement.lang = language;

// 键对应的文字，{name} 换成参数；count 还用来选复数写法（英文 1 用 one）。
// 没有这个键时是键本身。
export function L(key, args) {
  let entry = data.strings[key];
  if (entry === undefined) {
    console.warn(`语言文件里没有「${key}」`);
    return key;
  }
  if (typeof entry === 'object') {
    const count = args ? Number(args.count) : NaN;
    entry = (language === 'en' && count === 1 && entry.one !== undefined) ? entry.one : entry.other;
  }
  if (!args) {
    return entry;
  }
  return entry.replace(/\{(\w+)\}/g, (match, name) => (name in args ? String(args[name]) : match));
}

// 把几项连成一个列表（「A、B」/「A, B」）。
export function joinList(items) {
  return items.join(L('common.listSeparator'));
}

// 「在访达中显示」（macOS）、「在资源管理器中显示」（Windows）、「在文件夹中显示」（Linux）。
export function showInFolderLabel() {
  if (platform === 'mac') {
    return L('common.showInFinder');
  }
  return platform === 'win' ? L('common.showInExplorer') : L('common.showInFolder');
}
