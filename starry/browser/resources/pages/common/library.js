// Copyright 2026 The Starry Browser Authors
//
// 资料库页面（历史记录、书签、下载）共用的标题区、空状态、列表行与图标组件。

import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {L} from './l10n.js';
import {reveal} from './motion.js';

// 16×16 SVG 图标表，与 Views 界面的 starry/browser/ui/design/icons.cc 保持一致。
const ICONS = {
  'magnifyingglass': '<circle cx="7" cy="7" r="4.6" stroke-width="1.7"/><path d="M10.4 10.4 13.6 13.6" stroke-width="1.7"/>',
  'xmark': '<path d="M4 4 12 12M12 4 4 12" stroke-width="1.75"/>',
  'xmark.circle.fill': '<circle cx="8" cy="8" r="6.5" fill="currentColor" stroke="none"/><path d="M5.6 5.6 10.4 10.4M10.4 5.6 5.6 10.4" stroke="var(--surface-panel)" stroke-width="1.5"/>',
  'trash': '<path d="M2.6 4h10.8M6.2 4V2.5h3.6V4M3.9 4l.7 8.6a1.2 1.2 0 0 0 1.2 1.1h4.4a1.2 1.2 0 0 0 1.2-1.1L12.1 4M6.7 6.4l.2 4.8M9.3 6.4l-.2 4.8" stroke-width="1.3"/>',
  'clock': '<circle cx="8" cy="8" r="6" stroke-width="1.5"/><path d="M8 4.5V8l2.5 1.6" stroke-width="1.5"/>',
  'calendar': '<rect x="2.2" y="3.2" width="11.6" height="10.4" rx="2" stroke-width="1.3"/><path d="M2.2 6.5h11.6M5.4 2v2.4M10.6 2v2.4" stroke-width="1.3"/>',
  'star': '<path d="M8 2.1 9.53 6.2l4.37.18-3.43 2.72 1.17 4.21L8 10.9l-3.64 2.41 1.17-4.21L2.1 6.38l4.37-.18Z" stroke-width="1.4" stroke-linejoin="round"/>',
  'star.fill': '<path d="M8 2.1 9.53 6.2l4.37.18-3.43 2.72 1.17 4.21L8 10.9l-3.64 2.41 1.17-4.21L2.1 6.38l4.37-.18Z" fill="currentColor" stroke="none"/>',
  'star.slash': '<path d="M8 2.1 9.53 6.2l4.37.18-3.43 2.72 1.17 4.21L8 10.9l-3.64 2.41 1.17-4.21L2.1 6.38l4.37-.18Z" stroke-width="1.35" stroke-linejoin="round"/><path d="M2.4 2.4 13.6 13.6" stroke-width="1.4"/>',
  'plus.square.on.square': '<rect x="4.8" y="2.2" width="9" height="9" rx="1.8" stroke-width="1.3"/><path d="M3.2 5.4H3a1.6 1.6 0 0 0-1.6 1.6v5.2A1.6 1.6 0 0 0 3 13.8h5.2a1.6 1.6 0 0 0 1.6-1.6v-.2M9.3 4.6v4.2M7.2 6.7h4.2" stroke-width="1.3"/>',
  'globe': '<circle cx="8" cy="8" r="6" stroke-width="1.3"/><ellipse cx="8" cy="8" rx="2.6" ry="6" stroke-width="1.3"/><path d="M2 8h12M3.2 5h9.6M3.2 11h9.6" stroke-width="1.3"/>',
  'arrow.down': '<path d="M8 2.6v10.6M3.8 9 8 13.2 12.2 9" stroke-width="1.6"/>',
  'folder': '<path d="M2 4.6a1.6 1.6 0 0 1 1.6-1.6h2.6l1.5 1.6h4.7A1.6 1.6 0 0 1 14 6.2v5.6a1.6 1.6 0 0 1-1.6 1.6H3.6A1.6 1.6 0 0 1 2 11.8Z" stroke-width="1.3" stroke-linejoin="round"/>',
  'plus': '<path d="M8 2.75v10.5M2.75 8h10.5" stroke-width="1.75"/>',
  'ellipsis': '<circle cx="3.5" cy="8" r="1.25" fill="currentColor" stroke="none"/><circle cx="8" cy="8" r="1.25" fill="currentColor" stroke="none"/><circle cx="12.5" cy="8" r="1.25" fill="currentColor" stroke="none"/>',
  'pause.fill': '<rect x="3.6" y="2.8" width="3" height="10.4" rx="1" fill="currentColor" stroke="none"/><rect x="9.4" y="2.8" width="3" height="10.4" rx="1" fill="currentColor" stroke="none"/>',
  'play.rectangle': '<rect x="1.5" y="3" width="13" height="10" rx="2.2" stroke-width="1.3"/><path d="m6.2 5.3 4.2 2.7-4.2 2.7Z" fill="currentColor" stroke="none"/>',
  'play.fill': '<path d="M4.5 3.1a.9.9 0 0 1 1.36-.77l7.2 4.9a.9.9 0 0 1 0 1.5l-7.2 4.9a.9.9 0 0 1-1.36-.77Z" fill="currentColor" stroke="none"/>',
  'arrow.clockwise': '<path d="M11.75 4.85A5.3 5.3 0 1 1 8 3.3M6.8 1 9.1 3.3 6.8 5.6" stroke-width="1.6"/>',
  'arrow.down.circle': '<circle cx="8" cy="8" r="6.1" stroke-width="1.4"/><path d="M8 4.6V11M5.4 8.6 8 11.2l2.6-2.6" stroke-width="1.4"/>',
  'checkmark.circle': '<circle cx="8" cy="8" r="6.1" stroke-width="1.4"/><path d="M5.2 8.2 7.1 10.1 10.9 6" stroke-width="1.4"/>',
  'exclamationmark.circle': '<circle cx="8" cy="8" r="6.1" stroke-width="1.4"/><path d="M8 4.6v4.2" stroke-width="1.5"/><circle cx="8" cy="11" r=".9" fill="currentColor" stroke="none"/>',
  'exclamationmark': '<path d="M8 3v6.4" stroke-width="2.2"/><circle cx="8" cy="12.6" r="1.3" fill="currentColor" stroke="none"/>',
  'clock.fill': '<circle cx="8" cy="8" r="6.5" fill="currentColor" stroke="none"/><path d="M8 4.6V8l2.2 1.4" stroke="var(--surface-alt)" stroke-width="1.5"/>',
  'checkmark': '<path d="M3.5 8.5 6.6 11.6 12.5 4.5" stroke-width="1.8"/>',
  'pencil': '<path d="M10.4 3.1 12.9 5.6 5.4 13.1 2.5 13.5 2.9 10.6Z" stroke-width="1.3" stroke-linejoin="round"/><path d="M9 4.5 11.5 7" stroke-width="1.3"/>',
  'link': '<path d="M1.97 10.77 5.77 6.97a2.3 2.3 0 0 1 3.25 3.25l-3.8 3.8a2.3 2.3 0 0 1-3.25-3.25ZM6.97 5.77l3.8-3.8a2.3 2.3 0 0 1 3.25 3.25l-3.8 3.8a2.3 2.3 0 0 1-3.25-3.25Z" stroke-width="1.5"/>',
  'chevron.right': '<path d="M6 3.2 10.8 8 6 12.8" stroke-width="1.8" stroke-linejoin="round"/>',
  'chevron.left': '<path d="M10 3.2 5.2 8 10 12.8" stroke-width="1.8" stroke-linejoin="round"/>',
  'arrow.right': '<path d="M2.8 8h10.4M9 3.8 13.2 8 9 12.2" stroke-width="1.6" stroke-linejoin="round"/>',
  'arrow.up.forward': '<path d="M4.5 11.5 11.5 4.5M5.5 4.5h6v6" stroke-width="1.6" stroke-linejoin="round"/>',
  'gearshape': '<path d="M6.9 1.8h2.2l.4 1.8 1.2.7 1.8-.6 1.1 1.9-1.4 1.2v1.4l1.4 1.2-1.1 1.9-1.8-.6-1.2.7-.4 1.8H6.9l-.4-1.8-1.2-.7-1.8.6-1.1-1.9 1.4-1.2V7.3L2.4 6.1l1.1-1.9 1.8.6 1.2-.7Z" stroke-width="1.2" stroke-linejoin="round"/><circle cx="8" cy="8" r="2.1" stroke-width="1.2"/>',
  'paintpalette': '<path d="M8 2a6 6 0 0 0 0 12c1 0 1.5-.6 1.5-1.3 0-.9-.8-1.2-.8-2 0-.7.6-1.2 1.3-1.2h1.6A2.4 2.4 0 0 0 14 7.1C14 4.2 11.3 2 8 2Z" stroke-width="1.3" stroke-linejoin="round"/><circle cx="5" cy="7.6" r=".9" fill="currentColor" stroke="none"/><circle cx="6.6" cy="4.9" r=".9" fill="currentColor" stroke="none"/><circle cx="9.7" cy="4.9" r=".9" fill="currentColor" stroke="none"/>',
  'speedometer': '<path d="M2.3 11.6a6 6 0 1 1 11.4 0" stroke-width="1.4"/><path d="M8 9.4 10.6 5.6" stroke-width="1.5"/><circle cx="8" cy="9.6" r="1.1" fill="currentColor" stroke="none"/>',
  'hand.raised': '<path d="M5.4 8.6V3.6a1 1 0 0 1 2 0v4M7.4 7.4V2.6a1 1 0 0 1 2 0v4.8M9.4 7.4V3.4a1 1 0 0 1 2 0v5.4M11.4 7.6a1 1 0 0 1 2 0v1.6a5 5 0 0 1-5 5h-.3a4.6 4.6 0 0 1-3.6-1.8L2.3 9.6a1 1 0 0 1 1.5-1.3l1.6 1.5" stroke-width="1.2" stroke-linejoin="round"/>',
  'puzzlepiece': '<path d="M3 5h2.4a1.6 1.6 0 1 1 3.2 0H11v2.4a1.6 1.6 0 1 1 0 3.2V13H8.6a1.6 1.6 0 1 0-3.2 0H3v-2.4a1.6 1.6 0 1 0 0-3.2Z" stroke-width="1.25" stroke-linejoin="round"/>',
  'hammer': '<path d="M2.6 13.4 8.4 7.6M7 4.2l2.2-2.2 4.8 3-1.6 1.6-1.6-.8-1.6 1.6L7 5.2Z" stroke-width="1.3" stroke-linejoin="round"/>',
  'sparkles': '<path d="M7 2.5c.5 2.6 1.4 3.5 4 4-2.6.5-3.5 1.4-4 4-.5-2.6-1.4-3.5-4-4 2.6-.5 3.5-1.4 4-4ZM12 9.5c.3 1.3.7 1.7 2 2-1.3.3-1.7.7-2 2-.3-1.3-.7-1.7-2-2 1.3-.3 1.7-.7 2-2Z" stroke-width="1.1" stroke-linejoin="round"/>',
  'safari': '<circle cx="8" cy="8" r="6" stroke-width="1.3"/><path d="M10.6 5.4 9 9 5.4 10.6 7 7Z" stroke-width="1.1" stroke-linejoin="round"/>',
  'arrow.up.right.square': '<rect x="2.5" y="2.5" width="11" height="11" rx="2.4" stroke-width="1.3"/><path d="M6 10 10 6M6.6 6H10v3.4" stroke-width="1.3" stroke-linejoin="round"/>',
  'square.and.arrow.down': '<path d="M5.4 5.6H4.2a1.4 1.4 0 0 0-1.4 1.4v5.4a1.4 1.4 0 0 0 1.4 1.4h7.6a1.4 1.4 0 0 0 1.4-1.4V7a1.4 1.4 0 0 0-1.4-1.4h-1.2M8 1.8v7.6M5.6 7.2 8 9.6l2.4-2.4" stroke-width="1.3" stroke-linejoin="round"/>',
  'lock': '<rect x="3.4" y="7" width="9.2" height="7" rx="1.6" stroke-width="1.3"/><path d="M5.4 7V5.2a2.6 2.6 0 0 1 5.2 0V7" stroke-width="1.3"/>',
  'chevron.up.chevron.down': '<path d="M4.6 6 8 2.6 11.4 6M4.6 10 8 13.4 11.4 10" stroke-width="1.9" stroke-linejoin="round"/>',
  'sidebar.left': '<rect x="2" y="3" width="12" height="10" rx="2.2" stroke-width="1.3"/><path d="M6.2 3v10" stroke-width="1.3"/>',
  'sidebar.right': '<rect x="2" y="3" width="12" height="10" rx="2.2" stroke-width="1.3"/><path d="M9.8 3v10" stroke-width="1.3"/>',
  'rectangle.bottomthird.inset.filled': '<rect x="2" y="3" width="12" height="10" rx="2.2" stroke-width="1.3"/><rect x="4" y="9" width="8" height="2.4" rx="1.1" fill="currentColor" stroke="none"/>',
  'camera': '<path d="M2.2 5.7a1.4 1.4 0 0 1 1.4-1.4h1.6l1-1.5h3.6l1 1.5h1.6a1.4 1.4 0 0 1 1.4 1.4v5.9a1.4 1.4 0 0 1-1.4 1.4H3.6a1.4 1.4 0 0 1-1.4-1.4Z" stroke-width="1.3" stroke-linejoin="round"/><circle cx="8" cy="8.5" r="2.4" stroke-width="1.3"/>',
  'photo': '<rect x="2" y="3" width="12" height="10" rx="2" stroke-width="1.3"/><circle cx="5.7" cy="6.3" r="1.1" fill="currentColor" stroke="none"/><path d="M2.6 11.6 6 8.5l2.4 2.1 1.8-1.6 3.2 2.7" stroke-width="1.3" stroke-linejoin="round"/>',
  'circle': '<circle cx="8" cy="8" r="5.5" stroke-width="1.3"/>',
  'info.circle': '<circle cx="8" cy="8" r="6.1" stroke-width="1.4"/><path d="M8 7.2v4" stroke-width="1.5"/><circle cx="8" cy="5" r=".9" fill="currentColor" stroke="none"/>',
  'pin': '<path d="M5.6 2.4h4.8M6.6 2.4 6.2 6.6 4.4 9.4h7.2L9.8 6.6 9.4 2.4M8 9.4V14" stroke-width="1.35" stroke-linejoin="round"/>',
  'pin.fill': '<path d="M5.6 1.7h4.8a.7.7 0 0 1 0 1.4h-.35l.37 3.3 1.8 2.6a.7.7 0 0 1-.58 1.1H8.7v3.9a.7.7 0 0 1-1.4 0v-3.9H4.36a.7.7 0 0 1-.58-1.1l1.8-2.6.37-3.3H5.6a.7.7 0 0 1 0-1.4Z" fill="currentColor" stroke="none"/>',
  'doc.text': '<path d="M4.6 2h4.6L12 4.8v7.6A1.6 1.6 0 0 1 10.4 14H4.6A1.6 1.6 0 0 1 3 12.4V3.6A1.6 1.6 0 0 1 4.6 2Z" stroke-width="1.3" stroke-linejoin="round"/><path d="M9 2v3h3M5.6 8.2h4.8M5.6 10.8h3.4" stroke-width="1.3"/>',
  'doc.on.doc': '<rect x="5.4" y="4.8" width="7.6" height="9.2" rx="1.6" stroke-width="1.3"/><path d="M3 10.8V3.6A1.6 1.6 0 0 1 4.6 2h5.8" stroke-width="1.3"/>',
  'shield.lefthalf.filled': '<path d="M8 1.9 13 3.6v4c0 3.1-2 5.3-5 6.6-3-1.3-5-3.5-5-6.6v-4Z" stroke-width="1.35" stroke-linejoin="round"/><path d="M8 2.9v10.2c-2.3-1.1-4.1-3-4.1-5.5V4.2Z" fill="currentColor" stroke="none"/>',
  'shield.slash': '<path d="M8 1.9 13 3.6v4c0 3.1-2 5.3-5 6.6-3-1.3-5-3.5-5-6.6v-4Z" stroke-width="1.35" stroke-linejoin="round"/><path d="M2.2 2.2 13.8 13.8" stroke-width="1.35"/>',
  'rectangle.stack': '<rect x="2.2" y="6.2" width="11.6" height="7.8" rx="1.8" stroke-width="1.3"/><path d="M3.8 4.2h8.4M5.4 2.2h5.2" stroke-width="1.3"/>',
  'arrow.uturn.backward': '<path d="M5.8 3.2 2.6 6.4l3.2 3.2M2.9 6.4h6.7a3.5 3.5 0 0 1 0 7H7.4" stroke-width="1.5" stroke-linejoin="round"/>',
  'archivebox': '<rect x="1.9" y="2.6" width="12.2" height="3.4" rx="1" stroke-width="1.25"/><path d="M3 6v6.2a1.3 1.3 0 0 0 1.3 1.3h7.4a1.3 1.3 0 0 0 1.3-1.3V6" stroke-width="1.25"/><path d="M6.6 8.8h2.8" stroke-width="1.3"/>',
  'checkmark.shield': '<path d="M8 1.8 13 3.6v3.9c0 3.1-2.1 5.4-5 6.7-2.9-1.3-5-3.6-5-6.7V3.6Z" stroke-width="1.3" stroke-linejoin="round"/><path d="M5.7 8 7.3 9.6 10.4 6.4" stroke-width="1.3" stroke-linejoin="round"/>',
  'ladybug': '<ellipse cx="8" cy="9.4" rx="3.9" ry="4.4" stroke-width="1.3"/><path d="M8 5v8.8M5.5 3.3l1.1 1.6M10.5 3.3 9.4 4.9M2.4 8.6h1.7M11.9 8.6h1.7M2.8 12.6l1.6-.9M13.2 12.6l-1.6-.9" stroke-width="1.3"/>',
  'storefront': '<path d="M2.6 6.2 3.6 2.6h8.8l1 3.6M2.6 6.2a1.8 1.8 0 0 0 3.6 0 1.8 1.8 0 0 0 3.6 0 1.8 1.8 0 0 0 3.6 0M3.4 7.8v5.6h9.2V7.8M6.6 13.4V10.2h2.8v3.2" stroke-width="1.25" stroke-linejoin="round"/>',
  'arrow.triangle.2.circlepath': '<path d="M12.9 6.4A5.1 5.1 0 0 0 3.9 4.8M3.1 9.6a5.1 5.1 0 0 0 9 1.6M3.5 2.4v2.8h2.8M12.5 13.6v-2.8H9.7" stroke-width="1.4" stroke-linejoin="round"/>',
  'exclamationmark.triangle.fill': '<path d="M7.13 2.3a1 1 0 0 1 1.74 0l5.7 10.1a1 1 0 0 1-.87 1.5H2.3a1 1 0 0 1-.87-1.5Z" fill="currentColor" stroke="none"/><path d="M8 6.1v3.4" stroke="var(--surface-alt)" stroke-width="1.5"/><circle cx="8" cy="11.7" r=".85" fill="var(--surface-alt)" stroke="none"/>',
  // AI 助手：发送、停止、收起、新对话。
  'arrow.up': '<path d="M8 13V3.4M3.8 7.6 8 3.4l4.2 4.2" stroke-width="1.8" stroke-linejoin="round"/>',
  'stop.fill': '<rect x="4.2" y="4.2" width="7.6" height="7.6" rx="1.6" fill="currentColor" stroke="none"/>',
  'chevron.down': '<path d="M3 5.75 8 10.75 13 5.75" stroke-width="1.75" stroke-linejoin="round"/>',
  'square.and.pencil': '<path d="M13 8.6V12a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 3 12V5a1.5 1.5 0 0 1 1.5-1.5H8" stroke-width="1.3"/><path d="M11.9 2.5 13.5 4.1 7.9 9.7l-2.1.5.5-2.1Z" stroke-width="1.3" stroke-linejoin="round"/>',
  'bolt.fill': '<path d="M9.2 1.6 3.4 9h4l-.6 5.4L12.6 7h-4Z" fill="currentColor" stroke="currentColor" stroke-width=".8" stroke-linejoin="round"/>',
  'brain': '<path d="M6.4 2.6A2.1 2.1 0 0 0 4.3 4.7v.1A2.3 2.3 0 0 0 2.8 7a2.3 2.3 0 0 0 .7 1.7 2.3 2.3 0 0 0 1.6 3.6 2.1 2.1 0 0 0 4.1-.6V4.6a2 2 0 0 0-2.8-2ZM9.6 2.6a2.1 2.1 0 0 1 2.1 2.1v.1A2.3 2.3 0 0 1 13.2 7a2.3 2.3 0 0 1-.7 1.7 2.3 2.3 0 0 1-1.6 3.6 2.1 2.1 0 0 1-4.1-.6" stroke-width="1.2" stroke-linejoin="round"/>',
  'brain.fill': '<path d="M6.4 2.6A2.1 2.1 0 0 0 4.3 4.7v.1A2.3 2.3 0 0 0 2.8 7a2.3 2.3 0 0 0 .7 1.7 2.3 2.3 0 0 0 1.6 3.6 2.1 2.1 0 0 0 4.1-.6V4.6a2 2 0 0 0-2.8-2ZM9.6 2.6a2.1 2.1 0 0 1 2.1 2.1v.1A2.3 2.3 0 0 1 13.2 7a2.3 2.3 0 0 1-.7 1.7 2.3 2.3 0 0 1-1.6 3.6 2.1 2.1 0 0 1-4.1-.6V4.6a2 2 0 0 1 2.8-2Z" fill="currentColor" stroke-width="1.2" stroke-linejoin="round"/>',
  'checkmark.circle.fill': '<circle cx="8" cy="8" r="6.5" fill="currentColor" stroke="none"/><path d="M5.2 8.2 7.1 10.1 10.9 6" stroke="var(--surface-panel)" stroke-width="1.5" stroke-linejoin="round"/>',
  'exclamationmark.circle.fill': '<circle cx="8" cy="8" r="6.5" fill="currentColor" stroke="none"/><path d="M8 4.6v4.2" stroke="var(--surface-panel)" stroke-width="1.6"/><circle cx="8" cy="11.1" r=".95" fill="var(--surface-panel)" stroke="none"/>',
  'exclamationmark.shield.fill': '<path d="M8 1.6 13.2 3.5v4c0 3.2-2.2 5.6-5.2 7-3-1.4-5.2-3.8-5.2-7v-4Z" fill="currentColor" stroke="none"/><path d="M8 4.8v3.6" stroke="var(--surface-panel)" stroke-width="1.5"/><circle cx="8" cy="10.6" r=".9" fill="var(--surface-panel)" stroke="none"/>',
  'chevron.up': '<path d="M3 10.25 8 5.25 13 10.25" stroke-width="1.75" stroke-linejoin="round"/>',
  'stop.circle': '<circle cx="8" cy="8" r="6.1" stroke-width="1.4"/><rect x="5.9" y="5.9" width="4.2" height="4.2" rx=".8" fill="currentColor" stroke="none"/>',
  'text.alignleft': '<path d="M2.5 4h11M2.5 7h7M2.5 10h11M2.5 13h7" stroke-width="1.4"/>',
  'list.bullet': '<circle cx="3.2" cy="4" r="1" fill="currentColor" stroke="none"/><circle cx="3.2" cy="8" r="1" fill="currentColor" stroke="none"/><circle cx="3.2" cy="12" r="1" fill="currentColor" stroke="none"/><path d="M6 4h7.5M6 8h7.5M6 12h7.5" stroke-width="1.4"/>',
  'character.bubble': '<path d="M3.6 2.6h8.8a1.6 1.6 0 0 1 1.6 1.6v5.4a1.6 1.6 0 0 1-1.6 1.6H8.6L5.4 13.6v-2.4H3.6A1.6 1.6 0 0 1 2 9.6V4.2a1.6 1.6 0 0 1 1.6-1.6Z" stroke-width="1.3" stroke-linejoin="round"/><path d="M5.4 8.6 7.2 4.4h.4l1.8 4.2M5.9 7.4h3" stroke-width="1.1" stroke-linejoin="round"/>',
  'slider.horizontal.3': '<path d="M2 4h12M2 8h12M2 12h12" stroke-width="1.3"/><circle cx="10" cy="4" r="1.9" fill="currentColor" stroke="none"/><circle cx="5.5" cy="8" r="1.9" fill="currentColor" stroke="none"/><circle cx="9" cy="12" r="1.9" fill="currentColor" stroke="none"/>',
  'eye': '<path d="M1.8 8s2.4-4 6.2-4 6.2 4 6.2 4-2.4 4-6.2 4-6.2-4-6.2-4Z" stroke-width="1.3" stroke-linejoin="round"/><circle cx="8" cy="8" r="1.8" stroke-width="1.3"/>',
  'list.bullet.indent': '<path d="M3.2 3.2v8.8M3.2 7.6h3.2M3.2 12h4.8" stroke-width="1.35"/><path d="M6.4 3.2h6.8M8 7.6h5.2M9.6 12h3.6" stroke-width="1.5"/>',
  'macwindow': '<rect x="2" y="3" width="12" height="10" rx="2.2" stroke-width="1.3"/><path d="M2 6.2h12" stroke-width="1.3"/>',
};

// <starry-icon name="trash" size="12">：Chrome 带的 Lit 没有 svg 模板，图标用一个小元素自己画。
class StarryIcon extends HTMLElement {
  static observedAttributes = ['name', 'size'];

  connectedCallback() {
    this.render();
  }

  attributeChangedCallback() {
    this.render();
  }

  render() {
    const size = this.getAttribute('size') || '16';
    const paths = ICONS[this.getAttribute('name')] ?? '';
    this.innerHTML = `<svg width="${size}" height="${size}" viewBox="0 0 16 16" fill="none" ` +
        `stroke="currentColor" stroke-linecap="round" aria-hidden="true">${paths}</svg>`;
  }
}
customElements.define('starry-icon', StarryIcon);

export function icon(name, size = 16) {
  return html`<starry-icon class="icon" name=${name} size=${size}></starry-icon>`;
}

// 搜索框：聚焦时展开宽度，Esc 首次清空内容、再次按失焦。
export function searchField({value, placeholder, onInput}) {
  const input = (event) => onInput(event.target.value);
  const keydown = (event) => {
    if (event.key !== 'Escape') {
      return;
    }
    if (event.target.value) {
      event.target.value = '';
      onInput('');
    } else {
      event.target.blur();
    }
  };
  const clear = (event) => {
    const field = event.currentTarget.parentElement.querySelector('input');
    field.value = '';
    field.focus();
    onInput('');
  };
  return html`
    <label class="search ${value ? 'has-text' : ''}">
      ${icon('magnifyingglass', 12)}
      <input type="search" .value=${value} placeholder=${placeholder} spellcheck="false"
          @input=${input} @keydown=${keydown}>
      <button class="search-clear" title=${L('common.clear')} tabindex="-1" @click=${clear}>
        ${icon('xmark.circle.fill', 13)}
      </button>
    </label>`;
}

// 标题区：「资料库」、页面名、说明行，下面是搜索框和右边的操作。
export function libraryHeader({title, subtitle, search, actions}) {
  return html`
    <header class="library-header">
      <div class="eyebrow">${L('library.header')}</div>
      <h1 class="title">${title}</h1>
      <div class="subtitle">${subtitle || ' '}</div>
      <div class="toolbar">
        ${search}
        <div class="spacer"></div>
        ${actions}
      </div>
    </header>`;
}

// 首次呈现时按序播放标题区入场动效。
export function revealHeader(root) {
  const header = root.querySelector('.library-header');
  if (!header) {
    return;
  }
  reveal(header.querySelector('.eyebrow'), {delay: 0.02});
  reveal(header.querySelector('.title'), {delay: 0.06});
  reveal(header.querySelector('.subtitle'), {delay: 0.1});
  reveal(header.querySelector('.toolbar'), {delay: 0.14, distance: 6});
}

export function emptyState({iconName, title, detail}) {
  return html`
    <div class="empty-state">
      ${icon(iconName, 40)}
      <div class="empty-title">${title}</div>
      ${detail ? html`<div class="empty-detail">${detail}</div>` : ''}
    </div>`;
}

export function pillButton({iconName, title, onClick, disabled = false}) {
  return html`
    <button class="pill-button" ?disabled=${disabled} @click=${onClick}>
      ${iconName ? icon(iconName, 12) : ''}<span>${title}</span>
    </button>`;
}
