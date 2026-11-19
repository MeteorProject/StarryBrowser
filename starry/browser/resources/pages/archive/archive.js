// Copyright 2026 The Starry Browser Authors
//
// 已归档的标签页（starry://archive）：资料库页面的样子（同历史
// 记录、下载）。数据是标签管理的归档（starry/browser/ui/webui/archive_handler.h）。
//
// 标题区下面是按为什么归档的筛选和网站的小胶囊；内容按归档的那天分组，每个标签一张卡片：
// 归档前网页的缩略图（没有时是放大的网站图标），下面是标题、网站和为什么归档的。点卡片恢复并
// 切过去（⌘ 点在后台恢复），悬停时右上角写着「恢复」、左上角是从归档里删掉，右键还有拷贝链接、
// 只看这个网站。「选择」一次恢复、删除好几个。地址里的 ?q= 是一打开就搜的词（命令栏、侧栏的
// 归档页带过来）。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, faviconUrl, sendWithPromise, showMenu} from './common/browser.js';
import {dayLabel, startOfDay, timeLabel} from './common/format.js';
import {L} from './common/l10n.js';
import {emptyState, icon, revealHeader, searchField} from './common/library.js';
import {Motion, reduceMotion, reveal} from './common/motion.js';

const REASONS = [
  {id: 'all', title: () => L('common.all')},
  {id: 'idle', title: () => L('archive.filter.idle')},
  {id: 'duplicate', title: () => L('archive.filter.duplicate')},
  {id: 'manual', title: () => L('archive.filter.manual')},
  {id: 'assistant', title: () => L('archive.filter.assistant')},
];
const SEPARATOR = ' · ';
const HOUR = 3600 * 1000;
const DAY = 24 * HOUR;
const MAX_SITES = 8;
const MAX_TOPICS = 10;

function displayHost(host) {
  return (host || '').replace(/^www\./, '');
}

// 「1 周」「3 天」「12 小时」（同设置 › 标签管理里的写法）。
function durationLabel(hours) {
  if (hours >= 24 * 7 && hours % (24 * 7) === 0 && hours < 24 * 28) {
    return L('settings.tabs.duration.weeks', {count: hours / (24 * 7)});
  }
  if (hours >= 24 && hours % 24 === 0) {
    return L('settings.tabs.duration.days', {count: hours / 24});
  }
  return L('settings.tabs.duration.hours', {count: hours});
}

function retentionLabel(days) {
  if (days <= 0) {
    return L('archive.subtitle.keepForever');
  }
  const duration = days % 365 === 0 ? L('settings.tabs.duration.years', {count: days / 365}) :
                                      L('settings.tabs.duration.days', {count: days});
  return L('archive.subtitle.keep', {duration});
}

// 为什么归档的：「闲置 9 天」「重复的标签页」「手动归档」「AI 助手归档」。
function reasonLabel(tab) {
  switch (tab.reason) {
    case 'duplicate':
      return L('archive.reason.duplicate');
    case 'manual':
      return L('archive.reason.manual');
    case 'assistant':
      return L('archive.reason.assistant');
    default: {
      const idle = tab.lastActive ? Math.max(0, tab.archived - tab.lastActive) : 0;
      if (idle >= DAY) {
        return L('archive.reason.idleDays', {count: Math.floor(idle / DAY)});
      }
      if (idle >= HOUR) {
        return L('archive.reason.idleHours', {count: Math.floor(idle / HOUR)});
      }
      return L('archive.reason.idle');
    }
  }
}

// 没有缩略图时卡片的底色：按网站算一个色相，同一个网站总是同一个颜色。
function hostHue(host) {
  let hash = 0;
  for (const char of host || '') {
    hash = (hash * 31 + char.codePointAt(0)) >>> 0;
  }
  return hash % 360;
}

function isBackgroundClick(event) {
  const isMac = navigator.userAgentData ? navigator.userAgentData.platform === 'macOS' :
                                          navigator.platform.startsWith('Mac');
  return event.button === 1 || (isMac ? event.metaKey : event.ctrlKey);
}

class StarryArchive extends LitElement {
  static properties = {
    tabs_: {state: true},
    info_: {state: true},
    loaded_: {state: true},
    query_: {state: true},
    reason_: {state: true},
    host_: {state: true},
    topic_: {state: true},
    selecting_: {state: true},
    selected_: {state: true},
  };

  constructor() {
    super();
    this.tabs_ = [];
    this.info_ = {};
    this.loaded_ = false;
    this.query_ = new URLSearchParams(location.search).get('q') || '';
    this.reason_ = 'all';
    this.host_ = '';
    this.topic_ = '';
    this.selecting_ = false;
    this.selected_ = new Set();
    // 归档变了：刷新前每张卡片在哪（刷新后从那里滑过去）。
    this.before_ = null;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('archive-changed', (info) => this.setInfo_(info, true));
    this.setInfo_(await sendWithPromise('getArchive'), false);
    await this.updateComplete;
    reveal(this.querySelector('.archive-content'), {delay: 0.16, distance: 8});
    reveal(this.querySelector('.empty-state'), {delay: 0.18});
  }

  firstUpdated() {
    revealHeader(this);
    this.placeHighlight_(false);
  }

  updated() {
    this.placeHighlight_(true);
    if (this.before_) {
      this.animateCards_(this.before_);
      this.before_ = null;
    }
  }

  setInfo_(info, animated) {
    if (animated && this.loaded_) {
      this.before_ = this.captureCards_();
    }
    this.info_ = info;
    this.tabs_ = info.tabs || [];
    this.loaded_ = info.loaded !== false;
    // 选中的里面已经没了的（恢复了、删了）去掉。
    const ids = new Set(this.tabs_.map((tab) => tab.id));
    if ([...this.selected_].some((id) => !ids.has(id))) {
      this.selected_ = new Set([...this.selected_].filter((id) => ids.has(id)));
    }
    if (this.host_ && !this.tabs_.some((tab) => tab.host === this.host_)) {
      this.host_ = '';
    }
    if (this.reason_ !== 'all' && !this.tabs_.some((tab) => tab.reason === this.reason_)) {
      this.reason_ = 'all';
    }
    if (this.topic_ && !this.tabs_.some((tab) => tab.topic === this.topic_)) {
      this.topic_ = '';
    }
  }

  // ---- 筛选 ----

  matches_(tab, {reason = true} = {}) {
    if (reason && this.reason_ !== 'all' && tab.reason !== this.reason_) {
      return false;
    }
    if (this.host_ && tab.host !== this.host_) {
      return false;
    }
    if (this.topic_ && tab.topic !== this.topic_) {
      return false;
    }
    const words = this.query_.toLowerCase().split(/\s+/).filter(Boolean);
    if (!words.length) {
      return true;
    }
    const text = `${tab.title} ${tab.url} ${tab.topic || ''}`.toLowerCase();
    return words.every((word) => text.includes(word));
  }

  // 按归档的那天分组（新的在前，和归档里的顺序一样）。
  sections_(shown) {
    const days = new Map();
    for (const tab of shown) {
      const day = startOfDay(tab.archived);
      if (!days.has(day)) {
        days.set(day, []);
      }
      days.get(day).push(tab);
    }
    return [...days].map(([day, tabs]) => ({day, tabs}));
  }

  // 归档最多的几个网站。
  sites_() {
    const sites = new Map();
    for (const tab of this.tabs_) {
      if (!tab.host) {
        continue;
      }
      const site = sites.get(tab.host) || {host: tab.host, url: tab.url, count: 0};
      site.count++;
      sites.set(tab.host, site);
    }
    return [...sites.values()].sort((a, b) => b.count - a.count).slice(0, MAX_SITES);
  }

  // 归档时打的主题，多的在前（两个以上才显示）。
  topics_() {
    const topics = new Map();
    for (const tab of this.tabs_) {
      if (tab.topic) {
        topics.set(tab.topic, (topics.get(tab.topic) || 0) + 1);
      }
    }
    return [...topics].map(([topic, count]) => ({topic, count}))
        .sort((a, b) => b.count - a.count).slice(0, MAX_TOPICS);
  }

  subtitle_() {
    if (!this.loaded_) {
      return ' ';
    }
    const info = this.info_;
    const parts = [L('archive.subtitle.count', {count: this.tabs_.length})];
    parts.push(info.autoArchive ? L('archive.subtitle.auto', {duration: durationLabel(info.idleHours || 168)}) :
                                  L('archive.subtitle.autoOff'));
    parts.push(retentionLabel(info.retentionDays ?? 90));
    return parts.join(SEPARATOR);
  }

  toggleHost_(host) {
    this.host_ = this.host_ === host ? '' : host;
  }

  toggleTopic_(topic) {
    this.topic_ = this.topic_ === topic ? '' : topic;
  }

  // 将筛选高亮胶囊滑动到当前选中项。
  placeHighlight_(animated) {
    const picker = this.querySelector('.filter-picker');
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
    highlight.dataset.left = String(left);
    highlight.dataset.width = String(width);
    highlight.style.transition = animated ? '' : 'none';
    highlight.style.transform = `translateX(${left}px)`;
    highlight.style.width = `${width}px`;
  }

  // ---- 动作 ----

  restore_(ids, select) {
    if (ids.length) {
      chrome.send('restoreArchived', [ids, select]);
    }
  }

  remove_(ids) {
    if (ids.length) {
      chrome.send('removeArchived', [ids]);
    }
  }

  async clear_() {
    if (await confirm({icon: 'trash.fill', title: L('settings.tabs.archive.clear.confirm'),
                       message: L('settings.tabs.archive.clear.message'), confirmTitle: L('common.clear')})) {
      chrome.send('clearArchive');
    }
  }

  activate_(event, tab) {
    if (this.selecting_) {
      this.toggleSelected_(tab.id);
      return;
    }
    this.restore_([tab.id], !isBackgroundClick(event));
  }

  async showCardMenu_(event, tab) {
    event.preventDefault();
    const host = displayHost(tab.host);
    const id = await showMenu([
      {id: 'restore', title: L('archive.menu.restore'), icon: 'arrow.uturn.backward'},
      {id: 'background', title: L('archive.menu.restoreBackground'), icon: 'plus.square.on.square'},
      {id: 'copy', title: L('library.menu.copyLink'), icon: 'link'},
      {separator: true},
      {id: 'site', title: L('archive.menu.onlySite', {host}), icon: 'globe', enabled: Boolean(host)},
      {separator: true},
      {id: 'remove', title: L('archive.menu.remove'), icon: 'trash'},
    ], {event});
    switch (id) {
      case 'restore':
        this.restore_([tab.id], true);
        break;
      case 'background':
        this.restore_([tab.id], false);
        break;
      case 'copy':
        copyText(tab.url);
        break;
      case 'site':
        this.host_ = tab.host;
        break;
      case 'remove':
        this.remove_([tab.id]);
        break;
    }
  }

  toggleSelecting_() {
    this.selecting_ = !this.selecting_;
    this.selected_ = new Set();
  }

  toggleSelected_(id) {
    const selected = new Set(this.selected_);
    if (selected.has(id)) {
      selected.delete(id);
    } else {
      selected.add(id);
    }
    this.selected_ = selected;
  }

  // ---- 卡片换位置的动画（恢复、删掉以后后面的补上来）----

  captureCards_() {
    const rects = new Map();
    for (const card of this.querySelectorAll('.archive-card[data-key]')) {
      rects.set(card.dataset.key, card.getBoundingClientRect());
    }
    return rects;
  }

  animateCards_(before) {
    if (reduceMotion()) {
      return;
    }
    const {duration, easing} = Motion.listEdit;
    for (const card of this.querySelectorAll('.archive-card[data-key]')) {
      const old = before.get(card.dataset.key);
      const now = card.getBoundingClientRect();
      if (!old) {
        card.animate([{opacity: 0, transform: 'scale(0.96)'}, {opacity: 1, transform: 'none'}], {duration, easing});
        continue;
      }
      const dx = old.left - now.left;
      const dy = old.top - now.top;
      if (Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5) {
        card.animate([{transform: `translate(${dx}px, ${dy}px)`}, {transform: 'none'}], {duration, easing});
      }
    }
  }

  // ---- 界面 ----

  renderFilters_() {
    const counts = new Map();
    for (const tab of this.tabs_) {
      if (this.matches_(tab, {reason: false})) {
        counts.set(tab.reason, (counts.get(tab.reason) || 0) + 1);
      }
    }
    const shown = REASONS.filter((reason) => reason.id === 'all' || counts.get(reason.id) ||
                                             reason.id === this.reason_);
    if (shown.length <= 2) {
      return nothing;
    }
    return html`
      <div class="filter-picker">
        <div class="filter-highlight"></div>
        ${shown.map((reason) => html`
          <button class="filter ${reason.id === this.reason_ ? 'selected' : ''}"
              @click=${() => { this.reason_ = reason.id; }}>
            <span>${reason.title()}</span>
            ${reason.id !== 'all' && counts.get(reason.id) ?
                html`<span class="filter-count">${counts.get(reason.id)}</span>` : nothing}
          </button>`)}
      </div>`;
  }

  renderSites_() {
    const sites = this.sites_();
    if (sites.length < 2) {
      return nothing;
    }
    return html`
      <div class="archive-sites">
        <span class="sites-label">${L('archive.sites')}</span>
        <div class="site-chips">
          ${sites.map((site) => html`
            <button class="site-chip ${this.host_ === site.host ? 'active' : ''}"
                @click=${() => this.toggleHost_(site.host)}>
              <img src=${faviconUrl(site.url)} alt="">
              <span class="ellipsis">${displayHost(site.host)}</span>
              <span class="site-chip-count">${site.count}</span>
            </button>`)}
        </div>
      </div>`;
  }

  renderTopics_() {
    const topics = this.topics_();
    if (topics.length < 2) {
      return nothing;
    }
    return html`
      <div class="archive-sites archive-topics">
        <span class="sites-label">${L('archive.topics')}</span>
        <div class="site-chips">
          ${topics.map(({topic, count}) => html`
            <button class="site-chip topic-chip ${this.topic_ === topic ? 'active' : ''}"
                @click=${() => this.toggleTopic_(topic)}>
              <span class="ellipsis">${topic}</span>
              <span class="site-chip-count">${count}</span>
            </button>`)}
        </div>
      </div>`;
  }

  renderCard_(tab) {
    const selected = this.selected_.has(tab.id);
    const host = displayHost(tab.host);
    const hideBroken = (event) => {
      event.target.hidden = true;
    };
    return html`
      <div class="archive-card ${selected ? 'selected' : ''}" data-key=${tab.id} role="button" tabindex="0"
          title=${tab.url}
          @click=${(event) => this.activate_(event, tab)}
          @auxclick=${(event) => event.button === 1 && this.activate_(event, tab)}
          @keydown=${(event) => (event.key === 'Enter' || event.key === ' ') && this.activate_(event, tab)}
          @contextmenu=${(event) => this.showCardMenu_(event, tab)}>
        <div class="card-thumb" style="--hue: ${hostHue(host)}">
          <img class="thumb-favicon" src=${faviconUrl(tab.url, 32)} alt="">
          ${tab.thumbnail ? html`<img class="thumb-image" src="thumb/${tab.id}.jpg" alt="" loading="lazy"
              @error=${hideBroken}>` : nothing}
          ${this.selecting_ ? html`
            <span class="card-check">${icon(selected ? 'checkmark.circle.fill' : 'circle', 18)}</span>` : html`
            <button class="card-remove" title=${L('archive.menu.remove')} tabindex="-1"
                @click=${(event) => { event.stopPropagation(); this.remove_([tab.id]); }}>
              ${icon('xmark', 9)}
            </button>
            <span class="card-restore">${icon('arrow.uturn.backward', 11)}<span>${L('archive.restore')}</span></span>`}
        </div>
        <div class="card-meta">
          <img class="favicon" src=${faviconUrl(tab.url)} alt="">
          <div class="card-text">
            <div class="card-title ellipsis">${tab.title || host || tab.url}</div>
            <div class="card-detail ellipsis">${[host, tab.topic || reasonLabel(tab)].filter(Boolean)
                                                    .join(SEPARATOR)}</div>
          </div>
          <span class="card-time">${timeLabel(tab.archived)}</span>
        </div>
      </div>`;
  }

  renderEmpty_() {
    if (this.query_ || this.host_ || this.topic_ || this.reason_ !== 'all') {
      return emptyState({
        iconName: 'magnifyingglass',
        title: this.query_ ? L('library.search.noResults', {query: this.query_}) : L('archive.empty.filtered'),
      });
    }
    const info = this.info_;
    return emptyState({
      iconName: 'archivebox',
      title: L('archive.empty.title'),
      detail: info.autoArchive ? L('archive.empty.auto', {duration: durationLabel(info.idleHours || 168)}) :
                                 L('archive.empty.manual'),
    });
  }

  renderBatchBar_() {
    if (!this.selecting_) {
      return nothing;
    }
    const ids = [...this.selected_];
    return html`
      <div class="batch-bar">
        <span class="batch-count">${L('archive.select.count', {count: ids.length})}</span>
        <span class="batch-divider"></span>
        <button class="batch-btn" ?disabled=${!ids.length}
            @click=${() => { this.restore_(ids, false); this.selecting_ = false; }}>
          ${icon('arrow.uturn.backward', 12)}<span>${L('archive.restore')}</span>
        </button>
        <button class="batch-btn danger" ?disabled=${!ids.length}
            @click=${() => { this.remove_(ids); this.selecting_ = false; }}>
          ${icon('trash', 12)}<span>${L('archive.select.delete')}</span>
        </button>
        <button class="batch-btn ghost" @click=${() => this.toggleSelecting_()}>${L('common.done')}</button>
      </div>`;
  }

  render() {
    const shown = this.tabs_.filter((tab) => this.matches_(tab));
    const sections = this.sections_(shown);
    return html`
      <div class="page archive-page ${this.selecting_ ? 'has-batch-bar' : ''}">
        <header class="library-header archive-header">
          <div class="eyebrow">${L('library.header')}</div>
          <div class="title-row">
            <h1 class="title">${L('internalPage.archive')}</h1>
            <span class="spacer"></span>
            <div class="header-actions">
              <button class="pill-button ${this.selecting_ ? 'active' : ''}"
                  ?disabled=${!this.tabs_.length && !this.selecting_} @click=${() => this.toggleSelecting_()}>
                ${icon('checkmark.circle', 12)}
                <span>${this.selecting_ ? L('common.done') : L('archive.select.button')}</span>
              </button>
              <button class="pill-button" ?disabled=${!this.tabs_.length} @click=${() => this.clear_()}>
                ${icon('trash', 12)}<span>${L('settings.tabs.archive.clear.button')}</span>
              </button>
              <button class="round-button" title=${L('archive.settings.help')}
                  @click=${() => chrome.send('openTabSettings')}>
                ${icon('gearshape', 14)}
              </button>
            </div>
          </div>
          <div class="subtitle">${this.subtitle_()}</div>
          <div class="toolbar">
            ${searchField({value: this.query_, placeholder: L('archive.search.placeholder'),
                           onInput: (value) => { this.query_ = value; }})}
            <span class="spacer"></span>
            ${this.renderFilters_()}
          </div>
          ${this.renderSites_()}
          ${this.renderTopics_()}
        </header>
        ${this.loaded_ && !shown.length ? this.renderEmpty_() : nothing}
        <div class="archive-content ${this.selecting_ ? 'selecting' : ''}">
          ${repeat(sections, (section) => section.day, (section) => html`
            <section class="archive-day">
              <div class="day-header">
                <span class="day-title">${dayLabel(section.day)}</span>
                <span class="day-count">${section.tabs.length}</span>
                <span class="spacer"></span>
                ${this.selecting_ ? nothing : html`
                  <button class="ghost-pill day-restore"
                      @click=${() => this.restore_(section.tabs.map((tab) => tab.id), false)}>
                    ${icon('arrow.uturn.backward', 11)}<span>${L('archive.day.restoreAll')}</span>
                  </button>`}
              </div>
              <div class="archive-grid">
                ${repeat(section.tabs, (tab) => tab.id, (tab) => this.renderCard_(tab))}
              </div>
            </section>`)}
        </div>
      </div>
      ${this.renderBatchBar_()}`;
  }
}

customElements.define('starry-archive', StarryArchive);

document.documentElement.style.setProperty('--ease-page-filter', 'cubic-bezier(0.16, 1, 0.3, 1)');
document.documentElement.style.setProperty('--duration-page-filter', '360ms');
