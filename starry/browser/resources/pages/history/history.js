// Copyright 2026 The Starry Browser Authors
//
// 历史记录页：支持按时间段与指定日期筛选、30 天浏览活动柱状图、常访网站与书签筛选、
// 列表 / 时间线（按连续浏览时段聚合 + 日期跳转轴）双视图、搜索高亮与多选批量操作。
// 数据见 starry/browser/ui/webui/history_handler.h。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, faviconUrl, openUrl, sendWithPromise, showMenu, targetFromEvent} from './common/browser.js';
import {dayLabel, displayUrl, monthYearLabel, shortDayLabel, startOfDay, timeLabel, weekdayLabels} from './common/format.js';
import {L} from './common/l10n.js';
import {emptyState, icon, pillButton, revealHeader, searchField} from './common/library.js';
import {animateRows, captureRows, fadeOutRow, Motion, reduceMotion, reveal} from './common/motion.js';

const PAGE_SIZE = 120;
const SEARCH_DELAY = 180;
const OVERVIEW_DAYS = 30;
// 时间线视图里相邻访问间隔不超过 30 分钟归为同一连续浏览时段。
const SESSION_GAP_MS = 30 * 60 * 1000;
// 同一时段里紧挨着刷新同一个网址（2 分钟内）合并成一条显示。
const MERGE_SAME_URL_MS = 2 * 60 * 1000;

const TIME_FILTERS = [
  {id: 'all', title: () => L('common.all')},
  {id: 'today', title: () => L('library.day.today')},
  {id: 'yesterday', title: () => L('library.day.yesterday')},
  {id: 'week', title: () => L('history.filter.last7Days')},
  {id: 'month', title: () => L('history.filter.last30Days')},
];

const CLEAR_RANGES = [
  {id: 'lastHour', title: () => L('history.clearRange.lastHour'), confirm: () => L('history.clear.confirm.lastHour')},
  {id: 'today', title: () => L('history.clearRange.today'), confirm: () => L('history.clear.confirm.today')},
  {id: 'todayAndYesterday', title: () => L('history.clearRange.todayAndYesterday'), confirm: () => L('history.clear.confirm.todayAndYesterday')},
  {id: 'last7Days', title: () => L('history.clearRange.last7Days'), confirm: () => L('history.clear.confirm.last7Days')},
  {id: 'all', title: () => L('history.clearRange.all'), confirm: () => L('history.clear.confirm.all')},
];

function addDays(dayMs, delta) {
  const date = new Date(dayMs);
  date.setDate(date.getDate() + delta);
  date.setHours(0, 0, 0, 0);
  return date.getTime();
}

function listEntryKey(entry) {
  return `${startOfDay(entry.time)}|${entry.url}`;
}

function timelineEntryKey(entry) {
  return `v:${entry.id || entry.time}|${entry.url}`;
}

function escapeRegExp(text) {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function periodLabel(time) {
  const hour = new Date(time).getHours();
  if (hour < 6) {
    return L('history.timeline.period.lateNight');
  }
  if (hour < 11) {
    return L('history.timeline.period.morning');
  }
  if (hour < 14) {
    return L('history.timeline.period.noon');
  }
  if (hour < 18) {
    return L('history.timeline.period.afternoon');
  }
  return L('history.timeline.period.evening');
}

class StarryHistory extends LitElement {
  static properties = {
    query_: {state: true},
    viewMode_: {state: true},
    timeFilter_: {state: true},
    selectedDay_: {state: true},
    hostFilter_: {state: true},
    starredOnly_: {state: true},
    entries_: {state: true},
    total_: {state: true},
    loaded_: {state: true},
    incognito_: {state: true},
    overview_: {state: true},
    hoveredDay_: {state: true},
    hoveredHost_: {state: true},
    collapsedDays_: {state: true},
    collapsedSessions_: {state: true},
    activeDay_: {state: true},
    selecting_: {state: true},
    selectedKeys_: {state: true},
    copiedKey_: {state: true},
    calendar_: {state: true},
  };

  constructor() {
    super();
    const params = new URLSearchParams(location.search);
    this.query_ = params.get('q') || '';
    this.viewMode_ = params.get('view') === 'timeline' ? 'timeline' : 'list';
    this.timeFilter_ = 'all';
    this.selectedDay_ = null;
    this.hostFilter_ = params.get('host') || '';
    this.starredOnly_ = false;
    this.entries_ = [];
    this.total_ = 0;
    this.loaded_ = false;
    this.incognito_ = false;
    this.finished_ = false;
    this.cursor_ = 0;
    this.loadingMore_ = false;
    this.generation_ = 0;
    this.searchTimer_ = 0;
    this.copyTimer_ = 0;
    this.scrollRaf_ = 0;
    this.overview_ = {hosts: [], visits: []};
    this.hoveredDay_ = null;
    this.hoveredHost_ = '';
    this.collapsedDays_ = new Set();
    this.collapsedSessions_ = new Set();
    this.activeDay_ = null;
    this.selecting_ = false;
    this.selectedKeys_ = new Set();
    this.lastSelectedKey_ = '';
    this.copiedKey_ = '';
    this.calendar_ = null;
    this.onKeyDown_ = this.handleKeyDown_.bind(this);
    this.onScroll_ = this.handleScroll_.bind(this);
  }

  createRenderRoot() {
    return this;
  }

  connectedCallback() {
    super.connectedCallback();
    this.reload_({refreshOverview: true});
    addWebUiListener('history-changed', () => this.reload_({animated: true, refreshOverview: true}));
    this.observer_ = new IntersectionObserver((entries) => {
      if (entries.some((entry) => entry.isIntersecting)) {
        this.loadMore_();
      }
    }, {rootMargin: '420px'});
    this.resizeObserver_ = new ResizeObserver(() => this.placeHighlights_(false));
    this.resizeObserver_.observe(document.documentElement);
    window.addEventListener('keydown', this.onKeyDown_);
    window.addEventListener('scroll', this.onScroll_, {passive: true});
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    clearTimeout(this.searchTimer_);
    clearTimeout(this.copyTimer_);
    cancelAnimationFrame(this.scrollRaf_);
    this.observer_?.disconnect();
    this.resizeObserver_?.disconnect();
    window.removeEventListener('keydown', this.onKeyDown_);
    window.removeEventListener('scroll', this.onScroll_);
  }

  firstUpdated() {
    revealHeader(this);
    this.placeHighlights_(false);
  }

  updated() {
    const sentinel = this.querySelector('.load-more');
    if (sentinel && sentinel !== this.sentinel_) {
      this.observer_.disconnect();
      this.observer_.observe(sentinel);
      this.sentinel_ = sentinel;
    }
    this.placeHighlights_(true);
  }

  // ---- 时间区间与数据读取 ----

  activeRange_(now = Date.now()) {
    const today = startOfDay(now);
    if (this.selectedDay_ !== null) {
      return {after: this.selectedDay_, before: addDays(this.selectedDay_, 1)};
    }
    switch (this.timeFilter_) {
      case 'today':
        return {after: today, before: null};
      case 'yesterday':
        return {after: addDays(today, -1), before: today};
      case 'week':
        return {after: addDays(today, -6), before: null};
      case 'month':
        return {after: addDays(today, -(OVERVIEW_DAYS - 1)), before: null};
      default:
        return {after: null, before: null};
    }
  }

  get hasFilters_() {
    return this.selectedDay_ !== null || Boolean(this.hostFilter_) || this.starredOnly_ ||
        this.timeFilter_ !== 'all';
  }

  rowKey_(entry) {
    return this.viewMode_ === 'timeline' ? timelineEntryKey(entry) : listEntryKey(entry);
  }

  async fetchOverview_() {
    const start = addDays(startOfDay(Date.now()), -(OVERVIEW_DAYS - 1));
    const overview = await sendWithPromise('historyOverview', start);
    this.overview_ = {hosts: overview.hosts || [], visits: overview.visits || []};
    if (overview.incognito !== undefined) {
      this.incognito_ = overview.incognito;
    }
  }

  async reload_({animated = false, transition = false, refreshOverview = false} = {}) {
    const generation = ++this.generation_;
    const query = this.query_;
    const {after, before} = this.activeRange_();
    const allVisits = this.viewMode_ === 'timeline';
    const host = this.hostFilter_;
    const starredOnly = this.starredOnly_;
    const exactCountAvailable = !query && !host && !starredOnly;

    const overviewPromise = refreshOverview ? this.fetchOverview_() : Promise.resolve();
    const [page, count] = await Promise.all([
      sendWithPromise('queryHistory', {query, after, before, host, allVisits, starredOnly, max: PAGE_SIZE}),
      exactCountAvailable ? sendWithPromise('historyCount', after, before) : Promise.resolve(null),
      overviewPromise,
    ]);
    if (generation !== this.generation_) {
      return;
    }

    let entries = page.entries;
    let finished = page.finished;
    let cursor = page.nextCursor;
    this.incognito_ = Boolean(page.incognito);

    // 只看书签时书签可能散落在较早的历史里，多扫几页避免首屏空白。
    let scans = 0;
    while (starredOnly && entries.length < 40 && !finished && cursor > 0 && scans < 5) {
      scans++;
      const next = await sendWithPromise(
          'queryHistory', {query, after, before: cursor, host, allVisits, starredOnly, max: PAGE_SIZE});
      if (generation !== this.generation_) {
        return;
      }
      const known = new Set(entries.map((item) => this.rowKey_(item)));
      entries = [...entries, ...next.entries.filter((item) => !known.has(this.rowKey_(item)))];
      finished = next.finished;
      cursor = next.nextCursor;
    }

    const container = this.querySelector('.history-content');
    const beforeRects = animated && container ? captureRows(container) : null;

    this.entries_ = entries;
    this.finished_ = finished;
    this.cursor_ = cursor;
    this.total_ = exactCountAvailable ? count : entries.length;
    if (entries.length) {
      this.activeDay_ = startOfDay(entries[0].time);
    } else {
      this.activeDay_ = null;
    }

    // 清理已经不在列表里的选中项。
    if (this.selectedKeys_.size) {
      const validKeys = new Set(this.visibleOrder_().map((item) => item.key));
      const kept = new Set([...this.selectedKeys_].filter((key) => validKeys.has(key)));
      if (kept.size !== this.selectedKeys_.size) {
        this.selectedKeys_ = kept;
      }
    }

    const firstLoad = !this.loaded_;
    this.loaded_ = true;
    await this.updateComplete;

    if (beforeRects) {
      animateRows(this.querySelector('.history-content'), beforeRects);
    }
    if (firstLoad) {
      reveal(this.querySelector('.history-overview'), {delay: 0.13, distance: 6});
      reveal(this.querySelector('.history-content'), {delay: 0.17, distance: 8});
      reveal(this.querySelector('.empty-state'), {delay: 0.17, distance: 8});
    } else if (transition && !reduceMotion()) {
      const target = this.querySelector('.history-content') || this.querySelector('.empty-state');
      target?.animate(
          [{opacity: 0, transform: 'translateY(6px)'}, {opacity: 1, transform: 'none'}],
          {duration: Motion.tab.duration, easing: Motion.tab.easing});
    }
  }

  async loadMore_() {
    if (!this.loaded_ || this.finished_ || this.loadingMore_ || !this.cursor_) {
      return;
    }
    this.loadingMore_ = true;
    const generation = this.generation_;
    const {after} = this.activeRange_();
    const page = await sendWithPromise('queryHistory', {
      query: this.query_,
      after,
      before: this.cursor_,
      host: this.hostFilter_,
      allVisits: this.viewMode_ === 'timeline',
      starredOnly: this.starredOnly_,
      max: PAGE_SIZE,
    });
    this.loadingMore_ = false;
    if (generation !== this.generation_) {
      return;
    }
    const known = new Set(this.entries_.map((entry) => this.rowKey_(entry)));
    const added = page.entries.filter((entry) => !known.has(this.rowKey_(entry)));
    this.entries_ = [...this.entries_, ...added];
    this.finished_ = page.finished;
    this.cursor_ = page.nextCursor;
    if (this.query_ || this.hostFilter_ || this.starredOnly_) {
      this.total_ = this.entries_.length;
    }
  }

  // ---- 筛选与视图切换 ----

  onQuery_(value) {
    this.query_ = value;
    clearTimeout(this.searchTimer_);
    this.searchTimer_ = setTimeout(() => this.reload_(), value ? SEARCH_DELAY : 0);
  }

  setTimeFilter_(id) {
    if (this.timeFilter_ === id && this.selectedDay_ === null) {
      return;
    }
    this.timeFilter_ = id;
    this.selectedDay_ = null;
    this.collapsedDays_ = new Set();
    this.reload_({transition: true});
  }

  selectDay_(day) {
    this.selectedDay_ = this.selectedDay_ === day ? null : day;
    this.calendar_ = null;
    this.collapsedDays_ = new Set();
    this.reload_({transition: true});
  }

  toggleHostFilter_(host) {
    this.hostFilter_ = this.hostFilter_ === host ? '' : host;
    this.hoveredHost_ = '';
    this.collapsedDays_ = new Set();
    this.reload_({transition: true});
  }

  toggleStarredOnly_() {
    this.starredOnly_ = !this.starredOnly_;
    this.collapsedDays_ = new Set();
    this.reload_({transition: true});
  }

  resetFilters_() {
    this.timeFilter_ = 'all';
    this.selectedDay_ = null;
    this.hostFilter_ = '';
    this.starredOnly_ = false;
    this.collapsedDays_ = new Set();
    this.reload_({transition: true});
  }

  setViewMode_(mode) {
    if (this.viewMode_ === mode) {
      return;
    }
    this.viewMode_ = mode;
    this.selectedKeys_ = new Set();
    this.lastSelectedKey_ = '';
    this.reload_({transition: true});
  }

  toggleSelecting_(force) {
    const next = typeof force === 'boolean' ? force : !this.selecting_;
    this.selecting_ = next;
    this.selectedKeys_ = new Set();
    this.lastSelectedKey_ = '';
  }

  // ---- 键盘与滚动联动 ----

  handleKeyDown_(event) {
    if (event.key === 'Escape') {
      if (this.calendar_) {
        event.preventDefault();
        this.calendar_ = null;
        return;
      }
      if (this.selecting_) {
        event.preventDefault();
        this.toggleSelecting_(false);
        return;
      }
      if (document.activeElement?.tagName !== 'INPUT' &&
          (this.selectedDay_ !== null || this.hostFilter_ || this.starredOnly_)) {
        event.preventDefault();
        this.resetFilters_();
      }
      return;
    }
    if (event.key === '/' && !event.metaKey && !event.ctrlKey && !event.altKey &&
        document.activeElement?.tagName !== 'INPUT' && document.activeElement?.tagName !== 'TEXTAREA') {
      const input = this.querySelector('.search input');
      if (input) {
        event.preventDefault();
        input.focus();
      }
    }
  }

  handleScroll_() {
    if (this.viewMode_ !== 'timeline' || this.scrollRaf_) {
      return;
    }
    this.scrollRaf_ = requestAnimationFrame(() => {
      this.scrollRaf_ = 0;
      const sections = this.querySelectorAll('.timeline-stream .day-section[data-day]');
      if (!sections.length) {
        return;
      }
      let current = Number(sections[0].dataset.day);
      for (const section of sections) {
        const rect = section.getBoundingClientRect();
        if (rect.top <= 110) {
          current = Number(section.dataset.day);
        } else {
          break;
        }
      }
      if (current !== this.activeDay_) {
        this.activeDay_ = current;
      }
    });
  }

  jumpToDay_(day) {
    this.activeDay_ = day;
    const section = this.querySelector(`.timeline-stream .day-section[data-day="${day}"]`);
    if (section) {
      section.scrollIntoView({behavior: reduceMotion() ? 'auto' : 'smooth', block: 'start'});
    }
  }

  // ---- 高亮滑块定位 ----

  placeHighlights_(animated) {
    for (const picker of this.querySelectorAll('.segmented-picker')) {
      const highlight = picker.querySelector('.segmented-highlight');
      if (!highlight) {
        continue;
      }
      const selected = picker.querySelector('.segment-btn.selected');
      if (!selected) {
        highlight.style.opacity = '0';
        highlight.dataset.left = '';
        continue;
      }
      const left = selected.offsetLeft;
      const width = selected.offsetWidth;
      if (width === 0) {
        continue;
      }
      const wasHidden = highlight.style.opacity === '0';
      highlight.style.opacity = '1';
      if (highlight.dataset.left === String(left) && highlight.dataset.width === String(width)) {
        continue;
      }
      highlight.dataset.left = String(left);
      highlight.dataset.width = String(width);
      highlight.style.transition = animated && !wasHidden && !reduceMotion() ? '' : 'none';
      highlight.style.transform = `translateX(${left}px)`;
      highlight.style.width = `${width}px`;
    }

    const railList = this.querySelector('.timeline-rail-list');
    const railActive = railList?.querySelector('.timeline-rail-item.active');
    const railHighlight = railList?.querySelector('.timeline-rail-highlight');
    if (railActive && railHighlight) {
      const top = railActive.offsetTop;
      if (railHighlight.dataset.top !== String(top)) {
        railHighlight.dataset.top = String(top);
        railHighlight.style.transition = animated && !reduceMotion() ? '' : 'none';
        railHighlight.style.transform = `translateY(${top}px)`;
      }
    }
  }

  // ---- 副标题与 30 天概览数据 ----

  get subtitle_() {
    if (!this.loaded_) {
      return '';
    }
    if (this.selecting_ && this.selectedKeys_.size > 0) {
      return L('history.select.count', {count: this.selectedKeys_.size});
    }
    const parts = [];
    if (this.selectedDay_ !== null) {
      parts.push(dayLabel(this.selectedDay_));
    } else if (this.timeFilter_ !== 'all') {
      const filter = TIME_FILTERS.find((item) => item.id === this.timeFilter_);
      if (filter) {
        parts.push(filter.title());
      }
    }
    if (this.hostFilter_) {
      parts.push(this.hostFilter_);
    }
    if (this.starredOnly_) {
      parts.push(L('history.filter.bookmarked'));
    }

    let countText;
    if (this.query_) {
      countText = this.total_ === 0 ? L('history.subtitle.noMatches') :
                                      L('history.subtitle.found', {count: this.total_});
    } else {
      countText = this.total_ === 0 ? L('history.subtitle.empty') :
                                      L('history.subtitle.total', {count: this.total_});
    }
    parts.push(countText);
    return parts.join(' · ');
  }

  computeOverviewStats_() {
    const now = Date.now();
    const today = startOfDay(now);
    const {after, before} = this.activeRange_(now);
    const focusHost = this.hoveredHost_ || this.hostFilter_;
    const hosts = this.overview_.hosts;

    const dayCounts = new Map();
    const siteCounts = new Map();
    for (const [time, hostIndex] of this.overview_.visits) {
      const hostInfo = hostIndex >= 0 ? hosts[hostIndex] : null;
      const day = startOfDay(time);
      if (!focusHost || hostInfo?.host === focusHost) {
        dayCounts.set(day, (dayCounts.get(day) || 0) + 1);
      }
      if (hostInfo && (after === null || time >= after) && (before === null || time < before)) {
        siteCounts.set(hostIndex, (siteCounts.get(hostIndex) || 0) + 1);
      }
    }

    // 选了 30 天以前的某一天时，从当前页结果补充常访网站与计数。
    if (!siteCounts.size && this.entries_.length) {
      const fallback = new Map();
      for (const entry of this.entries_) {
        if (!entry.host) {
          continue;
        }
        const prev = fallback.get(entry.host);
        fallback.set(entry.host, {host: entry.host, url: prev?.url || entry.url, count: (prev?.count || 0) + 1});
      }
      const sites = [...fallback.values()].sort((a, b) => b.count - a.count).slice(0, 8);
      return this.buildOverviewResult_(today, after, before, dayCounts, sites);
    }

    const topSites = [...siteCounts.entries()]
        .sort((a, b) => b[1] - a[1])
        .slice(0, 8)
        .map(([index, count]) => ({...hosts[index], count}));
    return this.buildOverviewResult_(today, after, before, dayCounts, topSites);
  }

  buildOverviewResult_(today, after, before, dayCounts, topSites) {
    const days = [];
    let maxCount = 0;
    let activeDays = 0;
    let totalIn30Days = 0;
    for (let i = OVERVIEW_DAYS - 1; i >= 0; i--) {
      const day = addDays(today, -i);
      const count = dayCounts.get(day) || 0;
      if (count > 0) {
        activeDays++;
        totalIn30Days += count;
        maxCount = Math.max(maxCount, count);
      }
      const inRange = (after === null || day >= after) && (before === null || day < before);
      days.push({day, count, inRange, selected: this.selectedDay_ === day});
    }
    return {days, dayCounts, maxCount, activeDays, totalIn30Days, topSites};
  }

  // ---- 分组、时间线时段聚合与可见顺序 ----

  groupedDays_() {
    const days = [];
    for (const entry of this.entries_) {
      const day = startOfDay(entry.time);
      if (!days.length || days[days.length - 1].day !== day) {
        days.push({day, entries: []});
      }
      days[days.length - 1].entries.push(entry);
    }
    return days;
  }

  buildSessions_(entries, day) {
    // 先把紧挨着刷新同一网址的记录合并，保留真实浏览轨迹但去掉重复噪音。
    const merged = [];
    for (const entry of entries) {
      const prev = merged[merged.length - 1];
      if (prev && prev.url === entry.url && Math.abs(prev.earliestTime - entry.time) <= MERGE_SAME_URL_MS) {
        prev.rawEntries.push(entry);
        prev.earliestTime = entry.time;
        prev.repeatCount++;
      } else {
        merged.push({
          ...entry,
          key: timelineEntryKey(entry),
          rawEntries: [entry],
          earliestTime: entry.time,
          repeatCount: 1,
        });
      }
    }

    const sessions = [];
    for (const item of merged) {
      const current = sessions[sessions.length - 1];
      if (!current || current.oldestTime - item.time > SESSION_GAP_MS) {
        sessions.push({
          key: `session-${day}-${item.time}`,
          newestTime: item.time,
          oldestTime: item.earliestTime,
          period: periodLabel(item.time),
          items: [item],
        });
      } else {
        current.items.push(item);
        current.oldestTime = item.earliestTime;
      }
    }

    for (const session of sessions) {
      session.visitCount = session.items.reduce((sum, item) => sum + item.repeatCount, 0);
      const seenHosts = new Set();
      const seenUrls = new Set();
      session.favicons = [];
      session.uniqueUrls = [];
      for (const item of session.items) {
        if (!seenUrls.has(item.url)) {
          seenUrls.add(item.url);
          session.uniqueUrls.push(item.url);
        }
        const hostKey = item.host || item.url;
        if (!seenHosts.has(hostKey) && session.favicons.length < 4) {
          seenHosts.add(hostKey);
          session.favicons.push(item.url);
        }
      }
    }
    return sessions;
  }

  visibleOrder_() {
    const items = [];
    const days = this.groupedDays_();
    for (const day of days) {
      if (this.collapsedDays_.has(day.day)) {
        continue;
      }
      if (this.viewMode_ === 'timeline') {
        for (const session of this.buildSessions_(day.entries, day.day)) {
          if (this.collapsedSessions_.has(session.key)) {
            continue;
          }
          for (const item of session.items) {
            items.push({key: item.key, url: item.url, rawEntries: item.rawEntries, exact: true, day: day.day});
          }
        }
      } else {
        for (const entry of day.entries) {
          items.push({key: listEntryKey(entry), url: entry.url, rawEntries: [entry], exact: false, day: day.day});
        }
      }
    }
    return items;
  }

  // ---- 折叠、单条与批量操作 ----

  async toggleDayCollapse_(day) {
    const container = this.querySelector('.history-content');
    const before = container ? captureRows(container) : null;
    const next = new Set(this.collapsedDays_);
    if (next.has(day)) {
      next.delete(day);
    } else {
      next.add(day);
    }
    this.collapsedDays_ = next;
    await this.updateComplete;
    if (before) {
      animateRows(container, before, Motion.fold);
    }
  }

  async toggleSessionCollapse_(sessionKey) {
    const container = this.querySelector('.history-content');
    const before = container ? captureRows(container) : null;
    const next = new Set(this.collapsedSessions_);
    if (next.has(sessionKey)) {
      next.delete(sessionKey);
    } else {
      next.add(sessionKey);
    }
    this.collapsedSessions_ = next;
    await this.updateComplete;
    if (before) {
      animateRows(container, before, Motion.fold);
    }
  }

  async removeItem_(rawEntries, exact, rowElement) {
    const container = this.querySelector('.history-content');
    await fadeOutRow(rowElement);
    const before = container ? captureRows(container) : null;
    const removedSet = new Set(rawEntries);
    this.entries_ = this.entries_.filter((entry) => !removedSet.has(entry));
    this.total_ = Math.max(0, this.total_ - 1);
    await this.updateComplete;
    if (before) {
      animateRows(container, before);
    }
    if (rawEntries.length === 1) {
      chrome.send('removeEntry', [rawEntries[0].url, rawEntries[0].time, exact]);
    } else {
      chrome.send('removeEntries', [rawEntries.map((entry) => ({url: entry.url, time: entry.time, exact}))]);
    }
  }

  async toggleStar_(entry, button) {
    const starred = await sendWithPromise('toggleBookmark', entry.url, entry.title);
    if (button && !reduceMotion()) {
      button.animate(
          [{transform: 'scale(1)'}, {transform: 'scale(1.32)'}, {transform: 'scale(1)'}],
          {duration: Motion.lift.duration, easing: Motion.lift.easing});
    }
    if (this.starredOnly_ && !starred) {
      const row = button?.closest('[data-key]');
      const container = this.querySelector('.history-content');
      await fadeOutRow(row);
      const before = container ? captureRows(container) : null;
      this.entries_ = this.entries_.filter((other) => other.url !== entry.url);
      this.total_ = this.entries_.length;
      await this.updateComplete;
      if (before) {
        animateRows(container, before);
      }
      return;
    }
    this.entries_ = this.entries_.map(
        (other) => other.url === entry.url ? {...other, starred} : other);
  }

  copyLinkWithFeedback_(url, key) {
    copyText(url);
    this.copiedKey_ = key;
    clearTimeout(this.copyTimer_);
    this.copyTimer_ = setTimeout(() => {
      this.copiedKey_ = '';
    }, 1400);
  }

  async confirmRemoveSite_(host) {
    if (!host) {
      return;
    }
    const ok = await confirm({
      icon: 'trash.fill',
      tone: 'danger',
      title: L('history.clear.confirm.site', {host}),
      message: L('history.clear.message'),
      confirmTitle: L('common.remove'),
    });
    if (ok) {
      if (this.hostFilter_ === host) {
        this.hostFilter_ = '';
      }
      chrome.send('removeSite', [host]);
    }
  }

  async confirmClearDay_(day) {
    const ok = await confirm({
      icon: 'trash.fill',
      tone: 'danger',
      title: L('history.clear.confirm.day', {day: dayLabel(day)}),
      message: L('history.clear.message'),
      confirmTitle: L('common.clear'),
    });
    if (ok) {
      if (this.selectedDay_ === day) {
        this.selectedDay_ = null;
      }
      chrome.send('clearHistory', ['range', day, addDays(day, 1)]);
    }
  }

  async openUrlsInTabs_(urls) {
    if (!urls.length) {
      return;
    }
    if (urls.length > 8) {
      const ok = await confirm({
        icon: 'plus.square.on.square',
        tone: 'accent',
        title: L('history.timeline.openSession.confirm', {count: urls.length}),
        confirmTitle: L('common.open'),
      });
      if (!ok) {
        return;
      }
    }
    for (const url of urls) {
      openUrl(url, 'background');
    }
  }

  // ---- 多选与批量栏 ----

  toggleSelectRow_(event, key) {
    event.preventDefault();
    const order = this.visibleOrder_();
    const next = new Set(this.selectedKeys_);
    const selecting = !next.has(key);

    if (event.shiftKey && this.lastSelectedKey_) {
      const start = order.findIndex((item) => item.key === this.lastSelectedKey_);
      const end = order.findIndex((item) => item.key === key);
      if (start >= 0 && end >= 0) {
        const [from, to] = start < end ? [start, end] : [end, start];
        for (let i = from; i <= to; i++) {
          if (selecting) {
            next.add(order[i].key);
          } else {
            next.delete(order[i].key);
          }
        }
        this.selectedKeys_ = next;
        this.lastSelectedKey_ = key;
        return;
      }
    }

    if (selecting) {
      next.add(key);
    } else {
      next.delete(key);
    }
    this.selectedKeys_ = next;
    this.lastSelectedKey_ = key;
  }

  toggleSelectDay_(day) {
    const dayItems = this.visibleOrder_().filter((item) => item.day === day);
    if (!dayItems.length) {
      return;
    }
    const allSelected = dayItems.every((item) => this.selectedKeys_.has(item.key));
    const next = new Set(this.selectedKeys_);
    for (const item of dayItems) {
      if (allSelected) {
        next.delete(item.key);
      } else {
        next.add(item.key);
      }
    }
    this.selectedKeys_ = next;
  }

  selectedItems_() {
    return this.visibleOrder_().filter((item) => this.selectedKeys_.has(item.key));
  }

  async batchOpen_() {
    const urls = [...new Set(this.selectedItems_().map((item) => item.url))];
    await this.openUrlsInTabs_(urls);
    this.toggleSelecting_(false);
  }

  batchCopy_() {
    const urls = [...new Set(this.selectedItems_().map((item) => item.url))];
    if (!urls.length) {
      return;
    }
    this.copyLinkWithFeedback_(urls.join('\n'), '__batch__');
  }

  async batchDelete_() {
    const selected = this.selectedItems_();
    if (!selected.length) {
      return;
    }
    const container = this.querySelector('.history-content');
    const rowElements = selected
        .map((item) => container?.querySelector(`[data-select-key="${CSS.escape(item.key)}"]`))
        .filter(Boolean);
    await Promise.all(rowElements.slice(0, 24).map((el) => fadeOutRow(el)));

    const before = container ? captureRows(container) : null;
    const removedSet = new Set(selected.flatMap((item) => item.rawEntries));
    const payload = selected.flatMap((item) =>
        item.rawEntries.map((entry) => ({url: entry.url, time: entry.time, exact: item.exact})));

    this.entries_ = this.entries_.filter((entry) => !removedSet.has(entry));
    this.total_ = Math.max(0, this.total_ - selected.length);
    this.selectedKeys_ = new Set();
    this.lastSelectedKey_ = '';
    await this.updateComplete;
    if (before) {
      animateRows(container, before);
    }
    chrome.send('removeEntries', [payload]);
  }

  // ---- 菜单与日历浮层 ----

  async showRowMenu_(event, entry, rawEntries, exact, rowContainer) {
    event.preventDefault();
    event.stopPropagation();
    const items = [
      {id: 'open', title: L('common.open'), icon: 'arrow.up.forward'},
      {id: 'openTab', title: L('library.menu.openInNewTab'), icon: 'plus.square.on.square'},
      {id: 'openWindow', title: L('library.menu.openInNewWindow'), icon: 'macwindow'},
      {separator: true},
      {id: 'copy', title: L('library.menu.copyLink'), icon: 'link'},
      entry.starred ? {id: 'bookmark', title: L('history.row.removeBookmark'), icon: 'star.slash'} :
                      {id: 'bookmark', title: L('history.row.addBookmark'), icon: 'star'},
    ];
    if (entry.host && this.hostFilter_ !== entry.host) {
      items.push({id: 'filterSite', title: L('history.row.filterSite', {host: entry.host}), icon: 'magnifyingglass'});
    }
    items.push({separator: true});
    items.push({id: 'remove', title: L('history.row.remove'), icon: 'minus.circle'});
    if (entry.host) {
      items.push({id: 'removeSite', title: L('history.row.removeSite', {host: entry.host}), icon: 'trash'});
    }

    const anchor = event.type === 'contextmenu' ? {event} : {element: event.currentTarget};
    const id = await showMenu(items, anchor);
    switch (id) {
      case 'open':
        openUrl(entry.url, 'current');
        break;
      case 'openTab':
        openUrl(entry.url, 'foreground');
        break;
      case 'openWindow':
        openUrl(entry.url, 'window');
        break;
      case 'copy':
        copyText(entry.url);
        break;
      case 'bookmark':
        this.toggleStar_(entry, null);
        break;
      case 'filterSite':
        this.toggleHostFilter_(entry.host);
        break;
      case 'remove':
        this.removeItem_(rawEntries, exact, rowContainer);
        break;
      case 'removeSite':
        this.confirmRemoveSite_(entry.host);
        break;
    }
  }

  async showClearMenu_(event) {
    const button = event.currentTarget;
    const id = await showMenu(
        CLEAR_RANGES.map((range) => ({id: range.id, title: range.title()})),
        {element: button});
    const range = CLEAR_RANGES.find((candidate) => candidate.id === id);
    if (!range) {
      return;
    }
    const confirmed = await confirm({
      icon: 'trash.fill',
      tone: 'danger',
      title: range.confirm(),
      message: L('history.clear.message'),
      confirmTitle: L('common.clear'),
    });
    if (confirmed) {
      chrome.send('clearHistory', [range.id]);
    }
  }

  toggleCalendar_(event) {
    if (this.calendar_) {
      this.calendar_ = null;
      return;
    }
    const rect = event.currentTarget.getBoundingClientRect();
    const width = 266;
    const height = 280;
    const below = rect.bottom + 8 + height <= innerHeight || rect.top - 8 - height < 0;
    const top = (below ? rect.bottom + 8 : rect.top - 8 - height) + scrollY;
    const left = Math.max(12, Math.min(rect.right - width, innerWidth - width - 12)) + scrollX;
    const baseDate = new Date(this.selectedDay_ ?? Date.now());
    this.calendar_ = {top, left, below, year: baseDate.getFullYear(), month: baseDate.getMonth()};
    this.updateComplete.then(() => {
      const popover = this.querySelector('.calendar-popover');
      if (popover && !reduceMotion()) {
        popover.animate(
            [{opacity: 0, transform: `translateY(${below ? -5 : 5}px) scale(0.96)`},
             {opacity: 1, transform: 'none'}],
            {duration: Motion.popover.duration, easing: Motion.popover.easing});
      }
    });
  }

  shiftCalendarMonth_(delta) {
    if (!this.calendar_) {
      return;
    }
    const date = new Date(this.calendar_.year, this.calendar_.month + delta, 1);
    const now = new Date();
    if (date.getFullYear() > now.getFullYear() ||
        (date.getFullYear() === now.getFullYear() && date.getMonth() > now.getMonth())) {
      return;
    }
    this.calendar_ = {...this.calendar_, year: date.getFullYear(), month: date.getMonth()};
  }

  // ---- 渲染各区域 ----

  highlightText_(text) {
    if (!this.query_.trim() || !text) {
      return text;
    }
    const terms = this.query_.trim().split(/\s+/).filter(Boolean).map(escapeRegExp);
    if (!terms.length) {
      return text;
    }
    const pattern = new RegExp(`(${terms.join('|')})`, 'gi');
    const parts = text.split(pattern);
    if (parts.length === 1) {
      return text;
    }
    return parts.map((part, index) =>
        index % 2 === 1 ? html`<mark class="search-hit">${part}</mark>` : part);
  }

  onRowClick_(event, entry, key) {
    if (event.target.closest('button')) {
      return;
    }
    if (this.selecting_) {
      this.toggleSelectRow_(event, key);
      return;
    }
    event.preventDefault();
    openUrl(entry.url, targetFromEvent(event));
  }

  renderRowContent_(entry, {key, rawEntries, exact, badgeCount = 0, showTime = true, rowSelector = '.history-row'}) {
    const title = entry.title || displayUrl(entry.url);
    const selected = this.selecting_ && this.selectedKeys_.has(key);
    const copied = this.copiedKey_ === key;
    const classes = ['row', 'history-row'];
    if (this.selecting_) {
      classes.push('selecting-row');
    }
    if (selected) {
      classes.push('selected');
    }
    return html`
      <a class=${classes.join(' ')} href=${entry.url}
          data-key=${showTime ? key : nothing}
          data-select-key=${showTime ? key : nothing}
          title=${displayUrl(entry.url)}
          @click=${(event) => this.onRowClick_(event, entry, key)}
          @auxclick=${(event) => !this.selecting_ && event.button === 1 && this.onRowClick_(event, entry, key)}
          @contextmenu=${(event) => !this.selecting_ &&
              this.showRowMenu_(event, entry, rawEntries, exact, event.currentTarget.closest(rowSelector))}>
        ${this.selecting_ ? html`
          <span class="select-check" aria-hidden="true">${icon('checkmark', 10)}</span>` : nothing}
        ${showTime ? html`<span class="time">${timeLabel(entry.time)}</span>` : nothing}
        <img class="favicon" src=${faviconUrl(entry.url)} alt="">
        <span class="entry-main">
          <span class="entry-title ellipsis">${this.highlightText_(title)}</span>
          ${entry.host ? html`<span class="host ellipsis">${this.highlightText_(entry.host)}</span>` : nothing}
        </span>
        <span class="row-trailing">
          <span class="row-indicators">
            ${badgeCount > 1 ? html`
              <span class="visit-badge" title=${L('history.row.visits', {count: badgeCount})}>×${badgeCount}</span>` : nothing}
            ${entry.starred ? html`
              <span class="starred" title=${L('history.row.bookmarked.help')}>${icon('star.fill', 11)}</span>` : nothing}
          </span>
          <span class="row-actions">
            ${entry.host && this.hostFilter_ !== entry.host ? html`
              <button class="icon-button" title=${L('history.row.filterSite.help')}
                  @click=${(event) => {
                    event.preventDefault();
                    event.stopPropagation();
                    this.toggleHostFilter_(entry.host);
                  }}>
                ${icon('globe', 12)}
              </button>` : nothing}
            <button class="icon-button ${entry.starred ? 'starred-btn' : ''}"
                title=${entry.starred ? L('history.row.removeBookmark') : L('history.row.addBookmark')}
                @click=${(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  this.toggleStar_(entry, event.currentTarget);
                }}>
              ${icon(entry.starred ? 'star.fill' : 'star', 12)}
            </button>
            <button class="icon-button ${copied ? 'copied-btn' : ''}"
                title=${copied ? L('history.row.copied') : L('library.menu.copyLink')}
                @click=${(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  this.copyLinkWithFeedback_(entry.url, key);
                }}>
              ${icon(copied ? 'checkmark' : 'link', 11)}
            </button>
            <button class="icon-button" title=${L('history.row.remove.help')}
                @click=${(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  this.removeItem_(rawEntries, exact, event.currentTarget.closest(rowSelector));
                }}>
              ${icon('xmark', 10)}
            </button>
            <button class="icon-button" title=${L('common.moreActions')}
                @click=${(event) => this.showRowMenu_(event, entry, rawEntries, exact,
                                                      event.currentTarget.closest(rowSelector))}>
              ${icon('ellipsis', 12)}
            </button>
          </span>
        </span>
      </a>`;
  }

  renderDayHeader_(day, count) {
    const collapsed = this.collapsedDays_.has(day);
    const dayItems = this.selecting_ ? this.visibleOrder_().filter((item) => item.day === day) : [];
    const allDaySelected = dayItems.length > 0 && dayItems.every((item) => this.selectedKeys_.has(item.key));
    return html`
      <div class="day-header">
        <button class="day-toggle" title=${collapsed ? L('history.section.expand') : L('history.section.collapse')}
            @click=${() => this.toggleDayCollapse_(day)}>
          <span class="day-chevron">${icon('chevron.down', 11)}</span>
          <span class="day-title">${dayLabel(day)}</span>
          <span class="day-count">${count}</span>
        </button>
        <span class="spacer"></span>
        ${this.selecting_ && !collapsed ? html`
          <button class="ghost-pill" @click=${() => this.toggleSelectDay_(day)}>
            ${icon('checkmark.circle', 11)}
            <span>${allDaySelected ? L('history.select.dayNone') : L('history.select.dayAll')}</span>
          </button>` : html`
          <button class="ghost-pill day-hover-action" @click=${() => this.confirmClearDay_(day)}>
            ${icon('trash', 11)}<span>${L('history.day.clear')}</span>
          </button>`}
      </div>`;
  }

  renderListView_(days) {
    return html`
      <div class="history-list ${this.selecting_ ? 'selecting' : ''}">
        ${repeat(days, (day) => day.day, (day) => {
          const collapsed = this.collapsedDays_.has(day.day);
          return html`
            <section class="day-section ${collapsed ? 'collapsed' : ''}" data-key=${'day-' + day.day}>
              ${this.renderDayHeader_(day.day, day.entries.length)}
              ${collapsed ? nothing : html`
                <div class="history-card">
                  ${repeat(day.entries, listEntryKey, (entry) =>
                      this.renderRowContent_(entry, {
                        key: listEntryKey(entry),
                        rawEntries: [entry],
                        exact: false,
                        badgeCount: entry.visitCount || 0,
                        showTime: true,
                      }))}
                </div>`}
            </section>`;
        })}
      </div>`;
  }

  renderSession_(session) {
    const collapsed = this.collapsedSessions_.has(session.key);
    const newestLabel = timeLabel(session.newestTime);
    const oldestLabel = timeLabel(session.oldestTime);
    const span = newestLabel === oldestLabel ? newestLabel : `${oldestLabel} – ${newestLabel}`;
    return html`
      <div class="timeline-session ${collapsed ? 'collapsed' : ''}" data-key=${session.key}>
        <div class="session-header">
          <button class="session-toggle"
              title=${collapsed ? L('history.section.expand') : L('history.section.collapse')}
              @click=${() => this.toggleSessionCollapse_(session.key)}>
            <span class="session-node" aria-hidden="true"></span>
            <span class="session-period">${session.period}</span>
            <span class="session-span">${span}</span>
            <span class="session-count">${L('history.timeline.sessionCount', {count: session.visitCount})}</span>
            <span class="session-favicons">
              ${session.favicons.map((url) => html`<img src=${faviconUrl(url)} alt="">`)}
            </span>
            <span class="session-chevron">${icon('chevron.down', 10)}</span>
          </button>
          <span class="spacer"></span>
          <button class="ghost-pill session-open-btn"
              @click=${() => this.openUrlsInTabs_(session.uniqueUrls)}>
            ${icon('plus.square.on.square', 11)}<span>${L('history.timeline.openSession')}</span>
          </button>
        </div>
        ${collapsed ? nothing : html`
          <div class="session-items">
            ${repeat(session.items, (item) => item.key, (item) => {
              const selected = this.selecting_ && this.selectedKeys_.has(item.key);
              return html`
                <div class="timeline-item ${selected ? 'selected' : ''}"
                    data-key=${item.key} data-select-key=${item.key}>
                  <span class="timeline-time">${timeLabel(item.time)}</span>
                  <span class="timeline-axis" aria-hidden="true"><span class="timeline-dot"></span></span>
                  ${this.renderRowContent_(item, {
                    key: item.key,
                    rawEntries: item.rawEntries,
                    exact: true,
                    badgeCount: item.repeatCount,
                    showTime: false,
                    rowSelector: '.timeline-item',
                  })}
                </div>`;
            })}
          </div>`}
      </div>`;
  }

  renderTimelineView_(days) {
    const activeDay = this.activeDay_ ?? (days[0]?.day ?? null);
    return html`
      <div class="timeline-layout ${this.selecting_ ? 'selecting' : ''}">
        ${days.length > 0 ? html`
          <aside class="timeline-rail">
            <div class="timeline-rail-heading">${L('history.timeline.index')}</div>
            <div class="timeline-rail-list">
              <div class="timeline-rail-highlight"></div>
              ${days.map((day) => html`
                <button class="timeline-rail-item ${day.day === activeDay ? 'active' : ''}"
                    @click=${() => this.jumpToDay_(day.day)}>
                  <span class="ellipsis">${shortDayLabel(day.day)}</span>
                  <span class="timeline-rail-count">${day.entries.length}</span>
                </button>`)}
            </div>
            ${!this.finished_ ? html`
              <button class="timeline-load-more-btn" @click=${() => this.loadMore_()}>
                ${icon('arrow.down', 11)}<span>${L('history.timeline.loadEarlier')}</span>
              </button>` : nothing}
          </aside>` : nothing}
        <div class="timeline-stream">
          ${repeat(days, (day) => day.day, (day) => {
            const collapsed = this.collapsedDays_.has(day.day);
            const sessions = collapsed ? [] : this.buildSessions_(day.entries, day.day);
            return html`
              <section class="day-section ${collapsed ? 'collapsed' : ''}"
                  data-key=${'tday-' + day.day} data-day=${day.day}>
                ${this.renderDayHeader_(day.day, day.entries.length)}
                ${collapsed ? nothing : html`
                  <div class="timeline-sessions">
                    ${repeat(sessions, (session) => session.key, (session) => this.renderSession_(session))}
                  </div>`}
              </section>`;
          })}
        </div>
      </div>`;
  }

  renderOverview_(stats) {
    // 输入搜索词时收起概览卡片，让搜索结果紧贴搜索框显示；没有任何浏览历史时也不显示。
    if (!this.loaded_ || this.query_.trim() || (!stats.totalIn30Days && !this.entries_.length && !this.hasFilters_)) {
      return nothing;
    }
    const inspectDay = this.hoveredDay_ ?? this.selectedDay_;
    const inspectInfo = inspectDay !== null ? stats.days.find((d) => d.day === inspectDay) : null;
    let labelText = L('history.overview.title');
    if (inspectDay !== null) {
      const count = inspectInfo ? inspectInfo.count : (stats.dayCounts.get(inspectDay) || 0);
      labelText = count > 0 ? L('history.overview.dayCount', {day: dayLabel(inspectDay), count}) :
                              L('history.overview.dayEmpty', {day: dayLabel(inspectDay)});
    }

    return html`
      <div class="history-overview">
        <div class="overview-top">
          <span class="overview-label ${inspectDay !== null ? 'highlighted' : ''}">${labelText}</span>
          <span class="overview-summary">
            ${L('history.overview.summary', {days: stats.activeDays, count: stats.totalIn30Days})}
          </span>
        </div>
        <div class="activity-bars" @mouseleave=${() => { this.hoveredDay_ = null; }}>
          ${stats.days.map((item) => {
            const ratio = stats.maxCount > 0 && item.count > 0 ? Math.sqrt(item.count / stats.maxCount) : 0;
            const barPx = item.count > 0 ? Math.round(9 + ratio * 31) : 3;
            const tip = item.count > 0 ? L('history.overview.dayCount', {day: dayLabel(item.day), count: item.count}) :
                                         L('history.overview.dayEmpty', {day: dayLabel(item.day)});
            const classes = ['activity-day'];
            if (item.count > 0) {
              classes.push('has-visits');
            }
            if (item.inRange) {
              classes.push('in-range');
            }
            if (item.selected) {
              classes.push('selected');
            }
            return html`
              <button class=${classes.join(' ')} title=${tip} aria-label=${tip}
                  @mouseenter=${() => { this.hoveredDay_ = item.day; }}
                  @click=${() => this.selectDay_(item.day)}>
                <span class="activity-bar-fill" style="--bar-h: ${barPx}px"></span>
              </button>`;
          })}
        </div>
        <div class="overview-axis">
          <span>${shortDayLabel(stats.days[0].day)}</span>
          <span>${shortDayLabel(stats.days[15].day)}</span>
          <span>${L('library.day.today')}</span>
        </div>
        ${stats.topSites.length > 0 ? html`
          <div class="overview-sites" @mouseleave=${() => { this.hoveredHost_ = ''; }}>
            <span class="sites-label">${L('history.overview.topSites')}</span>
            <div class="site-chips">
              ${stats.topSites.map((site) => html`
                <button class="site-chip ${this.hostFilter_ === site.host ? 'active' : ''}"
                    @mouseenter=${() => { this.hoveredHost_ = site.host; }}
                    @click=${() => this.toggleHostFilter_(site.host)}>
                  <img src=${faviconUrl(site.url)} alt="">
                  <span class="ellipsis">${site.host}</span>
                  <span class="site-chip-count">${site.count}</span>
                </button>`)}
            </div>
          </div>` : nothing}
      </div>`;
  }

  renderActiveFilters_(stats) {
    const chips = [];
    if (this.selectedDay_ !== null) {
      chips.push(html`
        <button class="filter-chip" @click=${() => { this.selectedDay_ = null; this.reload_({transition: true}); }}>
          ${icon('calendar', 12)}
          <span>${dayLabel(this.selectedDay_)}</span>
          <span class="chip-close">${icon('xmark', 9)}</span>
        </button>`);
    }
    if (this.hostFilter_) {
      const sampleUrl = stats.topSites.find((s) => s.host === this.hostFilter_)?.url ||
          this.entries_[0]?.url || `https://${this.hostFilter_}`;
      chips.push(html`
        <button class="filter-chip" @click=${() => this.toggleHostFilter_(this.hostFilter_)}>
          <img class="chip-favicon" src=${faviconUrl(sampleUrl)} alt="">
          <span>${this.hostFilter_}</span>
          <span class="chip-close">${icon('xmark', 9)}</span>
        </button>`);
    }
    if (this.starredOnly_) {
      chips.push(html`
        <button class="filter-chip" @click=${() => this.toggleStarredOnly_()}>
          ${icon('star.fill', 11)}
          <span>${L('history.filter.bookmarked')}</span>
          <span class="chip-close">${icon('xmark', 9)}</span>
        </button>`);
    }
    if (!chips.length) {
      return nothing;
    }
    const activeCount = chips.length + (this.timeFilter_ !== 'all' && this.selectedDay_ === null ? 1 : 0);
    return html`
      <div class="active-filters">
        ${chips}
        ${this.hostFilter_ ? html`
          <button class="filter-link-btn danger" @click=${() => this.confirmRemoveSite_(this.hostFilter_)}>
            ${icon('trash', 11)}<span>${L('history.row.removeSite', {host: this.hostFilter_})}</span>
          </button>` : nothing}
        ${activeCount >= 2 ? html`
          <button class="filter-link-btn" @click=${() => this.resetFilters_()}>
            ${L('history.filter.reset')}
          </button>` : nothing}
      </div>`;
  }

  renderCalendar_(dayCounts) {
    const cal = this.calendar_;
    if (!cal) {
      return nothing;
    }
    const today = startOfDay(Date.now());
    const nowDate = new Date();
    const isCurrentMonth = cal.year === nowDate.getFullYear() && cal.month === nowDate.getMonth();
    const firstWeekday = new Date(cal.year, cal.month, 1).getDay();
    const daysInMonth = new Date(cal.year, cal.month + 1, 0).getDate();
    const cells = [];
    for (let i = 0; i < firstWeekday; i++) {
      cells.push(html`<span></span>`);
    }
    for (let d = 1; d <= daysInMonth; d++) {
      const dayMs = new Date(cal.year, cal.month, d).getTime();
      const future = dayMs > today;
      const isToday = dayMs === today;
      const selected = this.selectedDay_ === dayMs;
      const hasVisits = (dayCounts.get(dayMs) || 0) > 0;
      const classes = ['calendar-cell'];
      if (isToday) {
        classes.push('today');
      }
      if (selected) {
        classes.push('selected');
      }
      cells.push(html`
        <button class=${classes.join(' ')} ?disabled=${future}
            @click=${() => this.selectDay_(dayMs)}>
          <span>${d}</span>
          ${hasVisits ? html`<span class="calendar-dot"></span>` : nothing}
        </button>`);
    }

    return html`
      <div class="popover-scrim" @mousedown=${() => { this.calendar_ = null; }}></div>
      <div class="calendar-popover" style="top: ${cal.top}px; left: ${cal.left}px">
        <div class="calendar-header">
          <button class="icon-button" title=${L('history.calendar.prevMonth')}
              @click=${() => this.shiftCalendarMonth_(-1)}>
            ${icon('chevron.left', 11)}
          </button>
          <span class="calendar-month">${monthYearLabel(cal.year, cal.month)}</span>
          <button class="calendar-today-btn" @click=${() => this.selectDay_(today)}>
            ${L('library.day.today')}
          </button>
          <button class="icon-button" title=${L('history.calendar.nextMonth')} ?disabled=${isCurrentMonth}
              @click=${() => this.shiftCalendarMonth_(1)}>
            ${icon('chevron.right', 11)}
          </button>
        </div>
        <div class="calendar-weekdays">
          ${weekdayLabels().map((label) => html`<span class="calendar-weekday">${label}</span>`)}
        </div>
        <div class="calendar-grid">${cells}</div>
      </div>`;
  }

  renderBatchBar_() {
    if (!this.selecting_) {
      return nothing;
    }
    const count = this.selectedKeys_.size;
    const copied = this.copiedKey_ === '__batch__';
    return html`
      <div class="batch-bar">
        <span class="batch-count">${L('history.select.count', {count})}</span>
        <span class="batch-divider"></span>
        <button class="batch-btn" ?disabled=${count === 0} @click=${() => this.batchOpen_()}>
          ${icon('plus.square.on.square', 12)}<span>${L('history.select.openTabs')}</span>
        </button>
        <button class="batch-btn" ?disabled=${count === 0} @click=${() => this.batchCopy_()}>
          ${icon(copied ? 'checkmark' : 'link', 12)}
          <span>${copied ? L('history.row.copied') : L('history.select.copyLinks')}</span>
        </button>
        <button class="batch-btn danger" ?disabled=${count === 0} @click=${() => this.batchDelete_()}>
          ${icon('trash', 12)}<span>${L('history.select.delete')}</span>
        </button>
        <button class="batch-btn ghost" @click=${() => this.toggleSelecting_(false)}>
          ${L('common.done')}
        </button>
      </div>`;
  }

  renderEmpty_() {
    if (this.query_) {
      return emptyState({
        iconName: 'magnifyingglass',
        title: L('library.search.noResults', {query: this.query_}),
      });
    }
    if (this.starredOnly_) {
      return emptyState({
        iconName: 'star',
        title: L('history.filter.emptyBookmarked'),
      });
    }
    if (this.selectedDay_ !== null || this.timeFilter_ !== 'all' || this.hostFilter_) {
      return emptyState({
        iconName: 'clock',
        title: L('history.filter.empty'),
      });
    }
    return emptyState({
      iconName: 'clock',
      title: L('history.empty.title'),
      detail: this.incognito_ ? L('history.empty.private') : '',
    });
  }

  render() {
    const stats = this.computeOverviewStats_();
    const days = this.groupedDays_();
    const canInteract = !this.loaded_ || this.entries_.length > 0 || this.hasFilters_ || Boolean(this.query_);

    return html`
      <div class="page history-page ${this.selecting_ ? 'has-batch-bar' : ''}">
        <header class="library-header history-header">
          <div class="eyebrow">${L('library.header')}</div>
          <div class="title-row">
            <h1 class="title">${L('common.history')}</h1>
            <span class="spacer"></span>
            <div class="header-actions">
              <div class="segmented-picker compact view-picker">
                <div class="segmented-highlight"></div>
                <button class="segment-btn ${this.viewMode_ === 'list' ? 'selected' : ''}"
                    @click=${() => this.setViewMode_('list')}>
                  ${icon('list.bullet', 12)}<span>${L('history.view.list')}</span>
                </button>
                <button class="segment-btn ${this.viewMode_ === 'timeline' ? 'selected' : ''}"
                    @click=${() => this.setViewMode_('timeline')}>
                  ${icon('clock', 12)}<span>${L('history.view.timeline')}</span>
                </button>
              </div>
              <button class="pill-button" title=${L('history.archive.help')}
                  @click=${(event) => openUrl('starry://archive/', targetFromEvent(event))}>
                ${icon('archivebox', 12)}<span>${L('history.archive.button')}</span>
              </button>
              <button class="pill-button ${this.selecting_ ? 'active' : ''}"
                  ?disabled=${this.loaded_ && !this.entries_.length && !this.selecting_}
                  @click=${() => this.toggleSelecting_()}>
                ${icon('checkmark.circle', 12)}
                <span>${this.selecting_ ? L('common.done') : L('history.select.button')}</span>
              </button>
              ${pillButton({
                iconName: 'trash',
                title: L('history.clear.button'),
                disabled: this.loaded_ && !canInteract,
                onClick: (event) => this.showClearMenu_(event),
              })}
            </div>
          </div>
          <div class="subtitle">${this.subtitle_ || '\u00a0'}</div>
          <div class="toolbar">
            ${searchField({
              value: this.query_,
              placeholder: L('history.search.placeholder'),
              onInput: (value) => this.onQuery_(value),
            })}
            <div class="spacer"></div>
            <div class="toolbar-controls">
              <div class="segmented-picker filter-picker">
                <div class="segmented-highlight"></div>
                ${TIME_FILTERS.map((filter) => html`
                  <button class="segment-btn ${this.selectedDay_ === null && this.timeFilter_ === filter.id ? 'selected' : ''}"
                      @click=${() => this.setTimeFilter_(filter.id)}>
                    <span>${filter.title()}</span>
                  </button>`)}
              </div>
              <button class="round-button ${this.selectedDay_ !== null || this.calendar_ ? 'active' : ''}"
                  title=${L('history.filter.date.help')}
                  @click=${(event) => this.toggleCalendar_(event)}>
                ${icon('calendar', 14)}
              </button>
              <button class="round-button ${this.starredOnly_ ? 'active' : ''}"
                  title=${L('history.filter.bookmarked.help')}
                  @click=${() => this.toggleStarredOnly_()}>
                ${icon(this.starredOnly_ ? 'star.fill' : 'star', 14)}
              </button>
            </div>
          </div>
          ${this.renderActiveFilters_(stats)}
        </header>

        ${this.renderOverview_(stats)}
        ${this.loaded_ && !this.entries_.length ? this.renderEmpty_() : nothing}
        <div class="history-content">
          ${this.entries_.length ?
              (this.viewMode_ === 'timeline' ? this.renderTimelineView_(days) : this.renderListView_(days)) :
              nothing}
        </div>
        ${this.loaded_ && !this.finished_ ? html`<div class="load-more"></div>` : nothing}
      </div>
      ${this.renderCalendar_(stats.dayCounts)}
      ${this.renderBatchBar_()}`;
  }
}

customElements.define('starry-history', StarryHistory);

document.documentElement.style.setProperty('--ease-page-filter', 'cubic-bezier(0.16, 1, 0.3, 1)');
document.documentElement.style.setProperty('--duration-page-filter', '360ms');
