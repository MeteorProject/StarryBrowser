// Copyright 2026 The Starry Browser Authors
//
// 下载页（starry://downloads）：汇总进行中与按天分组的下载任务（包含浏览器内置下载与
// Starry 多线程分段下载，数据源见 starry/browser/ui/webui/downloads_handler.h）。
//
// 有下载进行中时标题区下面是总速度的卡片（最近一分钟的曲线、剩余时间、连接数、峰值）；
// 进行中的一行文件名后面是「N 线程」「不能续传」的小标签，进度条按段画出每一段下到了哪里
// （段的起点是一道细缝，新切出来的一段从零长出来，下载中一道光扫过）；点一行就地展开网址、
// 来源、保存位置、下载方式和更高的分段进度；下载好的文件可以拖到访达、桌面或别的 App。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, openUrl, sendWithPromise, showDialog, showMenu} from './common/browser.js';
import {bytes, dateTimeLabel, dayLabel, duration, speed, startOfDay, timeLabel} from './common/format.js';
import {L, showInFolderLabel} from './common/l10n.js';
import {emptyState, icon, revealHeader, searchField} from './common/library.js';
import {animateRows, captureRows, Motion, reduceMotion, reveal} from './common/motion.js';

const FILTERS = [
  {id: 'all', title: L('common.all'), empty: L('downloads.empty.title'), emptyIcon: 'arrow.down.circle'},
  {id: 'active', title: L('downloads.filter.active'), empty: L('downloads.filter.empty.active'), emptyIcon: 'arrow.down.circle'},
  {id: 'finished', title: L('downloads.filter.finished'), empty: L('downloads.filter.empty.finished'), emptyIcon: 'checkmark.circle'},
  {id: 'failed', title: L('downloads.filter.failed'), empty: L('downloads.filter.empty.failed'), emptyIcon: 'exclamationmark.circle'},
];

const SEPARATOR = ' · ';
// 速度曲线采样容量：每秒一次，保留最近 60 秒。
const SPEED_SAMPLES = 60;

function isActive(item) {
  return item.state === 'running' || item.state === 'paused' || item.state === 'queued';
}

// 在跑的（下载中、排队）：「全部暂停」管的。
function isPausable(item) {
  return item.state === 'running' || item.state === 'queued';
}

// Starry 自己分段下载的：按段画进度。
function isSegmented(item) {
  return item.starry && item.total > 0 && item.segments.length > 1;
}

function isFailed(item) {
  return item.state === 'interrupted' || item.state === 'cancelled';
}

function matchesFilter(item, filter) {
  switch (filter) {
    case 'active':
      return isActive(item);
    case 'finished':
      return item.state === 'complete';
    case 'failed':
      return isFailed(item);
    default:
      return true;
  }
}

function amount(item) {
  return item.total > 0 ? `${bytes(item.received)} / ${bytes(item.total)}` : bytes(item.received);
}

// 状态文案：优先使用浏览器下发的状态（涵盖排队、重试、连接中），缺失时本地推导。
function statusText(item) {
  if (item.status) {
    return item.status;
  }
  if (item.dangerous) {
    return L('downloads.status.dangerous');
  }
  switch (item.state) {
    case 'running': {
      if (item.received === 0 && item.speed === 0) {
        return L('downloads.status.connecting');
      }
      const parts = [amount(item)];
      if (item.speed > 0) {
        parts.push(speed(item.speed));
        if (item.total > 0) {
          parts.push(L('downloads.status.remaining', {time: duration((item.total - item.received) / item.speed)}));
        }
      }
      return parts.join(SEPARATOR);
    }
    case 'paused':
      if (item.canResume) {
        return item.received > 0 ? L('downloads.status.pausedAmount', {amount: amount(item)}) : L('downloads.status.paused');
      }
      return item.received > 0 ? L('downloads.status.pausedAmountRestart', {amount: amount(item)}) :
                                 L('downloads.status.pausedRestart');
    case 'complete':
      if (!item.fileExists) {
        return L('downloads.status.fileMissing');
      }
      return [bytes(item.total > 0 ? item.total : item.received), item.host].filter(Boolean).join(SEPARATOR);
    case 'cancelled':
      return L('downloads.status.cancelled');
    default:
      return item.error || L('downloads.error.failed');
  }
}

function fileIconUrl(item, size) {
  const params = new URLSearchParams({
    path: item.path || item.filename,
    scale: `${Math.max(1, Math.round(devicePixelRatio))}x`,
    iconSize: size > 32 ? 'large' : 'normal',
  });
  return `chrome://fileicon/?${params}`;
}

class StarryDownloads extends LitElement {
  static properties = {
    items_: {state: true},
    query_: {state: true},
    filter_: {state: true},
    loaded_: {state: true},
    expanded_: {state: true},
    speedHistory_: {state: true},
  };

  constructor() {
    super();
    this.items_ = [];
    this.query_ = '';
    this.filter_ = 'all';
    this.loaded_ = false;
    this.incognito_ = false;
    // 展开了详情的那一行（进行中的）。
    this.expanded_ = null;
    this.speedHistory_ = [];
    this.sampleTimer_ = 0;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('downloads-changed', (items) => this.setItems_(items));
    const reply = await sendWithPromise('getDownloads');
    this.incognito_ = reply.incognito;
    this.items_ = reply.downloads;
    this.loaded_ = true;
    this.updateSampling_();
    await this.updateComplete;
    reveal(this.querySelector('.download-sections'), {delay: 0.16, distance: 8});
    reveal(this.querySelector('.empty-state'), {delay: 0.18});
  }

  firstUpdated() {
    revealHeader(this);
    this.placeFilterHighlight_(false);
  }

  updated() {
    this.placeFilterHighlight_(true);
    this.drawSpeedChart_();
    this.syncSheens_();
  }

  // 将新出现的扫光元素对齐到同一时钟，使后续切分的分段与已有分段同步扫光；仅初始化一次避免重绘跳动。
  syncSheens_() {
    for (const sheen of this.querySelectorAll('.sheen:not([data-synced])')) {
      sheen.style.animationDelay = `-${Math.round(performance.now() % 1600)}ms`;
      sheen.dataset.synced = '1';
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    clearInterval(this.sampleTimer_);
  }

  // ---- 总速度 ----

  // 有下载在跑时每秒记一次总速度；都停了就不记，曲线清空。
  updateSampling_() {
    const running = this.items_.some((item) => item.state === 'running');
    if (running && !this.sampleTimer_) {
      this.sample_();
      this.sampleTimer_ = setInterval(() => this.sample_(), 1000);
    } else if (!running && this.sampleTimer_) {
      clearInterval(this.sampleTimer_);
      this.sampleTimer_ = 0;
      this.speedHistory_ = [];
    }
  }

  sample_() {
    const total = this.items_.filter((item) => item.state === 'running')
                      .reduce((sum, item) => sum + item.speed, 0);
    this.speedHistory_ = [...this.speedHistory_, total].slice(-SPEED_SAMPLES);
  }

  // 绘制速度曲线：纵轴上限为峰值的 1.15 倍（下限 256 KB/s）。
  drawSpeedChart_() {
    const canvas = this.querySelector('.speed-chart');
    if (!canvas) {
      return;
    }
    const ratio = devicePixelRatio || 1;
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) {
      canvas.width = Math.round(width * ratio);
      canvas.height = Math.round(height * ratio);
    }
    const context = canvas.getContext('2d');
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, width, height);
    const style = getComputedStyle(this);
    const accent = style.getPropertyValue('--accent').trim();
    const line = style.getPropertyValue('--on-surface').trim();
    const ring = style.getPropertyValue('--surface-alt').trim();
    const dark = style.getPropertyValue('--is-dark').trim() === '1';
    const edge = 6;
    const plot = {x: 0, y: edge, width: Math.max(width - edge, 1), height: Math.max(height - edge - 1, 1)};
    const bottom = plot.y + plot.height;
    context.globalAlpha = dark ? 0.1 : 0.08;
    context.strokeStyle = line;
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(0, bottom + 0.5);
    context.lineTo(width, bottom + 0.5);
    context.stroke();
    context.globalAlpha = 1;
    const values = this.speedHistory_;
    if (values.length < 2) {
      return;
    }
    const upper = Math.max(Math.max(...values), 256 * 1024) * 1.15;
    const step = plot.width / (SPEED_SAMPLES - 1);
    const points = values.map((value, index) => [
      plot.width - (values.length - 1 - index) * step,
      bottom - Math.min(Math.max(value / upper, 0), 1) * plot.height,
    ]);
    const gradient = context.createLinearGradient(0, plot.y, 0, bottom);
    gradient.addColorStop(0, `color-mix(in srgb, ${accent} 22%, transparent)`);
    gradient.addColorStop(1, `color-mix(in srgb, ${accent} 2%, transparent)`);
    context.beginPath();
    points.forEach(([x, y], index) => index ? context.lineTo(x, y) : context.moveTo(x, y));
    context.lineTo(points[points.length - 1][0], bottom);
    context.lineTo(points[0][0], bottom);
    context.closePath();
    context.fillStyle = gradient;
    context.fill();
    context.beginPath();
    points.forEach(([x, y], index) => index ? context.lineTo(x, y) : context.moveTo(x, y));
    context.strokeStyle = accent;
    context.lineWidth = 2;
    context.lineCap = 'round';
    context.lineJoin = 'round';
    context.stroke();
    const [lastX, lastY] = points[points.length - 1];
    context.fillStyle = ring;
    context.beginPath();
    context.arc(lastX, lastY, 6, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = accent;
    context.beginPath();
    context.arc(lastX, lastY, 4, 0, Math.PI * 2);
    context.fill();
  }

  renderDashboard_() {
    const running = this.items_.filter((item) => item.state === 'running');
    if (!running.length) {
      return nothing;
    }
    const total = running.reduce((sum, item) => sum + item.speed, 0);
    const remaining = running.reduce((sum, item) => sum + Math.max(0, item.total - item.received), 0);
    const eta = total > 0 && running.every((item) => item.total > 0) ? remaining / total : null;
    const peak = this.speedHistory_.length ? Math.max(...this.speedHistory_) : null;
    // 非分段下载按单个连接计入。
    const connections = running.reduce((sum, item) => sum + (item.starry ? item.connections : 1), 0);
    const stat = (title, value) => html`<span class="stat"><span>${title}</span><b>${value}</b></span>`;
    return html`
      <div class="download-dashboard" data-key="dashboard">
        <div class="dashboard-numbers">
          <div class="dashboard-label">${L('downloads.dashboard.speed')}</div>
          <div class="dashboard-speed">${speed(total)}</div>
          <div class="dashboard-stats">
            ${stat(L('downloads.dashboard.remaining'), eta === null ? '—' : duration(eta))}
            ${stat(L('downloads.dashboard.connections'), String(connections))}
            ${stat(L('downloads.dashboard.peak'), peak ? speed(peak) : '—')}
          </div>
        </div>
        <canvas class="speed-chart"></canvas>
      </div>`;
  }

  async setItems_(items) {
    const container = this.querySelector('.download-sections');
    const before = container ? captureRows(container) : null;
    this.items_ = items;
    if (this.expanded_ && !items.some((item) => item.id === this.expanded_ && isActive(item))) {
      this.expanded_ = null;
    }
    this.updateSampling_();
    await this.updateComplete;
    if (before) {
      animateRows(this.querySelector('.download-sections'), before);
    }
  }

  // 点进行中的一行：就地展开、收起详情（点在按钮上不算）。
  async toggleDetails_(event, item) {
    if (event.target.closest('button')) {
      return;
    }
    const opening = this.expanded_ !== item.id;
    const container = this.querySelector('.download-sections');
    const before = captureRows(container);
    this.expanded_ = opening ? item.id : null;
    await this.updateComplete;
    animateRows(container, before, Motion.fold);
    const details = this.querySelector(`.download-row[data-key="${item.id}"] .download-details`);
    if (opening && details && !reduceMotion()) {
      details.animate([{opacity: 0, transform: 'translateY(-8px)'}, {opacity: 1, transform: 'none'}],
                      {duration: Motion.fold.duration, easing: Motion.fold.easing});
    }
  }

  renderDetails_(item) {
    const row = (title, value) => html`<span class="detail-title">${title}</span><span class="detail-value">${value}</span>`;
    const started = new Date(item.startTime);
    return html`
      <div class="download-details">
        <div class="detail-grid">
          ${row(L('downloads.details.url'), item.url)}
          ${item.referrer ? row(L('downloads.details.referrer'), item.referrer) : nothing}
          ${item.path ? row(L('downloads.details.destination'), item.path) : nothing}
          ${item.method ? row(L('downloads.details.method'), item.method) : nothing}
          ${row(L('downloads.details.started'), dateTimeLabel(item.startTime))}
        </div>
        ${isSegmented(item) ? html`
          <div class="detail-segments">
            <div class="detail-title">${L('downloads.details.segments')}</div>
            ${this.renderProgress_(item, 'tall')}
          </div>` : nothing}
        <div class="detail-buttons">
          <button class="detail-pill" @click=${() => copyText(item.url)}>${icon('link', 11)}<span>${L('downloads.copyLink')}</span></button>
          ${item.path ? html`
            <button class="detail-pill" @click=${() => this.act_('showInFolder', item)}>
              ${icon('folder', 11)}<span>${showInFolderLabel()}</span>
            </button>` : nothing}
          ${item.referrer ? html`
            <button class="detail-pill" @click=${() => openUrl(item.referrer, 'foreground')}>
              ${icon('safari', 11)}<span>${L('downloads.openReferrer')}</span>
            </button>` : nothing}
        </div>
      </div>`;
  }

  get shown_() {
    const terms = this.query_.toLowerCase().split(/\s+/).filter(Boolean);
    return this.items_.filter((item) => matchesFilter(item, this.filter_) && terms.every((term) =>
        `${item.filename} ${item.url} ${item.referrer}`.toLowerCase().includes(term)));
  }

  get subtitle_() {
    if (!this.loaded_) {
      return '';
    }
    const running = this.items_.filter((item) => item.state === 'running').length;
    const queued = this.items_.filter((item) => item.state === 'queued').length;
    const paused = this.items_.filter((item) => item.state === 'paused').length;
    if (running + queued + paused > 0) {
      const parts = [];
      if (running) {
        parts.push(L('downloads.page.subtitle.running', {count: running}));
      }
      if (queued) {
        parts.push(L('downloads.page.subtitle.queued', {count: queued}));
      }
      if (paused) {
        parts.push(L('downloads.page.subtitle.paused', {count: paused}));
      }
      return parts.join(SEPARATOR);
    }
    if (!this.items_.length) {
      return this.incognito_ ? L('downloads.page.subtitle.private') : L('downloads.empty.title');
    }
    const size = this.items_.filter((item) => item.state === 'complete')
                     .reduce((sum, item) => sum + Math.max(item.total, 0), 0);
    return L('downloads.page.subtitle.total', {count: this.items_.length, size: bytes(size)});
  }

  // ---- 操作 ----

  act_(action, item) {
    chrome.send(action, [item.id]);
  }

  togglePause_(item) {
    this.act_(item.state === 'paused' ? 'resume' : 'pause', item);
  }

  async newDownload_() {
    const result = await showDialog({
      icon: 'arrow.down.circle',
      tone: 'accent',
      title: L('downloads.new.title'),
      message: L('downloads.new.prompt'),
      field: '',
      buttons: [{title: L('common.cancel'), variant: 'tertiary', cancel: true}, {title: L('downloads.new.download'), variant: 'filled'}],
    });
    const url = result && result.button === 1 ? result.input.trim() : '';
    if (url) {
      chrome.send('newDownload', [/^[a-z]+:\/\//i.test(url) ? url : `https://${url}`]);
    }
  }

  async showMoreMenu_(event) {
    const pausable = this.items_.some(isPausable);
    const resumable = this.items_.some((item) => item.state === 'paused');
    const id = await showMenu([
      {id: 'pauseAll', title: L('downloads.pauseAll'), icon: 'pause', enabled: pausable},
      {id: 'resumeAll', title: L('downloads.resumeAll'), icon: 'play', enabled: resumable},
      {separator: true},
      {id: 'clearInactive', title: L('downloads.clearInactive'), icon: 'checkmark.circle',
       enabled: this.items_.some((item) => !isActive(item))},
      {id: 'clearAll', title: L('downloads.clearAll'), icon: 'trash', enabled: this.items_.length > 0},
      {separator: true},
      {id: 'settings', title: L('downloads.settingsMenu'), icon: 'gearshape'},
    ], {element: event.currentTarget});
    switch (id) {
      case 'pauseAll':
      case 'resumeAll':
        this.setAllPaused_(id === 'pauseAll');
        break;
      case 'clearInactive':
        chrome.send('clearInactive');
        break;
      case 'clearAll':
        if (await confirm({icon: 'trash.fill', title: L('downloads.clearAllConfirm.title'),
                           message: L('downloads.clearAllConfirm.message'), confirmTitle: L('common.clear')})) {
          chrome.send('clearAll');
        }
        break;
      case 'settings':
        openUrl('starry://settings/downloads', 'foreground');
        break;
    }
  }

  setAllPaused_(pause) {
    // 继续时先开始的先排上（列表是新的在前）。
    for (const item of pause ? this.items_ : [...this.items_].reverse()) {
      if (pause && isPausable(item)) {
        this.act_('pause', item);
      } else if (!pause && item.state === 'paused') {
        this.act_('resume', item);
      }
    }
  }

  async showItemMenu_(event, item) {
    event.preventDefault();
    event.stopPropagation();
    const items = [];
    if (item.state === 'complete') {
      items.push({id: 'open', title: L('common.open'), icon: 'arrow.up.forward', enabled: item.fileExists});
      items.push({id: 'showInFolder', title: showInFolderLabel(), icon: 'folder', enabled: item.fileExists});
    }
    if (item.starry && isActive(item) && item.path) {
      // 进行中的分段下载允许在文件管理器中定位临时文件。
      items.push({id: 'showInFolder', title: showInFolderLabel(), icon: 'folder'});
    }
    switch (item.state) {
      case 'queued':
      case 'running':
        items.push({id: 'pause', title: L('common.pause'), icon: 'pause'});
        items.push({id: 'cancel', title: L('downloads.cancel'), icon: 'xmark'});
        break;
      case 'paused':
        items.push({id: 'resume', title: L('downloads.resume'), icon: 'play'});
        items.push({id: 'cancel', title: L('downloads.cancel'), icon: 'xmark'});
        break;
      case 'interrupted':
        items.push({id: 'retry', title: item.canResume ? L('downloads.resume') : L('common.retry'),
                    icon: 'arrow.clockwise'});
        break;
      default:
        items.push({id: 'retry', title: L('downloads.redownload'), icon: 'arrow.down.circle'});
        break;
    }
    items.push({separator: true});
    items.push({id: 'copy', title: L('downloads.copyDownloadLink'), icon: 'link'});
    if (item.referrer) {
      items.push({id: 'referrer', title: L('downloads.openReferrer'), icon: 'globe'});
    }
    items.push({separator: true});
    items.push({id: 'remove', title: L('downloads.removeFromList'), icon: 'minus.circle'});
    const id = await showMenu(items, event.type === 'contextmenu' ? {event} : {element: event.currentTarget});
    switch (id) {
      case null:
      case undefined:
        break;
      case 'copy':
        copyText(item.url);
        break;
      case 'referrer':
        openUrl(item.referrer, 'foreground');
        break;
      default:
        this.act_(id, item);
        break;
    }
  }

  // ---- 筛选 ----

  placeFilterHighlight_(animated) {
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

  renderFilters_() {
    const counts = Object.fromEntries(FILTERS.map(
        (filter) => [filter.id, this.items_.filter((item) => matchesFilter(item, filter.id)).length]));
    return html`
      <div class="filter-picker">
        <div class="filter-highlight"></div>
        ${FILTERS.map((filter) => html`
          <button class="filter ${filter.id === this.filter_ ? 'selected' : ''}"
              @click=${() => { this.filter_ = filter.id; }}>
            <span>${filter.title}</span>
            ${filter.id !== 'all' && counts[filter.id] > 0 ?
                html`<span class="filter-count ${filter.id === 'failed' ? 'danger' : ''}">${counts[filter.id]}</span>` :
                nothing}
          </button>`)}
      </div>`;
  }

  // ---- 行 ----

  renderIcon_(item, size) {
    const badge = item.dangerous ? 'exclamationmark' :
        item.state === 'paused' ? 'pause.fill' :
        item.state === 'queued' ? 'clock.fill' :
        item.state === 'interrupted' ? 'exclamationmark' : null;
    const dimmed = item.state === 'cancelled' || (item.state === 'complete' && !item.fileExists);
    return html`
      <span class="file-icon ${dimmed ? 'dimmed' : ''}" style="--size: ${size}px">
        <img src=${fileIconUrl(item, size)} alt="">
        ${badge ? html`<span class="badge ${badge === 'exclamationmark' ? 'danger' : ''}">${icon(badge, 9)}</span>` :
                  nothing}
      </span>`;
  }

  // 进度条：分段下载按段绘制进度与 1px 接缝，新分段通过 @starting-style 从零展开；
  // 下载中每 1.6 秒扫过一道高光；未知大小时播放循环不确定动画。`size` 为 'tall' 时用于详情区。
  renderProgress_(item, size = '') {
    const indeterminate = item.state === 'running' && item.total <= 0;
    const classes = `progress ${item.state} ${indeterminate ? 'indeterminate' : ''} ${size}`;
    if (!indeterminate && isSegmented(item)) {
      const total = item.total;
      const segments = [...item.segments].sort((a, b) => a[0] - b[0]);
      return html`
        <div class="${classes} segmented">
          ${repeat(segments, (segment) => segment[0], ([start, end, received]) => {
            const left = start / total;
            const fill = Math.min(received, Math.max(0, end - start)) / total;
            // 光的轨道和整条一样宽，按这一段的位置挪回去。
            const track = fill > 0 ? `left: ${-left / fill * 100}%; width: ${100 / fill}%` : '';
            return html`
              <div class="segment-fill" style="left: ${left * 100}%; --fill: ${fill * 100}%">
                ${item.state === 'running' && fill > 0 ? html`
                  <div class="sheen-track" style=${track}><div class="sheen"></div></div>` : nothing}
              </div>`;
          })}
          ${repeat(segments.slice(1), (segment) => segment[0], ([start]) => html`
            <div class="segment-seam" style="left: ${start / total * 100}%"></div>`)}
        </div>`;
    }
    const progress = item.total > 0 ? Math.min(1, item.received / item.total) : 0;
    return html`
      <div class=${classes}>
        <div class="progress-fill" style="width: ${indeterminate ? 30 : progress * 100}%">
          ${item.state === 'running' && !indeterminate && progress > 0 ? html`
            <div class="sheen-track" style="width: ${100 / progress}%"><div class="sheen"></div></div>` :
            nothing}
        </div>
      </div>`;
  }

  // 文件名后的状态标签（「N 线程」「不能续传」）。
  renderTags_(item) {
    const threads = item.starry && item.rangeSupport === 'supported' && item.maxConnections > 1;
    const notResumable = item.state !== 'queued' &&
        (item.rangeSupport === 'unsupported' || (!item.starry && !item.resumable));
    return html`
      ${threads ? html`<span class="soft-tag accent">${L('downloads.tag.threads', {count: item.maxConnections})}</span>` :
                  nothing}
      ${notResumable ? html`
        <span class="soft-tag warning" title=${L('downloads.tag.notResumableHelp')}>${L('downloads.tag.notResumable')}</span>` :
        nothing}`;
  }

  renderActiveRow_(item) {
    const paused = item.state === 'paused';
    const expanded = this.expanded_ === item.id;
    const pauseTitle = paused ? L('downloads.resume') :
        item.resumable || item.state === 'queued' ? L('common.pause') : L('downloads.pause.notResumable');
    return html`
      <div class="download-row active-row ${expanded ? 'expanded' : ''}" data-key=${item.id}
          @contextmenu=${(event) => this.showItemMenu_(event, item)}>
        <div class="active-summary" title=${expanded ? L('downloads.details.hide') : L('downloads.details.show')}
            @click=${(event) => this.toggleDetails_(event, item)}>
        ${this.renderIcon_(item, 36)}
        <div class="row-text">
          <div class="filename-row">
            <div class="filename ellipsis">${item.filename}</div>
            ${this.renderTags_(item)}
          </div>
          ${this.renderProgress_(item)}
          <div class="status ellipsis ${item.dangerous ? 'warning' : ''}">${statusText(item)}</div>
        </div>
        <div class="row-actions">
          ${item.dangerous ? html`
            <button class="icon-button large" title=${L('downloads.keep')} @click=${() => this.act_('keep', item)}>
              ${icon('checkmark', 11)}
            </button>
            <button class="icon-button large" title=${L('downloads.discard')} @click=${() => this.act_('discard', item)}>
              ${icon('xmark', 10)}
            </button>` : html`
            <button class="icon-button large" title=${pauseTitle}
                @click=${() => this.togglePause_(item)}>
              ${icon(paused ? 'play.fill' : 'pause.fill', 11)}
            </button>
            <button class="icon-button large" title=${L('downloads.cancel')} @click=${() => this.act_('cancel', item)}>
              ${icon('xmark', 10)}
            </button>`}
        </div>
        <span class="row-chevron">${icon('chevron.right', 10)}</span>
        </div>
        ${expanded ? this.renderDetails_(item) : nothing}
      </div>`;
  }

  renderFinishedRow_(item) {
    const failed = isFailed(item);
    return html`
      <div class="download-row finished-row" data-key=${item.id} title=${item.url}
          draggable=${item.state === 'complete' && item.fileExists ? 'true' : 'false'}
          @dragstart=${(event) => {
            // 交由浏览器发起原生文件拖拽，而非网页默认的 URL 拖拽。
            event.preventDefault();
            this.act_('drag', item);
          }}
          @dblclick=${() => item.state === 'complete' && item.fileExists && this.act_('open', item)}
          @contextmenu=${(event) => this.showItemMenu_(event, item)}>
        ${this.renderIcon_(item, 30)}
        <div class="row-text">
          <div class="filename ellipsis ${item.state === 'complete' && !item.fileExists ? 'missing' : ''}
              ${item.state === 'cancelled' ? 'dim' : ''}">${item.filename}</div>
          <div class="status ellipsis ${item.state === 'interrupted' ? 'danger' : ''}">${statusText(item)}</div>
        </div>
        <div class="row-trailing">
          <span class="time">${timeLabel(item.endTime || item.startTime)}</span>
          <div class="hover-actions">
            ${item.state === 'complete' ? html`
              <button class="icon-button medium" title=${showInFolderLabel()} ?disabled=${!item.fileExists}
                  @click=${() => this.act_('showInFolder', item)}>
                ${icon('magnifyingglass', 11)}
              </button>` : html`
              <button class="icon-button medium" title=${failed && item.canResume ? L('downloads.resumeDownload') : L('downloads.redownload')}
                  @click=${() => this.act_('retry', item)}>
                ${icon('arrow.clockwise', 11)}
              </button>`}
            <button class="icon-button medium" title=${L('common.moreActions')} @click=${(event) => this.showItemMenu_(event, item)}>
              ${icon('ellipsis', 12)}
            </button>
          </div>
        </div>
      </div>`;
  }

  renderSection_(key, title, items, renderRow, trailing = nothing) {
    return html`
      <section class="download-section" data-key=${key}>
        <div class="download-section-header">
          <span class="section-title">${title}</span>
          <span class="section-count">${items.length}</span>
          <span class="spacer"></span>
          ${trailing}
        </div>
        <div class="download-card">
          ${repeat(items, (item) => item.id, renderRow)}
        </div>
      </section>`;
  }

  renderSections_() {
    const shown = this.shown_;
    const active = shown.filter(isActive);
    const days = [];
    for (const item of shown.filter((item) => !isActive(item))) {
      const day = startOfDay(item.endTime || item.startTime);
      let group = days.find((candidate) => candidate.day === day);
      if (!group) {
        group = {day, items: []};
        days.push(group);
      }
      group.items.push(item);
    }
    days.sort((a, b) => b.day - a.day);
    const anyRunning = active.some(isPausable);
    const pauseAll = html`
      <button class="ghost-pill" @click=${() => this.setAllPaused_(anyRunning)}>
        ${icon(anyRunning ? 'pause.fill' : 'play.fill', 10)}<span>${anyRunning ? L('downloads.pauseAll') : L('downloads.resumeAll')}</span>
      </button>`;
    return html`
      ${active.length ? this.renderSection_('active', L('downloads.page.section.active'), active, (item) => this.renderActiveRow_(item), pauseAll) :
                        nothing}
      ${repeat(days, (day) => day.day, (day) => this.renderSection_(
          `day-${day.day}`, dayLabel(day.day), day.items, (item) => this.renderFinishedRow_(item)))}`;
  }

  renderEmpty_() {
    if (!this.items_.length) {
      return emptyState({
        iconName: 'arrow.down.circle',
        title: L('downloads.empty.title'),
        detail: this.incognito_ ? L('downloads.empty.privateDetail') :
                                  L('downloads.empty.detail'),
      });
    }
    if (this.query_) {
      return emptyState({iconName: 'magnifyingglass', title: L('downloads.search.noResults', {query: this.query_})});
    }
    const filter = FILTERS.find((candidate) => candidate.id === this.filter_);
    return emptyState({iconName: filter.emptyIcon, title: filter.empty});
  }

  render() {
    return html`
      <div class="page">
        <header class="library-header downloads-header">
          <div class="eyebrow">${L('downloads.page.eyebrow')}</div>
          <div class="title-row">
            <h1 class="title">${L('common.downloads')}</h1>
            <span class="spacer"></span>
            <button class="filled-pill" title=${L('downloads.new.help')} @click=${() => this.newDownload_()}>
              ${icon('plus', 12)}<span>${L('downloads.new.title')}</span>
            </button>
          </div>
          <div class="subtitle">${this.subtitle_ || ' '}</div>
          <div class="toolbar">
            ${searchField({value: this.query_, placeholder: L('downloads.search.placeholder'),
                           onInput: (value) => { this.query_ = value; }})}
            <span class="spacer"></span>
            ${this.renderFilters_()}
            <button class="round-button" title=${L('downloads.openFolder')} @click=${() => chrome.send('openFolder')}>
              ${icon('folder', 13)}
            </button>
            <button class="round-button" title=${L('common.moreActions')} @click=${(event) => this.showMoreMenu_(event)}>
              ${icon('ellipsis', 13)}
            </button>
          </div>
        </header>
        ${this.renderDashboard_()}
        ${this.loaded_ && !this.shown_.length ? this.renderEmpty_() : nothing}
        <div class="download-sections">${this.renderSections_()}</div>
      </div>`;
  }
}

customElements.define('starry-downloads', StarryDownloads);

// 别处要用这条曲线（筛选的高亮）。
document.documentElement.style.setProperty('--ease-page-filter', 'cubic-bezier(0.16, 1, 0.3, 1)');
document.documentElement.style.setProperty('--duration-page-filter', '360ms');
document.documentElement.style.setProperty('--ease-progress', Motion.progress.easing);
