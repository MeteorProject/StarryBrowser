// Copyright 2026 The Starry Browser Authors
//
// 设置 › 扩展：支持搜索、筛选、检查更新与行内展开详情（设置、快捷键、权限与维护操作）；
// chrome://extensions 也会重定向到此页。数据与操作见 starry/browser/ui/webui/extensions_handler.h。

import {removeWebUiListener} from 'chrome://resources/js/cr.js';
import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, openUrl, sendWithPromise, showDialog, showMenu} from './common/browser.js';
import {joinList, L} from './common/l10n.js';
import {emptyState, icon, searchField} from './common/library.js';
import {animateRows, captureRows, Motion, reduceMotion, reveal} from './common/motion.js';

const STORE_URL = 'https://chromewebstore.google.com/category/extensions';

const FILTERS = [
  {id: 'all', title: L('common.all')},
  {id: 'enabled', title: L('extensions.filter.enabled')},
  {id: 'disabled', title: L('extensions.filter.disabled')},
];

const SITE_ACCESS = [
  {value: 'onClick', title: L('extensions.siteAccess.onClick')},
  {value: 'specific', title: L('extensions.siteAccess.specific'), disabled: true},
  {value: 'all', title: L('extensions.siteAccess.all')},
];

const IS_MAC = navigator.userAgentData ? navigator.userAgentData.platform === 'macOS' :
                                         navigator.platform.startsWith('Mac');

// ---- 快捷键（同 Chrome 的 cr_shortcut_util：Command、Ctrl、Alt、Shift 加一个键）----

const KEY_NAMES = {
  9: 'Tab', 32: 'Space', 33: 'PageUp', 34: 'PageDown', 35: 'End', 36: 'Home', 37: 'Left',
  38: 'Up', 39: 'Right', 40: 'Down', 45: 'Insert', 46: 'Delete', 176: 'MediaNextTrack',
  177: 'MediaPrevTrack', 178: 'MediaStop', 179: 'MediaPlayPause', 188: 'Comma', 190: 'Period',
};
const MEDIA_KEYS = new Set([176, 177, 178, 179]);

function keyName(code) {
  if ((code >= 65 && code <= 90) || (code >= 48 && code <= 57)) {
    return String.fromCharCode(code);
  }
  return KEY_NAMES[code] || null;
}

// 回 {keys}（Chrome 的写法，例如 Command+Shift+Y）或 {error}；只按了修饰键时回 null。
function keystroke(event) {
  if (['Meta', 'Control', 'Alt', 'Shift'].includes(event.key)) {
    return null;
  }
  const key = keyName(event.keyCode);
  if (!key) {
    return {error: L('extensions.shortcut.invalidKey')};
  }
  const modified = event.ctrlKey || event.altKey || (IS_MAC && event.metaKey);
  if (MEDIA_KEYS.has(event.keyCode)) {
    if (modified || event.shiftKey) {
      return {error: L('extensions.shortcut.mediaModifier')};
    }
  } else if (!modified) {
    return {error: IS_MAC ? L('extensions.shortcut.needsModifier') : L('extensions.shortcut.needsModifierCtrl')};
  }
  const parts = [];
  if (IS_MAC && event.metaKey) {
    parts.push('Command');
  }
  if (event.ctrlKey) {
    parts.push('Ctrl');
  }
  if (event.altKey) {
    parts.push('Alt');
  }
  if (event.shiftKey) {
    parts.push('Shift');
  }
  parts.push(key);
  return {keys: parts.join('+')};
}

// ---- 小部件（和设置页的一样，见 settings.js）----

function switchControl(on, onChange, {disabled = false, label = ''} = {}) {
  return html`
    <button class="switch ${on ? 'on' : ''}" role="switch" aria-checked=${on ? 'true' : 'false'}
        aria-label=${label} ?disabled=${disabled}
        @click=${(event) => {
          event.stopPropagation();
          onChange(!on);
        }}></button>`;
}

function detailButton(title, iconName, onClick, {danger = false, disabled = false} = {}) {
  return html`
    <button class="detail-button ${danger ? 'danger' : ''}" ?disabled=${disabled} @click=${onClick}>
      ${iconName ? icon(iconName, 11) : nothing}<span>${title}</span>
    </button>`;
}

function menuControl(value, options, onChange) {
  const current = options.find((option) => option.value === value);
  const open = async (event) => {
    const id = await showMenu(options.map((option) => ({
      id: String(option.value),
      title: option.title,
      checked: option.value === value,
      enabled: !option.disabled,
    })), {element: event.currentTarget});
    const picked = options.find((option) => String(option.value) === id);
    if (picked && picked.value !== value) {
      onChange(picked.value);
    }
  };
  return html`
    <button class="menu-button" @click=${open}>
      <span>${current ? current.title : '—'}</span>${icon('chevron.up.chevron.down', 9)}
    </button>`;
}

function tag(text, tone = 'neutral') {
  return html`<span class="soft-tag ${tone}">${text}</span>`;
}

class StarryExtensions extends LitElement {
  static properties = {
    compact: {type: Boolean},
    incognito: {type: Boolean},
    list_: {state: true},
    query_: {state: true},
    filter_: {state: true},
    expanded_: {state: true},
    notice_: {state: true},
    checking_: {state: true},
    recording_: {state: true},
    shortcutError_: {state: true},
  };

  constructor() {
    super();
    this.compact = false;
    this.incognito = false;
    this.list_ = null;
    this.query_ = '';
    this.filter_ = 'all';
    // 展开详情的扩展（按 ID 记）。chrome://extensions/?id=… 打开时展开那一个，
    // /shortcuts 打开时展开所有有快捷键的。
    this.expanded_ = new Set();
    this.notice_ = null;
    this.checking_ = false;
    // 正在录快捷键的 {id, command}。
    this.recording_ = null;
    this.shortcutError_ = null;
    this.onRecordKey_ = (event) => this.recordKey_(event);
    this.revealed_ = false;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    // 切到别的分类时这个元素被拿掉，切回来是新的一个。
    this.listeners_ = [
      addWebUiListener('extensions-changed', (list) => this.setList_(list)),
      addWebUiListener('extensions-notice', (notice) => this.showNotice_(notice)),
    ];
    if (this.list_) {
      return;
    }
    const list = await sendWithPromise('getExtensions');
    const params = new URLSearchParams(location.search);
    const id = params.get('id');
    if (id && list.some((extension) => extension.id === id)) {
      this.expanded_ = new Set([id]);
    } else if (params.has('shortcuts')) {
      this.expanded_ = new Set(list.filter((extension) => extension.commands.length)
                                   .map((extension) => extension.id));
    }
    this.list_ = list;
    await this.updateComplete;
    this.reveal_();
    if (id) {
      this.querySelector(`.ext-row[data-key="${CSS.escape(id)}"]`)?.scrollIntoView({block: 'center'});
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.stopRecording_();
    for (const listener of this.listeners_ || []) {
      removeWebUiListener(listener);
    }
    this.listeners_ = [];
  }

  updated() {
    this.placeFilterHighlight_();
  }

  // ---- 动效 ----

  // 首次呈现时按序播放入场动效。
  reveal_() {
    if (this.revealed_) {
      return;
    }
    this.revealed_ = true;
    reveal(this.querySelector('.pane-tile'), {distance: 0, scale: 0.8});
    reveal(this.querySelector('.pane-text'), {delay: 0.03, distance: 6});
    reveal(this.querySelector('.ext-add'), {delay: 0.06, distance: 6});
    reveal(this.querySelector('.ext-toolbar'), {delay: 0.14, distance: 6});
    reveal(this.querySelector('.ext-list, .empty-state'), {delay: 0.16});
    this.querySelectorAll('.ext-row').forEach((row, index) => {
      reveal(row, {delay: 0.18 + Math.min(index, 10) * 0.028, distance: 6});
    });
    reveal(this.querySelector('.store-link'), {delay: 0.22, distance: 6});
  }

  // 列表变了（筛选、搜索、装上卸下）：其余的行滑过去，新的淡入。
  async changeList_(change) {
    const container = this.querySelector('.ext-list');
    const before = container ? captureRows(container) : null;
    change();
    await this.updateComplete;
    const after = this.querySelector('.ext-list');
    if (before && after) {
      animateRows(after, before, Motion.listEdit);
    }
  }

  setList_(list) {
    this.changeList_(() => {
      this.list_ = list;
    });
  }

  showNotice_(notice) {
    this.notice_ = notice;
    this.updateComplete.then(() => {
      const element = this.querySelector('.ext-notice');
      if (element && !reduceMotion()) {
        element.animate([{opacity: 0, transform: 'translateY(-6px)'}, {opacity: 1, transform: 'none'}],
                        {duration: Motion.listEdit.duration, easing: Motion.listEdit.easing});
      }
    });
  }

  placeFilterHighlight_() {
    const picker = this.querySelector('.ext-filters');
    const selected = picker && picker.querySelector('.filter.selected');
    const highlight = picker && picker.querySelector('.filter-highlight');
    if (!selected || !highlight) {
      return;
    }
    const left = selected.offsetLeft;
    const width = selected.offsetWidth;
    if (highlight.dataset.left === String(left) && highlight.dataset.width === String(width)) {
      return;
    }
    const first = highlight.dataset.left === undefined;
    highlight.dataset.left = String(left);
    highlight.dataset.width = String(width);
    highlight.style.transition = first ? 'none' : '';
    highlight.style.transform = `translateX(${left}px)`;
    highlight.style.width = `${width}px`;
    if (first) {
      highlight.getBoundingClientRect();
      highlight.style.transition = '';
    }
  }

  // ---- 操作 ----

  act_(extension, action, value) {
    chrome.send('extensionAction', value === undefined ? [extension.id, action] : [extension.id, action, value]);
  }

  // 先在页面上改（开关马上动），浏览器改完会发 extensions-changed 对上。
  patch_(extension, changes) {
    this.list_ = this.list_.map((other) => other.id === extension.id ? {...other, ...changes} : other);
  }

  async toggleDetails_(extension) {
    const container = this.querySelector('.ext-list');
    const before = captureRows(container);
    const opening = !this.expanded_.has(extension.id);
    this.stopRecording_();
    this.expanded_ = opening ? new Set([extension.id]) : new Set();
    await this.updateComplete;
    animateRows(container, before, Motion.fold);
    const details = this.querySelector(`.ext-row[data-key="${CSS.escape(extension.id)}"] .ext-details`);
    if (opening && details && !reduceMotion()) {
      details.animate([{opacity: 0, transform: 'translateY(-8px)'}, {opacity: 1, transform: 'none'}],
                      {duration: Motion.fold.duration, easing: Motion.fold.easing});
    }
  }

  async remove_(extension) {
    const unpacked = extension.source === 'unpacked';
    if (await confirm({
          icon: 'trash.fill',
          title: L('extensions.remove.title', {name: extension.name}),
          message: unpacked ? L('extensions.remove.message.unpacked') :
                              L('extensions.remove.message.packed'),
          confirmTitle: L('common.remove'),
        })) {
      this.act_(extension, 'uninstall');
    }
  }

  async addMenu_(event) {
    const id = await showMenu([
      {id: 'browse', title: L('extensions.add.browseStore'), icon: 'globe'},
      {id: 'store', title: L('extensions.add.fromStore'), icon: 'link'},
      {separator: true},
      {id: 'crx', title: L('extensions.add.crx'), icon: 'arrow.down'},
      {id: 'unpacked', title: L('extensions.add.unpacked'), icon: 'folder'},
    ], {element: event.currentTarget});
    if (id === 'browse') {
      openUrl(STORE_URL, 'foreground');
    } else if (id === 'store') {
      this.installFromStore_();
    } else if (id === 'crx' || id === 'unpacked') {
      chrome.send('addExtension', [id]);
    }
  }

  async installFromStore_() {
    const result = await showDialog({
      icon: 'link',
      title: L('extensions.storeInstall.title'),
      message: L('extensions.storeInstall.subtitle'),
      field: '',
      buttons: [
        {title: L('common.cancel'), variant: 'tertiary', cancel: true},
        {title: L('extensions.crxPanel.prompt'), variant: 'filled'},
      ],
    });
    if (result && result.button === 1 && result.input.trim()) {
      chrome.send('addExtension', ['store', result.input.trim()]);
    }
  }

  async checkUpdates_() {
    if (this.checking_) {
      return;
    }
    this.checking_ = true;
    this.notice_ = null;
    const result = await sendWithPromise('checkUpdates');
    this.checking_ = false;
    if (result.unavailable) {
      this.showNotice_({text: L('extensions.updates.unavailable'), error: true});
    } else {
      this.showNotice_({
        text: result.updated ? L('extensions.updates.updatedCount', {count: result.updated}) :
                              L('extensions.updates.upToDate'),
        error: false,
      });
    }
  }

  async rowMenu_(event, extension) {
    event.stopPropagation();
    const items = [];
    if (extension.hasOptions) {
      items.push({id: 'options', title: L('extensions.options'), icon: 'gearshape', enabled: extension.enabled});
    }
    if (extension.source === 'unpacked') {
      items.push({id: 'reload', title: L('common.reload'), icon: 'arrow.clockwise'});
    }
    items.push({id: 'copyId', title: L('extensions.copyID'), icon: 'link'});
    items.push({separator: true});
    items.push({id: 'remove', title: L('extensions.removeEllipsis'), icon: 'trash', enabled: extension.mayUninstall});
    const id = await showMenu(items, {element: event.currentTarget});
    if (id === 'remove') {
      this.remove_(extension);
    } else if (id === 'copyId') {
      copyText(extension.id);
    } else if (id) {
      this.act_(extension, id);
    }
  }

  // ---- 录快捷键：点一下开始，Esc 取消，⌫ 清除 ----

  startRecording_(extension, command) {
    this.stopRecording_();
    this.recording_ = {id: extension.id, command: command.name};
    this.shortcutError_ = null;
    chrome.send('suspendShortcuts', [true]);
    document.addEventListener('keydown', this.onRecordKey_, true);
  }

  stopRecording_() {
    if (!this.recording_) {
      return;
    }
    this.recording_ = null;
    chrome.send('suspendShortcuts', [false]);
    document.removeEventListener('keydown', this.onRecordKey_, true);
  }

  async recordKey_(event) {
    const recording = this.recording_;
    if (!recording) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    const plain = !event.metaKey && !event.ctrlKey && !event.altKey && !event.shiftKey;
    if (event.key === 'Escape' && plain) {
      this.shortcutError_ = null;
      this.stopRecording_();
      return;
    }
    let keys = '';
    if (!((event.key === 'Backspace' || event.key === 'Delete') && plain)) {
      const result = keystroke(event);
      if (!result) {
        return;
      }
      if (result.error) {
        this.shortcutError_ = {...recording, text: result.error};
        return;
      }
      keys = result.keys;
    }
    this.stopRecording_();
    const answer = await sendWithPromise('setShortcut', recording.id, recording.command, keys);
    this.shortcutError_ = answer && answer.error ? {...recording, text: answer.error} : null;
  }

  // ---- 显示 ----

  filtered_() {
    const terms = this.query_.toLowerCase().split(/\s+/).filter(Boolean);
    return this.list_.filter((extension) => {
      if (this.filter_ === 'enabled' && !extension.enabled) {
        return false;
      }
      if (this.filter_ === 'disabled' && extension.enabled) {
        return false;
      }
      const haystack = `${extension.name} ${extension.description} ${extension.id}`.toLowerCase();
      return terms.every((term) => haystack.includes(term));
    });
  }

  tags_(extension) {
    const tags = [];
    if (extension.terminated) {
      tags.push(tag(L('extensions.tag.stopped'), 'danger'));
    } else if (extension.problem) {
      tags.push(tag(L('extensions.tag.attention'), 'warning'));
    }
    if (extension.source === 'unpacked') {
      tags.push(tag(L('extensions.tag.developer')));
    } else if (extension.source === 'policy') {
      tags.push(tag(L('extensions.tag.policy')));
    }
    return tags;
  }

  renderHeader_() {
    const list = this.list_;
    const enabled = list.filter((extension) => extension.enabled).length;
    const summary = list.length ? L('extensions.settings.summary', {installed: list.length, enabled}) :
                                  L('settings.pane.extensions.summary');
    return html`
      <div class="ext-header">
        <div class="pane-header">
          <div class="pane-tile">${icon('puzzlepiece', 19)}</div>
          <div class="pane-text">
            <h1 class="pane-title">${L('common.extensions')}</h1>
            <div class="pane-summary">${summary}</div>
          </div>
        </div>
        <span class="spacer"></span>
        <button class="filled-pill ext-add" ?disabled=${this.incognito} @click=${(event) => this.addMenu_(event)}>
          ${icon('plus', 12)}<span>${L('extensions.add.title')}</span>
        </button>
      </div>`;
  }

  renderFilters_() {
    return html`
      <div class="filter-picker ext-filters">
        <div class="filter-highlight"></div>
        ${FILTERS.map((filter) => html`
          <button class="filter ${filter.id === this.filter_ ? 'selected' : ''}"
              @click=${() => filter.id !== this.filter_ && this.changeList_(() => {
                this.filter_ = filter.id;
              })}>${filter.title}</button>`)}
      </div>`;
  }

  renderToolbar_() {
    const updatable = this.list_.some((extension) => extension.source === 'store');
    const search = searchField({
      value: this.query_,
      placeholder: L('extensions.settings.search'),
      onInput: (value) => this.changeList_(() => {
        this.query_ = value.trim() ? value : '';
      }),
    });
    const update = html`
      <button class="round-button ext-update ${this.checking_ ? 'checking' : ''}"
          title=${this.checking_ ? L('extensions.updates.checking') : L('extensions.updates.check')} ?disabled=${this.checking_ || !updatable}
          @click=${() => this.checkUpdates_()}>
        ${icon('arrow.triangle.2.circlepath', 14)}
      </button>`;
    return html`
      <div class="ext-toolbar ${this.compact ? 'stacked' : ''}">
        ${this.compact ? html`
          <div class="ext-toolbar-line">${this.renderFilters_()}<span class="spacer"></span>${update}</div>
          ${search}` : html`
          <div class="search-slot">${search}</div><span class="spacer"></span>${this.renderFilters_()}${update}`}
      </div>`;
  }

  renderNotice_() {
    const notice = this.notice_;
    if (!notice) {
      return nothing;
    }
    return html`
      <div class="ext-notice ${notice.error ? 'error' : ''}">
        ${icon(notice.error ? 'exclamationmark.circle' : 'checkmark.circle', 13)}
        <span class="ext-notice-text">${notice.text}</span>
        <button class="icon-button" title=${L('extensions.settings.dismissNotice')} @click=${() => {
          this.notice_ = null;
        }}>${icon('xmark', 9)}</button>
      </div>`;
  }

  renderRow_(extension, index, items) {
    const expanded = this.expanded_.has(extension.id);
    const neighbourExpanded = index > 0 && this.expanded_.has(items[index - 1].id);
    const divider = index === 0 ? 'none' : (expanded || neighbourExpanded ? 'full' : 'inset');
    const pinTitle = extension.pinned ? L('extensions.menu.unpin.sidebar') : L('extensions.pin.sidebar');
    return html`
      <div class="ext-row ${expanded ? 'expanded' : ''} ${extension.enabled ? '' : 'disabled'} divider-${divider}"
          data-key=${extension.id}>
        <div class="ext-summary" title=${expanded ? L('extensions.row.hideDetails') : L('extensions.row.showDetails')}
            @click=${() => this.toggleDetails_(extension)}>
          <div class="ext-icon"><img src=${extension.icon} alt="" draggable="false"></div>
          <div class="ext-text">
            <div class="ext-name-line">
              <span class="ext-name">${extension.name}</span>
              <span class="ext-version">${extension.version}</span>
              ${this.tags_(extension)}
            </div>
            <div class="ext-description">${extension.description || L('extensions.row.noDescription')}</div>
          </div>
          <div class="ext-actions">
            ${extension.pinnable ? html`
              <button class="icon-button ext-pin ${extension.pinned ? 'pinned' : ''}" title=${pinTitle}
                  ?disabled=${extension.forcePinned}
                  @click=${(event) => {
                    event.stopPropagation();
                    this.patch_(extension, {pinned: !extension.pinned});
                    this.act_(extension, 'pin', !extension.pinned);
                  }}>${icon(extension.pinned ? 'pin.fill' : 'pin', 12)}</button>` : nothing}
            <button class="icon-button ext-more" title=${L('common.moreActions')} @click=${(event) => this.rowMenu_(event, extension)}>
              ${icon('ellipsis', 13)}
            </button>
          </div>
          ${switchControl(extension.enabled, (on) => {
            this.patch_(extension, {enabled: on});
            this.act_(extension, on ? 'enable' : 'disable');
          }, {disabled: !extension.mayToggle, label: L('extensions.row.enableToggle', {name: extension.name})})}
          <span class="ext-chevron">${icon('chevron.right', 9)}</span>
        </div>
        ${expanded ? this.renderDetails_(extension) : nothing}
      </div>`;
  }

  renderSection_(title, rows) {
    const shown = rows.filter(Boolean);
    if (!shown.length) {
      return nothing;
    }
    return html`
      <div class="ext-section">
        <div class="ext-section-title">${title}</div>
        <div class="ext-section-card">${shown}</div>
      </div>`;
  }

  settingRow_(title, control, detail = '') {
    return html`
      <div class="ext-setting">
        <div class="ext-setting-labels">
          <div class="ext-setting-title">${title}</div>
          ${detail ? html`<div class="ext-setting-detail">${detail}</div>` : nothing}
        </div>
        ${control}
      </div>`;
  }

  renderShortcut_(extension, command) {
    const recording = this.recording_ && this.recording_.id === extension.id &&
        this.recording_.command === command.name;
    const error = this.shortcutError_ && this.shortcutError_.id === extension.id &&
        this.shortcutError_.command === command.name ? this.shortcutError_.text : '';
    return html`
      <div class="ext-setting">
        <div class="ext-setting-labels">
          <div class="ext-setting-title">${command.description}</div>
          ${error ? html`<div class="ext-setting-detail danger">${error}</div>` :
                    recording ? html`<div class="ext-setting-detail">${L('extensions.shortcut.recordingHint')}</div>` : nothing}
        </div>
        <button class="shortcut-button ${recording ? 'recording' : ''} ${command.shortcut ? '' : 'empty'}"
            ?disabled=${!extension.enabled}
            @click=${() => recording ? this.stopRecording_() : this.startRecording_(extension, command)}>
          ${recording ? L('extensions.shortcut.recording') : command.shortcut || L('extensions.shortcut.none')}
        </button>
      </div>`;
  }

  renderDetails_(extension) {
    const settings = [
      extension.pinnable ? this.settingRow_(L('extensions.pin.sidebar'), switchControl(extension.pinned, (on) => {
        this.patch_(extension, {pinned: on});
        this.act_(extension, 'pin', on);
      }, {disabled: extension.forcePinned, label: L('extensions.pin.sidebar')})) : null,
      extension.incognito.available ? this.settingRow_(L('extensions.details.allowInPrivate'), switchControl(
          extension.incognito.allowed, (on) => {
            this.patch_(extension, {incognito: {...extension.incognito, allowed: on}});
            this.act_(extension, 'incognito', on);
          }, {label: L('extensions.details.allowInPrivate')}), L('extensions.details.allowInPrivate.detail')) : null,
      extension.fileAccess.available ? this.settingRow_(L('extensions.details.allowFileAccess'), switchControl(
          extension.fileAccess.allowed, (on) => {
            this.patch_(extension, {fileAccess: {...extension.fileAccess, allowed: on}});
            this.act_(extension, 'fileAccess', on);
          }, {label: L('extensions.details.allowFileAccess')}), L('extensions.details.allowFileAccess.detail')) : null,
      extension.userScripts.available ? this.settingRow_(L('extensions.details.allowUserScripts'), switchControl(
          extension.userScripts.allowed, (on) => {
            this.patch_(extension, {userScripts: {...extension.userScripts, allowed: on}});
            this.act_(extension, 'userScripts', on);
          }, {label: L('extensions.details.allowUserScripts')}), L('extensions.details.allowUserScripts.detail')) : null,
      extension.siteAccess ? this.settingRow_(L('extensions.details.siteAccess'), menuControl(extension.siteAccess, SITE_ACCESS,
          (value) => {
            this.patch_(extension, {siteAccess: value});
            this.act_(extension, 'siteAccess', value);
          }), L('extensions.details.siteAccess.detail')) : null,
    ];
    const permissions = extension.permissions.length ?
        extension.permissions.map((permission) => html`
          <div class="ext-permission">
            ${icon('checkmark.shield', 12)}
            <div>
              <div>${permission.message}</div>
              ${permission.details.length ?
                  html`<div class="ext-permission-details">${joinList(permission.details)}</div>` : nothing}
            </div>
          </div>`) :
        [html`<div class="ext-permission none">${L('extensions.details.noPermissions')}</div>`];
    const problemAction = {
      restart: [L('extensions.details.restart'), 'arrow.clockwise', 'restart'],
      approve: [L('extensions.problem.review'), 'checkmark', 'approve'],
      developerMode: [L('extensions.problem.allowDeveloperMode'), 'checkmark', 'developerMode'],
    }[extension.problemAction];
    return html`
      <div class="ext-details">
        ${extension.problem ? html`
          <div class="ext-banner danger">
            ${icon('exclamationmark.triangle.fill', 12)}
            <span>${extension.problem}</span>
            ${problemAction ? detailButton(problemAction[0], problemAction[1],
                                           () => this.act_(extension, problemAction[2])) : nothing}
          </div>` : nothing}
        ${this.renderSection_(L('common.settings'), settings)}
        ${this.renderSection_(L('extensions.details.shortcuts'), extension.commands.map((command) => this.renderShortcut_(extension, command)))}
        ${this.renderSection_(L('extensions.details.permissions'), permissions)}
        <div class="ext-footer">
          <div class="ext-identity">
            <span class="ext-id" title=${L('extensions.details.id')}>${extension.id}</span>
            <button class="icon-button" title=${L('extensions.copyID')} @click=${() => copyText(extension.id)}>
              ${icon('doc.on.doc', 11)}
            </button>
            ${extension.path ? html`<span class="ext-path" title=${extension.path}>${extension.path}</span>` : nothing}
          </div>
          <div class="ext-footer-actions">
            ${extension.hasOptions ?
                detailButton(L('extensions.menu.options'), 'slider.horizontal.3', () => this.act_(extension, 'options'),
                             {disabled: !extension.enabled}) : nothing}
            ${extension.background ?
                detailButton(extension.background === 'worker' ? L('extensions.details.inspectServiceWorker') : L('extensions.details.inspectBackground'), 'ladybug',
                             () => this.act_(extension, 'inspect'), {disabled: !extension.enabled}) : nothing}
            ${extension.source === 'unpacked' ?
                detailButton(L('common.reload'), 'arrow.clockwise', () => this.act_(extension, 'reload')) : nothing}
            ${extension.storeUrl ?
                detailButton(L('extensions.details.storePage'), 'arrow.up.right.square', () => openUrl(extension.storeUrl, 'foreground')) :
                nothing}
            ${detailButton(L('common.remove'), 'trash', () => this.remove_(extension),
                           {danger: true, disabled: !extension.mayUninstall})}
          </div>
        </div>
      </div>`;
  }

  renderList_() {
    const items = this.filtered_();
    if (!items.length) {
      const none = !this.list_.length;
      return emptyState({
        iconName: none ? 'puzzlepiece' : 'magnifyingglass',
        title: none ? L('extensions.settings.empty') : L('extensions.settings.noMatches'),
        detail: none ? L('extensions.settings.empty.detail') : '',
      });
    }
    return html`
      <div class="ext-list">
        ${repeat(items, (extension) => extension.id, (extension, index) => this.renderRow_(extension, index, items))}
      </div>`;
  }

  render() {
    if (!this.list_) {
      return nothing;
    }
    return html`
      ${this.renderHeader_()}
      ${this.renderToolbar_()}
      ${this.renderNotice_()}
      ${this.renderList_()}
      <button class="store-link" @click=${() => openUrl(STORE_URL, 'foreground')}>
        ${icon('storefront', 12)}<span>${L('extensions.webStore')}</span><span class="store-arrow">${icon('arrow.up.forward', 9)}</span>
      </button>`;
  }
}

customElements.define('starry-extensions', StarryExtensions);
