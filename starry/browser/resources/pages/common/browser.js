// Copyright 2026 The Starry Browser Authors
//
// 和浏览器说话（starry/browser/ui/webui/page_handler.h）：菜单、对话框用 Starry 自己的
// （和侧栏、标签的是同一个玻璃菜单、对话框层），打开网址、拷贝。

import {addWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';

import {L} from './l10n.js';

export {addWebUiListener, sendWithPromise};

export function send(name, ...args) {
  chrome.send(name, args);
}

// 在鼠标处（event）或者挂在元素下面（element）打开菜单。items 见 page_handler.h。
// 选了一项回它的 id，没选回 null。
export function showMenu(items, {event, element} = {}) {
  let anchor = {x: 0, y: 0};
  if (element) {
    const rect = element.getBoundingClientRect();
    anchor = {x: rect.left, y: rect.top, width: rect.width, height: rect.height, below: true};
  } else if (event) {
    anchor = {x: event.clientX, y: event.clientY};
  }
  return sendWithPromise('showMenu', items, anchor);
}

// 对话框：{icon, tone, title, message, field, buttons: [{title, variant, cancel}]}。
// 回 {button, input}；button 是按钮的序号，没回答（窗口关了）时没有。
export function showDialog(spec) {
  return sendWithPromise('showDialog', spec);
}

// 确认框：按了确认的按钮回 true。
export async function confirm({icon, tone = 'danger', title, message, confirmTitle, cancelTitle = L('common.cancel')}) {
  const result = await showDialog({
    icon, tone, title, message,
    buttons: [
      {title: cancelTitle, variant: 'tertiary', cancel: true},
      {title: confirmTitle, variant: tone === 'danger' ? 'danger' : 'filled'},
    ],
  });
  return result && result.button === 1;
}

// 打开的方式，和网页里点链接一样：单击当前标签，⌘（Windows、Linux 是 Ctrl）单击后台新标签，
// 再按 ⇧ 是前台新标签。
export function targetFromEvent(event) {
  const isMac = navigator.userAgentData ? navigator.userAgentData.platform === 'macOS' :
                                          navigator.platform.startsWith('Mac');
  const modifier = isMac ? event.metaKey : event.ctrlKey;
  if (event.button === 1 || modifier) {
    return event.shiftKey ? 'foreground' : 'background';
  }
  if (event.shiftKey) {
    return 'window';
  }
  return 'current';
}

export function openUrl(url, target = 'current') {
  send('openUrl', url, target);
}

export function copyText(text) {
  send('copyText', text);
}

// 网站图标（Chrome 的 favicon 服务）。
export function faviconUrl(pageUrl, size = 16) {
  const params = new URLSearchParams({
    size: String(size),
    scaleFactor: `${Math.max(1, Math.round(devicePixelRatio))}x`,
    pageUrl,
    allowGoogleServerFallback: '0',
  });
  return `chrome://favicon2/?${params}`;
}
