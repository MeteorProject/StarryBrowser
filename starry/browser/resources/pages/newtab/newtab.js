// Copyright 2026 The Starry Browser Authors
//
// 新标签页（starry://newtab，卡片风格；全屏浮层风格下的空白标签也是它）：问候、搜索框、
// 小组件（快捷网站、正在播放、最近关闭、已归档、本周拦截、便签）。
//
// 搜索框是浏览器的命令栏：点它、在页面上打字时请浏览器把命令栏停上来（newTabSearch），
// 搜索框的位置报给浏览器（newTabSearchRect），命令栏停着时（newtab-docked）把自己的搜索框
// 藏起来。数据和设置在 NewTabHandler（starry/browser/ui/webui/new_tab_handler.h）。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, faviconUrl, openUrl, send, sendWithPromise, showDialog, showMenu, targetFromEvent} from './common/browser.js';
import {L, locale, platform} from './common/l10n.js';
import {icon} from './common/library.js';
import {Motion, reduceMotion} from './common/motion.js';

const WIDGETS = ['clock', 'shortcuts', 'continue', 'playing', 'archive', 'shield', 'note'];
const BACKGROUNDS = ['theme', 'sky', 'image', 'solid'];

// library.js 中未收录的 16×16 图标。
const GLYPHS = {
  grid: '<rect x="2.4" y="2.4" width="4.6" height="4.6" rx="1.3"/><rect x="9" y="2.4" width="4.6" height="4.6" rx="1.3"/><rect x="2.4" y="9" width="4.6" height="4.6" rx="1.3"/><rect x="9" y="9" width="4.6" height="4.6" rx="1.3"/>',
  sliders: '<path d="M2.5 4.6h6M12.2 4.6h1.3M2.5 11.4h1.3M7.5 11.4h6"/><circle cx="10.4" cy="4.6" r="1.6"/><circle cx="5.6" cy="11.4" r="1.6"/>',
  backward: '<path d="M3.2 3.4v9.2" stroke-width="1.6"/><path d="M13 3.8v8.4a.6.6 0 0 1-.94.5L5.9 8.5a.6.6 0 0 1 0-1l6.16-4.2a.6.6 0 0 1 .94.5Z" fill="currentColor" stroke="none"/>',
  forward: '<path d="M12.8 3.4v9.2" stroke-width="1.6"/><path d="M3 3.8v8.4a.6.6 0 0 0 .94.5l6.16-4.2a.6.6 0 0 0 0-1L3.94 3.3a.6.6 0 0 0-.94.5Z" fill="currentColor" stroke="none"/>',
  note: '<rect x="3" y="2.2" width="10" height="11.6" rx="2" /><path d="M5.6 5.8h4.8M5.6 8.2h4.8M5.6 10.6h2.8"/>',
};

function glyph(name, size = 16) {
  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('viewBox', '0 0 16 16');
  svg.setAttribute('width', size);
  svg.setAttribute('height', size);
  svg.setAttribute('fill', 'none');
  svg.setAttribute('stroke', 'currentColor');
  svg.setAttribute('stroke-width', '1.4');
  svg.setAttribute('stroke-linecap', 'round');
  svg.setAttribute('stroke-linejoin', 'round');
  svg.innerHTML = GLYPHS[name];
  return html`<span class="icon">${svg}</span>`;
}

function shortcutKey(key) {
  return platform === 'mac' ? `⌘${key}` : `Ctrl+${key}`;
}

function hostOf(url) {
  try {
    return new URL(url).hostname.replace(/^www\./, '');
  } catch (e) {
    return url;
  }
}

function timeAgo(ms) {
  const delta = Date.now() - ms;
  if (!ms || delta < 60e3) {
    return L('newTab.ago.now');
  }
  if (delta < 3600e3) {
    return L('newTab.ago.minutes', {count: Math.floor(delta / 60e3)});
  }
  if (delta < 86400e3) {
    return L('newTab.ago.hours', {count: Math.floor(delta / 3600e3)});
  }
  return L('newTab.ago.days', {count: Math.floor(delta / 86400e3)});
}

function greeting(hour) {
  if (hour >= 5 && hour < 11) {
    return L('newTab.greeting.morning');
  }
  if (hour >= 11 && hour < 14) {
    return L('newTab.greeting.noon');
  }
  if (hour >= 14 && hour < 18) {
    return L('newTab.greeting.afternoon');
  }
  return L('newTab.greeting.evening');
}

// 星空：画一次（窗口大小变了再画），不做一直在跑的动画。
function drawStars(canvas, light) {
  const ratio = devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  const context = canvas.getContext('2d');
  context.scale(ratio, ratio);
  const hash = (n) => {
    const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
    return x - Math.floor(x);
  };
  const count = Math.round(Math.min(220, (width * height) / 5200));
  for (let i = 0; i < count; i++) {
    const x = hash(i * 3.17) * width;
    const y = Math.pow(hash(i * 7.73), 1.5) * height * 0.92;
    const size = 0.5 + Math.pow(hash(i * 1.31), 3) * 1.2;
    const alpha = 0.25 + hash(i * 9.2) * 0.65;
    context.fillStyle = light ? `rgba(122, 108, 196, ${alpha * 0.35})` : `rgba(255, 255, 255, ${alpha})`;
    context.beginPath();
    context.arc(x, y, size, 0, Math.PI * 2);
    context.fill();
  }
}

class StarryNewTab extends LitElement {
  static properties = {
    state_: {state: true},
    docked_: {state: true},
    now_: {state: true},
    customizing_: {state: true},
    dragging_: {state: true},
  };

  constructor() {
    super();
    this.state_ = null;
    this.docked_ = false;
    this.now_ = new Date();
    this.customizing_ = false;
    this.dragging_ = '';
    this.noteTimer_ = 0;
    this.rectFrame_ = 0;
    this.revealed_ = false;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('newtab-changed', (state) => this.state_ = state);
    addWebUiListener('newtab-docked', (docked) => this.docked_ = docked);
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => this.requestUpdate());
    document.addEventListener('keydown', (event) => this.onKeyDown_(event));
    addEventListener('resize', () => {
      this.reportSearchRect_();
      this.drawBackground_();
    });
    addEventListener('scroll', () => this.reportSearchRect_(), {passive: true});
    document.addEventListener('pointerdown', (event) => {
      if (this.customizing_ && !event.target.closest('.nt-panel, .nt-customize')) {
        this.customizing_ = false;
      }
    });
    this.tick_();
    const state = await sendWithPromise('getNewTab');
    this.state_ = state;
    this.docked_ = state.docked;
  }

  tick_() {
    this.now_ = new Date();
    const delay = 60000 - (Date.now() % 60000) + 50;
    setTimeout(() => this.tick_(), delay);
  }

  updated(changed) {
    if (!this.state_) {
      return;
    }
    if (changed.has('state_') && !changed.get('state_')) {
      this.playIntro_();
    }
    this.reportSearchRect_();
    if (changed.has('state_')) {
      this.drawBackground_();
    }
  }

  playIntro_() {
    if (this.revealed_) {
      return;
    }
    this.revealed_ = true;
    if (reduceMotion()) {
      return;
    }
    // 搜索框由原生命令栏接管，不参与入场动画。
    [...this.querySelectorAll('.st')].forEach((element, index) => element.animate(
        [{opacity: 0, transform: 'translateY(12px)'}, {opacity: 1, transform: 'none'}],
        {duration: Motion.reveal.duration, easing: Motion.reveal.easing,
         delay: 80 + Math.min(index, Motion.staggerLimit) * Motion.staggerStep, fill: 'backwards'}));
  }

  drawBackground_() {
    const canvas = this.querySelector('canvas.nt-stars');
    if (canvas) {
      drawStars(canvas, !this.isDarkBackground_());
    }
  }

  reportSearchRect_() {
    if (this.rectFrame_) {
      return;
    }
    this.rectFrame_ = requestAnimationFrame(() => {
      this.rectFrame_ = 0;
      const search = this.querySelector('.nt-search');
      if (!search) {
        return;
      }
      const rect = search.getBoundingClientRect();
      const key = `${rect.left},${rect.top},${rect.width},${rect.height}`;
      if (key === this.lastRect_) {
        return;
      }
      this.lastRect_ = key;
      send('newTabSearchRect', rect.left, rect.top, rect.width, rect.height);
    });
  }

  // 在页面上打字：交给命令栏（第一个字带过去）。输入法的字没法带，命令栏打开后接着打。
  onKeyDown_(event) {
    if (this.docked_ || event.metaKey || event.ctrlKey || event.altKey || event.isComposing) {
      return;
    }
    const target = event.target;
    if (target.closest && target.closest('textarea, input')) {
      return;
    }
    if (event.key === '/' || (event.key.length === 1 && event.key.trim())) {
      event.preventDefault();
      send('newTabSearch', event.key === '/' ? '' : event.key);
    }
  }

  // ---- 状态 ----

  widgets_() {
    return (this.state_.widgets || []).filter((id) => WIDGETS.includes(id));
  }

  enabled_(id) {
    return this.widgets_().includes(id);
  }

  isDark_() {
    return matchMedia('(prefers-color-scheme: dark)').matches;
  }

  isDarkBackground_() {
    const background = this.state_.background;
    return background === 'image' || (background === 'sky' && this.isDark_());
  }

  setWidgets_(ids) {
    this.state_ = {...this.state_, widgets: ids};
    send('newTabSetWidgets', ids);
  }

  toggleWidget_(id) {
    const ids = this.widgets_();
    this.setWidgets_(ids.includes(id) ? ids.filter((x) => x !== id) : [...ids, id]);
  }

  // ---- 显示 ----

  render() {
    if (!this.state_) {
      return html`<div class="nt bg-theme"></div>`;
    }
    const s = this.state_;
    const background = BACKGROUNDS.includes(s.background) ? s.background : 'theme';
    return html`
      <div class="nt bg-${background} ${this.isDarkBackground_() ? 'on-dark' : ''} ${this.customizing_ ? 'editing' : ''}">
        <div class="nt-bg" aria-hidden="true">
          ${background === 'sky' ? html`<canvas class="nt-stars"></canvas><div class="nt-glow"></div>` : nothing}
          ${background === 'image' ? html`<div class="nt-image" style="background-image:url(background?v=${s.imageVersion})"></div>` : nothing}
        </div>
        <main class="nt-main">
          ${this.enabled_('clock') ? this.renderHello_() : nothing}
          <button class="nt-search ${this.docked_ ? 'docked' : ''}" aria-label=${L('commandBar.placeholder')}
              @click=${() => send('newTabSearch', '')}>
            ${icon('magnifyingglass', 16)}
            <span class="nt-search-text">${L('commandBar.placeholder')}</span>
            <span class="nt-search-key">${shortcutKey('L')}</span>
          </button>
          <section class="nt-grid">${this.renderWidgets_()}</section>
        </main>
        <button class="nt-customize ${this.customizing_ ? 'on' : ''}" title=${L('newTab.customize')}
            aria-label=${L('newTab.customize')} @click=${() => this.customizing_ = !this.customizing_}>
          ${this.customizing_ ? icon('checkmark', 15) : glyph('sliders', 16)}
        </button>
        ${this.customizing_ ? this.renderPanel_() : nothing}
      </div>`;
  }

  renderHello_() {
    const now = this.now_;
    const date = new Intl.DateTimeFormat(locale, {month: 'long', day: 'numeric', weekday: 'long'}).format(now);
    const time = new Intl.DateTimeFormat(locale, {hour: '2-digit', minute: '2-digit', hour12: false}).format(now);
    return html`
      <header class="nt-hello st">
        <h1>${greeting(now.getHours())}</h1>
        <div class="nt-date">${date} · ${time}</div>
      </header>`;
  }

  renderWidgets_() {
    const s = this.state_;
    const cards = this.widgets_().filter((id) => id !== 'clock').filter((id) => {
      // 没在放的时候「正在播放」不占位置（编辑时留着，好拖）。
      return id !== 'playing' || s.media || this.customizing_;
    });
    return repeat(cards, (id) => id, (id) => this.renderCard_(id));
  }

  renderCard_(id) {
    const s = this.state_;
    const span = id === 'shortcuts' ? `wide rows-${s.shortcutRows}` : '';
    let body;
    switch (id) {
      case 'shortcuts':
        body = this.renderShortcuts_();
        break;
      case 'playing':
        body = this.renderPlaying_();
        break;
      case 'continue':
        body = this.renderContinue_();
        break;
      case 'archive':
        body = this.renderArchive_();
        break;
      case 'shield':
        body = this.renderShield_();
        break;
      case 'note':
        body = this.renderNote_();
        break;
    }
    return html`
      <div class="nt-card st ${span} ${this.dragging_ === id ? 'lifted' : ''}" data-key=${id}
          draggable=${this.customizing_ ? 'true' : 'false'}
          @dragstart=${(event) => this.onDragStart_(event, id)} @dragend=${() => this.onDragEnd_()}
          @dragover=${(event) => this.onDragOver_(event, id)} @drop=${(event) => event.preventDefault()}>
        ${this.customizing_ ? html`
          <button class="nt-remove" aria-label=${L('newTab.widget.hide')} title=${L('newTab.widget.hide')}
              @click=${() => this.toggleWidget_(id)}>${icon('xmark', 10)}</button>` : nothing}
        ${body}
      </div>`;
  }

  cardHead_(iconHtml, title, action = nothing) {
    return html`<div class="nt-card-h">${iconHtml}<span>${title}</span><span class="sp"></span>${action}</div>`;
  }

  renderShortcuts_() {
    const s = this.state_;
    const max = s.shortcutRows === 2 ? 11 : 5;
    const tiles = s.shortcuts.slice(0, max);
    const more = html`
      <button class="nt-link" aria-label=${L('newTab.shortcut.more')} title=${L('newTab.shortcut.more')}
          @click=${(event) => this.shortcutsMenu_(event)}>${icon('ellipsis', 14)}</button>`;
    return html`
      ${this.cardHead_(glyph('grid', 13), L('newTab.shortcuts'), more)}
      <div class="nt-tiles">
        ${repeat(tiles, (tile) => tile.url, (tile) => html`
          <div class="nt-tile">
            <a href=${tile.url} class="nt-tile-link" title=${tile.url}
                @click=${(event) => {
                  event.preventDefault();
                  openUrl(tile.url, targetFromEvent(event));
                }}
                @auxclick=${(event) => {
                  if (event.button === 1) {
                    event.preventDefault();
                    openUrl(tile.url, 'background');
                  }
                }}
                @contextmenu=${(event) => this.tileMenu_(event, tile)}>
              <span class="nt-tile-box"><img src=${faviconUrl(tile.url, 24)} width="24" height="24" alt=""></span>
              <span class="nt-tile-title">${tile.title || hostOf(tile.url)}</span>
            </a>
            <button class="nt-tile-more" aria-label=${L('newTab.shortcut.more')}
                @click=${(event) => this.tileMenu_(event, tile)}>${icon('ellipsis', 12)}</button>
          </div>`)}
        ${tiles.length < max ? html`
          <div class="nt-tile">
            <button class="nt-tile-link" @click=${() => this.addShortcut_()}>
              <span class="nt-tile-box">${icon('plus', 18)}</span>
              <span class="nt-tile-title">${L('common.add')}</span>
            </button>
          </div>` : nothing}
      </div>`;
  }

  async tileMenu_(event, tile) {
    event.preventDefault();
    event.stopPropagation();
    const id = await showMenu([
      {id: 'background', title: L('newTab.shortcut.openBackground'), icon: 'plus.square.on.square'},
      {id: 'rename', title: L('newTab.shortcut.rename'), icon: 'pencil'},
      {separator: true},
      {id: 'remove', title: L('newTab.shortcut.remove'), icon: 'trash'},
    ], event.type === 'contextmenu' ? {event} : {element: event.currentTarget});
    if (id === 'background') {
      openUrl(tile.url, 'background');
    } else if (id === 'rename') {
      const result = await showDialog({
        icon: 'pencil', title: L('newTab.shortcut.renameTitle'), field: tile.title || hostOf(tile.url),
        buttons: [{title: L('common.cancel'), variant: 'tertiary', cancel: true}, {title: L('common.done'), variant: 'filled'}],
      });
      if (result && result.button === 1 && result.input.trim()) {
        send('newTabShortcut', 'rename', tile.url, result.input.trim());
      }
    } else if (id === 'remove') {
      send('newTabShortcut', 'remove', tile.url);
    }
  }

  async shortcutsMenu_(event) {
    const s = this.state_;
    const id = await showMenu([
      {id: 'add', title: L('newTab.shortcut.add'), icon: 'plus'},
      {id: 'rows', title: s.shortcutRows === 2 ? L('newTab.shortcut.oneRow') : L('newTab.shortcut.twoRows'),
       icon: 'rectangle.stack'},
      {separator: true},
      {id: 'reset', title: L('newTab.shortcut.reset'), icon: 'arrow.uturn.backward', enabled: s.shortcutsCustomized},
    ], {element: event.currentTarget});
    if (id === 'add') {
      this.addShortcut_();
    } else if (id === 'rows') {
      send('newTabSetShortcutRows', s.shortcutRows === 2 ? 1 : 2);
    } else if (id === 'reset') {
      send('newTabShortcut', 'reset');
    }
  }

  async addShortcut_() {
    const result = await showDialog({
      icon: 'plus', title: L('newTab.shortcut.addTitle'), message: L('newTab.shortcut.addMessage'), field: '',
      buttons: [{title: L('common.cancel'), variant: 'tertiary', cancel: true}, {title: L('common.add'), variant: 'filled'}],
    });
    if (result && result.button === 1 && result.input.trim()) {
      send('newTabShortcut', 'add', result.input.trim());
    }
  }

  renderPlaying_() {
    const media = this.state_.media;
    if (!media) {
      return html`${this.cardHead_(icon('play.rectangle', 13), L('newTab.playing'))}
        <div class="nt-empty">${L('newTab.playing.none')}</div>`;
    }
    return html`
      <div class="nt-np">
        <button class="nt-art" @click=${() => send('newTabMedia', 'show')} title=${L('newTab.playing.show')}>
          ${media.artwork ? html`<img src=${media.artwork} alt="">` : icon('play.rectangle', 20)}
        </button>
        <div class="nt-np-ctl">
          ${media.canPrevious ? html`<button aria-label=${L('newTab.playing.previous')}
              @click=${() => send('newTabMedia', 'previous')}>${glyph('backward', 13)}</button>` : nothing}
          <button class="pp" aria-label=${media.playing ? L('newTab.playing.pause') : L('newTab.playing.play')}
              @click=${() => send('newTabMedia', 'toggle')}>${icon(media.playing ? 'pause.fill' : 'play.fill', 12)}</button>
          ${media.canNext ? html`<button aria-label=${L('newTab.playing.next')}
              @click=${() => send('newTabMedia', 'next')}>${glyph('forward', 13)}</button>` : nothing}
        </div>
      </div>
      <button class="nt-np-meta" @click=${() => send('newTabMedia', 'show')}>
        <b>${media.title}</b>
        <span>${[media.artist, media.source].filter(Boolean).join(' · ')}</span>
      </button>`;
  }

  renderContinue_() {
    const closed = this.state_.closed.slice(0, 3);
    return html`
      ${this.cardHead_(icon('arrow.uturn.backward', 12), L('newTab.continue'))}
      ${closed.length ? closed.map((entry) => html`
        <button class="nt-row" title=${entry.url}
            @click=${(event) => send('newTabRestoreClosed', entry.id, targetFromEvent(event) !== 'current')}>
          ${entry.window ? icon('macwindow', 15) : html`<img class="nt-fav" src=${faviconUrl(entry.url, 16)} alt="">`}
          <span class="tt">${entry.window ? L('newTab.closed.windowTitle', {count: entry.tabs}) : (entry.title || hostOf(entry.url))}</span>
          <span class="tm">${timeAgo(entry.closed)}</span>
        </button>`) : html`<div class="nt-empty">${L('newTab.continue.none')}</div>`}`;
  }

  renderArchive_() {
    const archive = this.state_.archive;
    const today = archive.today > 0;
    const count = today ? archive.today : archive.week;
    const open = html`<button class="nt-link" @click=${() => send('newTabOpen', 'archive')}>${L('common.all')}</button>`;
    if (!archive.total) {
      return html`${this.cardHead_(icon('archivebox', 13), L('newTab.archive'), open)}
        <div class="nt-empty">${L('newTab.archive.none')}</div>`;
    }
    return html`
      ${this.cardHead_(icon('archivebox', 13), L('newTab.archive'), open)}
      <div class="nt-big">${new Intl.NumberFormat(locale).format(count)}
        <small>${count === 0 ? L('newTab.archive.quiet') : today ? L('newTab.archive.today') : L('newTab.archive.week')}</small>
      </div>
      <div class="nt-stack">
        ${archive.recent.map((tab) => html`<img src=${faviconUrl(tab.url, 16)} alt="" title=${tab.title || tab.url}>`)}
      </div>`;
  }

  renderShield_() {
    const blocked = this.state_.blocked;
    if (!blocked.enabled) {
      return html`
        ${this.cardHead_(icon('shield.slash', 13), L('newTab.shield'))}
        <div class="nt-empty">${L('newTab.shield.off')}</div>
        <button class="nt-chip" @click=${() => send('newTabOpen', 'privacy')}>${L('newTab.shield.turnOn')}</button>`;
    }
    const max = Math.max(1, ...blocked.days);
    return html`
      ${this.cardHead_(icon('shield.lefthalf.filled', 13), L('newTab.shield'))}
      <div class="nt-shield">
        <div>
          <div class="nt-big">${new Intl.NumberFormat(locale).format(blocked.total)}</div>
          <div class="nt-sub">${L('newTab.shield.detail')}</div>
        </div>
        <div class="nt-bars" aria-hidden="true">
          ${blocked.days.map((day, index) => html`
            <i class=${index === 6 ? 'today' : ''} style="height:${Math.max(3, Math.round((day / max) * 34))}px"></i>`)}
        </div>
      </div>`;
  }

  renderNote_() {
    return html`
      ${this.cardHead_(glyph('note', 13), L('newTab.note'))}
      <textarea class="nt-note" .value=${this.state_.note} placeholder=${L('newTab.note.placeholder')}
          spellcheck="false" @input=${(event) => this.onNoteInput_(event)}></textarea>`;
  }

  onNoteInput_(event) {
    const text = event.target.value;
    this.state_.note = text;
    clearTimeout(this.noteTimer_);
    this.noteTimer_ = setTimeout(() => send('newTabSetNote', text), 400);
  }

  // ---- 自定义 ----

  renderPanel_() {
    const s = this.state_;
    const backgrounds = [
      ['theme', L('newTab.background.theme')],
      ['sky', L('newTab.background.sky')],
      ['image', L('newTab.background.image')],
      ['solid', L('newTab.background.solid')],
    ];
    const items = [
      ['clock', L('newTab.widget.clock')],
      ['shortcuts', L('newTab.shortcuts')],
      ['playing', L('newTab.playing')],
      ['continue', L('newTab.continue')],
      ['archive', L('newTab.archive')],
      ['shield', L('newTab.shield')],
      ['note', L('newTab.note')],
    ];
    return html`
      <div class="nt-panel" role="dialog" aria-label=${L('newTab.customize')}>
        <div class="nt-panel-h">${L('newTab.customize')}<span>${L('newTab.customize.dragHint')}</span></div>
        <div class="nt-swatches" role="radiogroup" aria-label=${L('newTab.background')}>
          ${backgrounds.map(([value, title]) => html`
            <button class="nt-swatch sw-${value} ${s.background === value ? 'sel' : ''}" role="radio"
                aria-checked=${s.background === value ? 'true' : 'false'} title=${title}
                @click=${() => value === 'image' && s.background === 'image' ?
                    send('newTabChooseImage') : send('newTabSetBackground', value)}>
              ${value === 'image' ? icon('photo', 14) : nothing}
              <span>${title}</span>
            </button>`)}
        </div>
        ${items.map(([id, title]) => html`
          <div class="nt-line">
            <span>${title}</span>
            <button class="nt-switch ${this.enabled_(id) ? 'on' : ''}" role="switch" aria-label=${title}
                aria-checked=${this.enabled_(id) ? 'true' : 'false'} @click=${() => this.toggleWidget_(id)}></button>
          </div>`)}
        <button class="nt-more" @click=${() => send('newTabOpen', 'settings')}>
          ${L('newTab.customize.more')}<span>${shortcutKey(',')}</span>
        </button>
      </div>`;
  }

  // 编辑时拖动卡片排顺序：拖到别的卡片上就换过去，其余的卡片滑开（FLIP）。
  onDragStart_(event, id) {
    if (!this.customizing_) {
      event.preventDefault();
      return;
    }
    this.dragging_ = id;
    event.dataTransfer.effectAllowed = 'move';
    event.dataTransfer.setData('text/plain', id);
  }

  onDragEnd_() {
    this.dragging_ = '';
  }

  onDragOver_(event, target) {
    if (!this.dragging_ || this.dragging_ === target) {
      return;
    }
    event.preventDefault();
    const ids = this.widgets_();
    const from = ids.indexOf(this.dragging_);
    const to = ids.indexOf(target);
    if (from < 0 || to < 0) {
      return;
    }
    const before = new Map();
    for (const card of this.querySelectorAll('.nt-card[data-key]')) {
      before.set(card.dataset.key, card.getBoundingClientRect());
    }
    ids.splice(from, 1);
    ids.splice(to, 0, this.dragging_);
    this.setWidgets_(ids);
    this.updateComplete.then(() => {
      if (reduceMotion()) {
        return;
      }
      for (const card of this.querySelectorAll('.nt-card[data-key]')) {
        const old = before.get(card.dataset.key);
        if (!old) {
          continue;
        }
        const now = card.getBoundingClientRect();
        const dx = old.left - now.left;
        const dy = old.top - now.top;
        if (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5) {
          card.animate([{transform: `translate(${dx}px, ${dy}px)`}, {transform: 'none'}],
                       {duration: Motion.listEdit.duration, easing: Motion.listEdit.easing});
        }
      }
    });
  }
}

customElements.define('starry-newtab', StarryNewTab);
