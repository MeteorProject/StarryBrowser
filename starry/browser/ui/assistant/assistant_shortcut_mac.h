// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_SHORTCUT_MAC_H_
#define STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_SHORTCUT_MAC_H_

namespace starry {

// ⌘E 在 Chrome 的菜单栏里是「编辑 › 查找 › 用所选内容查找」，菜单栏先拿到按键：把那一项
// 的快捷键拿掉，⌘E 才轮到助手（窗口的快捷键）。菜单项还在，点它照常用。
void ReleaseCommandEShortcut();

}  // namespace starry

#endif  // STARRY_BROWSER_UI_ASSISTANT_ASSISTANT_SHORTCUT_MAC_H_
