// Copyright 2026 The Starry Browser Authors
//
// AI 助手界面（对话、问题卡片、输入框与聊天记录）。页面铺满浮岛玻璃或右侧侧栏，背景透明。
// 悬浮模式下内容从输入框向上生长，页面通过 resize 通知浏览器调整玻璃高度；默认仅展示当前一轮，
// 向上滚动或点击「上文」后展开历史，新消息发送后恢复贴合本轮高度；侧栏模式始终展示完整对话。
//
// 同一窗口支持多对话并行，切换对话时浏览器下发 reset，未发送的文本、图片与 @ 标签按对话保留。
// 数据与动作由浏览器侧 starry/browser/ui/assistant/assistant_controller.h 管理。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, faviconUrl, send, showMenu} from './common/browser.js';
import {L} from './common/l10n.js';
import {icon} from './common/library.js';
import {Motion, reduceMotion} from './common/motion.js';
import {renderMarkdown} from './markdown.js';

// 确认卡片刚弹出来 0.4 秒内按回车不算（免得正好在打字时误按）。
const APPROVAL_GUARD = 400;
// 输入框最多 8 行，再多在框里滚动。
const COMPOSER_MAX_LINES = 8;
// 标题行的高度（侧栏时 44）。
const HEADER = 42;
const DOCK_HEADER = 44;
// 很长的话默认折叠：超过 12 行时显示开头 8 行。
const FOLD_LINES = 12;
// 离底部这么近时跟着新内容往下走。
const FOLLOW = 40;
// 拷贝之后打勾这么久。
const COPIED = 1400;
// 聊天记录开着时要多高：浏览器按最高的给。
const TALLEST = 100000;

// 图片预处理：仅透传不超过 4 MB 且最长边 <= 2048 的 PNG/JPEG/GIF/WebP，其余缩放转码（单次最多 8 张）。
const IMAGE_TYPES = new Set(['image/png', 'image/jpeg', 'image/gif', 'image/webp']);
const MAX_SIDE = 2048;
const PASS_THROUGH_BYTES = 4 * 1024 * 1024;
const MAX_IMAGES = 8;

// @ 的标签：一次最多 6 个；@ 后面打的字最多 40 个还算在找标签。
const MAX_MENTIONS = 6;
const MENTION_QUERY_MAX = 40;
// 这些字后面的 @ 不算（a@b.com）。
const MENTION_WORD = /[A-Za-z0-9_.+-]$/;
const FULLWIDTH_AT = String.fromCharCode(0xff20);

const REASONING_LEVELS = new Set(['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max', 'on']);

const EASE_OUT = 'cubic-bezier(0, 0, 0.58, 1)';

// 一直重复的动画（assistant.css 里转圈、思考的点、思考时的图标）按每秒 30 格走。浏览器告诉我们
// 它的循环（岛的外圈、网页的描边、标签上的环，LayerLoop）的格子落在这一页时间线的哪里
// （loopGrid），这里把开始时间挪到同一套格子上：两边在同一帧变，岛是实时的玻璃，错开
// 的话窗口每秒要多画一倍的帧。
const LOOP_STEP = 1000 / 30;
const LOOP_ANIMATIONS = new Set(['spin', 'pulse', 'thinking']);
let loopPhase = null;

function alignLoop(animation) {
  if (loopPhase === null || !LOOP_ANIMATIONS.has(animation.animationName) ||
      typeof animation.startTime !== 'number') {
    return;
  }
  const start = animation.startTime;
  const aligned = loopPhase + Math.floor((start - loopPhase) / LOOP_STEP) * LOOP_STEP;
  if (Math.abs(aligned - start) > 0.001) {
    animation.startTime = aligned;
  }
}

function alignLoops(phase) {
  loopPhase = phase;
  for (const animation of document.getAnimations()) {
    alignLoop(animation);
  }
}

// ---- 动效 ----

// 出现：淡入 0.14 秒，同时绕 origin 从 scale 放大、从下面 rise 处升上来（气泡的弹簧）。
function playIn(element, {scale = 0.9, rise = 10, origin = 'center bottom', delay = 0} = {}) {
  if (!element || reduceMotion()) {
    return;
  }
  element.style.transformOrigin = origin;
  element.animate([{opacity: 0}, {opacity: 1}], {duration: 140, easing: EASE_OUT, delay, fill: 'backwards'});
  element.animate([{transform: `translateY(${rise}px) scale(${scale})`}, {transform: 'none'}],
                  {...Motion.bubbleIn, delay, fill: 'backwards'});
}

// 新的一行：淡入 0.22 秒，从下面 8 处升上来。
function riseIn(element) {
  if (!element || reduceMotion()) {
    return;
  }
  element.animate([{opacity: 0}, {opacity: 1}], {duration: 220, easing: EASE_OUT});
  element.animate([{transform: 'translateY(8px)'}, {transform: 'none'}], Motion.item);
}

// 切换对话时淡入内容与输入框。
function fadeIn(element) {
  if (element && !reduceMotion()) {
    element.animate([{opacity: 0}, {opacity: 1}], {duration: 180, easing: EASE_OUT});
  }
}

// 消失：在原来的位置放一份拷贝，缩小、下沉、淡出，放完拿掉。下面的东西当时就按没有它排。
function playOut(snapshot, layer) {
  if (!snapshot || !layer || reduceMotion()) {
    return;
  }
  const host = layer.getBoundingClientRect();
  const {node, rect, origin} = snapshot;
  Object.assign(node.style, {
    position: 'absolute',
    left: `${rect.left - host.left}px`,
    top: `${rect.top - host.top}px`,
    width: `${rect.width}px`,
    height: `${rect.height}px`,
    margin: '0',
    transformOrigin: origin,
  });
  layer.appendChild(node);
  const animation = node.animate([{opacity: 1, transform: 'none'},
                                  {opacity: 0, transform: 'translateY(6px) scale(0.94)'}],
                                 {duration: 160, easing: EASE_OUT, fill: 'forwards'});
  animation.onfinish = () => node.remove();
}

function snapshotOf(element, origin) {
  return element ? {node: element.cloneNode(true), rect: element.getBoundingClientRect(), origin} : null;
}

function spinner() {
  return html`<span class="spinner"></span>`;
}

function chevron(open) {
  return html`<span class="chevron ${open ? 'open' : ''}">${icon('chevron.right', 8)}</span>`;
}

function reasoningName(level) {
  return REASONING_LEVELS.has(level) ? L(`assistant.reasoning.${level}`) : level;
}

// ---- 图片 ----

async function base64Of(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  let text = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(text);
}

// 读不懂时是 null。
async function loadImage(blob) {
  let bitmap;
  try {
    bitmap = await createImageBitmap(blob, {imageOrientation: 'from-image'});
  } catch {
    return null;
  }
  const {width, height} = bitmap;
  if (!width || !height) {
    bitmap.close();
    return null;
  }
  if (IMAGE_TYPES.has(blob.type) && Math.max(width, height) <= MAX_SIDE && blob.size <= PASS_THROUGH_BYTES) {
    bitmap.close();
    return {mime: blob.type, data: await base64Of(blob)};
  }
  const scale = Math.min(1, MAX_SIDE / Math.max(width, height));
  const canvas = new OffscreenCanvas(Math.max(1, Math.round(width * scale)), Math.max(1, Math.round(height * scale)));
  const context = canvas.getContext('2d');
  context.imageSmoothingQuality = 'high';
  context.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
  bitmap.close();
  const alpha = blob.type === 'image/png' || blob.type === 'image/webp' || blob.type === 'image/gif';
  const out = await canvas.convertToBlob(alpha ? {type: 'image/png'} : {type: 'image/jpeg', quality: 0.85});
  return {mime: out.type, data: await base64Of(out)};
}

function imageFiles(dataTransfer) {
  return [...(dataTransfer?.items || [])]
      .filter((item) => item.kind === 'file' && item.type.startsWith('image/'))
      .map((item) => item.getAsFile())
      .filter(Boolean);
}

function hasImages(dataTransfer) {
  return [...(dataTransfer?.items || [])].some((item) => item.kind === 'file' && item.type.startsWith('image/'));
}

// 标签列表里标题后面的网站。
function hostOf(url) {
  try {
    return new URL(url).host.replace(/^www\./, '');
  } catch {
    return '';
  }
}

// 回答：Markdown 画成 HTML（文字都转义过），链接交给浏览器打开，代码块悬停时可以拷贝。
class StarryMarkdown extends HTMLElement {
  set text(value) {
    if (value === this.text_) {
      return;
    }
    this.text_ = value;
    this.innerHTML = renderMarkdown(value || '');
    for (const button of this.querySelectorAll('.copy-code')) {
      button.title = L('assistant.code.copy');
      button.innerHTML = '<starry-icon class="icon" name="doc.on.doc" size="11"></starry-icon>';
    }
  }

  get text() {
    return this.text_ || '';
  }

  connectedCallback() {
    if (this.listening_) {
      return;
    }
    this.listening_ = true;
    this.addEventListener('click', (event) => {
      const link = event.target.closest('a[data-url]');
      if (link) {
        event.preventDefault();
        send('openLink', link.dataset.url);
        return;
      }
      const copy = event.target.closest('.copy-code');
      if (copy) {
        copyText(copy.dataset.code);
        // 拷好了：图标打勾 1.4 秒，工具条一直显示。
        copy.innerHTML = '<starry-icon class="icon" name="checkmark" size="11"></starry-icon>';
        copy.closest('.code')?.classList.add('copied');
        clearTimeout(copy.timer_);
        copy.timer_ = setTimeout(() => {
          copy.innerHTML = '<starry-icon class="icon" name="doc.on.doc" size="11"></starry-icon>';
          copy.closest('.code')?.classList.remove('copied');
        }, COPIED);
      }
    });
  }
}
customElements.define('starry-markdown', StarryMarkdown);

class StarryAssistant extends LitElement {
  static properties = {
    items_: {state: true},
    state_: {state: true},
    draft_: {state: true},
    attachments_: {state: true},
    expanded_: {state: true},
    tall_: {state: true},
    foldable_: {state: true},
    copied_: {state: true},
    focused_: {state: true},
    history_: {state: true},
    thumbs_: {state: true},
    preview_: {state: true},
    mentions_: {state: true},
    picker_: {state: true},
    mentionTabs_: {state: true},
  };

  constructor() {
    super();
    this.items_ = [];
    this.state_ = {};
    this.draft_ = '';
    this.attachments_ = [];
    // @ 的标签：{id, title, url}。
    this.mentions_ = [];
    // 打 @ 时的标签列表：{start, end, query, index}（@ 在哪、后面打了什么、选着第几个）。
    this.picker_ = null;
    // 浏览器给的能 @ 的标签（每次打开列表时重新要，还没来时是 null）。
    this.mentionTabs_ = null;
    // Esc 关掉了列表的那个 @：接着打字不再弹出来。
    this.dismissedMention_ = -1;
    this.expanded_ = new Set();
    this.foldable_ = new Set();
    this.tall_ = false;
    this.copied_ = '';
    this.focused_ = false;
    // 聊天记录开着时：{query, data}（data 是浏览器给的列表，还没来时是 null）。
    this.history_ = null;
    this.thumbs_ = {};
    this.preview_ = '';
    this.stick_ = true;
    this.lastHeight_ = 0;
    this.cardShownAt_ = 0;
    this.conversation_ = '';
    // 各个对话没发的字和图片。
    this.drafts_ = new Map();
    this.nextAttachment_ = 1;
    // 已经画过的行：新出现的才放出现的动画（刚载入、换对话时整个画出来，不放）。
    this.seen_ = new Set();
    this.animate_ = false;
    this.switched_ = false;
  }

  createRenderRoot() {
    return this;
  }

  connectedCallback() {
    super.connectedCallback();
    addWebUiListener('reset', ({items, state}) => {
      const switched = !!this.conversation_ && state.conversation !== this.conversation_;
      if (state.conversation !== this.conversation_) {
        // 换了对话：没发的字和图片按对话收起来、拿回来。
        if (this.conversation_) {
          this.drafts_.set(this.conversation_,
                           {draft: this.draft_, attachments: this.attachments_, mentions: this.mentions_});
        }
        const saved = this.drafts_.get(state.conversation);
        this.drafts_.delete(state.conversation);
        this.draft_ = saved?.draft || '';
        this.attachments_ = saved?.attachments || [];
        this.mentions_ = saved?.mentions || [];
        this.conversation_ = state.conversation;
      }
      this.picker_ = null;
      this.items_ = items;
      this.state_ = state;
      this.stick_ = true;
      this.tall_ = false;
      this.expanded_ = new Set();
      this.foldable_ = new Set();
      this.history_ = null;
      this.seen_ = new Set(items.map((item) => item.id));
      this.animate_ = false;
      this.switched_ = switched;
    });
    addWebUiListener('append', (item) => {
      this.animate_ = true;
      this.items_ = [...this.items_, item];
      if (item.pending) {
        this.cardShownAt_ = performance.now();
      }
      if (item.type === 'user') {
        // 新的一轮：回到只和这一轮一样高。
        this.stick_ = true;
        this.tall_ = false;
      }
    });
    addWebUiListener('patch', (patch) => {
      this.items_ = this.items_.map((item) => item.id === patch.id ? {...item, ...patch} : item);
    });
    addWebUiListener('delta', ({id, text}) => {
      this.items_ = this.items_.map((item) => item.id === id ? {...item, text: (item.text || '') + text} : item);
    });
    addWebUiListener('state', (state) => {
      this.state_ = state;
    });
    addWebUiListener('focus', () => this.focusComposer_());
    addWebUiListener('history', (data) => {
      if (this.history_ && data.query === this.history_.query) {
        this.history_ = {...this.history_, data};
      }
    });
    addWebUiListener('historyThumb', ({id, src}) => {
      this.thumbs_ = {...this.thumbs_, [id]: src};
    });
    addWebUiListener('openHistory', () => this.openHistory_());
    addWebUiListener('attach', (image) => this.addImages_([image]));
    addWebUiListener('mentionTabs', (tabs) => {
      if (this.picker_) {
        this.mentionTabs_ = tabs;
      }
    });
    this.resizeObserver_ = new ResizeObserver(() => this.reportHeight_());
    document.addEventListener('keydown', (event) => {
      if (event.key !== 'Escape' || event.defaultPrevented) {
        return;
      }
      event.preventDefault();
      // 先关图片、聊天记录，再交给浏览器（收起、关掉）。
      if (this.preview_) {
        this.preview_ = '';
      } else if (this.history_) {
        this.closeHistory_();
      } else {
        send('escape');
      }
    });
    // 拖图片进来：外面一圈渐变线；放下就附上。
    document.addEventListener('dragover', (event) => {
      if (hasImages(event.dataTransfer)) {
        event.preventDefault();
        event.dataTransfer.dropEffect = 'copy';
        this.setDragOver_(true);
      }
    });
    document.addEventListener('dragleave', (event) => {
      if (!event.relatedTarget) {
        this.setDragOver_(false);
      }
    });
    document.addEventListener('drop', (event) => {
      this.setDragOver_(false);
      const files = imageFiles(event.dataTransfer);
      if (files.length) {
        event.preventDefault();
        this.addFiles_(files);
      }
    });
    addWebUiListener('loopGrid', alignLoops);
    // 新出现的循环动画开始时对齐（带延迟的在延迟完了时，之前不动）。
    document.addEventListener('animationstart', (event) => {
      for (const animation of event.target.getAnimations()) {
        alignLoop(animation);
      }
    });
    send('ready');
  }

  // 要消失的问题卡片、「正在思考」先拍一份，画完以后在原地放消失的动画。
  willUpdate() {
    const card = this.querySelector('.card');
    this.leavingCard_ = card ? {id: card.dataset.card, snapshot: snapshotOf(card, 'center bottom')} : null;
    const waiting = this.querySelector('.entry.waiting');
    this.leavingWaiting_ = waiting ? snapshotOf(waiting, 'left center') : null;
  }

  updated() {
    for (const selector of ['.header', '.transcript-content', '.card-wrap', '.mention-picker-wrap', '.setup',
                            '.suggestions', '.bar']) {
      const element = this.querySelector(selector);
      if (element && !element.observed_) {
        element.observed_ = true;
        this.resizeObserver_.observe(element);
      }
    }
    this.resizeComposer_();
    this.measureBubbles_();
    this.revealMention_();
    this.playMotion_();
    this.reportHeight_();
    const transcript = this.querySelector('.transcript');
    if (transcript && this.stick_) {
      transcript.scrollTop = transcript.scrollHeight;
    }
  }

  playMotion_() {
    if (this.switched_) {
      this.switched_ = false;
      fadeIn(this.querySelector('.transcript'));
      fadeIn(this.querySelector('.bar'));
      fadeIn(this.querySelector('.empty'));
    }
    const layer = this.querySelector('.leaving-layer');
    const card = this.querySelector('.card');
    if (this.leavingCard_ && this.leavingCard_.id !== card?.dataset.card) {
      playOut(this.leavingCard_.snapshot, layer);
    }
    if (this.leavingWaiting_ && !this.querySelector('.entry.waiting')) {
      playOut(this.leavingWaiting_, layer);
    }
    for (const row of this.querySelectorAll('[data-row]')) {
      const id = row.dataset.row;
      if (this.seen_.has(id)) {
        continue;
      }
      this.seen_.add(id);
      if (!this.animate_) {
        continue;
      }
      if (row.classList.contains('user')) {
        playIn(row.querySelector('.bubble-stack'), {scale: 0.94, rise: 14, origin: 'right bottom'});
      } else {
        riseIn(row);
      }
    }
    if (card && !this.seen_.has(`card-${card.dataset.card}`)) {
      this.seen_.add(`card-${card.dataset.card}`);
      playIn(card, {scale: 0.97});
    }
    // 建议一条接一条地放大上来。
    const suggestions = [...this.querySelectorAll('.suggestion')];
    if (suggestions.length && !this.seen_.has('suggestions')) {
      this.seen_.add('suggestions');
      suggestions.forEach((chip, index) => playIn(chip, {delay: index * 50}));
    } else if (!suggestions.length) {
      this.seen_.delete('suggestions');
    }
    // 输入框上面新附上的图片。
    for (const thumb of this.querySelectorAll('.thumb[data-attachment]')) {
      const key = `attachment-${thumb.dataset.attachment}`;
      if (!this.seen_.has(key)) {
        this.seen_.add(key);
        playIn(thumb, {scale: 0.9, rise: 6});
      }
    }
    // 打 @ 时的标签列表同问题卡片放大上来；新 @ 的标签同图片。拿掉了的忘掉，再加时还放。
    const picker = this.querySelector('.mention-picker');
    if (picker && !this.seen_.has('picker')) {
      this.seen_.add('picker');
      playIn(picker, {scale: 0.97, rise: 6});
    } else if (!picker) {
      this.seen_.delete('picker');
    }
    const chips = new Set();
    for (const chip of this.querySelectorAll('.mention-chip[data-mention]')) {
      const key = `chip:${chip.dataset.mention}`;
      chips.add(key);
      if (!this.seen_.has(key)) {
        this.seen_.add(key);
        playIn(chip, {scale: 0.9, rise: 4});
      }
    }
    for (const key of this.seen_) {
      if (key.startsWith('chip:') && !chips.has(key)) {
        this.seen_.delete(key);
      }
    }
  }

  // 很长的话（超过 12 行）默认折叠。
  measureBubbles_() {
    let changed = false;
    for (const text of this.querySelectorAll('.bubble-text:not([data-measured])')) {
      text.dataset.measured = '1';
      const line = parseFloat(getComputedStyle(text).lineHeight) || 19;
      if (text.scrollHeight > line * FOLD_LINES + 2) {
        this.foldable_.add(text.dataset.id);
        changed = true;
      }
    }
    if (changed) {
      this.foldable_ = new Set(this.foldable_);
    }
  }

  // 内容要多高：浏览器按它定玻璃的高度。平时只算这一轮（从用户这一句到最后），变高时算整个对话；
  // 聊天记录开着时要最高。侧栏不用量（一直是整列高）。
  reportHeight_() {
    let total;
    if (this.history_) {
      total = TALLEST;
    } else {
      const height = (selector) => this.querySelector(selector)?.getBoundingClientRect().height || 0;
      const content = this.querySelector('.transcript-content');
      let transcript = 0;
      if (content) {
        const full = content.scrollHeight;
        const users = content.querySelectorAll(':scope > .entry.user');
        const last = users[users.length - 1];
        transcript = this.tall_ || !last ? full : full - last.offsetTop + 6;
      }
      total = Math.ceil((content ? HEADER : 0) + transcript + height('.card-wrap') + height('.mention-picker-wrap') +
                        height('.setup') + height('.suggestions') + height('.bar'));
    }
    if (total !== this.lastHeight_) {
      this.lastHeight_ = total;
      send('resize', total);
    }
  }

  focusComposer_() {
    this.updateComplete.then(() => this.querySelector('textarea')?.focus());
  }

  resizeComposer_() {
    const textarea = this.querySelector('textarea');
    if (!textarea) {
      return;
    }
    const line = parseFloat(getComputedStyle(textarea).lineHeight) || 19;
    if (!textarea.value) {
      textarea.style.height = `${line}px`;
      return;
    }
    textarea.style.height = 'auto';
    textarea.style.height = `${Math.min(textarea.scrollHeight, line * COMPOSER_MAX_LINES)}px`;
  }

  get pendingCard_() {
    return this.items_.find((item) => item.pending);
  }

  get userCount_() {
    return this.items_.filter((item) => item.type === 'user').length;
  }

  get docked_() {
    return !!this.state_.docked;
  }

  // ---- 动作 ----

  submit_() {
    const text = this.draft_.trim();
    const card = this.pendingCard_;
    if (card?.type === 'approval') {
      // 回车是允许（一般操作才有，刚弹出来 0.4 秒内不算）；打了字再回车是拒绝并告诉模型要改什么。
      if (text) {
        this.approve_(card, 'deny', text);
        this.draft_ = '';
      } else if (!card.sensitive && performance.now() - this.cardShownAt_ > APPROVAL_GUARD) {
        this.approve_(card, 'allow');
      }
      return;
    }
    if (card?.type === 'question') {
      if (text) {
        send('answer', card.id, text);
        this.draft_ = '';
      }
      return;
    }
    if (this.state_.running) {
      // 这一轮做完才能发：岛晃一下。
      send('refused');
      return;
    }
    if (!text && !this.attachments_.length && !this.mentions_.length) {
      return;
    }
    send('send', text, this.attachments_.map(({mime, data}) => ({mime, data})),
         this.mentions_.map((tab) => tab.id));
    this.draft_ = '';
    this.attachments_ = [];
    this.mentions_ = [];
    this.picker_ = null;
  }

  approve_(card, decision, note = '') {
    send('approve', card.id, decision, note);
  }

  // 展开、收起：这一行用 fold 弹簧变高，里面的东西被裁着；箭头跟着转。
  async toggle_(id, rowId = id) {
    const row = this.querySelector(`[data-row="${CSS.escape(rowId)}"]`);
    const before = row?.getBoundingClientRect().height;
    const expanded = new Set(this.expanded_);
    expanded.has(id) ? expanded.delete(id) : expanded.add(id);
    this.expanded_ = expanded;
    await this.updateComplete;
    if (!row || !row.isConnected || reduceMotion()) {
      return;
    }
    const after = row.getBoundingClientRect().height;
    if (Math.abs(after - before) < 1) {
      return;
    }
    row.style.overflow = 'hidden';
    const animation = row.animate([{height: `${before}px`}, {height: `${after}px`}], Motion.fold);
    animation.onfinish = animation.oncancel = () => row.style.overflow = '';
  }

  showEarlier_() {
    this.tall_ = true;
    this.stick_ = false;
    this.updateComplete.then(() => {
      const transcript = this.querySelector('.transcript');
      const users = this.querySelectorAll('.transcript-content > .entry.user');
      const last = users[users.length - 1];
      if (transcript && last) {
        transcript.scrollTop = Math.max(0, last.offsetTop - 120);
      }
    });
  }

  copyReply_(item) {
    copyText(item.text || '');
    this.copied_ = item.id;
    clearTimeout(this.copiedTimer_);
    this.copiedTimer_ = setTimeout(() => this.copied_ = '', COPIED);
  }

  setDragOver_(over) {
    if (this.dragOver_ !== over) {
      this.dragOver_ = over;
      send('dragOver', over);
    }
  }

  // 附上图片（最多 8 张，多的不要）。
  addImages_(images) {
    const room = MAX_IMAGES - this.attachments_.length;
    const added = images.filter(Boolean).slice(0, Math.max(0, room)).map((image) => ({
      ...image,
      id: this.nextAttachment_++,
      url: `data:${image.mime};base64,${image.data}`,
    }));
    if (added.length) {
      this.attachments_ = [...this.attachments_, ...added];
      this.focusComposer_();
    }
  }

  async addFiles_(files) {
    const room = MAX_IMAGES - this.attachments_.length;
    const images = await Promise.all(files.slice(0, Math.max(0, room)).map(loadImage));
    this.addImages_(images);
  }

  removeAttachment_(id) {
    this.attachments_ = this.attachments_.filter((attachment) => attachment.id !== id);
  }

  // ---- @ 引用标签 ----

  // 光标前面的 @ 和后面打的字（同一行、不以空白开头、最多 40 个）。@ 在开头、空白、标点或者中文
  // 后面才算，a@b.com 不算；全角的 @（U+FF20）也认。
  mentionQuery_(textarea) {
    if (!textarea || textarea.selectionStart !== textarea.selectionEnd) {
      return null;
    }
    const before = textarea.value.slice(0, textarea.selectionStart);
    const at = Math.max(before.lastIndexOf('@'), before.lastIndexOf(FULLWIDTH_AT));
    if (at < 0 || MENTION_WORD.test(before.slice(0, at))) {
      return null;
    }
    const query = before.slice(at + 1);
    if (query.length > MENTION_QUERY_MAX || query.includes('\n') || /^\s/.test(query)) {
      return null;
    }
    return {start: at, end: textarea.selectionStart, query};
  }

  // 打字、挪光标后重新看要不要标签列表。只在打字（typing）、紧跟着 @ 时弹出来；开着时接着筛，
  // 打了空格还对不上哪个标签就当它是普通的 @，收起来。
  updateMention_(typing) {
    const found = this.pendingCard_ || this.mentions_.length >= MAX_MENTIONS ?
        null : this.mentionQuery_(this.querySelector('textarea'));
    if (!found) {
      this.dismissedMention_ = -1;
      this.picker_ = null;
      return;
    }
    const same = this.picker_?.start === found.start;
    if (found.start === this.dismissedMention_ || (!same && (!typing || /\s/.test(found.query)))) {
      this.picker_ = null;
      return;
    }
    if (!same) {
      this.mentionTabs_ = null;
      send('mentionTabs');
    }
    this.picker_ = {...found, index: same && this.picker_.query === found.query ? this.picker_.index : 0};
    if (this.mentionTabs_ && /\s$/.test(found.query) && !this.mentionMatches_().length) {
      this.dismissedMention_ = found.start;
      this.picker_ = null;
    }
  }

  // 列表里的标签：还没 @ 的，标题或网址里有打的每个词（不分大小写）。
  mentionMatches_() {
    if (!this.picker_ || !this.mentionTabs_) {
      return [];
    }
    const words = this.picker_.query.toLowerCase().split(/\s+/).filter(Boolean);
    const taken = new Set(this.mentions_.map((tab) => tab.id));
    return this.mentionTabs_.filter((tab) => {
      const text = `${tab.title}\n${tab.url}`.toLowerCase();
      return !taken.has(tab.id) && words.every((word) => text.includes(word));
    });
  }

  // 选中一个：拿掉 @ 和后面打的字，标签变成输入框里的小胶囊。
  pickMention_(tab) {
    const picker = this.picker_;
    if (!picker || !tab) {
      return;
    }
    const before = this.draft_.slice(0, picker.start);
    let after = this.draft_.slice(picker.end);
    if (/\s$/.test(before) && after.startsWith(' ')) {
      after = after.slice(1);
    }
    this.draft_ = before + after;
    this.mentions_ = [...this.mentions_, {id: tab.id, title: tab.title, url: tab.url}];
    this.picker_ = null;
    this.updateComplete.then(() => {
      const textarea = this.querySelector('textarea');
      if (textarea) {
        textarea.focus();
        textarea.setSelectionRange(before.length, before.length);
      }
    });
  }

  removeMention_(id) {
    this.mentions_ = this.mentions_.filter((tab) => tab.id !== id);
    this.focusComposer_();
  }

  // + 菜单的「引用标签页…」：在光标处打一个 @。
  startMention_() {
    const textarea = this.querySelector('textarea');
    if (!textarea || this.mentions_.length >= MAX_MENTIONS) {
      return;
    }
    textarea.focus();
    const start = textarea.selectionStart;
    const before = this.draft_.slice(0, start);
    const insert = MENTION_WORD.test(before) ? ' @' : '@';
    this.draft_ = before + insert + this.draft_.slice(textarea.selectionEnd);
    this.dismissedMention_ = -1;
    this.updateComplete.then(() => {
      const caret = before.length + insert.length;
      textarea.setSelectionRange(caret, caret);
      this.resizeComposer_();
      this.updateMention_(true);
    });
  }

  hoverMention_(index) {
    if (this.picker_ && this.picker_.index !== index) {
      this.picker_ = {...this.picker_, index};
    }
  }

  // 用方向键选到列表外面的一行时滚过去。
  revealMention_() {
    const list = this.querySelector('.mention-picker');
    const row = list?.querySelector('.mention-row.selected');
    if (!row) {
      return;
    }
    if (row.offsetTop < list.scrollTop) {
      list.scrollTop = row.offsetTop - 5;
    } else if (row.offsetTop + row.offsetHeight > list.scrollTop + list.clientHeight) {
      list.scrollTop = row.offsetTop + row.offsetHeight - list.clientHeight + 5;
    }
  }

  // 列表开着时的按键：↑↓ 选，回车、Tab 确定，Esc 关掉（不收起岛）。处理了就是 true。
  mentionKey_(event) {
    if (!this.picker_ || event.isComposing) {
      return false;
    }
    const matches = this.mentionMatches_();
    if ((event.key === 'ArrowDown' || event.key === 'ArrowUp') && matches.length) {
      const step = event.key === 'ArrowDown' ? 1 : -1;
      const index = Math.min(this.picker_.index, matches.length - 1);
      this.picker_ = {...this.picker_, index: (index + step + matches.length) % matches.length};
    } else if ((event.key === 'Enter' || event.key === 'Tab') && !event.shiftKey && !event.altKey &&
               matches.length) {
      this.pickMention_(matches[Math.min(this.picker_.index, matches.length - 1)]);
    } else if (event.key === 'Escape') {
      this.dismissedMention_ = this.picker_.start;
      this.picker_ = null;
    } else {
      return false;
    }
    event.preventDefault();
    return true;
  }

  chooseImages_() {
    this.querySelector('.file-input')?.click();
  }

  onFilesChosen_(event) {
    const files = [...(event.target.files || [])];
    event.target.value = '';
    this.addFiles_(files);
  }

  onPaste_(event) {
    // 有图片（截图、拷贝的图片文件）时附上；纯文字照常粘贴。
    const files = imageFiles(event.clipboardData);
    if (files.length) {
      event.preventDefault();
      this.addFiles_(files);
    }
  }

  async plusMenu_(event) {
    const backend = this.state_.backend || 'chat';
    const id = await showMenu([
      {id: 'choose', title: L('assistant.attach.choose')},
      {id: 'capture', title: L('assistant.attach.capture')},
      {id: 'mention', title: L('assistant.mention.menu'), enabled: this.mentions_.length < MAX_MENTIONS},
      {separator: true},
      {header: L('settings.ai.assistant.backend.title')},
      {id: 'chat', title: L('assistant.backend.chatCompletions'), checked: backend === 'chat'},
      {id: 'claudeCode', title: 'Claude Code', checked: backend === 'claudeCode'},
      {id: 'codex', title: 'Codex', checked: backend === 'codex'},
      {separator: true},
      {id: 'settings', title: L('assistant.settings'), icon: 'gearshape'},
    ], {element: event.currentTarget});
    if (id === 'settings') {
      send('openSettings');
    } else if (id === 'choose') {
      this.chooseImages_();
    } else if (id === 'capture') {
      send('capture');
    } else if (id === 'mention') {
      this.startMention_();
    } else if (id) {
      send('setBackend', id);
    }
  }

  async modeMenu_(event) {
    const mode = this.state_.mode || 'ask';
    const id = await showMenu([
      {id: 'ask', title: L('assistant.mode.ask.menu'), checked: mode === 'ask'},
      {id: 'agent', title: L('assistant.mode.agent.menu'), checked: mode === 'agent'},
      {id: 'auto', title: L('assistant.mode.auto.menu'), checked: mode === 'auto'},
    ], {element: event.currentTarget});
    if (id) {
      send('setMode', id);
    }
  }

  async reasoningMenu_(event) {
    const current = this.state_.reasoning || '';
    const id = await showMenu([
      {header: L('assistant.reasoning.title')},
      {id: 'default', title: L('assistant.reasoning.default'), checked: !current},
      ...(this.state_.reasoningLevels || []).map((level) => ({
        id: level,
        title: reasoningName(level),
        checked: current === level,
      })),
    ], {element: event.currentTarget});
    if (id) {
      send('setReasoning', id === 'default' ? '' : id);
    }
  }

  onKeyDown_(event) {
    if (this.mentionKey_(event)) {
      return;
    }
    // 光标在最前面时退格：拿掉最后一个 @ 的标签。
    const textarea = event.currentTarget;
    if (event.key === 'Backspace' && !event.isComposing && this.mentions_.length && textarea.selectionStart === 0 &&
        textarea.selectionEnd === 0) {
      event.preventDefault();
      this.mentions_ = this.mentions_.slice(0, -1);
      return;
    }
    if (event.key === 'Enter' && !event.shiftKey && !event.altKey && !event.isComposing) {
      event.preventDefault();
      this.submit_();
      return;
    }
    // 问题卡片：输入框是空的时按数字选选项。
    const card = this.pendingCard_;
    if (card?.type === 'question' && !this.draft_ && /^[1-9]$/.test(event.key)) {
      const option = (card.options || [])[Number(event.key) - 1];
      if (option) {
        event.preventDefault();
        send('answer', card.id, option);
      }
    }
  }

  onScroll_(event) {
    const element = event.currentTarget;
    this.stick_ = element.scrollHeight - element.scrollTop - element.clientHeight < FOLLOW;
  }

  onWheel_(event) {
    // 往上滚：露出上文（变到平时的最高）。
    if (event.deltaY < 0 && !this.tall_ && this.userCount_ > 1) {
      this.tall_ = true;
    }
  }

  // ---- 聊天记录 ----

  openHistory_() {
    this.history_ = {query: '', data: null};
    this.thumbs_ = {};
    send('history', '');
  }

  closeHistory_() {
    this.history_ = null;
    send('history', null);
  }

  toggleHistory_() {
    this.history_ ? this.closeHistory_() : this.openHistory_();
  }

  searchHistory_(query) {
    this.history_ = {...this.history_, query};
    send('history', query);
  }

  async clearHistory_() {
    if (await confirm({icon: 'trash.fill', title: L('assistant.history.clearConfirm'),
                       message: L('assistant.history.clearDetail'), confirmTitle: L('common.clear')})) {
      send('clearHistory');
    }
  }

  // 按日期分：今天、昨天、这周、更早。
  historySections_(rows) {
    const today = new Date();
    today.setHours(0, 0, 0, 0);
    const day = 24 * 60 * 60 * 1000;
    const sections = [
      {title: L('assistant.history.today'), rows: []},
      {title: L('assistant.history.yesterday'), rows: []},
      {title: L('assistant.history.week'), rows: []},
      {title: L('assistant.history.earlier'), rows: []},
    ];
    for (const row of rows) {
      const index = row.updated >= today.getTime() ? 0 : row.updated >= today.getTime() - day ? 1 :
                    row.updated >= today.getTime() - 7 * day ? 2 : 3;
      sections[index].rows.push(row);
    }
    return sections.filter((section) => section.rows.length);
  }

  historyTime_(updated) {
    const date = new Date(updated);
    const today = new Date();
    return date.toDateString() === today.toDateString() ?
        date.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit'}) :
        date.toLocaleDateString();
  }

  renderHistory_() {
    const data = this.history_.data;
    const query = this.history_.query;
    const live = data?.live || [];
    const rows = data?.rows || [];
    return html`
      <div class="history">
        <label class="history-search">
          ${icon('magnifyingglass', 12)}
          <input type="search" .value=${query} placeholder=${L('assistant.history.search')}
              @input=${(event) => this.searchHistory_(event.target.value)}>
        </label>
        <div class="history-list">
          ${live.length && !query ? html`
            <div class="live-tiles">
              ${live.map((tile) => html`
                <button class="live-tile ${tile.waiting ? 'waiting' : ''}" @click=${() => send('activate', tile.id)}>
                  <span class="live-thumb">${this.thumbs_[tile.id] ? html`<img src=${this.thumbs_[tile.id]} alt="">` :
                                                                     nothing}</span>
                  <span class="live-text">
                    <span class="live-title">${tile.title}</span>
                    <span class="live-status">${tile.status}</span>
                  </span>
                </button>`)}
            </div>` : nothing}
          ${data?.private ? html`<div class="history-note">${L('assistant.history.private')}</div>` : nothing}
          ${data && !rows.length && !live.length ? html`
            <div class="history-empty">${query ? L('assistant.history.noMatch') : L('assistant.history.empty')}</div>` :
            nothing}
          ${this.historySections_(rows).map((section) => html`
            <div class="history-section">${section.title}</div>
            ${section.rows.map((row) => html`
              <div class="history-row" @click=${() => send('openRecord', row.id)}>
                ${row.site ? html`<img class="history-icon" src=${faviconUrl(row.site, 16)} alt="">` :
                             html`<span class="history-icon">${icon('globe', 14)}</span>`}
                <span class="history-title">${row.title}</span>
                <span class="history-time">${this.historyTime_(row.updated)}</span>
                <button class="history-delete" title=${L('assistant.history.delete')}
                    @click=${(event) => { event.stopPropagation(); send('deleteRecord', row.id); }}>
                  ${icon('xmark', 9.5)}
                </button>
              </div>`)}`)}
          ${rows.length && !query ? html`
            <button class="history-clear" @click=${this.clearHistory_}>${L('assistant.history.clear')}</button>` :
            nothing}
        </div>
      </div>`;
  }

  // ---- 对话里的行 ----

  // 合并连续步骤：从首个工具调用起，将后续工具及穿插的已答确认/问题收进同一折叠行；
  // 已完成的思考仅在后面仍有工具时并入；至少包含两步工具才合并。
  rows_() {
    const items = this.items_.filter((item) => !item.pending && !(item.type === 'tool' && item.name === 'ask_user'));
    const rows = [];
    for (let i = 0; i < items.length; i++) {
      const item = items[i];
      if (item.type === 'tool') {
        let end = i + 1;
        let tools = 1;
        let j = i + 1;
        while (j < items.length) {
          const next = items[j];
          if (next.type === 'tool') {
            tools++;
            end = j + 1;
          } else if (next.type === 'approval' || next.type === 'question') {
            end = j + 1;
          } else if (!(next.type === 'reasoning' && next.done)) {
            break;
          }
          j++;
        }
        if (tools >= 2) {
          rows.push({kind: 'group', id: item.id, members: items.slice(i, end)});
          i = end - 1;
          continue;
        }
      }
      rows.push({kind: 'item', id: item.id, item});
    }
    return rows;
  }

  renderRow_(row, index, rows) {
    if (row.kind === 'group') {
      return this.renderGroup_(row);
    }
    const item = row.item;
    switch (item.type) {
      case 'user':
        return this.renderUser_(item);
      case 'assistant': {
        // 一轮结束时的回答下面有「拷贝回答」。
        const next = rows.slice(index + 1).find((other) => other.kind !== 'item' || other.item.type !== 'notice');
        const footer = next ? next.kind === 'item' && next.item.type === 'user' : !this.state_.running;
        return html`
          <div class="entry reply" data-row=${item.id}><starry-markdown .text=${item.text}></starry-markdown></div>
          ${footer && item.text ? html`
            <div class="entry reply-footer" data-row=${`${item.id}-footer`}>
              <button class="icon-button" title=${L('assistant.reply.copy')} @click=${() => this.copyReply_(item)}>
                ${icon(this.copied_ === item.id ? 'checkmark' : 'doc.on.doc', 11)}
              </button>
            </div>` : nothing}`;
      }
      case 'reasoning':
        return this.renderReasoning_(item, true);
      case 'tool':
        return this.renderTool_(item, true);
      case 'notice':
        return this.renderNotice_(item);
      case 'approval':
      case 'question':
        return this.renderRecord_(item, true);
    }
    return nothing;
  }

  renderUser_(item) {
    const foldable = this.foldable_.has(item.id);
    const open = this.expanded_.has(item.id);
    const count = [...(item.text || '')].length;
    const images = item.images || [];
    const mentions = item.mentions || [];
    return html`
      <div class="entry user" data-row=${item.id}>
        <div class="bubble-stack">
          ${item.tab ? html`
            <div class="tab-label" title=${item.tab.url}>
              <img src=${faviconUrl(item.tab.url, 16)} alt="">
              <span>${item.tab.title || item.tab.url}</span>
            </div>` : nothing}
          ${item.selection ? html`
            <div class="quote"><span class="quote-bar"></span><span class="quote-text">${item.selection}</span></div>` :
            nothing}
          ${images.length ? html`
            <div class="bubble-images">
              ${images.map((image) => image.src ? html`
                <button class="bubble-image" @click=${() => this.preview_ = image.src}>
                  <img src=${image.src} alt="">
                </button>` : html`<span class="bubble-image missing">${icon('photo', 16)}</span>`)}
            </div>` : nothing}
          ${mentions.length ? html`
            <div class="bubble-mentions">
              ${mentions.map((tab) => html`
                <span class="bubble-mention" title=${tab.url}>
                  <img src=${faviconUrl(tab.url, 16)} alt=""><span>${tab.title || tab.url}</span>
                </span>`)}
            </div>` : nothing}
          ${item.text ? html`
            <div class="bubble ${foldable && !open ? 'folded' : ''} ${foldable && open ? 'expanded' : ''}">
              <div class="bubble-text" data-id=${item.id}>${item.text}</div>
              ${foldable ? html`
                <button class="fold" @click=${() => this.toggle_(item.id)}>
                  <span>${open ? L('assistant.user.collapse') :
                                 L('assistant.user.expand', {count: count.toLocaleString()})}</span>
                  ${icon(open ? 'chevron.up' : 'chevron.down', 9)}
                </button>` : nothing}
            </div>` : nothing}
        </div>
      </div>`;
  }

  renderReasoning_(item, top) {
    const open = this.expanded_.has(item.id);
    return html`
      <div class="entry reasoning ${item.done ? '' : 'active'}" data-row=${top ? item.id : nothing}>
        <button class="disclosure" @click=${() => this.toggle_(item.id)}>
          <span class="brain">${icon('brain', 11)}</span>
          <span>${item.done ? L('assistant.reasoning.done') : L('assistant.reasoning.active')}</span>
          ${chevron(open)}
        </button>
        ${open ? html`<div class="reasoning-body">${item.text}</div>` : nothing}
      </div>`;
  }

  statusIcon_(status) {
    if (status === 'running') {
      return spinner();
    }
    return status === 'error' ? html`<span class="status-icon error">${icon('exclamationmark.circle.fill', 12)}</span>` :
                                html`<span class="status-icon done">${icon('checkmark.circle.fill', 12)}</span>`;
  }

  renderTool_(item, top) {
    const open = this.expanded_.has(item.id);
    const hasOutput = item.status !== 'running';
    return html`
      <div class="entry tool ${item.status}" data-row=${top ? item.id : nothing}>
        <button class="pill" title=${item.name} ?disabled=${!hasOutput} @click=${() => this.toggle_(item.id)}>
          ${this.statusIcon_(item.status)}
          <span class="pill-title">${item.title}</span>
          ${hasOutput ? chevron(open) : nothing}
        </button>
        ${open ? html`<pre class="output ${item.status === 'error' ? 'error' : ''}">${
            item.output || L('assistant.tool.noOutput')}</pre>` : nothing}
      </div>`;
  }

  renderGroup_(row) {
    // 展开状态按「组」记，和组里第一步（同一个 id）的展开分开。
    const key = `group:${row.id}`;
    const open = this.expanded_.has(key);
    const tools = row.members.filter((member) => member.type === 'tool');
    const running = tools.filter((tool) => tool.status === 'running');
    const failed = tools.filter((tool) => tool.status === 'error').length;
    // 做的时候是正在做的那一步（原地换字），做完是「做完 3 步」。
    const title = running.length ? running[running.length - 1].title :
        failed ? L('assistant.steps.doneFailed', {count: tools.length, failed}) :
                 L('assistant.steps.done', {count: tools.length});
    const status = running.length ? 'running' : failed ? 'error' : 'done';
    return html`
      <div class="entry group" data-row=${row.id}>
        <button class="pill" @click=${() => this.toggle_(key, row.id)}>
          ${this.statusIcon_(status)}
          <span class="pill-title">${title}</span>
          ${chevron(open)}
        </button>
        ${open ? html`
          <div class="members">
            ${row.members.map((member) => member.type === 'tool' ? this.renderTool_(member, false) :
                                          member.type === 'reasoning' ? this.renderReasoning_(member, false) :
                                                                        this.renderRecord_(member, false))}
          </div>` : nothing}
      </div>`;
  }

  renderNotice_(item) {
    const open = this.expanded_.has(item.id);
    // 种类：info、warning（自动审核拒绝了、工具连不上）、failed、stopped。
    const kind = item.kind || (item.error ? 'failed' : item.text === L('assistant.notice.stopped') ? 'stopped' : 'info');
    const glyph = {
      failed: 'exclamationmark.triangle.fill',
      warning: 'exclamationmark.circle',
      stopped: 'stop.circle',
      info: 'info.circle',
    }[kind] || 'info.circle';
    return html`
      <div class="entry notice ${kind}" data-row=${item.id}>
        <div class="notice-line">
          <span class="notice-icon">${icon(glyph, 11.5)}</span>
          <span class="notice-text">${item.text}${item.detail ? html`<button class="link" @click=${() => this.toggle_(item.id)}>
            ${open ? L('assistant.notice.hideDetail') : L('assistant.notice.showDetail')}</button>` : nothing}</span>
        </div>
        ${open && item.detail ? html`<pre class="notice-detail">${item.detail}</pre>` : nothing}
      </div>`;
  }

  // 答过的确认、问题：一行记录。
  renderRecord_(item, top) {
    let ok = true;
    let body;
    if (item.type === 'approval') {
      ok = item.decision !== 'deny';
      body = item.decision === 'deny' ? L('assistant.approval.denied', {action: item.title}) :
          item.decision === 'site' ? L('assistant.approval.allowedSite', {action: item.title, site: item.site}) :
                                     L('assistant.approval.allowed', {action: item.title});
    } else {
      const unanswered = !item.answer || item.answer === L('assistant.ask.unanswered');
      ok = !unanswered;
      body = html`${item.question}<b>${unanswered ? L('assistant.ask.unanswered') : item.answer}</b>`;
    }
    return html`
      <div class="entry record ${ok ? 'ok' : 'no'}" data-row=${top ? item.id : nothing}>
        <span class="record-icon">${icon(ok ? 'checkmark.circle.fill' : 'xmark.circle.fill', 12)}</span>
        <span class="record-text">${body}</span>
      </div>`;
  }

  // 等模型的时候最后一行是「正在思考…」（本机智能体在启动时是「正在启动…」）。
  renderWaiting_() {
    if (!this.state_.running || this.pendingCard_) {
      return nothing;
    }
    const last = this.items_[this.items_.length - 1];
    if (!last || last.type === 'assistant' || (last.type === 'tool' && last.status === 'running') ||
        (last.type === 'reasoning' && !last.done)) {
      return nothing;
    }
    const label = this.state_.status === 'starting' ? L('assistant.status.starting') :
                                                      L('assistant.status.thinking');
    return html`
      <div class="entry waiting" data-row=${`waiting-${last.id}`}>
        <span class="dots"><i></i><i></i><i></i></span><span>${label}</span>
      </div>`;
  }

  // ---- 其余的部分 ----

  renderHeader_() {
    const docked = this.docked_;
    const hugging = !docked && !this.tall_ && !this.history_;
    const empty = !this.items_.length;
    // 标题：聊天记录开着时是「聊天记录」；悬浮时只在变高以后写对话的标题，侧栏时一直写。
    const title = this.history_ ? L('assistant.history.title') : !hugging ? this.state_.title || '' : '';
    return html`
      <div class="header">
        <div class="header-left">
          ${title ? html`<span class="header-title">${title}</span>` : nothing}
          ${hugging && this.userCount_ > 1 ? html`
            <button class="chip earlier" @click=${this.showEarlier_}>
              ${icon('chevron.up', 9)}<span>${L('assistant.earlier')}</span>
            </button>` : nothing}
        </div>
        <div class="actions">
          <button class="icon-button ${empty ? 'dim' : ''}" title=${L('assistant.newChat.help')}
              @click=${() => send('newChat')}>${icon('square.and.pencil', 14)}</button>
          <button class="icon-button" title=${L('assistant.history.title')} @click=${this.toggleHistory_}>
            ${icon('clock', 14)}
          </button>
          <button class="icon-button" title=${docked ? L('assistant.panelStyle.float') : L('assistant.panelStyle.dock')}
              @click=${() => send('setPanelStyle', docked ? 'float' : 'dock')}>
            ${icon(docked ? 'rectangle.bottomthird.inset.filled' : 'sidebar.right', 14)}
          </button>
          ${docked ? html`
            <button class="icon-button" title=${L('assistant.close')} @click=${() => send('close')}>
              ${icon('xmark', 12)}
            </button>` : html`
            <button class="icon-button" title=${L('assistant.collapse')} @click=${() => send('collapse')}>
              ${icon('chevron.down', 12)}
            </button>`}
        </div>
      </div>`;
  }

  renderCard_() {
    const card = this.pendingCard_;
    if (!card || this.history_) {
      return nothing;
    }
    if (card.type === 'approval') {
      const site = card.site && card.site.length > 20 ? `${card.site.slice(0, 19)}…` : card.site;
      // 「自动」时用户确认的不能整站放行（每次敏感操作都单独审）。
      const siteWide = !card.sensitive && card.site && card.siteWide !== false;
      return html`
        <div class="card-wrap">
          <div class="card approval ${card.sensitive ? 'sensitive' : ''}" data-card=${card.id}>
            <div class="card-title">
              ${card.sensitive ? html`<span class="shield">${icon('exclamationmark.shield.fill', 13)}</span>` : nothing}
              <span>${card.title}</span>
            </div>
            ${card.sensitive || card.site ? html`<div class="card-subtitle">${
                card.sensitive ? L('assistant.approval.sensitive') : card.site}</div>` : nothing}
            ${card.detail ? html`<div class="card-detail">${card.detail}</div>` : nothing}
            <div class="options">
              <button class="option primary" @click=${() => this.approve_(card, 'allow')}>
                <span>${L('assistant.approval.allow')}</span>
                ${card.sensitive ? nothing : html`<span class="key">↩</span>`}
              </button>
              ${siteWide ? html`
                <button class="option" @click=${() => this.approve_(card, 'site')}>
                  ${L('assistant.approval.allowSite', {site})}
                </button>` : nothing}
              <button class="option" @click=${() => this.approve_(card, 'deny', this.draft_.trim())}>
                ${L('assistant.approval.deny')}
              </button>
            </div>
          </div>
        </div>`;
    }
    return html`
      <div class="card-wrap">
        <div class="card question" data-card=${card.id}>
          <div class="card-title"><span>${card.question}</span></div>
          ${card.detail ? html`<div class="card-subtitle">${card.detail}</div>` : nothing}
          <div class="options">
            ${(card.options || []).map((option, index) => html`
              <button class="option keyed" @click=${() => send('answer', card.id, option)}>
                <span class="key">${index + 1}</span><span>${option}</span>
              </button>`)}
          </div>
        </div>
      </div>`;
  }

  suggestionList_() {
    return [
      ['text.alignleft', L('assistant.suggestion.summarize')],
      ['list.bullet', L('assistant.suggestion.keyPoints')],
      ['character.bubble', L('assistant.suggestion.translate')],
    ];
  }

  // 悬浮空对话状态下的快捷建议，点击直接发送。
  renderSuggestions_() {
    if (this.docked_ || this.history_ || !this.state_.configured || this.items_.length || this.draft_ ||
        this.attachments_.length || this.mentions_.length || this.state_.running) {
      return nothing;
    }
    return html`
      <div class="suggestions">
        ${this.suggestionList_().map(([glyph, text]) => html`
          <button class="suggestion" @click=${() => send('send', text, [])}>${icon(glyph, 12)}<span>${text}</span></button>`)}
      </div>`;
  }

  // 侧栏空对话状态。
  renderEmpty_() {
    if (!this.docked_ || this.history_ || this.items_.length || this.state_.configured === false) {
      return nothing;
    }
    return html`
      <div class="empty">
        <div class="glyph">${icon('sparkles', 23)}</div>
        <div class="setup-title">${L('assistant.empty.title')}</div>
        <div class="setup-detail">${this.state_.private ? L('assistant.empty.private') : L('assistant.empty.detail')}</div>
        <div class="empty-suggestions">
          ${this.suggestionList_().map(([glyph, text]) => html`
            <button class="empty-suggestion" @click=${() => send('send', text, [])}>
              ${icon(glyph, 12)}<span>${text}</span>
            </button>`)}
        </div>
      </div>`;
  }

  renderSetup_() {
    if (this.state_.configured !== false || this.items_.length || this.history_) {
      return nothing;
    }
    return html`
      <div class="setup">
        <div class="glyph">${icon('sparkles', 23)}</div>
        <div class="setup-title">${L('assistant.setup.title')}</div>
        <div class="setup-detail">${L('assistant.setup.chatCompletions')}</div>
        <button class="setup-button" @click=${() => send('openSettings')}>
          ${icon('gearshape', 12)}<span>${L('assistant.setup.open')}</span>
        </button>
      </div>`;
  }

  renderThumbs_() {
    if (!this.attachments_.length) {
      return nothing;
    }
    return html`
      <div class="thumbs">
        ${this.attachments_.map((attachment) => html`
          <div class="thumb" data-attachment=${attachment.id}>
            <button class="thumb-image" @click=${() => this.preview_ = attachment.url}>
              <img src=${attachment.url} alt="">
            </button>
            <button class="thumb-remove" title=${L('assistant.attach.remove')}
                @click=${() => this.removeAttachment_(attachment.id)}>${icon('xmark', 8)}</button>
          </div>`)}
      </div>`;
  }

  renderBar_() {
    const card = this.pendingCard_;
    const placeholder = card?.type === 'approval' ? L('assistant.composer.placeholderDeny') :
        card?.type === 'question' ? L('assistant.composer.placeholderAnswer') :
        this.items_.length ? L('assistant.composer.placeholderContinue') :
                             L('assistant.composer.placeholderPage');
    const hasDraft = !!this.draft_.trim() || (!card && (this.attachments_.length > 0 || this.mentions_.length > 0));
    const stops = this.state_.running && !card && !hasDraft;
    const enabled = hasDraft || stops || (card?.type === 'approval' && !card.sensitive);
    const mode = this.state_.mode || 'ask';
    const modeIcon = {ask: 'hand.raised', agent: 'bolt.fill', auto: 'checkmark.shield'}[mode] || 'hand.raised';
    const levels = this.state_.reasoningLevels || [];
    const reasoning = this.state_.reasoning || '';
    return html`
      <div class="bar ${this.focused_ ? 'focused' : ''}">
        ${this.renderThumbs_()}
        ${this.renderMentionChips_()}
        <div class="bar-row">
          <button class="round plus" title=${L('assistant.attach.help')} @click=${this.plusMenu_}>
            ${icon('plus', 15)}
          </button>
          <textarea rows="1" .value=${this.draft_} placeholder=${placeholder}
              @input=${(event) => {
                this.draft_ = event.target.value;
                this.resizeComposer_();
                this.updateMention_(true);
              }}
              @click=${() => this.updateMention_(false)}
              @keyup=${() => this.updateMention_(false)}
              @paste=${this.onPaste_}
              @focus=${() => { this.focused_ = true; send('composerFocus', true); }}
              @blur=${() => { this.focused_ = false; this.picker_ = null; send('composerFocus', false); }}
              @keydown=${this.onKeyDown_}></textarea>
          ${levels.length ? html`
            <button class="chip reasoning" title=${L('assistant.reasoning.help')} @click=${this.reasoningMenu_}>
              ${icon(reasoning === 'none' ? 'brain' : 'brain.fill', 10.5)}
              <span>${reasoning ? reasoningName(reasoning) : L('assistant.reasoning.chip')}</span>
            </button>` : nothing}
          <button class="chip mode ${mode !== 'ask' ? 'active' : ''}" title=${L('assistant.mode.help')}
              @click=${this.modeMenu_}>
            ${icon(modeIcon, 10.5)}
            <span>${L(`assistant.mode.${mode}`)}</span>
          </button>
          <button class="round camera" title=${L('assistant.attach.capture')} @click=${() => send('capture')}>
            ${icon('camera', 15)}
          </button>
          <button class="round send ${enabled ? 'enabled' : ''}"
              title=${stops ? L('assistant.composer.stop') : L('assistant.composer.send')} ?disabled=${!enabled}
              @click=${() => stops ? send('stop') : this.submit_()}>
            ${icon(stops ? 'stop.fill' : 'arrow.up', stops ? 11 : 14)}
          </button>
        </div>
        <input class="file-input" type="file" accept="image/png,image/jpeg,image/gif,image/webp,image/heic" multiple
            hidden @change=${this.onFilesChosen_}>
      </div>`;
  }

  // 打 @ 时输入框上面的标签列表：这个窗口的在前面，正在看的写「当前」，别的窗口的写出来。
  renderMentionPicker_() {
    if (!this.picker_ || !this.mentionTabs_ || this.pendingCard_ || this.history_) {
      return nothing;
    }
    const matches = this.mentionMatches_();
    const selected = Math.min(this.picker_.index, matches.length - 1);
    return html`
      <div class="mention-picker-wrap">
        <div class="mention-picker" role="listbox">
          ${matches.length ? matches.map((tab, index) => html`
            <div class="mention-row ${index === selected ? 'selected' : ''}" role="option"
                aria-selected=${index === selected ? 'true' : 'false'} title=${tab.url}
                @mousedown=${(event) => event.preventDefault()}
                @mousemove=${() => this.hoverMention_(index)}
                @click=${() => this.pickMention_(tab)}>
              <img class="mention-icon" src=${faviconUrl(tab.url, 16)} alt="">
              <span class="mention-title">${tab.title || tab.url}</span>
              <span class="mention-host">${hostOf(tab.url)}</span>
              ${tab.current || tab.otherWindow ? html`
                <span class="mention-badge">${
                    tab.current ? L('assistant.mention.current') : L('assistant.mention.otherWindow')}</span>` :
                nothing}
            </div>`) : html`<div class="mention-empty">${L('assistant.mention.none')}</div>`}
        </div>
      </div>`;
  }

  // @ 了的标签：输入框里文字上面一排小胶囊（网站图标、标题、×）。
  renderMentionChips_() {
    if (!this.mentions_.length) {
      return nothing;
    }
    return html`
      <div class="mention-chips">
        ${this.mentions_.map((tab) => html`
          <span class="mention-chip" data-mention=${tab.id} title=${tab.url}>
            <img src=${faviconUrl(tab.url, 16)} alt="">
            <span class="mention-chip-title">${tab.title || tab.url}</span>
            <button class="mention-chip-remove" title=${L('assistant.mention.remove')}
                @click=${() => this.removeMention_(tab.id)}>${icon('xmark', 7.5)}</button>
          </span>`)}
      </div>`;
  }

  renderPreview_() {
    if (!this.preview_) {
      return nothing;
    }
    return html`
      <div class="preview" @click=${() => this.preview_ = ''}>
        <img src=${this.preview_} alt="">
      </div>`;
  }

  render() {
    const docked = this.docked_;
    const conversation = this.items_.length > 0;
    const header = conversation || docked || !!this.history_;
    const rows = this.rows_();
    const classes = [
      'island',
      conversation || this.history_ ? 'conversation' : '',
      docked ? 'docked' : '',
      docked && this.state_.capsules > 0 ? 'has-capsules' : '',
    ].join(' ');
    return html`
      <div class=${classes}>
        ${header ? this.renderHeader_() : nothing}
        ${this.history_ ? this.renderHistory_() : conversation ? html`
          <div class="transcript" @scroll=${this.onScroll_} @wheel=${this.onWheel_}>
            <div class="transcript-content">
              ${repeat(rows, (row) => row.id, (row, index) => this.renderRow_(row, index, rows))}
              ${this.renderWaiting_()}
            </div>
          </div>` : nothing}
        ${this.renderEmpty_()}
        ${this.renderSetup_()}
        ${this.renderSuggestions_()}
        ${this.renderCard_()}
        ${this.renderMentionPicker_()}
        ${this.renderBar_()}
        ${this.renderPreview_()}
        <div class="leaving-layer"></div>
      </div>`;
  }
}

customElements.define('starry-assistant', StarryAssistant);
