// Copyright 2026 The Starry Browser Authors
//
// 设置 › 外观：单栏层次引导 + 智能条件折叠。按「主题与色彩 → 窗口布局与工具栏 →
// 标签交互与预览 → 快捷按钮」分四段排列；各项修改都会立刻反映在当前浏览器窗口上，
// 不适用或已关闭的依赖子项用 Motion.fold 平滑收起。
//
// 每张卡片里的行对齐成两列：左边 28 宽的图标（开着时垫强调色），右边是文字和控件；子设置
// 缩进到文字那一列，左边一条竖线从父设置的图标下面垂下来。

import {html, nothing} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {L} from './common/l10n.js';
import {icon} from './common/library.js';
import {reveal} from './common/motion.js';

// 工具按钮的图标（同 starry/browser/ui/sidebar/utility_buttons.cc）。
export const BUTTON_ICONS = {
  downloads: 'arrow.down',
  history: 'clock',
  bookmarks: 'star',
  extensions: 'puzzlepiece',
  assistant: 'sparkles',
  layout: 'sidebar.left',
  settings: 'gearshape',
};

function switchButton(on, onChange, {label = '', disabled = false} = {}) {
  return html`
    <button class="switch ${on ? 'on' : ''}" role="switch" aria-checked=${on ? 'true' : 'false'}
        aria-label=${label} ?disabled=${disabled} @click=${() => onChange(!on)}></button>`;
}

// 内联分段控件：选中底块用弹簧滑过去（位置在 placeAppearanceHighlights 里放）。
function segmentedControl(name, value, options, onChange, {label = ''} = {}) {
  return html`
    <div class="ap-seg" role="radiogroup" aria-label=${label} data-seg=${name}>
      <div class="ap-seg-thumb"></div>
      ${options.map((option) => html`
        <button class="ap-seg-btn ${option.value === value ? 'selected' : ''}" role="radio"
            aria-checked=${option.value === value ? 'true' : 'false'}
            @click=${() => option.value !== value && onChange(option.value)}>
          ${option.title}
        </button>`)}
    </div>`;
}

// 紧致横排视觉单选：左边小示意图、右边标题，选中框在选项间平滑滑移。
function compactPicker(name, value, options, onChange, {label = ''} = {}) {
  return html`
    <div class="ap-picker" role="radiogroup" aria-label=${label} data-ap-picker=${name}>
      <div class="ap-picker-ring"></div>
      ${options.map((option) => html`
        <button class="ap-pick ${option.value === value ? 'selected' : ''}" role="radio"
            aria-checked=${option.value === value ? 'true' : 'false'}
            @click=${() => option.value !== value && onChange(option.value)}>
          <div class="ap-thumb">${option.preview}</div>
          <span class="ap-pick-title">${option.title}</span>
        </button>`)}
    </div>`;
}

function foldBlock(open, content) {
  return html`
    <div class="ap-fold ${open ? '' : 'closed'}" ?inert=${!open} aria-hidden=${open ? 'false' : 'true'}>
      <div class="ap-fold-inner">${content}</div>
    </div>`;
}

// 选择器下面的一行小字（浮动侧栏怎么呼出、顶部标签收起了哪些设置）。
function caption(open, text) {
  return foldBlock(open, html`<div class="ap-caption">${icon('info.circle', 12)}<span>${text}</span></div>`);
}

function stepHeader(title, detail) {
  return html`
    <div class="ap-step-head">
      <span class="ap-step-title">${title}</span>
      ${detail ? html`<span class="ap-step-detail">${detail}</span>` : nothing}
    </div>`;
}

function rowWithLead({key, iconName, accent = false, title, detail = '', control}) {
  return html`
    <div class="ap-row" data-key=${key}>
      <span class="ap-lead ${accent ? 'accent' : ''}">${icon(iconName, 14)}</span>
      <div class="entry-labels">
        <div class="entry-title">${title}</div>
        ${detail ? html`<div class="entry-detail">${detail}</div>` : nothing}
      </div>
      <div class="entry-control">${control}</div>
    </div>`;
}

function subRow({key, title, detail = '', control}) {
  return html`
    <div class="ap-row ap-sub" data-key=${key}>
      <div class="entry-labels">
        <div class="entry-title">${title}</div>
        ${detail ? html`<div class="entry-detail">${detail}</div>` : nothing}
      </div>
      <div class="entry-control">${control}</div>
    </div>`;
}

// 渲染外观页主体：四段分层引导卡片。
export function renderAppearance(s, {mockWindow, set, toggleButton}) {
  const dark = s.colorScheme === 'dark' ||
               (s.colorScheme === 'system' && matchMedia('(prefers-color-scheme: dark)').matches);
  const sidebar = s.sidebarLayout !== 'top';
  const addressInToolbar = !sidebar || s.addressBarOnTop;

  const schemes = [
    {value: 'system', title: L('appearance.system'), preview: html`${mockWindow()}${mockWindow({dark: true, half: true})}`},
    {value: 'light', title: L('appearance.light'), preview: mockWindow()},
    {value: 'dark', title: L('appearance.dark'), preview: mockWindow({dark: true})},
  ];
  const layoutMock = {dark, addressOnTop: s.addressBarOnTop, compact: s.compactAddressBar};
  const layouts = [
    {value: 'docked', title: L('layout.dockedSidebar'), preview: mockWindow(layoutMock)},
    {value: 'floating', title: L('layout.floatingSidebar'), preview: mockWindow({...layoutMock, floating: true})},
    {value: 'top', title: L('layout.top'), preview: mockWindow({...layoutMock, top: true})},
  ];

  // 侧栏宽度：标题行右边是「恢复默认」和数字（数字贴右边，和下面的开关对齐；
  // 「恢复默认」不在默认宽度时才出现），滑块单独一行，和文字左对齐。
  const width = s.sidebarWidth;
  const atDefault = width === s.sidebarWidthDefault;
  const fraction = (value) => (value - s.sidebarWidthMin) / (s.sidebarWidthMax - s.sidebarWidthMin);
  const resetWidth = () => set('sidebarWidth', s.sidebarWidthDefault);

  const closeParentDetails = [
    L('settings.appearance.treeTabsCloseParent.smartDetail'),
    L('settings.appearance.treeTabsCloseParent.outdentDetail'),
    L('settings.appearance.treeTabsCloseParent.closeAllDetail'),
  ];

  return html`
    <div class="ap-page">
      <section class="ap-step" data-step="theme">
        ${stepHeader(L('settings.appearance.step.theme.title'), L('settings.appearance.step.theme.detail'))}
        <div class="group-card ap-card">
          <div class="ap-picker-wrap" data-key="appearance">
            ${compactPicker('scheme', s.colorScheme, schemes, (value) => set('colorScheme', value),
                            {label: L('settings.appearance.appearance.title')})}
          </div>
          ${rowWithLead({
            key: 'tintFromWebsite',
            iconName: 'paintpalette',
            accent: s.tintFromWebsite,
            title: L('settings.appearance.tintFromWebsite.title'),
            detail: L('settings.appearance.tintFromWebsite.detail'),
            control: switchButton(s.tintFromWebsite, (value) => set('tintFromWebsite', value),
                                  {label: L('settings.appearance.tintFromWebsite.title')}),
          })}
        </div>
      </section>

      <section class="ap-step" data-step="layout">
        ${stepHeader(L('settings.appearance.step.layout.title'), L('settings.appearance.step.layout.detail'))}
        <div class="group-card ap-card">
          <div class="ap-picker-wrap" data-key="tabLayout">
            ${compactPicker('layout', s.sidebarLayout, layouts, (value) => set('sidebarLayout', value),
                            {label: L('settings.appearance.tabLayout.title')})}
            ${caption(s.sidebarLayout === 'floating', L('settings.appearance.floatingNote'))}
            ${caption(!sidebar, L('settings.appearance.sidebarHiddenNote'))}
          </div>

          ${foldBlock(sidebar, html`
            <div class="ap-row ap-width-row" data-key="sidebarWidth">
              <div class="ap-width-head">
                <span class="ap-lead">${icon('slider.horizontal.3', 14)}</span>
                <div class="entry-labels">
                  <div class="entry-title">${L('settings.appearance.sidebarWidth.title')}</div>
                  <div class="entry-detail">${L('settings.appearance.sidebarWidth.detail')}</div>
                </div>
                <div class="ap-width-readout">
                  <button class="ap-reset-icon ${atDefault ? 'hidden' : ''}" ?disabled=${atDefault}
                      title=${L('settings.appearance.sidebarWidth.reset')}
                      aria-label=${L('settings.appearance.sidebarWidth.reset')} @click=${resetWidth}>
                    ${icon('arrow.clockwise', 12)}
                  </button>
                  <span class="width-value">${L('settings.appearance.sidebarWidth.value', {width})}</span>
                </div>
              </div>
              <div class="width-slider ap-width-slider"
                  style="--fill: ${fraction(width)}; --default: ${fraction(s.sidebarWidthDefault)}">
                <input type="range" min=${s.sidebarWidthMin} max=${s.sidebarWidthMax} step="1"
                    .value=${String(width)} aria-label=${L('settings.appearance.sidebarWidth.title')}
                    @input=${(event) => set('sidebarWidth', Number(event.target.value))} @dblclick=${resetWidth}>
                <span class="width-default"></span>
              </div>
            </div>
            ${rowWithLead({
              key: 'addressBarOnTop',
              iconName: 'macwindow',
              accent: s.addressBarOnTop,
              title: L('settings.appearance.addressBarOnTop.title'),
              detail: s.addressBarOnTop ? L('settings.appearance.inToolbar') : L('settings.appearance.inSidebar'),
              control: switchButton(s.addressBarOnTop, (value) => set('addressBarOnTop', value),
                                    {label: L('settings.appearance.addressBarOnTop.title')}),
            })}
            ${rowWithLead({
              key: 'extensionsOnTop',
              iconName: 'puzzlepiece',
              accent: s.extensionsOnTop,
              title: L('settings.appearance.extensionsOnTop.title'),
              detail: s.extensionsOnTop ? L('settings.appearance.inToolbar') : L('settings.appearance.inSidebar'),
              control: switchButton(s.extensionsOnTop, (value) => set('extensionsOnTop', value),
                                    {label: L('settings.appearance.extensionsOnTop.title')}),
            })}
          `)}

          ${foldBlock(addressInToolbar, html`
            ${rowWithLead({
              key: 'compactAddressBar',
              iconName: 'macwindow',
              accent: addressInToolbar && s.compactAddressBar,
              title: L('settings.appearance.compactAddressBar.title'),
              detail: s.tintFromWebsite ? L('settings.appearance.compactAddressBar.tinted') :
                                          L('settings.appearance.compactAddressBar.merged'),
              control: switchButton(s.compactAddressBar, (value) => set('compactAddressBar', value),
                                    {label: L('settings.appearance.compactAddressBar.title')}),
            })}
          `)}

          ${s.isWindows ? html`
            ${rowWithLead({
              key: 'windowsControlsStyle',
              iconName: 'macwindow',
              title: L('settings.appearance.windowControls.title'),
              detail: s.windowsControlsStyle === 'rail'
                  ? L('settings.appearance.windowControls.railDetail')
                  : L('settings.appearance.windowControls.glassDetail'),
              control: segmentedControl('windowsControlsStyle', s.windowsControlsStyle, [
                {value: 'glass', title: L('settings.appearance.windowControls.glass')},
                {value: 'rail', title: L('settings.appearance.windowControls.rail')},
              ], (value) => set('windowsControlsStyle', value),
              {label: L('settings.appearance.windowControls.title')}),
            })}
            ${foldBlock(s.windowsControlsStyle === 'glass', subRow({
              key: 'windowsControlsAutoHide',
              title: L('settings.appearance.windowControlsAutoHide.title'),
              detail: L('settings.appearance.windowControlsAutoHide.detail'),
              control: switchButton(s.windowsControlsAutoHide, (value) => set('windowsControlsAutoHide', value),
                                    {label: L('settings.appearance.windowControlsAutoHide.title')}),
            }))}
          ` : nothing}
        </div>
      </section>

      <section class="ap-step" data-step="tabs">
        ${stepHeader(L('settings.appearance.step.tabs.title'), L('settings.appearance.step.tabs.detail'))}
        <div class="group-card ap-card">
          ${foldBlock(sidebar, html`
            ${rowWithLead({
              key: 'treeTabs',
              iconName: 'list.bullet.indent',
              accent: s.treeTabs,
              title: L('settings.appearance.treeTabs.title'),
              detail: L('settings.appearance.treeTabs.detail'),
              control: switchButton(s.treeTabs, (value) => set('treeTabs', value),
                                    {label: L('settings.appearance.treeTabs.title')}),
            })}
            ${foldBlock(sidebar && s.treeTabs, html`
              ${subRow({
                key: 'treeTabsMaxDepth',
                title: L('settings.appearance.treeTabsMaxDepth.title'),
                detail: L('settings.appearance.treeTabsMaxDepth.detail'),
                control: segmentedControl('treeTabsMaxDepth', s.treeTabsMaxDepth, [3, 4, 6, 0].map((value) => ({
                  value,
                  title: value ? L('settings.appearance.treeTabsMaxDepth.levels', {count: value}) :
                                 L('settings.appearance.treeTabsMaxDepth.unlimited'),
                })), (value) => set('treeTabsMaxDepth', value),
                {label: L('settings.appearance.treeTabsMaxDepth.title')}),
              })}
              ${subRow({
                key: 'treeTabsCloseParent',
                title: L('settings.appearance.treeTabsCloseParent.title'),
                detail: closeParentDetails[s.treeTabsCloseParent] || closeParentDetails[0],
                control: segmentedControl('treeTabsCloseParent', s.treeTabsCloseParent, [
                  {value: 0, title: L('settings.appearance.treeTabsCloseParent.smart')},
                  {value: 1, title: L('settings.appearance.treeTabsCloseParent.outdentShort')},
                  {value: 2, title: L('settings.appearance.treeTabsCloseParent.closeAllShort')},
                ], (value) => set('treeTabsCloseParent', value),
                {label: L('settings.appearance.treeTabsCloseParent.title')}),
              })}
              ${subRow({
                key: 'treeTabsAutoCollapse',
                title: L('settings.appearance.treeTabsAutoCollapse.title'),
                detail: L('settings.appearance.treeTabsAutoCollapse.detail'),
                control: switchButton(s.treeTabsAutoCollapse, (value) => set('treeTabsAutoCollapse', value),
                                      {label: L('settings.appearance.treeTabsAutoCollapse.title')}),
              })}
            `)}
          `)}

          ${rowWithLead({
            key: 'tabPreviewsEnabled',
            iconName: 'eye',
            accent: s.tabPreviewsEnabled,
            title: L('settings.appearance.tabPreviewsEnabled.title'),
            detail: L('settings.appearance.tabPreviewsEnabled.detail'),
            control: switchButton(s.tabPreviewsEnabled, (value) => set('tabPreviewsEnabled', value),
                                  {label: L('settings.appearance.tabPreviewsEnabled.title')}),
          })}

          ${foldBlock(s.tabPreviewsEnabled, subRow({
            key: 'tabPreviewShowsUsage',
            title: L('settings.appearance.tabPreviewShowsUsage.title'),
            detail: L('settings.appearance.tabPreviewShowsUsage.detail'),
            control: switchButton(s.tabPreviewShowsUsage, (value) => set('tabPreviewShowsUsage', value),
                                  {label: L('settings.appearance.tabPreviewShowsUsage.title')}),
          }))}

          ${rowWithLead({
            key: 'pinnedPeek',
            iconName: 'pin',
            accent: s.pinnedPeek,
            title: L('settings.appearance.pinnedPeek'),
            control: switchButton(s.pinnedPeek, (value) => set('pinnedPeek', value),
                                  {label: L('settings.appearance.pinnedPeek')}),
          })}
        </div>
      </section>

      <section class="ap-step" data-step="buttons">
        ${stepHeader(sidebar ? L('settings.appearance.utilityButtons.sidebarTitle') :
                               L('settings.appearance.utilityButtons.toolbarTitle'),
                     L('settings.appearance.step.buttons.detail'))}
        <div class="group-card ap-card">
          <div class="ap-btn-wrap" data-key="utilityButtons">
            <div class="button-picker" style="--count: ${s.utilityButtons.length}">
              ${s.utilityButtons.map((button) => html`
                <button class="button-chip ${button.hidden ? 'hidden' : ''}"
                    aria-pressed=${button.hidden ? 'false' : 'true'}
                    title=${button.hidden ? L('settings.appearance.utilityButtons.show', {name: button.title}) :
                                            L('settings.appearance.utilityButtons.hide', {name: button.title})}
                    @click=${() => toggleButton(button)}>
                  <span class="chip-icon">
                    ${icon(BUTTON_ICONS[button.id] || 'circle', 15)}
                    <span class="chip-badge">${icon('checkmark', 8)}</span>
                  </span>
                  <span class="chip-title">${button.title}</span>
                </button>`)}
            </div>
            <div class="ap-btn-hint">${L('settings.appearance.utilityButtons.detail')}</div>
          </div>
        </div>
      </section>
    </div>`;
}

// 放好横排视觉单选的选中框，和分段控件的滑块（用相对父容器的 offsetLeft/offsetWidth，
// 折叠中的分组也能算出准确位置）。
function placeSlidingIndicator(container, indicatorSelector, activeSelector) {
  const indicator = container.querySelector(indicatorSelector);
  const active = container.querySelector(activeSelector);
  if (!indicator) {
    return;
  }
  indicator.style.opacity = active ? '1' : '0';
  if (!active || active.offsetWidth === 0) {
    return;
  }
  const x = active.offsetLeft;
  const y = active.offsetTop;
  const w = active.offsetWidth;
  const h = active.offsetHeight;
  const key = `${x},${y},${w},${h}`;
  if (indicator.dataset.at === key) {
    return;
  }
  const first = indicator.dataset.at === undefined;
  indicator.dataset.at = key;
  indicator.style.width = `${w}px`;
  indicator.style.height = `${h}px`;
  indicator.style.transition = first ? 'none' : '';
  indicator.style.transform = `translate(${x}px, ${y}px)`;
  if (first) {
    indicator.getBoundingClientRect();
    indicator.style.transition = '';
  }
}

export function placeAppearanceHighlights(root) {
  for (const picker of root.querySelectorAll('.ap-picker')) {
    placeSlidingIndicator(picker, '.ap-picker-ring', '.ap-pick.selected');
  }
  for (const seg of root.querySelectorAll('.ap-seg')) {
    placeSlidingIndicator(seg, '.ap-seg-thumb', '.ap-seg-btn.selected');
  }
}

// 切到外观页时：四段卡片逐个弹簧升起。
export function revealAppearance(root) {
  root.querySelectorAll('.ap-step').forEach((step, index) => {
    reveal(step, {delay: 0.03 + index * 0.04, distance: 10});
  });
}
