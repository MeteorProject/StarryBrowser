// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UI_DESIGN_METRICS_H_
#define STARRY_BROWSER_UI_DESIGN_METRICS_H_

namespace starry {

// 界面圆角与布局尺寸常量（单位 DIP）。

namespace radius {
inline constexpr int kButton = 6;
inline constexpr int kMenu = 8;
inline constexpr int kRow = 9;
inline constexpr int kCard = 12;
inline constexpr int kLarge = 16;
// 网页的内容卡片。
inline constexpr int kContent = 10;
}  // namespace radius

namespace metrics {
inline constexpr int kWindowMinWidth = 720;
inline constexpr int kWindowMinHeight = 480;

inline constexpr int kSidebarWidth = 248;
// 拖动右边缘能调到的范围；边缘的感应区 8 宽。
inline constexpr int kSidebarMinWidth = 200;
inline constexpr int kSidebarMaxWidth = 360;
inline constexpr int kSidebarResizeHandleWidth = 8;
// 自动隐藏的侧栏收起时，窗口左边缘的感应条。
inline constexpr int kSidebarEdgeTriggerWidth = 8;
inline constexpr int kFloatingSidebarInset = 8;
inline constexpr int kFloatingSidebarRadius = 12;
inline constexpr int kSidebarRowHeight = 34;
inline constexpr int kSidebarHeaderHeight = 44;
// 侧栏里的内容离侧栏左右边的距离。
inline constexpr int kSidebarPadding = 10;

// 内容卡片上方的工具栏（地址栏、扩展按钮移到顶部时），和侧栏顶栏同高、同一行。
inline constexpr int kToolbarHeight = 44;
// 紧凑地址栏：内容卡片顶部的细栏，按钮和工具栏里一样大（28），上下各留 4。
inline constexpr int kCompactToolbarHeight = 36;
// 顶部标签布局的标签行（红绿灯也在这一行）和里面的标签。
inline constexpr int kTabStripHeight = 40;
inline constexpr int kTopTabHeight = 30;
inline constexpr int kTopTabMinWidth = 112;
inline constexpr int kTopTabMaxWidth = 220;
inline constexpr int kTopPinnedTabWidth = 38;
// 红绿灯占的宽度（从侧栏或窗口的左边算起），侧栏顶栏的内容从这里开始。
inline constexpr int kWindowButtonsWidth = 70;
// 顶部标签行里第一个标签的起点：让开红绿灯（x = 16 起的三个按钮）再留一点空。
inline constexpr int kTabStripLeading = 88;
// 红绿灯（关闭按钮的左上角）离侧栏、窗口左边的距离和按钮的高度。
inline constexpr int kWindowButtonsLeading = 16;
inline constexpr int kWindowButtonHeight = 14;

// 内容卡片离窗口边的距离。
inline constexpr int kContentInset = 8;
// Windows 纯侧栏无顶栏模式下保留的顶部拖动栏高度。
inline constexpr int kWindowRailHeight = 30;
// 助手停靠在右侧栏时的固定宽度。
inline constexpr int kAssistantDockWidth = 392;
inline constexpr int kAddressPillHeight = 34;
inline constexpr int kTabRowHeight = 34;
inline constexpr int kFaviconSize = 16;
// 工具栏、侧栏顶栏里的图标按钮。
inline constexpr int kToolButtonSize = 28;

// 侧栏顶部的固定标签网格。
inline constexpr int kPinnedTileHeight = 40;
inline constexpr int kPinnedColumns = 4;
inline constexpr int kPinnedSpacing = 6;

// 命令栏（⌘L / ⌘T）：居中，离内容卡片顶边 96，每行建议 40。
inline constexpr int kCommandBarWidth = 640;
inline constexpr int kCommandBarTopOffset = 96;
inline constexpr int kCommandBarRowHeight = 40;
inline constexpr int kFindBarWidth = 360;

// 对话框的宽度、离窗口边的最小距离、最高多高。
inline constexpr int kDialogWidth = 440;
inline constexpr int kDialogMargin = 24;
inline constexpr int kDialogMaxHeight = 640;

// 标签的悬停预览。
inline constexpr int kTabPreviewWidth = 264;
inline constexpr int kTabPreviewImageHeight = 148;
}  // namespace metrics

}  // namespace starry

#endif  // STARRY_BROWSER_UI_DESIGN_METRICS_H_
