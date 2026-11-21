// Copyright 2026 The Starry Browser Authors
//
// 设置页（starry://settings）：左侧分类栏、右侧内容区，视口窄于 680px 时分类栏收至顶部。
// 支持跨分类全文搜索与关键词高亮；数据源见 starry/browser/ui/webui/settings_handler.h，
// 「外观」分组的独立布局见 appearance.js。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, confirm, copyText, openUrl, sendWithPromise, showDialog, showMenu} from './common/browser.js';
import {bytes} from './common/format.js';
import {joinList, L, language, locale} from './common/l10n.js';
import {emptyState, icon, searchField} from './common/library.js';
import {Motion, reduceMotion, reveal} from './common/motion.js';
import {BUTTON_ICONS, placeAppearanceHighlights, renderAppearance, revealAppearance} from './appearance.js';
import './extensions.js';

const COMPACT_WIDTH = 680;
// 下载配置选项：单文件连接数、并发任务数、限速（KB/s）。
const DOWNLOAD_CONNECTIONS = [2, 4, 8, 16];
const DOWNLOAD_CONCURRENCY = [1, 2, 3, 5, 10];
const DOWNLOAD_SPEED_LIMITS = [0, 512, 1024, 2048, 5120, 10240, 20480, 51200];
// 连接中断时每个连接最多重试几次（starry/browser/downloads/download_model.h 的 kMaxRetryAttempts）。
const DOWNLOAD_RETRY_ATTEMPTS = 5;

function speedLimitLabel(limit) {
  if (!limit) {
    return L('downloads.speedLimit.unlimited');
  }
  return limit < 1024 ? `${limit} KB/s` : `${limit / 1024} MB/s`;
}
const PANE_KEY = 'starry.settings.pane';

const PANES = [
  {id: 'general', title: L('settings.pane.general.title'), icon: 'gearshape', summary: L('settings.pane.general.summary')},
  {id: 'appearance', title: L('settings.pane.appearance.title'), icon: 'paintpalette',
   summary: L('settings.pane.appearance.summary')},
  {id: 'newTab', title: L('settings.pane.newTab.title'), icon: 'macwindow',
   summary: L('settings.pane.newTab.summary')},
  {id: 'tabs', title: L('settings.pane.tabs.title'), icon: 'rectangle.stack',
   summary: L('settings.pane.tabs.summary')},
  {id: 'performance', title: L('settings.pane.performance.title'), icon: 'speedometer',
   summary: L('settings.pane.performance.summary')},
  {id: 'media', title: L('settings.pane.media.title'), icon: 'play.rectangle',
   summary: L('settings.pane.media.summary')},
  {id: 'downloads', title: L('common.downloads'), icon: 'arrow.down.circle',
   summary: L('settings.pane.downloads.summary')},
  {id: 'privacy', title: L('settings.pane.privacy.title'), icon: 'hand.raised',
   summary: L('settings.pane.privacy.summary')},
  {id: 'extensions', title: L('common.extensions'), icon: 'puzzlepiece',
   summary: L('settings.pane.extensions.summary')},
  {id: 'ai', title: L('settings.pane.ai.title'), icon: 'sparkles', summary: L('settings.pane.ai.summary')},
  {id: 'developer', title: L('settings.pane.developer.title'), icon: 'hammer',
   summary: L('settings.pane.developer.summary'),
   advanced: true},
  // 「通用」里点「导入…」进来的一页（不在分类栏里，上面有回到「通用」的链接）。
  {id: 'importData', title: L('settings.import.title'), icon: 'square.and.arrow.down',
   summary: L('settings.import.summary'), parent: 'general'},
];

// 分类栏里的分类（子页算它的上一级）。
function railPane(id) {
  const pane = PANES.find((candidate) => candidate.id === id);
  return pane && pane.parent ? pane.parent : id;
}

// 导入的几样（同 starry/browser/ui/webui/import_handler.cc 的名字和顺序）。
const IMPORT_ITEMS = ['bookmarks', 'history', 'passwords', 'searchEngines', 'autofill'];

const CLEAR_RANGES = [
  {id: 'lastHour', title: L('history.clearRange.lastHour'), confirm: L('history.clear.confirm.lastHour')},
  {id: 'today', title: L('history.clearRange.today'), confirm: L('history.clear.confirm.today')},
  {id: 'todayAndYesterday', title: L('history.clearRange.todayAndYesterday'), confirm: L('history.clear.confirm.todayAndYesterday')},
  {id: 'all', title: L('history.clearRange.all'), confirm: L('history.clear.confirm.all')},
];

// ---- 小部件 ----

// 高亮搜索命中的关键词片段。
function highlight(text, query) {
  const terms = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (!text || !terms.length) {
    return text;
  }
  const lower = text.toLowerCase();
  const marked = new Array(text.length).fill(false);
  for (const term of terms) {
    let start = 0;
    while ((start = lower.indexOf(term, start)) !== -1) {
      marked.fill(true, start, start + term.length);
      start += term.length;
    }
  }
  const parts = [];
  let i = 0;
  while (i < text.length) {
    let j = i;
    while (j < text.length && marked[j] === marked[i]) {
      j++;
    }
    const piece = text.slice(i, j);
    parts.push(marked[i] ? html`<mark>${piece}</mark>` : piece);
    i = j;
  }
  return parts;
}

function switchControl(on, onChange, {disabled = false, label = ''} = {}) {
  return html`
    <button class="switch ${on ? 'on' : ''}" role="switch" aria-checked=${on ? 'true' : 'false'}
        aria-label=${label} ?disabled=${disabled} @click=${() => onChange(!on)}></button>`;
}

// 胶囊形的下拉选择：当前值加上下箭头，点开是 Starry 的菜单，当前项打勾。
function menuControl(value, options, onChange, {disabled = false} = {}) {
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
    <button class="menu-button" ?disabled=${disabled} @click=${open}>
      <span>${current ? current.title : '—'}</span>${icon('chevron.up.chevron.down', 9)}
    </button>`;
}

// prominent 为 true 时使用主操作强调样式。
function detailButton(title, iconName, onClick, {danger = false, disabled = false, prominent = false} = {}) {
  return html`
    <button class="detail-button ${danger ? 'danger' : ''} ${prominent ? 'prominent' : ''}" ?disabled=${disabled}
        @click=${onClick}>
      ${iconName ? icon(iconName, 11) : nothing}<span>${title}</span>
    </button>`;
}

function timeZoneControl(state, onChange) {
  const open = async (event) => {
    const id = await showMenu(state.webTimeZoneMenu, {element: event.currentTarget});
    if (id !== null && id !== undefined && id !== state.webTimeZone) {
      onChange(id);
    }
  };
  return html`
    <button class="menu-button" aria-label=${L('settings.general.webTimeZone.title')} @click=${open}>
      <span>${state.webTimeZoneLabel}</span>${icon('chevron.up.chevron.down', 9)}
    </button>`;
}

// 带示意图的单选：选中框在选项间滑动（位置在 updated() 里放）。
function visualPicker(name, value, options, onChange, {disabled = false} = {}) {
  return html`
    <div class="visual-picker" data-picker=${name}>
      <div class="visual-ring"></div>
      ${options.map((option) => html`
        <button class="visual-option ${option.value === value ? 'selected' : ''}" ?disabled=${disabled}
            @click=${() => option.value !== value && onChange(option.value)}>
          <div class="visual-frame"><div class="preview">${option.preview}</div></div>
          <span class="visual-title">${option.title}</span>
        </button>`)}
    </div>`;
}

// 窗口布局缩略示意图：根据侧栏/顶部标签、地址栏位置与紧凑模式组合渲染。
function mockWindow({dark = false, floating = false, half = false, top = false,
                     addressOnTop = false, compact = false} = {}) {
  const compactHeader = compact && (addressOnTop || top);
  const toolbarRow = (addressOnTop || top) && !compactHeader;
  const classes = [dark ? 'dark' : '', floating ? 'floating' : '', half ? 'half' : '',
                   top ? 'top' : '', toolbarRow ? 'address-top' : '',
                   compactHeader ? 'compact' : ''].join(' ');
  return html`
    <div class="mock ${classes}">
      ${top ? html`<div class="mock-strip"><b></b><b></b><b></b></div>` :
              html`<div class="mock-sidebar ${addressOnTop ? 'no-address' : ''}"><i></i><i></i><i></i><i></i></div>`}
      ${toolbarRow ? html`<div class="mock-address"></div>` : nothing}
      <div class="mock-card"></div>
    </div>`;
}

function newTabMock(style, dark) {
  const page = html`<div class="mock-page"><b></b><u></u><u></u><u class="short"></u></div>`;
  return html`
    <div class="mock ${dark ? 'dark' : ''} mock-nt ${style}">
      <div class="mock-sidebar"><i></i><i></i><i></i><i></i></div>
      <div class="mock-card">
        ${style === 'card' ? html`
          <div class="mock-nt-pill"></div>
          <div class="mock-nt-grid"><i class="wide"></i><i></i><i></i><i></i><i></i></div>` : page}
      </div>
      ${style === 'minimal' ? html`<div class="mock-nt-dim"></div><div class="mock-nt-pill floating"></div>` : nothing}
      ${style === 'overlay' ? html`
        <div class="mock-nt-blur"></div><div class="mock-nt-pill floating"></div>
        <div class="mock-nt-tiles">${[0, 1, 2, 3, 4, 5, 6, 7, 8, 9].map(() => html`<i></i>`)}</div>` : nothing}
    </div>`;
}

// 格式化相对时间，例如「3 小时前」「昨天」。
function relativeTime(time) {
  const seconds = (time - Date.now()) / 1000;
  const format = new Intl.RelativeTimeFormat(locale, {numeric: 'auto'});
  const units = [['year', 31536000], ['month', 2592000], ['week', 604800], ['day', 86400],
                 ['hour', 3600], ['minute', 60]];
  for (const [unit, size] of units) {
    if (Math.abs(seconds) >= size) {
      return format.format(Math.round(seconds / size), unit);
    }
  }
  return format.format(0, 'minute');
}

function formatCount(count) {
  return Number(count).toLocaleString(locale);
}

// 导入的来源的名字：浏览器，有好几个用户资料时带上是哪个（同 import_service.cc 的 SourceName）。
function importSourceName(source) {
  return source.profile ? L('settings.import.sourceWithProfile', {browser: source.browser, profile: source.profile}) :
                          source.browser;
}

// 广告与跟踪拦截过滤列表分类。
const FILTER_CATEGORIES = ['ads', 'privacy', 'annoyances', 'custom'];

// onSelect：整行是一个单选的选项（导入的来源），selected 时右边打勾。leading：左边的图标。
function entry(id, title, control, {detail = '', keywords = '', layout = 'row', disabled = false,
                                    leading = '', onSelect = null, selected = false, tone = ''} = {}) {
  return {id, title, control, detail, keywords, layout, disabled, leading, onSelect, selected, tone};
}

function group(id, title, entries, {footer = ''} = {}) {
  return {id, title, entries, footer};
}

class StarrySettings extends LitElement {
  static properties = {
    settings_: {state: true},
    pane_: {state: true},
    query_: {state: true},
    compact_: {state: true},
    historyCount_: {state: true},
    assistant_: {state: true},
    keyDraft_: {state: true},
    modelNote_: {state: true},
    blocking_: {state: true},
    customDraft_: {state: true},
    allowDraft_: {state: true},
    fingerprintDraft_: {state: true},
    locationDraft_: {state: true},
    organizerDraft_: {state: true},
    ruleSiteDraft_: {state: true},
    ruleFolderDraft_: {state: true},
    import_: {state: true},
    importSource_: {state: true},
    importItems_: {state: true},
    update_: {state: true},
    mirrorDraft_: {state: true},
    mirrorError_: {state: true},
  };

  constructor() {
    super();
    this.settings_ = null;
    this.query_ = '';
    this.compact_ = false;
    this.historyCount_ = null;
    this.assistant_ = {};
    this.keyDraft_ = '';
    this.modelNote_ = '';
    this.blocking_ = null;
    // 正在编辑的自定义规则（null 是没在编辑）。
    this.customDraft_ = null;
    this.allowDraft_ = '';
    this.fingerprintDraft_ = '';
    this.locationDraft_ = '';
    this.organizerDraft_ = '';
    this.ruleSiteDraft_ = '';
    this.ruleFolderDraft_ = '';
    // 导入浏览器数据：浏览器给的状态（来源、进度），选中的来源，每个来源关掉了哪几样。
    this.import_ = null;
    this.importSource_ = '';
    this.importItems_ = {};
    this.update_ = null;
    this.mirrorDraft_ = null;
    this.mirrorError_ = '';
    const fromPath = location.pathname.replace(/^\/+|\/+$/g, '');
    let saved = null;
    try {
      saved = localStorage.getItem(PANE_KEY);
    } catch (e) {
      // 存不了就每次从「通用」开始。
    }
    this.pane_ = [fromPath, saved].find((id) => PANES.some((pane) => pane.id === id)) || 'general';
    this.updateTitle_();
  }

  // 标签的标题：「扩展」分类（chrome://extensions 也到这里）叫「扩展」，同侧栏的地址栏。
  updateTitle_() {
    document.title = this.pane_ === 'extensions' && !this.query_ ? L('common.extensions') : L('common.settings');
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('settings-changed', (settings) => {
      this.settings_ = settings;
    });
    addWebUiListener('history-changed', () => this.refreshHistoryCount_());
    addWebUiListener('assistant-settings-changed', (state) => {
      this.assistant_ = state;
    });
    sendWithPromise('getAssistantSettings').then((state) => this.assistant_ = state);
    addWebUiListener('content-blocking-changed', (state) => {
      this.blocking_ = state;
    });
    sendWithPromise('getContentBlocking').then((state) => this.blocking_ = state);
    addWebUiListener('import-changed', (state) => {
      this.import_ = state;
    });
    addWebUiListener('update-changed', (state) => {
      this.update_ = state;
    });
    sendWithPromise('getUpdateState').then((state) => this.update_ = state);
    // 系统的深浅变了：外观是「跟随系统」时预览跟着变。
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => this.requestUpdate());
    if (this.pane_ === 'importData') {
      this.loadImport_();
    }
    new ResizeObserver(() => {
      const compact = this.clientWidth < COMPACT_WIDTH;
      if (compact !== this.compact_) {
        this.compact_ = compact;
      }
      placeAppearanceHighlights(this);
    }).observe(this);
    // 回到这个标签（可能在系统设置里改了默认浏览器）时再查一次。
    document.addEventListener('visibilitychange', () => {
      if (!document.hidden) {
        chrome.send('checkDefaultBrowser');
      }
    });
    this.settings_ = await sendWithPromise('getSettings');
    chrome.send('checkDefaultBrowser');
    this.refreshHistoryCount_();
    await this.updateComplete;
    this.revealPage_();
  }

  async refreshHistoryCount_() {
    this.historyCount_ = await sendWithPromise('historyCount');
  }

  updated() {
    this.placeRailHighlight_();
    for (const picker of this.querySelectorAll('.visual-picker')) {
      this.placeRing_(picker);
    }
    placeAppearanceHighlights(this);
  }

  // ---- 动效 ----

  revealPage_() {
    reveal(this.querySelector('.rail-title'), {delay: 0.02, distance: 4});
    reveal(this.querySelector('.settings > .rail .search, .compact-bar .search'), {delay: 0.04, distance: 4});
    this.querySelectorAll('.rail-item, .rail-heading, .pane-chips').forEach((item, index) => {
      reveal(item, {delay: 0.06 + index * 0.025, distance: 6});
    });
    reveal(this.querySelector('.rail-footer'), {delay: 0.2, distance: 4});
    this.revealContent_(0);
  }

  // 新分类沿切换方向淡入，下方各设置组依次播放入场动效。
  revealContent_(direction) {
    const inner = this.querySelector('.content-inner');
    if (!inner) {
      return;
    }
    if (direction && !reduceMotion()) {
      inner.animate([{opacity: 0, transform: `translateY(${18 * direction}px)`}, {opacity: 1, transform: 'none'}],
                    {duration: Motion.tab.duration, easing: Motion.tab.easing});
    }
    const header = inner.querySelector('.pane-header');
    if (header) {
      reveal(header.querySelector('.pane-tile'), {distance: 0, scale: 0.8});
      reveal(header.querySelector('.pane-text'), {delay: 0.03, distance: 6});
    }
    inner.querySelectorAll(':scope > .group').forEach((element, index) => {
      reveal(element, {delay: 0.03 + Math.min(index, 8) * 0.035, distance: 10});
    });
    if (this.pane_ === 'appearance' && !this.query_) {
      revealAppearance(inner);
    }
  }

  placeRailHighlight_() {
    const list = this.querySelector('.rail-list');
    const highlight = list && list.querySelector('.rail-highlight');
    if (!highlight) {
      return;
    }
    const active = list.querySelector('.rail-item.active');
    highlight.style.opacity = active ? '1' : '0';
    if (!active) {
      return;
    }
    const top = active.offsetTop;
    const first = highlight.dataset.top === undefined;
    if (highlight.dataset.top === String(top)) {
      return;
    }
    highlight.dataset.top = String(top);
    highlight.style.transition = first ? 'none' : '';
    highlight.style.transform = `translateY(${top}px)`;
    if (first) {
      highlight.getBoundingClientRect();
      highlight.style.transition = '';
    }
  }

  placeRing_(picker) {
    const ring = picker.querySelector('.visual-ring');
    const selected = picker.querySelector('.visual-option.selected .visual-frame');
    ring.style.opacity = selected ? '1' : '0';
    if (!selected) {
      return;
    }
    const pickerRect = picker.getBoundingClientRect();
    const rect = selected.getBoundingClientRect();
    const x = rect.left - pickerRect.left;
    const y = rect.top - pickerRect.top;
    const key = `${x},${y}`;
    if (ring.dataset.at === key) {
      return;
    }
    const first = ring.dataset.at === undefined;
    ring.dataset.at = key;
    ring.style.width = `${selected.offsetWidth}px`;
    ring.style.height = `${selected.offsetHeight}px`;
    ring.style.transition = first ? 'none' : '';
    ring.style.transform = `translate(${x}px, ${y}px)`;
    if (first) {
      ring.getBoundingClientRect();
      ring.style.transition = '';
    }
  }

  // ---- 操作 ----

  set_(key, value) {
    // 先在页面上改（控件马上动），浏览器改完会发 settings-changed 对上。
    this.settings_ = {...this.settings_, [key]: value};
    chrome.send('setSetting', [key, value]);
  }

  toggleUtilityButton_(button) {
    const s = this.settings_;
    this.settings_ = {
      ...s,
      utilityButtons: s.utilityButtons.map(
          (other) => other.id === button.id ? {...other, hidden: !other.hidden} : other),
    };
    chrome.send('setSetting', ['utilityButtonHidden', {id: button.id, hidden: !button.hidden}]);
  }

  async select_(pane) {
    const ids = PANES.map((candidate) => candidate.id);
    // 子页算在它的上一级后面：进去时从下面来，回来时从上面来。
    const order = (id) => ids.indexOf(railPane(id)) + (railPane(id) !== id ? 0.5 : 0);
    const direction = order(pane) >= order(this.pane_) ? 1 : -1;
    if (pane === 'importData') {
      this.loadImport_();
    }
    const changed = pane !== this.pane_ || this.query_;
    this.query_ = '';
    this.pane_ = pane;
    this.updateTitle_();
    history.replaceState(null, '', `/${pane}`);
    try {
      localStorage.setItem(PANE_KEY, pane);
    } catch (e) {
      // 不记也行。
    }
    const content = this.querySelector('.content');
    await this.updateComplete;
    if (changed) {
      if (content) {
        content.scrollTop = 0;
      }
      this.revealContent_(direction);
    }
  }

  async clearHistory_(event) {
    const id = await showMenu(CLEAR_RANGES.map((range) => ({id: range.id, title: range.title})),
                              {element: event.currentTarget});
    const range = CLEAR_RANGES.find((candidate) => candidate.id === id);
    if (range && await confirm({icon: 'trash.fill', title: range.confirm,
                                message: L('history.clear.message'), confirmTitle: L('common.clear')})) {
      chrome.send('clearHistory', [range.id]);
    }
  }

  // ---- 设置项 ----

  groups_(pane) {
    const s = this.settings_;
    switch (pane) {
      case 'general':
        return this.general_(s);
      case 'appearance':
        return this.appearance_(s);
      case 'newTab':
        return this.newTab_(s);
      case 'tabs':
        return this.tabs_(s);
      case 'performance':
        return this.performance_(s);
      case 'media':
        return this.media_(s);
      case 'downloads':
        return this.downloads_(s);
      case 'privacy':
        return this.privacy_(s);
      case 'extensions':
        return [group('manageExtensions', '', [
          entry('manageExtensions', L('settings.extensions.manage.title'),
                detailButton(L('common.open'), 'puzzlepiece', () => this.select_('extensions')),
                {detail: L('settings.pane.extensions.summary'), keywords: L('settings.extensions.manage.keywords')}),
        ])];
      case 'ai':
        return this.ai_();
      case 'developer':
        return this.developer_(s);
      case 'importData':
        return this.importData_();
    }
    return [];
  }

  // 「AI 智能体」设置：内置助手、模型服务或本机 Claude Code / Codex、外部智能体（MCP）及权限。
  ai_() {
    const a = this.assistant_ || {};
    const providers = a.providers || [];
    const provider = providers.find((candidate) => candidate.id === a.provider);
    const setAssistant = (key, value) => {
      this.assistant_ = {...a, [key]: value};
      chrome.send('setAssistantSetting', [key, value]);
    };
    const saveKey = async () => {
      this.assistant_ = await sendWithPromise('setAssistantKey', this.keyDraft_.trim());
      this.keyDraft_ = '';
    };
    const chooseModel = async (event) => {
      const button = event.currentTarget;
      this.modelNote_ = L('settings.ai.assistant.local.checking');
      const result = await sendWithPromise('fetchAssistantModels');
      if (result.error) {
        this.modelNote_ = L('settings.ai.assistant.model.failed', {reason: result.error});
        return;
      }
      this.modelNote_ = '';
      const models = [...(result.models || [])].sort((x, y) => (y.tools !== false) - (x.tools !== false) ||
                                                       x.id.localeCompare(y.id));
      if (!models.length) {
        this.modelNote_ = L('settings.ai.assistant.model.none');
        return;
      }
      const id = await showMenu(models.slice(0, 200).map((model) => ({
        id: model.id,
        title: model.tools === false ? `${model.id} · ${L('settings.ai.assistant.model.noTools')}` : model.id,
        checked: model.id === a.model,
      })), {element: button});
      if (id) {
        setAssistant('model', id);
      }
    };
    const textInput = (value, placeholder, onCommit, {type = 'text'} = {}) => html`
      <input class="text-input" type=${type} .value=${value || ''} placeholder=${placeholder}
          spellcheck="false" autocomplete="off"
          @change=${(event) => onCommit(event.target.value.trim())}
          @keydown=${(event) => event.key === 'Enter' && event.target.blur()}>`;
    const service = [
      entry('assistantProvider', L('settings.ai.assistant.provider.title'),
            menuControl(a.provider || '', [
              {value: '', title: L('assistant.backend.notSet')},
              ...providers.map((candidate) => ({value: candidate.id, title: candidate.title})),
            ], (value) => setAssistant('provider', value)),
            {detail: provider?.local ? L('settings.ai.assistant.provider.local', {address: provider.baseUrl}) :
                                       L('settings.ai.assistant.backend.chatCompletions'),
             keywords: L('settings.ai.assistant.provider.keywords')}),
    ];
    if (provider?.custom) {
      service.push(entry('assistantCustomUrl', L('settings.ai.assistant.custom.url'),
                         textInput(a.customUrl, L('settings.ai.assistant.custom.urlPlaceholder'),
                                   (value) => setAssistant('customUrl', value)),
                         {detail: L('settings.ai.assistant.custom.detail'),
                          keywords: L('settings.ai.assistant.custom.keywords')}));
    }
    if (provider && (provider.requiresKey || provider.custom)) {
      service.push(entry('assistantKey', L('settings.ai.assistant.key.title'), html`
        <div class="inline-controls">
          ${textInput(this.keyDraft_, a.hasKey ? L('settings.ai.assistant.key.saved') :
                                                L('settings.ai.assistant.key.placeholder'),
                      (value) => this.keyDraft_ = value, {type: 'password'})}
          ${this.keyDraft_ ? detailButton(L('common.save'), '', saveKey) : nothing}
          ${a.hasKey && !this.keyDraft_ ? detailButton(L('settings.ai.assistant.key.remove'), '', async () => {
            this.keyDraft_ = '';
            this.assistant_ = await sendWithPromise('setAssistantKey', '');
          }, {danger: true}) : nothing}
        </div>`,
        {detail: a.hasKey ? L('settings.ai.assistant.key.saved') : L('settings.ai.assistant.key.detail'),
         keywords: L('settings.ai.assistant.key.keywords')}));
      if (provider.keyPage) {
        service.push(entry('assistantKeyPage', L('settings.ai.assistant.key.get'),
                           detailButton(L('common.openEllipsis'), 'arrow.up.right.square',
                                        () => openUrl(provider.keyPage, 'foreground')),
                           {keywords: L('settings.ai.assistant.key.keywords')}));
      }
    }
    if (provider) {
      service.push(entry('assistantModel', L('settings.ai.assistant.model.title'), html`
        <div class="inline-controls">
          ${textInput(a.model, L('settings.ai.assistant.model.placeholder'),
                      (value) => setAssistant('model', value))}
          ${detailButton(L('settings.ai.assistant.model.choose'), '', chooseModel)}
        </div>`,
        {detail: this.modelNote_ || L('settings.ai.assistant.model.detail'),
         keywords: L('settings.ai.assistant.model.keywords')}));
    }
    const backend = a.backend || 'chat';
    const local = backend !== 'chat';
    const localName = backend === 'claudeCode' ? 'Claude Code' : 'Codex';
    // 助手总开关关闭时，下方选项置灰，模型服务与本机智能体分组隐藏。
    const enabled = a.enabled !== false;
    const assistant = [
      entry('assistantEnabled', L('settings.ai.assistant.enabled.title'),
            switchControl(enabled, (value) => setAssistant('enabled', value),
                          {label: L('settings.ai.assistant.enabled.title')}),
            {detail: L('settings.ai.assistant.enabled.detail'),
             keywords: L('settings.ai.assistant.enabled.keywords')}),
      entry('assistantBackend', L('settings.ai.assistant.backend.title'),
            menuControl(backend, [
              {value: 'chat', title: L('assistant.backend.chatCompletions')},
              {value: 'claudeCode', title: 'Claude Code'},
              {value: 'codex', title: 'Codex'},
            ], (value) => setAssistant('backend', value)),
            {disabled: !enabled,
             detail: local ? L('settings.ai.assistant.backend.local', {name: localName}) :
                             L('settings.ai.assistant.backend.chatCompletions'),
             keywords: L('settings.ai.assistant.backend.keywords')}),
      entry('assistantApproval', L('settings.ai.assistant.approval.title'),
            menuControl(a.mode || 'ask', [
              {value: 'ask', title: L('assistant.mode.ask')},
              {value: 'agent', title: L('assistant.mode.agent')},
              {value: 'auto', title: L('assistant.mode.auto')},
            ], (value) => setAssistant('mode', value)),
            {disabled: !enabled,
             detail: a.mode === 'agent' ? L('settings.ai.assistant.approval.agentDetail') :
                     a.mode === 'auto' ? L('settings.ai.assistant.approval.autoDetail') :
                                         L('settings.ai.assistant.approval.askDetail'),
             keywords: L('settings.ai.assistant.approval.keywords')}),
      entry('assistantPanelStyle', L('settings.ai.assistant.panelStyle.title'),
            menuControl(a.panelStyle || 'float', [
              {value: 'float', title: L('settings.ai.assistant.panelStyle.floating')},
              {value: 'dock', title: L('settings.ai.assistant.panelStyle.docked')},
            ], (value) => setAssistant('panelStyle', value)),
            {disabled: !enabled,
             detail: a.panelStyle === 'dock' ? L('settings.ai.assistant.panelStyle.dockedDetail') :
                                               L('settings.ai.assistant.panelStyle.floatingDetail'),
             keywords: L('settings.ai.assistant.panelStyle.keywords')}),
    ];
    const groups = [group('assistant', L('settings.ai.assistant.title'), assistant,
                          {footer: L('settings.ai.assistant.footer')})];
    if (enabled && local) {
      groups.push(group('assistantLocal', localName, this.localAgent_(a, backend, localName, setAssistant, textInput),
                        {footer: backend === 'claudeCode' ? L('settings.ai.assistant.local.claudeFooter') :
                                                            L('settings.ai.assistant.local.codexFooter')}));
    } else if (enabled) {
      groups.push(group('assistantService', L('settings.ai.assistant.service.title'), service,
                        {footer: L('settings.ai.assistant.service.footer')}));
    }
    const clients = a.agentClients || [];
    groups.push(group('agentControl', L('settings.ai.control.title'), [
      entry('agentControl', L('settings.ai.agentControl.title'),
            switchControl(!!a.allowExternal, (value) => setAssistant('allowExternal', value),
                          {label: L('settings.ai.agentControl.title')}),
            {detail: a.agentError ? L('settings.ai.agentControl.failed', {reason: a.agentError}) :
                                    L('settings.ai.agentControl.detail'),
             keywords: L('settings.ai.agentControl.keywords')}),
      entry('agentConnect', L('settings.ai.agentConnect.title'), html`
        <div class="inline-controls">
          ${detailButton(L('settings.ai.agentConnect.copy'), 'doc.on.doc', () => copyText(a.connectClaude || ''))}
          ${detailButton(L('settings.ai.agentConnect.copyJSON'), '', () => copyText(a.connectJson || ''))}
        </div>`,
            {detail: L('settings.ai.agentConnect.detail', {command: a.connectCommand || ''}),
             keywords: L('settings.ai.agentConnect.keywords')}),
      entry('agentSessions', L('settings.ai.agentSessions.title'),
            detailButton(L('settings.ai.agentSessions.disconnect'), '', () => chrome.send('disconnectAgents'),
                         {disabled: !clients.length}),
            {detail: clients.length ? clients.join(L('common.listSeparator')) : L('settings.ai.agentSessions.none'),
             keywords: L('settings.ai.agentSessions.keywords')}),
    ], {footer: L('settings.ai.control.footer')}));
    groups.push(
      group('agentPermissions', L('settings.ai.permissions.title'), [
        entry('agentUploads', L('settings.ai.agentUploads.title'),
              switchControl(a.allowUploads !== false, (value) => setAssistant('allowUploads', value),
                            {label: L('settings.ai.agentUploads.title')}),
              {detail: L('settings.ai.agentUploads.detail'), keywords: L('settings.ai.agentUploads.keywords')}),
        entry('agentPrivateTabs', L('settings.ai.agentPrivateTabs.title'),
              switchControl(a.allowPrivateTabs !== false, (value) => setAssistant('allowPrivateTabs', value),
                            {label: L('settings.ai.agentPrivateTabs.title')}),
              {detail: L('settings.ai.agentPrivateTabs.detail'), keywords: L('settings.ai.agentPrivateTabs.keywords')}),
        entry('agentScripts', L('settings.ai.agentScripts.title'),
              switchControl(!!a.allowScripts, (value) => setAssistant('allowScripts', value),
                            {label: L('settings.ai.agentScripts.title')}),
              {detail: L('settings.ai.agentScripts.detail'), keywords: L('settings.ai.agentScripts.keywords')}),
      ], {footer: L('settings.ai.permissions.footer')}));
    return groups;
  }

  // 本机的 Claude Code / Codex：找没找到、登没登录，可执行文件和模型（空着是自动找、用它的默认）。
  localAgent_(a, backend, name, setAssistant, textInput) {
    const info = a.local;
    if (!info && this.localChecked_ !== backend) {
      // 首次展示本机智能体分组时自动检测一次。
      this.localChecked_ = backend;
      chrome.send('checkLocalAgent');
    }
    let status = L('settings.ai.assistant.local.checking');
    if (info && !info.checking) {
      if (!info.path) {
        status = L('settings.ai.assistant.local.missing', {name});
      } else if (info.loggedIn === true) {
        status = L('settings.ai.assistant.local.ready', {path: info.path});
      } else if (info.loggedIn === false) {
        status = L('settings.ai.assistant.local.loggedOut',
                   {path: info.path, command: backend === 'claudeCode' ? 'claude' : 'codex login'});
      } else {
        status = L('settings.ai.assistant.local.found', {path: info.path});
      }
    }
    const keywords = L('settings.ai.assistant.local.keywords');
    return [
      entry('assistantLocalStatus', L('settings.ai.assistant.local.status'),
            detailButton(L('settings.ai.assistant.local.check'), 'arrow.clockwise',
                         () => chrome.send('checkLocalAgent'), {disabled: !info || info.checking}),
            {detail: status, keywords}),
      entry('assistantLocalPath', L('settings.ai.assistant.local.path'),
            textInput(a.localPath, L('settings.ai.assistant.local.pathPlaceholder'),
                      (value) => setAssistant('localPath', value)),
            {detail: L('settings.ai.assistant.local.pathDetail', {name: backend === 'claudeCode' ? 'claude' : 'codex'}),
             keywords}),
      entry('assistantLocalModel', L('settings.ai.assistant.local.model'),
            textInput(a.localModel, L('settings.ai.assistant.local.modelPlaceholder'),
                      (value) => setAssistant('localModel', value)),
            {detail: L('settings.ai.assistant.local.modelDetail', {name}), keywords}),
    ];
  }

  general_(s) {
    const defaultDetail = {
      default: L('settings.general.defaultBrowser.isDefault'),
      notDefault: L('settings.general.defaultBrowser.notDefault'),
      unknown: L('settings.general.defaultBrowser.unknown'),
      checking: L('settings.general.defaultBrowser.checking'),
    }[s.defaultBrowser.state];
    const defaultControl = s.defaultBrowser.state === 'default' ?
        html`<span class="value-text">${icon('checkmark.circle', 12)}${L('settings.general.defaultBrowser.title')}</span>` :
        detailButton(s.defaultBrowser.state === 'checking' ? L('settings.general.defaultBrowser.setting') : L('settings.general.defaultBrowser.set'), '',
                     () => chrome.send('setDefaultBrowser'),
                     {disabled: !s.defaultBrowser.canSet || s.defaultBrowser.state === 'checking'});
    const startupOptions = [
      {value: 'last', title: L('startup.restoreSession')},
      {value: 'newTab', title: L('startup.newWindow')},
    ];
    if (s.startup === 'urls' || s.startup === 'lastAndUrls') {
      startupOptions.push({value: s.startup, title: L('startup.specificPages'), disabled: true});
    }
    const externalLinkOptions = [
      {value: 'miniWindow', title: L('externalLinks.miniWindow')},
      {value: 'browserWindow', title: L('externalLinks.browserWindow')},
    ];
    return [
      group('defaultBrowser', '', [
        entry('defaultBrowser', L('settings.general.defaultBrowser.title'), defaultControl,
              {detail: defaultDetail, keywords: L('settings.general.defaultBrowser.keywords')}),
      ]),
      group('externalLinks', L('settings.general.externalLinks.title'), [
        entry('externalLinks', L('settings.general.externalLinks.label'),
              menuControl(s.externalLinks, externalLinkOptions, (value) => this.set_('externalLinks', value)),
              {detail: s.externalLinks === 'browserWindow' ? L('settings.general.externalLinks.browserWindowDetail') :
                                                             L('settings.general.externalLinks.miniWindowDetail'),
               keywords: L('settings.general.externalLinks.keywords')}),
      ]),
      group('search', L('settings.general.search.title'), [
        entry('searchEngine', L('settings.general.searchEngine.title'),
              menuControl(s.searchEngine, s.searchEngines.map((engine) => ({value: engine.id, title: engine.name})),
                          (value) => this.set_('searchEngine', value), {disabled: s.searchEngineManaged}),
              {detail: s.searchEngineManaged ? L('settings.general.searchEngine.managed') : L('settings.general.searchEngine.detail'),
               keywords: L('settings.general.searchEngine.keywords')}),
        entry('searchSuggest', L('settings.general.searchSuggestions.title'),
              switchControl(s.searchSuggest, (value) => this.set_('searchSuggest', value), {label: L('settings.general.searchSuggestions.title')}),
              {detail: L('settings.general.searchSuggestions.detail'),
               keywords: L('settings.general.searchSuggestions.keywords')}),
        entry('manageSearchEngines', L('settings.general.manageSearchEngines.title'),
              detailButton(L('common.manageEllipsis'), '', () => openUrl('starry://browser-settings/searchEngines', 'foreground')),
              {detail: L('settings.general.manageSearchEngines.detail'), keywords: L('settings.general.manageSearchEngines.keywords')}),
      ]),
      group('startup', L('settings.general.startup.title'), [
        entry('startup', L('settings.general.startup.label'),
              menuControl(s.startup, startupOptions, (value) => this.set_('startup', value),
                          {disabled: s.startupManaged}),
              {keywords: L('settings.general.startup.keywords')}),
        // 恢复上次的窗口和标签时（也包括重新启动、崩溃后恢复），没有显示的标签先休眠。
        entry('sleepRestoredTabs', L('settings.general.sleepRestoredTabs.title'),
              switchControl(s.sleepRestoredTabs, (value) => this.set_('sleepRestoredTabs', value),
                            {label: L('settings.general.sleepRestoredTabs.title')}),
              {detail: L('settings.general.sleepRestoredTabs.detail'),
               keywords: L('settings.general.sleepRestoredTabs.keywords')}),
      ]),
      group('region', L('settings.general.region.title'), [
        // 修改界面语言或网页时区后提示重启生效。
        entry('interfaceLanguage', L('settings.general.interfaceLanguage.title'),
              menuControl(s.interfaceLanguage,
                          s.interfaceLanguages.map((language) => ({value: language.id, title: language.name})),
                          (value) => this.set_('interfaceLanguage', value)),
              {detail: L('settings.general.interfaceLanguage.detail'),
               keywords: L('settings.general.interfaceLanguage.keywords')}),
        entry('webLanguage', L('settings.general.webLanguage.title'),
              menuControl(s.webLanguage, s.webLanguages.map((language) => ({value: language.id, title: language.name})),
                          (value) => this.set_('webLanguage', value)),
              {detail: L('settings.general.webLanguage.detail'), keywords: L('settings.general.webLanguage.keywords')}),
        entry('webTimeZone', L('settings.general.webTimeZone.title'),
              timeZoneControl(s, (value) => this.set_('webTimeZone', value)),
              {detail: L('settings.general.webTimeZone.detail'), keywords: L('settings.general.webTimeZone.keywords')}),
        ...(s.interfaceLanguageNeedsRelaunch || s.webTimeZoneNeedsRelaunch ? [
          entry('relaunch', L('settings.general.relaunch.title'),
                detailButton(L('common.relaunch'), 'arrow.clockwise', () => chrome.send('relaunch')),
                {detail: L('settings.general.relaunch.detail'), keywords: L('settings.general.relaunch.keywords')}),
        ] : []),
        entry('moreLanguages', L('settings.general.moreLanguages.title'),
              detailButton(L('common.openEllipsis'), '', () => openUrl('starry://browser-settings/languages', 'foreground')),
              {detail: L('settings.general.moreLanguages.detail'), keywords: L('settings.general.moreLanguages.keywords')}),
      ]),
      group('import', L('settings.general.import.title'), [
        entry('importData', L('settings.import.title'),
              detailButton(L('settings.import.open'), 'square.and.arrow.down', () => this.select_('importData')),
              {detail: L('settings.general.import.detail'), keywords: L('settings.general.import.keywords')}),
        entry('onboarding', L('settings.general.onboarding.title'),
              detailButton(L('settings.general.onboarding.button'), '', () => chrome.send('startOnboarding')),
              {detail: L('settings.general.onboarding.detail'), keywords: L('settings.general.onboarding.keywords')}),
      ]),
      ...this.updatesGroup_(s),
    ];
  }

  // 软件更新分组：通过 GitHub Releases 检查与下载，内置中国大陆多级镜像自动回退。
  updatesGroup_(s) {
    const u = this.update_;
    if (!u) {
      return [];
    }
    const keywords = L('settings.general.updates.keywords');
    const isLinuxPkg = u.installKind === 'linuxDeb' || u.installKind === 'linuxRpm';
    const routeTag = u.activeRouteHost && u.activeRouteHost !== 'github.com'
        ? L('settings.general.updates.viaMirror', {host: u.activeRouteHost})
        : '';
    const lastCheckedText = u.lastChecked
        ? L('settings.general.updates.lastChecked', {time: relativeTime(u.lastChecked)})
        : '';

    let leading = 'arrow.triangle.2.circlepath';
    let tone = '';
    let title = L('settings.general.updates.version', {version: u.currentVersion || s.version});
    let detail = [lastCheckedText, routeTag].filter(Boolean).join(' · ');
    let control = detailButton(L('settings.general.updates.check'), 'arrow.clockwise',
                               () => chrome.send('checkForUpdates'));

    switch (u.status) {
      case 'checking':
        title = L('settings.general.updates.checking');
        detail = [L('settings.general.updates.currentVersion', {version: u.currentVersion}), routeTag]
            .filter(Boolean).join(' · ');
        control = html`<span class="spinner" aria-hidden="true"></span>`;
        break;
      case 'upToDate':
        leading = 'checkmark.circle.fill';
        tone = 'success';
        title = L('settings.general.updates.upToDate');
        detail = [L('settings.general.updates.currentVersion', {version: u.currentVersion}),
                  lastCheckedText, routeTag].filter(Boolean).join(' · ');
        break;
      case 'available':
        leading = 'arrow.down.circle';
        title = L('settings.general.updates.available', {version: u.availableVersion});
        if (u.installKind === 'macReadOnly') {
          detail = L('settings.general.updates.macReadOnly');
        } else if (!u.canAutoInstall) {
          detail = L('settings.general.updates.manualDetail');
        } else {
          detail = [L('settings.general.updates.currentVersion', {version: u.currentVersion}), routeTag]
              .filter(Boolean).join(' · ');
        }
        control = u.canAutoInstall ? html`
          <div class="inline-controls">
            ${detailButton(L('settings.general.updates.releasePage'), 'arrow.up.right.square',
                           () => openUrl(u.releaseUrl, 'foreground'))}
            ${detailButton(L('settings.general.updates.download'), 'arrow.down.circle',
                           () => chrome.send('downloadUpdate'), {prominent: true})}
          </div>` :
          detailButton(L('settings.general.updates.openDownload'), 'arrow.up.right.square',
                       () => openUrl(u.releaseUrl, 'foreground'), {prominent: true});
        break;
      case 'downloading': {
        leading = 'arrow.down.circle';
        title = L('settings.general.updates.downloading', {version: u.availableVersion});
        const percent = u.totalBytes > 0 ? Math.min(100, Math.round(u.bytesReceived / u.totalBytes * 100)) : null;
        const transferred = u.totalBytes > 0 ? `${bytes(u.bytesReceived)} / ${bytes(u.totalBytes)}` :
                                               bytes(u.bytesReceived);
        detail = [transferred, percent !== null ? `${percent}%` : '', routeTag].filter(Boolean).join(' · ');
        control = detailButton(L('common.cancel'), '', () => chrome.send('cancelUpdateDownload'));
        break;
      }
      case 'verifying':
        title = L('settings.general.updates.verifying');
        detail = L('settings.general.updates.version', {version: u.availableVersion});
        control = html`<span class="spinner" aria-hidden="true"></span>`;
        break;
      case 'ready':
        leading = 'checkmark.circle.fill';
        tone = 'success';
        title = L('settings.general.updates.ready', {version: u.availableVersion});
        detail = isLinuxPkg ? L('settings.general.updates.readyLinuxDetail') :
                              L('settings.general.updates.readyDetail');
        control = html`
          <div class="inline-controls">
            ${isLinuxPkg ? detailButton(L('settings.general.updates.reveal'), 'folder',
                                        () => chrome.send('revealUpdatePackage')) : nothing}
            ${detailButton(isLinuxPkg ? L('updates.action.install') : L('updates.action.relaunch'),
                           isLinuxPkg ? 'arrow.down.circle' : 'arrow.clockwise',
                           () => chrome.send('applyUpdate'), {prominent: true})}
          </div>`;
        break;
      case 'error':
        leading = 'exclamationmark.triangle.fill';
        tone = 'warning';
        title = L('settings.general.updates.failed');
        detail = u.error || '';
        control = html`
          <div class="inline-controls">
            ${u.releaseUrl ? detailButton(L('settings.general.updates.releasePage'), 'arrow.up.right.square',
                                          () => openUrl(u.releaseUrl, 'foreground')) : nothing}
            ${detailButton(L('settings.general.updates.check'), 'arrow.clockwise',
                           () => chrome.send('checkForUpdates'))}
          </div>`;
        break;
    }

    const entries = [
      entry('updateStatus', title, control, {detail, leading, tone, keywords}),
    ];

    if (u.status === 'downloading' || u.status === 'verifying') {
      const indeterminate = u.status === 'verifying' || !(u.totalBytes > 0);
      const ratio = indeterminate ? 0 : Math.min(1, Math.max(0, u.bytesReceived / u.totalBytes));
      entries.push(entry('updateProgress', '', html`
        <div class="update-progress ${indeterminate ? 'indeterminate' : ''}">
          <div class="update-progress-fill" style="--progress: ${ratio}"></div>
        </div>`, {layout: 'block'}));
    }

    if (u.releaseNotes && (u.status === 'available' || u.status === 'downloading' ||
                           u.status === 'verifying' || u.status === 'ready')) {
      entries.push(entry('updateNotes', L('settings.general.updates.notesTitle'), html`
        <div class="update-notes">${u.releaseNotes}</div>`,
        {layout: 'block', keywords}));
    }

    const modeDetails = {
      auto: L('settings.general.updates.mode.autoDetail'),
      notify: L('settings.general.updates.mode.notifyDetail'),
      off: L('settings.general.updates.mode.offDetail'),
    };
    entries.push(entry('autoUpdateMode', L('settings.general.updates.mode.title'),
                       menuControl(u.mode, [
                         {value: 'auto', title: L('settings.general.updates.mode.auto')},
                         {value: 'notify', title: L('settings.general.updates.mode.notify')},
                         {value: 'off', title: L('settings.general.updates.mode.off')},
                       ], (value) => {
                         this.update_ = {...u, mode: value};
                         chrome.send('setAutoUpdateMode', [value]);
                       }),
                       {detail: modeDetails[u.mode] || '', keywords}));

    const mirrorValue = this.mirrorDraft_ !== null ? this.mirrorDraft_ : (u.customMirror || '');
    const commitMirror = async (raw) => {
      const result = await sendWithPromise('setUpdateMirror', raw.trim());
      if (result && result.error) {
        this.mirrorError_ = result.error;
      } else {
        this.mirrorError_ = '';
        this.mirrorDraft_ = null;
      }
    };
    entries.push(entry('updateMirror', L('settings.general.updates.mirror.title'), html`
      <div class="inline-controls">
        <input class="text-input" type="url" .value=${mirrorValue}
            placeholder=${L('settings.general.updates.mirror.placeholder')}
            spellcheck="false" autocomplete="off"
            @input=${(event) => {
              this.mirrorDraft_ = event.target.value;
              this.mirrorError_ = '';
            }}
            @change=${(event) => commitMirror(event.target.value)}
            @keydown=${(event) => event.key === 'Enter' && event.target.blur()}>
        ${mirrorValue ? detailButton(L('common.clear'), '', () => commitMirror('')) : nothing}
      </div>`,
      {detail: this.mirrorError_ || L('settings.general.updates.mirror.detail'),
       tone: this.mirrorError_ ? 'warning' : '',
       keywords: `${keywords} ${L('settings.general.updates.mirror.keywords')}`}));

    return [group('updates', L('settings.general.updates.title'), entries,
                  {footer: L('settings.general.updates.footer')})];
  }

  async openUpdates_() {
    await this.select_('general');
    const target = this.querySelector('[data-key="updates"]');
    if (target) {
      target.scrollIntoView({behavior: reduceMotion() ? 'auto' : 'smooth', block: 'start'});
    }
  }

  appearance_(s) {
    const schemes = [
      {value: 'system', title: L('appearance.system'), preview: html`${mockWindow()}${mockWindow({dark: true, half: true})}`},
      {value: 'light', title: L('appearance.light'), preview: mockWindow()},
      {value: 'dark', title: L('appearance.dark'), preview: mockWindow({dark: true})},
    ];
    const dark = matchMedia('(prefers-color-scheme: dark)').matches;
    const sidebar = s.sidebarLayout !== 'top';
    // 地址栏、扩展按钮实际在不在工具栏里（顶部标签布局总在）。
    const addressInToolbar = !sidebar || s.addressBarOnTop;
    const mock = {dark, addressOnTop: s.addressBarOnTop, compact: s.compactAddressBar};
    const layouts = [
      {value: 'docked', title: L('layout.dockedSidebar'), preview: mockWindow(mock)},
      {value: 'floating', title: L('layout.floatingSidebar'), preview: mockWindow({...mock, floating: true})},
      {value: 'top', title: L('layout.top'), preview: mockWindow({...mock, top: true})},
    ];
    const onTopDetail = (on) => !sidebar ? L('settings.appearance.alwaysOnWithTopTabs') :
        (on ? L('settings.appearance.inToolbar') : L('settings.appearance.inSidebar'));
    // 侧栏宽度：整行的滑块，轨道上标出默认宽度；双击滑块恢复默认（同双击侧栏的右边缘）。
    // 「恢复默认」一直占着位置，出现、消失时滑块不变长变短。
    const width = s.sidebarWidth;
    const atDefault = width === s.sidebarWidthDefault;
    const fraction = (value) => (value - s.sidebarWidthMin) / (s.sidebarWidthMax - s.sidebarWidthMin);
    const resetWidth = () => this.set_('sidebarWidth', s.sidebarWidthDefault);
    const widthControl = html`
      <div class="width-control">
        <div class="width-slider" style="--fill: ${fraction(width)}; --default: ${fraction(s.sidebarWidthDefault)}">
          <input type="range" min=${s.sidebarWidthMin} max=${s.sidebarWidthMax} step="1" .value=${String(width)}
              aria-label=${L('settings.appearance.sidebarWidth.title')}
              @input=${(event) => this.set_('sidebarWidth', Number(event.target.value))} @dblclick=${resetWidth}>
          <span class="width-default"></span>
        </div>
        <span class="width-value">${L('settings.appearance.sidebarWidth.value', {width})}</span>
        <span class="width-reset ${atDefault ? 'hidden' : ''}">
          ${detailButton(L('common.restoreDefault'), 'arrow.clockwise', resetWidth, {disabled: atDefault})}
        </span>
      </div>`;
    const buttons = html`
      <div class="button-picker">
        ${s.utilityButtons.map((button) => html`
          <button class="button-chip ${button.hidden ? 'hidden' : ''}" aria-pressed=${button.hidden ? 'false' : 'true'}
              title=${button.hidden ? L('settings.appearance.utilityButtons.show', {name: button.title}) :
                                   L('settings.appearance.utilityButtons.hide', {name: button.title})}
              @click=${() => {
                this.settings_ = {
                  ...s,
                  utilityButtons: s.utilityButtons.map(
                      (other) => other.id === button.id ? {...other, hidden: !other.hidden} : other),
                };
                chrome.send('setSetting', ['utilityButtonHidden', {id: button.id, hidden: !button.hidden}]);
              }}>
            <span class="chip-icon">
              ${icon(BUTTON_ICONS[button.id] || 'circle', 15)}
              <span class="chip-badge">${icon('checkmark', 8)}</span>
            </span>
            <span class="chip-title">${button.title}</span>
            <span class="chip-state">${button.hidden ? L('settings.appearance.utilityButtons.hidden') : L('settings.appearance.utilityButtons.shown')}</span>
          </button>`)}
      </div>`;
    return [
      group('theme', L('settings.appearance.theme.title'), [
        entry('appearance', L('settings.appearance.appearance.title'),
              visualPicker('scheme', s.colorScheme, schemes, (value) => this.set_('colorScheme', value)),
              {layout: 'block', keywords: L('settings.appearance.appearance.keywords')}),
        entry('tintFromWebsite', L('settings.appearance.tintFromWebsite.title'),
              switchControl(s.tintFromWebsite, (value) => this.set_('tintFromWebsite', value),
                            {label: L('settings.appearance.tintFromWebsite.title')}),
              {detail: L('settings.appearance.tintFromWebsite.detail'),
               keywords: L('settings.appearance.tintFromWebsite.keywords')}),
      ]),
      group('layout', L('settings.appearance.layout.title'), [
        entry('tabLayout', L('settings.appearance.tabLayout.title'),
              visualPicker('layout', s.sidebarLayout, layouts, (value) => this.set_('sidebarLayout', value)),
              {layout: 'block',
               detail: s.sidebarLayout === 'floating' ? L('settings.appearance.sidebarAutoHide.detail') : '',
               keywords: `${L('settings.appearance.tabLayout.keywords')} ${L('settings.appearance.sidebarAutoHide.keywords')}`}),
        // 仅适用于侧栏布局的选项在顶部标签模式下置灰并展示原因。
        entry('sidebarWidth', L('settings.appearance.sidebarWidth.title'), widthControl,
              {layout: 'block', disabled: !sidebar,
               detail: sidebar ? L('settings.appearance.sidebarWidth.detail') : L('settings.appearance.sidebarOnly'),
               keywords: L('settings.appearance.sidebarWidth.keywords')}),
        ...(s.hasWindowControls ? [
          entry('windowsControlsStyle', L('settings.appearance.windowControls.title'),
                menuControl(s.windowsControlsStyle, [
                  {value: 'glass', title: L('settings.appearance.windowControls.glass')},
                  {value: 'rail', title: L('settings.appearance.windowControls.rail')},
                ], (value) => this.set_('windowsControlsStyle', value)),
                {detail: s.windowsControlsStyle === 'rail'
                     ? L('settings.appearance.windowControls.railDetail')
                     : L('settings.appearance.windowControls.glassDetail'),
                 keywords: L('settings.appearance.windowControls.keywords')}),
          entry('windowsControlsAutoHide', L('settings.appearance.windowControlsAutoHide.title'),
                switchControl(s.windowsControlsStyle === 'glass' && s.windowsControlsAutoHide,
                              (value) => this.set_('windowsControlsAutoHide', value),
                              {label: L('settings.appearance.windowControlsAutoHide.title'),
                               disabled: s.windowsControlsStyle !== 'glass'}),
                {disabled: s.windowsControlsStyle !== 'glass',
                 detail: s.windowsControlsStyle === 'glass'
                     ? L('settings.appearance.windowControlsAutoHide.detail')
                     : L('settings.appearance.windowControlsAutoHide.glassOnly'),
                 keywords: L('settings.appearance.windowControlsAutoHide.keywords')}),
        ] : []),
      ]),
      // 树状标签页：只在侧栏里显示（顶部标签布局下变灰）。
      group('treeTabs', L('settings.appearance.treeTabs.groupTitle'), [
        entry('treeTabs', L('settings.appearance.treeTabs.title'),
              switchControl(s.treeTabs, (value) => this.set_('treeTabs', value),
                            {label: L('settings.appearance.treeTabs.title')}),
              {disabled: !sidebar,
               detail: sidebar ? L('settings.appearance.treeTabs.detail') : L('settings.appearance.sidebarOnly'),
               keywords: L('settings.appearance.treeTabs.keywords')}),
        entry('treeTabsMaxDepth', L('settings.appearance.treeTabsMaxDepth.title'),
              menuControl(s.treeTabsMaxDepth, [3, 4, 6, 0].map((value) => ({
                value,
                title: value ? L('settings.appearance.treeTabsMaxDepth.levels', {count: value}) :
                               L('settings.appearance.treeTabsMaxDepth.unlimited'),
              })), (value) => this.set_('treeTabsMaxDepth', value), {disabled: !sidebar || !s.treeTabs}),
              {disabled: !sidebar || !s.treeTabs,
               detail: L('settings.appearance.treeTabsMaxDepth.detail'),
               keywords: L('settings.appearance.treeTabsMaxDepth.keywords')}),
        entry('treeTabsCloseParent', L('settings.appearance.treeTabsCloseParent.title'),
              menuControl(s.treeTabsCloseParent, [
                {value: 0, title: L('settings.appearance.treeTabsCloseParent.smart')},
                {value: 1, title: L('settings.appearance.treeTabsCloseParent.outdent')},
                {value: 2, title: L('settings.appearance.treeTabsCloseParent.closeAll')},
              ], (value) => this.set_('treeTabsCloseParent', value), {disabled: !sidebar || !s.treeTabs}),
              {disabled: !sidebar || !s.treeTabs,
               detail: [L('settings.appearance.treeTabsCloseParent.smartDetail'),
                        L('settings.appearance.treeTabsCloseParent.outdentDetail'),
                        L('settings.appearance.treeTabsCloseParent.closeAllDetail')][s.treeTabsCloseParent] ||
                       L('settings.appearance.treeTabsCloseParent.smartDetail'),
               keywords: L('settings.appearance.treeTabsCloseParent.keywords')}),
        entry('treeTabsAutoCollapse', L('settings.appearance.treeTabsAutoCollapse.title'),
              switchControl(s.treeTabsAutoCollapse, (value) => this.set_('treeTabsAutoCollapse', value),
                            {label: L('settings.appearance.treeTabsAutoCollapse.title'),
                             disabled: !sidebar || !s.treeTabs}),
              {disabled: !sidebar || !s.treeTabs,
               detail: L('settings.appearance.treeTabsAutoCollapse.detail'),
               keywords: L('settings.appearance.treeTabsAutoCollapse.keywords')}),
      ]),
      group('tabPreview', L('settings.appearance.tabPreview.title'), [
        entry('pinnedPeek', L('settings.appearance.pinnedPeek'),
              switchControl(s.pinnedPeek, (value) => this.set_('pinnedPeek', value),
                            {label: L('settings.appearance.pinnedPeek')})),
        entry('tabPreviewsEnabled', L('settings.appearance.tabPreviewsEnabled.title'),
              switchControl(s.tabPreviewsEnabled, (value) => this.set_('tabPreviewsEnabled', value),
                            {label: L('settings.appearance.tabPreviewsEnabled.title')}),
              {detail: L('settings.appearance.tabPreviewsEnabled.detail'),
               keywords: L('settings.appearance.tabPreviewsEnabled.keywords')}),
        entry('tabPreviewShowsUsage', L('settings.appearance.tabPreviewShowsUsage.title'),
              switchControl(s.tabPreviewShowsUsage, (value) => this.set_('tabPreviewShowsUsage', value),
                            {label: L('settings.appearance.tabPreviewShowsUsage.title')}),
              {disabled: !s.tabPreviewsEnabled,
               detail: L('settings.appearance.tabPreviewShowsUsage.detail'),
               keywords: L('settings.appearance.tabPreviewShowsUsage.keywords')}),
      ]),
      group('toolbar', L('settings.appearance.toolbar.title'), [
        entry('addressBarOnTop', L('settings.appearance.addressBarOnTop.title'),
              switchControl(sidebar ? s.addressBarOnTop : true, (value) => this.set_('addressBarOnTop', value),
                            {label: L('settings.appearance.addressBarOnTop.title')}),
              {disabled: !sidebar, detail: onTopDetail(s.addressBarOnTop),
               keywords: L('settings.appearance.addressBarOnTop.keywords')}),
        entry('extensionsOnTop', L('settings.appearance.extensionsOnTop.title'),
              switchControl(sidebar ? s.extensionsOnTop : true, (value) => this.set_('extensionsOnTop', value),
                            {label: L('settings.appearance.extensionsOnTop.title')}),
              {disabled: !sidebar, detail: onTopDetail(s.extensionsOnTop),
               keywords: L('settings.appearance.extensionsOnTop.keywords')}),
        entry('compactAddressBar', L('settings.appearance.compactAddressBar.title'),
              switchControl(addressInToolbar && s.compactAddressBar,
                            (value) => this.set_('compactAddressBar', value),
                            {label: L('settings.appearance.compactAddressBar.title')}),
              {disabled: !addressInToolbar,
               detail: !addressInToolbar ? L('settings.appearance.compactAddressBar.requiresTop') :
                       (s.tintFromWebsite ? L('settings.appearance.compactAddressBar.tinted') :
                                            L('settings.appearance.compactAddressBar.merged')),
               keywords: L('settings.appearance.compactAddressBar.keywords')}),
      ]),
      group('utilityButtons', sidebar ? L('settings.appearance.utilityButtons.sidebarTitle') :
                                        L('settings.appearance.utilityButtons.toolbarTitle'), [
        entry('utilityButtons', L('settings.appearance.utilityButtons.title'), buttons,
              {layout: 'block', detail: L('settings.appearance.utilityButtons.detail'),
               keywords: L('settings.appearance.utilityButtons.keywords')}),
      ]),
    ];
  }

  // 「新标签页」：风格、背景与页面小组件。
  newTab_(s) {
    const dark = matchMedia('(prefers-color-scheme: dark)').matches;
    const styles = [
      {value: 'minimal', title: L('newTab.style.minimal'), preview: newTabMock('minimal', dark)},
      {value: 'card', title: L('newTab.style.card'), preview: newTabMock('card', dark)},
      {value: 'overlay', title: L('newTab.style.overlay'), preview: newTabMock('overlay', dark)},
    ];
    const style = s.newTabStyle || 'minimal';
    const minimal = style === 'minimal';
    const widgets = s.newTabWidgets || [];
    const backgrounds = [
      {value: 'theme', title: L('newTab.background.theme')},
      {value: 'sky', title: L('newTab.background.sky')},
      {value: 'solid', title: L('newTab.background.solid')},
    ];
    if (s.newTabBackground === 'image') {
      backgrounds.push({value: 'image', title: L('newTab.background.image'), disabled: true});
    }
    // [id, title, detail, overlaySupported]
    const items = [
      ['clock', L('newTab.widget.clock'), L('settings.newTab.clock.detail'), false],
      ['shortcuts', L('newTab.shortcuts'), L('settings.newTab.shortcuts.detail'), true],
      ['continue', L('newTab.continue'), L('settings.newTab.continue.detail'), true],
      ['playing', L('newTab.playing'), L('settings.newTab.playing.detail'), true],
      ['archive', L('newTab.archive'), L('settings.newTab.archive.detail'), false],
      ['shield', L('newTab.shield'), L('settings.newTab.shield.detail'), true],
      ['note', L('newTab.note'), L('settings.newTab.note.detail'), false],
    ];
    const styleDetail = {
      minimal: L('newTab.style.minimal.detail'),
      card: L('newTab.style.card.detail'),
      overlay: L('newTab.style.overlay.detail'),
    }[style];
    return [
      group('newTabStyle', L('settings.newTab.style.title'), [
        entry('newTabStyle', L('settings.newTab.style.title'),
              visualPicker('newTabStyle', style, styles, (value) => this.set_('newTabStyle', value)),
              {layout: 'block', detail: styleDetail, keywords: L('settings.newTab.style.keywords')}),
      ]),
      group('newTabPage', L('settings.newTab.page.title'), [
        entry('newTabBackground', L('newTab.background'),
              menuControl(s.newTabBackground, backgrounds, (value) => this.set_('newTabBackground', value),
                          {disabled: minimal}),
              {detail: L('settings.newTab.background.detail'), disabled: minimal,
               keywords: L('settings.newTab.background.keywords')}),
        ...items.map(([id, title, detail, overlay]) => entry(`newTabWidget-${id}`, title,
            switchControl(widgets.includes(id), (on) => this.setNewTabWidget_(id, on),
                          {label: title, disabled: minimal || (style === 'overlay' && !overlay)}),
            {detail: style === 'overlay' && !overlay ? L('settings.newTab.cardOnly') : detail,
             disabled: minimal || (style === 'overlay' && !overlay), keywords: L('settings.newTab.style.keywords')})),
        entry('newTabShortcutRows', L('settings.newTab.shortcutRows.title'),
              switchControl(s.newTabShortcutRows === 2, (on) => this.set_('newTabShortcutRows', on ? 2 : 1),
                            {label: L('settings.newTab.shortcutRows.title'), disabled: style !== 'card'}),
              {detail: style === 'card' ? L('settings.newTab.shortcutRows.detail') : L('settings.newTab.cardOnly'),
               disabled: style !== 'card'}),
      ], {footer: minimal ? L('settings.newTab.minimalFooter') : ''}),
    ];
  }

  setNewTabWidget_(id, on) {
    const widgets = (this.settings_.newTabWidgets || []).filter((widget) => widget !== id);
    if (on) {
      widgets.push(id);
    }
    this.settings_ = {...this.settings_, newTabWidgets: widgets};
    chrome.send('setSetting', ['newTabWidget', {id, on}]);
  }

  // 「标签管理」：自动整理、归档、AI 整理用哪个模型。
  tabs_(s) {
    const count = s.archiveCount || 0;
    const a = this.assistant_ || {};
    const providers = a.providers || [];
    const backend = s.organizerBackend || '';
    const providerId = s.organizerProvider || a.provider || '';
    const provider = providers.find((candidate) => candidate.id === providerId);
    const localName = backend === 'claudeCode' ? 'Claude Code' : 'Codex';
    const clear = async () => {
      if (await confirm({icon: 'trash.fill', title: L('settings.tabs.archive.clear.confirm'),
                         message: L('settings.tabs.archive.clear.message'), confirmTitle: L('common.clear')})) {
        chrome.send('clearArchive');
      }
    };
    const ai = [
      entry('organizerBackend', L('settings.tabs.ai.backend.title'),
            menuControl(backend, [
              {value: '', title: L('settings.tabs.ai.backend.sameAsAssistant')},
              {value: 'chat', title: L('assistant.backend.chatCompletions')},
              {value: 'claudeCode', title: 'Claude Code'},
              {value: 'codex', title: 'Codex'},
            ], (value) => this.set_('organizerBackend', value)),
            {detail: backend === '' ? L('settings.tabs.ai.backend.sameDetail') :
                     backend === 'chat' ? L('settings.tabs.ai.backend.chatDetail') :
                     L('settings.tabs.ai.backend.localDetail', {name: localName}),
             keywords: L('settings.tabs.ai.keywords')}),
    ];
    if (backend === 'chat') {
      ai.push(entry('organizerProvider', L('settings.ai.assistant.provider.title'),
                    menuControl(s.organizerProvider || '', [
                      {value: '', title: L('settings.tabs.ai.provider.sameAsAssistant')},
                      ...providers.map((candidate) => ({value: candidate.id, title: candidate.title})),
                    ], (value) => this.set_('organizerProvider', value)),
                    {detail: provider && provider.requiresKey && !s.organizerHasKey ?
                                 L('settings.tabs.ai.provider.noKey') : '',
                     keywords: L('settings.tabs.ai.keywords')}));
      ai.push(entry('organizerModel', L('settings.ai.assistant.model.title'), html`
        <input class="text-input" type="text" .value=${s.organizerModel || ''}
            placeholder=${L('settings.tabs.ai.model.placeholder')} spellcheck="false" autocomplete="off"
            @change=${(event) => this.set_('organizerModel', event.target.value.trim())}
            @keydown=${(event) => event.key === 'Enter' && event.target.blur()}>`,
        {detail: L('settings.tabs.ai.model.detail'), keywords: L('settings.tabs.ai.keywords')}));
    }
    // 一键整理的提示、归档时打主题：都用上面选的模型。
    ai.push(
      entry('organizeHint', L('settings.tabs.organizeHint.title'),
            switchControl(!!s.organizeHint, (value) => this.set_('organizeHint', value),
                          {label: L('settings.tabs.organizeHint.title')}),
            {detail: L('settings.tabs.organizeHint.detail'), keywords: L('settings.tabs.ai.keywords')}),
      entry('archiveTopics', L('settings.tabs.archiveTopics.title'),
            switchControl(!!s.archiveTopics, (value) => this.set_('archiveTopics', value),
                          {label: L('settings.tabs.archiveTopics.title')}),
            {detail: backend === 'claudeCode' || backend === 'codex' ?
                         L('settings.tabs.archiveTopics.localDetail', {name: localName}) :
                         L('settings.tabs.archiveTopics.detail'),
             keywords: L('settings.tabs.ai.keywords')}));
    return [
      group('autoOrganize', L('settings.tabs.auto.title'), [
        entry('autoArchive', L('settings.tabs.autoArchive.title'),
              switchControl(!!s.autoArchive, (value) => this.set_('autoArchive', value),
                            {label: L('settings.tabs.autoArchive.title')}),
              {detail: L('settings.tabs.autoArchive.detail'), keywords: L('settings.tabs.autoArchive.keywords')}),
        entry('autoArchiveHours', L('settings.tabs.autoArchiveAfter.title'),
              menuControl(s.autoArchiveHours, [
                {value: 12, title: L('settings.tabs.duration.hours', {count: 12})},
                {value: 24, title: L('settings.tabs.duration.days', {count: 1})},
                {value: 72, title: L('settings.tabs.duration.days', {count: 3})},
                {value: 168, title: L('settings.tabs.duration.weeks', {count: 1})},
                {value: 336, title: L('settings.tabs.duration.weeks', {count: 2})},
                {value: 720, title: L('settings.tabs.duration.days', {count: 30})},
              ], (value) => this.set_('autoArchiveHours', value)),
              {disabled: !s.autoArchive, keywords: L('settings.tabs.autoArchive.keywords')}),
        entry('archiveDuplicates', L('settings.tabs.duplicates.title'),
              switchControl(!!s.archiveDuplicates, (value) => this.set_('archiveDuplicates', value),
                            {label: L('settings.tabs.duplicates.title')}),
              {detail: L('settings.tabs.duplicates.detail'), keywords: L('settings.tabs.duplicates.keywords')}),
        entry('organizerExceptions', L('settings.tabs.exceptions.title'), this.organizerExceptions_(s),
              {layout: 'block', detail: L('settings.tabs.exceptions.detail'),
               keywords: L('settings.tabs.exceptions.keywords')}),
      ], {footer: L('settings.tabs.auto.footer')}),
      group('archive', L('settings.tabs.archive.title'), [
        entry('archiveView', L('settings.tabs.archive.view.title'),
              detailButton(L('settings.tabs.archive.view.button'), 'arrow.up.forward',
                           () => chrome.send('openArchive')),
              {detail: count ? L('settings.tabs.archive.count', {count}) : L('settings.tabs.archive.empty'),
               keywords: L('settings.tabs.archive.keywords')}),
        entry('archiveRetention', L('settings.tabs.archive.retention.title'),
              menuControl(s.archiveRetentionDays, [
                {value: 30, title: L('settings.tabs.duration.days', {count: 30})},
                {value: 90, title: L('settings.tabs.duration.days', {count: 90})},
                {value: 365, title: L('settings.tabs.duration.years', {count: 1})},
                {value: 0, title: L('settings.tabs.archive.retention.forever')},
              ], (value) => this.set_('archiveRetentionDays', value)),
              {keywords: L('settings.tabs.archive.keywords')}),
        entry('archiveClear', L('settings.tabs.archive.clear.title'),
              detailButton(L('settings.tabs.archive.clear.button'), '', clear, {danger: true, disabled: !count}),
              {detail: L('settings.tabs.archive.clear.message'), keywords: L('settings.tabs.archive.keywords')}),
      ], {footer: L('settings.tabs.archive.footer')}),
      group('organizerAi', L('settings.tabs.ai.title'), ai, {footer: L('settings.tabs.ai.footer')}),
      // 按网站规则提示归入文件夹：仅给出建议，不自动移动标签。
      group('folderSuggest', L('settings.tabs.suggest.title'), [
        entry('organizeSuggest', L('settings.tabs.suggest.toggle.title'),
              switchControl(!!s.organizeSuggest, (value) => this.set_('organizeSuggest', value),
                            {label: L('settings.tabs.suggest.toggle.title')}),
              {detail: L('settings.tabs.suggest.toggle.detail'), keywords: L('settings.tabs.suggest.keywords')}),
        entry('organizerRules', L('settings.tabs.suggest.rules.title'), this.organizerRules_(s),
              {layout: 'block', detail: L('settings.tabs.suggest.rules.detail'),
               keywords: L('settings.tabs.suggest.keywords')}),
        entry('organizerForget', L('settings.tabs.suggest.forget.title'),
              detailButton(L('settings.tabs.suggest.forget.button'), '',
                           () => chrome.send('forgetOrganizerMemory'), {disabled: !s.organizerLearned}),
              {detail: L('settings.tabs.suggest.forget.detail'), keywords: L('settings.tabs.suggest.keywords')}),
      ], {footer: L('settings.tabs.suggest.footer')}),
    ];
  }

  // 按网站的规则：每条「网站 → 文件夹」（学到的写着「学到的」），最下面加一条。
  organizerRules_(s) {
    const folders = s.pinFolders || [];
    const folder = this.ruleFolderDraft_ && folders.some((item) => item.id === this.ruleFolderDraft_) ?
        this.ruleFolderDraft_ : (folders[0] ? folders[0].id : '');
    const submit = () => {
      const site = this.ruleSiteDraft_.trim();
      if (site && folder) {
        chrome.send('setOrganizerRule', [site, folder]);
        this.ruleSiteDraft_ = '';
      }
    };
    return html`
      <div class="site-list">
        ${repeat(s.organizerRules || [], (rule) => rule.site, (rule) => html`
          <div class="site-row">
            ${icon('folder', 11)}
            <span class="site-name">${L('settings.tabs.suggest.rule', {site: rule.site, folder: rule.folderTitle})}</span>
            ${rule.learned ? html`<span class="soft-tag">${L('settings.tabs.suggest.learned')}</span>` : ''}
            <button class="icon-action" title=${L('settings.tabs.suggest.removeHelp')}
                @click=${() => chrome.send('setOrganizerRule', [rule.site, ''])}>${icon('xmark.circle.fill', 12)}</button>
          </div>`)}
        ${folders.length ? html`
          <div class="inline-controls">
            <input class="text-input" type="text" placeholder="github.com" spellcheck="false" autocomplete="off"
                aria-label=${L('settings.tabs.suggest.site')}
                .value=${this.ruleSiteDraft_}
                @input=${(event) => this.ruleSiteDraft_ = event.target.value}
                @keydown=${(event) => event.key === 'Enter' && submit()}>
            ${menuControl(folder, folders.map((item) => ({value: item.id, title: item.title})),
                          (value) => { this.ruleFolderDraft_ = value; })}
            ${detailButton(L('common.add'), 'plus', submit, {disabled: !this.ruleSiteDraft_.trim()})}
          </div>` : html`<div class="custom-rules-hint">${L('settings.tabs.suggest.noFolders')}</div>`}
      </div>`;
  }

  organizerExceptions_(s) {
    const submit = () => {
      const site = this.organizerDraft_.trim();
      if (site) {
        chrome.send('setOrganizerException', [site, true]);
        this.organizerDraft_ = '';
      }
    };
    return html`
      <div class="site-list">
        ${repeat(s.organizerExceptions || [], (site) => site, (site) => html`
          <div class="site-row">
            ${icon('pin', 11)}
            <span class="site-name">${site}</span>
            <button class="icon-action" title=${L('settings.tabs.exceptions.removeHelp')}
                @click=${() => chrome.send('setOrganizerException', [site, false])}>${icon('xmark.circle.fill', 12)}</button>
          </div>`)}
        <div class="inline-controls">
          <input class="text-input" type="text" placeholder="example.com" spellcheck="false" autocomplete="off"
              aria-label=${L('settings.tabs.exceptions.title')}
              .value=${this.organizerDraft_}
              @input=${(event) => this.organizerDraft_ = event.target.value}
              @keydown=${(event) => event.key === 'Enter' && submit()}>
          ${detailButton(L('common.add'), 'plus', submit, {disabled: !this.organizerDraft_.trim()})}
        </div>
      </div>`;
  }

  performance_(s) {
    return [
      group('tabSleep', L('settings.performance.tabSleep.title'), [
        entry('memorySaver', L('settings.performance.memorySaver.title'),
              switchControl(s.memorySaver, (value) => this.set_('memorySaver', value), {label: L('settings.performance.memorySaver.title')}),
              {detail: L('settings.performance.memorySaver.detail'),
               keywords: L('settings.performance.memorySaver.keywords')}),
        entry('memorySaverAggressiveness', L('settings.performance.memorySaverMode.title'),
              menuControl(s.memorySaverAggressiveness, [
                {value: 0, title: L('settings.performance.memorySaverMode.conservative')},
                {value: 1, title: L('settings.performance.memorySaverMode.moderate')},
                {value: 2, title: L('settings.performance.memorySaverMode.aggressive')},
              ], (value) => this.set_('memorySaverAggressiveness', value)),
              {disabled: !s.memorySaver, keywords: L('settings.performance.memorySaverMode.keywords')}),
        entry('memorySaverExceptions', L('settings.performance.memorySaverExceptions.title'),
              detailButton(L('common.manageEllipsis'), '', () => openUrl('starry://browser-settings/performance', 'foreground')),
              {disabled: !s.memorySaver, keywords: L('settings.performance.memorySaverExceptions.keywords')}),
      ], {footer: L('settings.performance.tabSleep.footer')}),
      group('loading', L('settings.performance.loading.title'), [
        entry('preload', L('settings.performance.preload.title'),
              menuControl(s.preload, [
                {value: 0, title: L('settings.performance.preload.standard')},
                {value: 3, title: L('settings.performance.preload.extended')},
                {value: 2, title: L('settings.performance.preload.off')},
              ], (value) => this.set_('preload', value)),
              {detail: L('settings.performance.preload.detail'),
               keywords: L('settings.performance.preload.keywords')}),
      ]),
    ];
  }

  media_(s) {
    // 收起只管音乐放在哪里：视频的画中画照样浮在角上，位置和自动画中画一直可调。
    const enabled = s.mediaControlsEnabled;
    return [
      group('mediaControls', L('settings.media.controls.title'), [
        entry('mediaControlsEnabled', L('settings.media.enabled.title'),
              switchControl(s.mediaControlsEnabled, (value) => this.set_('mediaControlsEnabled', value),
                            {label: L('settings.media.enabled.title')}),
              {detail: L('settings.media.enabled.detail')}),
        entry('mediaDocked', L('settings.media.display.title'),
              menuControl(s.mediaDocked, [
                {value: false, title: L('settings.media.display.floating')},
                {value: true, title: L('settings.media.display.docked')},
              ], (value) => this.set_('mediaDocked', value), {disabled: !s.mediaControlsEnabled}),
              {disabled: !s.mediaControlsEnabled, detail: L('settings.media.display.detail')}),
        entry('mediaCorner', L('settings.media.corner.title'),
              menuControl(s.mediaCorner, [
                {value: 0, title: L('settings.media.corner.topLeft')},
                {value: 1, title: L('settings.media.corner.topRight')},
                {value: 2, title: L('settings.media.corner.bottomLeft')},
                {value: 3, title: L('settings.media.corner.bottomRight')},
              ], (value) => this.set_('mediaCorner', value), {disabled: !enabled}),
              {disabled: !enabled, detail: L('settings.media.corner.detail')}),
      ]),
      group('mediaVideo', L('settings.media.video.title'), [
        entry('mediaAutoPip', L('settings.media.autoPip.title'),
              switchControl(s.mediaAutoPip, (value) => this.set_('mediaAutoPip', value),
                            {disabled: !enabled, label: L('settings.media.autoPip.title')}),
              {disabled: !enabled,
               detail: L('settings.media.autoPip.detail'),
               keywords: L('settings.media.autoPip.keywords')}),
      ]),
      group('mediaTabs', L('settings.media.tabs.title'), [
        entry('mediaTabMeters', L('settings.media.tabMeters.title'),
              switchControl(s.mediaTabMeters, (value) => this.set_('mediaTabMeters', value),
                            {label: L('settings.media.tabMeters.title')}),
              {detail: L('settings.media.tabMeters.detail')}),
      ]),
    ];
  }

  downloads_(s) {
    return [
      group('downloadLocation', L('settings.downloads.location.title'), [
        entry('downloadDirectory', L('settings.downloads.directory.title'),
              detailButton(L('downloads.settings.change'), 'folder', () => chrome.send('chooseDownloadFolder')),
              {detail: s.downloadDirectory,
               keywords: L('settings.downloads.directory.keywords')}),
        entry('askWhereToSave', L('settings.downloads.asksWhereToSave.title'),
              switchControl(s.askWhereToSave, (value) => this.set_('askWhereToSave', value),
                            {label: L('settings.downloads.asksWhereToSave.title')}),
              {keywords: L('settings.downloads.asksWhereToSave.keywords')}),
      ]),
      group('downloadSpeed', L('settings.downloads.speed.title'), [
        entry('multiConnection', L('settings.downloads.multiConnection.title'),
              switchControl(s.downloadMultiConnection, (value) => this.set_('downloadMultiConnection', value),
                            {label: L('settings.downloads.multiConnection.title')}),
              {detail: L('settings.downloads.multiConnection.detail'),
               keywords: L('settings.downloads.multiConnection.keywords')}),
        entry('connectionsPerDownload', L('settings.downloads.connectionsPerDownload.title'),
              menuControl(s.downloadConnections, DOWNLOAD_CONNECTIONS.map((count) => ({
                value: count, title: L('settings.downloads.connectionsPerDownload.option', {count}),
              })), (value) => this.set_('downloadConnections', value), {disabled: !s.downloadMultiConnection}),
              {detail: s.downloadMultiConnection ? L('settings.downloads.connectionsPerDownload.detail') : '',
               keywords: L('settings.downloads.connectionsPerDownload.keywords'),
               disabled: !s.downloadMultiConnection}),
        entry('maxConcurrentDownloads', L('settings.downloads.maxConcurrentDownloads.title'),
              menuControl(s.downloadMaxConcurrent, DOWNLOAD_CONCURRENCY.map((count) => ({
                value: count, title: L('settings.downloads.maxConcurrentDownloads.option', {count}),
              })), (value) => this.set_('downloadMaxConcurrent', value)),
              {detail: L('settings.downloads.maxConcurrentDownloads.detail'),
               keywords: L('settings.downloads.maxConcurrentDownloads.keywords')}),
        entry('speedLimit', L('settings.downloads.speedLimit.title'),
              menuControl(s.downloadSpeedLimit, DOWNLOAD_SPEED_LIMITS.map((limit) => ({
                value: limit, title: speedLimitLabel(limit),
              })), (value) => this.set_('downloadSpeedLimit', value)),
              {detail: s.downloadSpeedLimit ? L('settings.downloads.speedLimit.detail') : '',
               keywords: L('settings.downloads.speedLimit.keywords')}),
      ], {footer: L('settings.downloads.speed.footer')}),
      group('downloadResume', L('settings.downloads.resume.title'), [
        entry('retriesAutomatically', L('settings.downloads.retriesAutomatically.title'),
              switchControl(s.downloadAutoRetry, (value) => this.set_('downloadAutoRetry', value),
                            {label: L('settings.downloads.retriesAutomatically.title')}),
              {detail: L('settings.downloads.retriesAutomatically.detail', {count: DOWNLOAD_RETRY_ATTEMPTS}),
               keywords: L('settings.downloads.retriesAutomatically.keywords')}),
        entry('resumesOnLaunch', L('settings.downloads.resumesOnLaunch.title'),
              switchControl(s.downloadResumeOnLaunch, (value) => this.set_('downloadResumeOnLaunch', value),
                            {label: L('settings.downloads.resumesOnLaunch.title')}),
              {detail: L('settings.downloads.resumesOnLaunch.detail'),
               keywords: L('settings.downloads.resumesOnLaunch.keywords')}),
      ]),
      group('downloadHistory', L('settings.downloads.history.title'), [
        entry('notifiesOnCompletion', L('settings.downloads.notifiesOnCompletion.title'),
              switchControl(s.downloadNotifyOnCompletion, (value) => this.set_('downloadNotifyOnCompletion', value),
                            {label: L('settings.downloads.notifiesOnCompletion.title')}),
              {detail: L('settings.downloads.notifiesOnCompletion.detail'),
               keywords: L('settings.downloads.notifiesOnCompletion.keywords')}),
        entry('downloadsPage', L('settings.downloads.manage.title'),
              detailButton(L('common.open'), 'arrow.down.circle', () => openUrl('starry://downloads', 'current')),
              {detail: L('settings.downloads.manage.detail'), keywords: L('settings.downloads.manage.keywords')}),
      ]),
    ];
  }

  // ---- 广告与跟踪拦截 ----

  // 「拦截广告和跟踪器」下面的一行。
  blockingStatus_(b) {
    if (!b.enabled) {
      return L('contentBlocking.status.off');
    }
    if (b.activity === 'downloading') {
      return L('contentBlocking.status.downloading', {current: b.downloadsDone + 1, total: b.downloadsTotal});
    }
    if (b.activity === 'building' || !b.ready) {
      return L('contentBlocking.status.compiling');
    }
    const enabled = b.subscriptions.filter((list) => list.enabled);
    if (!b.totalRules) {
      if (!enabled.length && !b.hasCustomRules) {
        return L('contentBlocking.status.noLists');
      }
      return enabled.some((list) => list.lastError) ? L('contentBlocking.status.downloadFailed') :
                                                      L('contentBlocking.status.notDownloaded');
    }
    const parts = [L('contentBlocking.status.summary', {
      lists: L('contentBlocking.status.listCount', {count: enabled.length}),
      rules: L('contentBlocking.status.ruleTotal', {count: formatCount(b.totalRules)}),
    })];
    if (b.lastUpdated) {
      parts.push(L('contentBlocking.updatedAgo', {time: relativeTime(b.lastUpdated)}));
    }
    return parts.join(' · ');
  }

  filterListDetail_(list) {
    const parts = [];
    if (list.lastError) {
      parts.push(L('contentBlocking.list.updateFailed.detail', {reason: list.lastError}));
    } else if (!list.hasText) {
      parts.push(list.enabled ? L('contentBlocking.list.notDownloaded') : list.summary);
    } else if (list.enabled && list.hasStats) {
      parts.push(L('contentBlocking.list.rules', {count: formatCount(list.rules)}));
      if (list.skipped > 0) {
        parts.push(L('contentBlocking.list.skipped', {count: formatCount(list.skipped)}));
      }
      if (list.lastUpdated) {
        parts.push(L('contentBlocking.updatedAgo', {time: relativeTime(list.lastUpdated)}));
      }
    } else {
      parts.push(list.summary);
    }
    return parts.filter(Boolean).join(' · ');
  }

  renderFilterList_(list, enabled) {
    return html`
      <div class="filter-list" data-key=${list.id}>
        <div class="filter-list-labels">
          <div class="filter-list-title">
            <span>${list.title}</span>
            ${list.lastError ? html`<span class="soft-tag warning">${L('contentBlocking.list.updateFailed.tag')}</span>` :
                               nothing}
          </div>
          <div class="filter-list-detail ${list.lastError ? 'warning' : ''}">${this.filterListDetail_(list)}</div>
        </div>
        ${list.homepage ? html`
          <button class="icon-action" title=${L('contentBlocking.list.homepage.help')}
              @click=${() => openUrl(list.homepage, 'foreground')}>${icon('arrow.up.right.square', 12)}</button>` :
          nothing}
        ${list.builtin ? nothing : html`
          <button class="icon-action" title=${L('contentBlocking.list.remove.help')}
              @click=${() => this.removeFilterList_(list)}>${icon('trash', 12)}</button>`}
        ${switchControl(list.enabled, (value) => {
          this.blocking_ = {
            ...this.blocking_,
            subscriptions: this.blocking_.subscriptions.map(
                (other) => other.id === list.id ? {...other, enabled: value} : other),
          };
          chrome.send('setFilterListEnabled', [list.id, value]);
        }, {disabled: !enabled, label: list.title})}
      </div>`;
  }

  filterListsControl_(b) {
    const busy = b.activity === 'downloading';
    const working = busy || b.activity === 'building';
    return html`
      <div class="filter-lists ${b.enabled ? '' : 'inactive'}">
        ${FILTER_CATEGORIES.map((category) => {
          const items = b.subscriptions.filter((list) => list.category === category);
          return items.length ? html`
            <div class="filter-category">
              <div class="filter-category-title">${L(`contentBlocking.category.${category}`)}</div>
              ${repeat(items, (list) => list.id, (list) => this.renderFilterList_(list, b.enabled))}
            </div>` : nothing;
        })}
        <div class="filter-actions">
          ${detailButton(L('contentBlocking.addSubscription'), 'plus', () => this.addFilterList_(),
                         {disabled: !b.enabled})}
          ${detailButton(busy ? L('contentBlocking.updating') : L('contentBlocking.updateNow'), 'arrow.clockwise',
                         () => chrome.send('updateFilterLists'), {disabled: !b.enabled || busy})}
          ${working ? html`<span class="spinner" aria-hidden="true"></span>` : nothing}
        </div>
      </div>`;
  }

  // 添加订阅：校验 URL 下载结果确为过滤列表后再加入，失败时携带原因重新弹窗。
  async addFilterList_(input = '', error = '') {
    const result = await showDialog({
      icon: 'shield.lefthalf.filled',
      tone: error ? 'warning' : 'accent',
      title: L('contentBlocking.add.title'),
      message: error || L('contentBlocking.add.prompt'),
      field: input || 'https://',
      buttons: [
        {title: L('common.cancel'), variant: 'tertiary', cancel: true},
        {title: L('common.add'), variant: 'filled'},
      ],
    });
    if (!result || result.button !== 1 || !result.input || !result.input.trim()) {
      return;
    }
    const added = await sendWithPromise('addFilterList', result.input.trim());
    if (added && added.error) {
      this.addFilterList_(result.input.trim(), added.error);
    }
  }

  async removeFilterList_(list) {
    if (await confirm({icon: 'trash.fill', title: L('contentBlocking.list.remove.title', {name: list.title}),
                       message: L('contentBlocking.list.remove.message'), confirmTitle: L('common.remove')})) {
      chrome.send('removeFilterList', [list.id]);
    }
  }

  customRulesDetail_(b) {
    if (!b.hasCustomRules) {
      return L('settings.privacy.customRules.syntax');
    }
    const count = formatCount(b.customRuleCount);
    return b.customSkipped > 0 ?
        L('settings.privacy.customRules.activeSkipped', {count, skipped: formatCount(b.customSkipped)}) :
        L('settings.privacy.customRules.active', {count});
  }

  async editCustomRules_() {
    this.customDraft_ = await sendWithPromise('getCustomRules');
    await this.updateComplete;
    const editor = this.querySelector('.custom-rules textarea');
    if (editor) {
      editor.focus();
    }
  }

  // 自定义规则行内编辑器：每行一条规则，下方附常见写法示例。
  customRulesEditor_() {
    const example = (rule, meaning) => html`
      <div class="rule-example"><code>${rule}</code><span>${meaning}</span></div>`;
    return html`
      <div class="custom-rules">
        <div class="custom-rules-hint">${L('contentBlocking.customRules.hint')}</div>
        <textarea spellcheck="false" autocomplete="off" .value=${this.customDraft_}
            aria-label=${L('contentBlocking.customRules.title')}
            @input=${(event) => this.customDraft_ = event.target.value}></textarea>
        <div class="rule-examples">
          ${example('||ads.example.com^', L('contentBlocking.customRules.example.block'))}
          ${example('example.com##.banner', L('contentBlocking.customRules.example.hide'))}
          ${example('@@||example.com^$document', L('contentBlocking.customRules.example.allow'))}
        </div>
        <div class="custom-rules-actions">
          ${detailButton(L('common.cancel'), '', () => this.customDraft_ = null)}
          ${detailButton(L('contentBlocking.customRules.save'), 'checkmark', () => {
            chrome.send('setCustomRules', [this.customDraft_]);
            this.customDraft_ = null;
          })}
        </div>
      </div>`;
  }

  // 不拦截的网站：每行一个、可以删掉，下面一个输入框添加（接受网站名，也接受整个网址）。
  allowlistControl_(b) {
    const submit = () => {
      const value = this.allowDraft_.trim();
      if (value) {
        chrome.send('setAllowlisted', [value, true]);
        this.allowDraft_ = '';
      }
    };
    return html`
      <div class="site-list ${b.enabled ? '' : 'inactive'}">
        ${repeat(b.allowlist, (site) => site, (site) => html`
          <div class="site-row">
            ${icon('shield.slash', 11)}
            <span class="site-name">${site}</span>
            <button class="icon-action" title=${L('contentBlocking.allowlist.remove.help')}
                @click=${() => chrome.send('setAllowlisted', [site, false])}>${icon('xmark.circle.fill', 12)}</button>
          </div>`)}
        <div class="inline-controls">
          <input class="text-input" type="text" placeholder="example.com" spellcheck="false" autocomplete="off"
              .value=${this.allowDraft_} ?disabled=${!b.enabled}
              @input=${(event) => this.allowDraft_ = event.target.value}
              @keydown=${(event) => event.key === 'Enter' && submit()}>
          ${detailButton(L('common.add'), 'plus', submit, {disabled: !b.enabled || !this.allowDraft_.trim()})}
        </div>
      </div>`;
  }

  contentBlocking_() {
    const b = this.blocking_;
    if (!b) {
      return [];
    }
    const editing = this.customDraft_ !== null;
    return [
      group('contentBlocking', L('settings.privacy.contentBlocking.title'), [
        entry('contentBlockingEnabled', L('settings.privacy.contentBlockingEnabled.title'),
              switchControl(b.enabled, (value) => {
                this.blocking_ = {...b, enabled: value};
                chrome.send('setContentBlocking', ['enabled', value]);
              }, {label: L('settings.privacy.contentBlockingEnabled.title')}),
              {detail: this.blockingStatus_(b), keywords: L('settings.privacy.contentBlockingEnabled.keywords')}),
        entry('filterLists', L('settings.privacy.filterLists.title'), this.filterListsControl_(b),
              {layout: 'block', keywords: L('settings.privacy.filterLists.keywords')}),
        entry('customRules', L('settings.privacy.customRules.title'),
              editing ? this.customRulesEditor_() :
                        detailButton(L('contentBlocking.customRules.edit'), 'square.and.pencil',
                                     () => this.editCustomRules_(), {disabled: !b.enabled}),
              {layout: editing ? 'block' : 'row', detail: this.customRulesDetail_(b),
               keywords: L('settings.privacy.customRules.keywords')}),
        entry('allowlist', L('settings.privacy.allowlist.title'), this.allowlistControl_(b),
              {layout: 'block',
               detail: b.allowlist.length ? L('settings.privacy.siteCount', {count: b.allowlist.length}) : '',
               keywords: L('settings.privacy.allowlist.keywords')}),
      ], {footer: L('settings.privacy.contentBlocking.footer')}),
    ];
  }

  fingerprintExceptions_(s) {
    const enabled = s.fingerprintProtection !== 'off';
    const submit = () => {
      const site = this.fingerprintDraft_.trim();
      if (site) {
        chrome.send('setFingerprintException', [site, true]);
        this.fingerprintDraft_ = '';
      }
    };
    return html`
      <div class="site-list ${enabled ? '' : 'inactive'}">
        ${repeat(s.fingerprintExceptions || [], (site) => site, (site) => html`
          <div class="site-row">
            ${icon('shield.slash', 11)}
            <span class="site-name">${site}</span>
            <button class="icon-action" title=${L('settings.privacy.fingerprintExceptions.removeHelp')}
                @click=${() => chrome.send('setFingerprintException', [site, false])}>${icon('xmark.circle.fill', 12)}</button>
          </div>`)}
        <div class="inline-controls">
          <input class="text-input" type="text" placeholder="example.com" spellcheck="false" autocomplete="off"
              aria-label=${L('settings.privacy.fingerprintExceptions.title')}
              .value=${this.fingerprintDraft_} ?disabled=${!enabled}
              @input=${(event) => this.fingerprintDraft_ = event.target.value}
              @keydown=${(event) => event.key === 'Enter' && submit()}>
          ${detailButton(L('common.add'), 'plus', submit, {disabled: !enabled || !this.fingerprintDraft_.trim()})}
        </div>
      </div>`;
  }

  // 选点页在这个窗口的对话框里打开（site 为空是全局的位置）。选好了回 true。
  pickLocation_(site) {
    return sendWithPromise('pickLocation', site);
  }

  async setLocationMode_(mode) {
    // 还没有全局的位置：先选一个，取消就保持关闭。
    if (mode !== 'off' && !this.settings_.simulatedLocation && !await this.pickLocation_('')) {
      return;
    }
    this.set_('locationSimulation', mode);
  }

  // 「东京 · 日本 · GMT+9」（选点页里才写时区的全名）。
  locationText_(location) {
    return location.timeZoneOffset ?
        L('settings.privacy.location.summary', {place: location.label, zone: location.timeZoneOffset}) :
        location.label;
  }

  siteLocations_(s) {
    const draft = this.locationDraft_.trim();
    const add = async (mode) => {
      if (!draft) {
        return;
      }
      if (mode === 'real') {
        chrome.send('setSiteLocation', [draft, {mode: 'real'}]);
        this.locationDraft_ = '';
      } else if (await this.pickLocation_(draft)) {
        this.locationDraft_ = '';
      }
    };
    return html`
      <div class="site-list">
        ${repeat(s.siteLocations || [], (item) => item.site, (item) => html`
          <div class="site-row location-row">
            ${icon(item.mode === 'real' ? 'mappin.slash' : 'mappin', 11)}
            <span class="site-name">${item.site}</span>
            <span class="site-location">${item.mode === 'real' ?
                L('settings.privacy.siteLocations.real') : this.locationText_(item.location)}</span>
            ${item.mode === 'simulated' ? html`
              <button class="icon-action" title=${L('settings.privacy.siteLocations.editHelp')}
                  @click=${() => this.pickLocation_(item.site)}>${icon('pencil', 11)}</button>` : nothing}
            <button class="icon-action" title=${L('settings.privacy.siteLocations.removeHelp')}
                @click=${() => chrome.send('setSiteLocation', [item.site, null])}>${icon('xmark.circle.fill', 12)}</button>
          </div>`)}
        <div class="inline-controls">
          <input class="text-input" type="text" placeholder="example.com" spellcheck="false" autocomplete="off"
              aria-label=${L('settings.privacy.siteLocations.title')}
              .value=${this.locationDraft_}
              @input=${(event) => this.locationDraft_ = event.target.value}
              @keydown=${(event) => event.key === 'Enter' && add('simulated')}>
          ${detailButton(L('settings.privacy.siteLocations.simulate'), 'mappin', () => add('simulated'), {disabled: !draft})}
          ${detailButton(L('settings.privacy.siteLocations.useReal'), '', () => add('real'), {disabled: !draft})}
        </div>
      </div>`;
  }

  privacy_(s) {
    const count = this.historyCount_;
    const b = this.blocking_;
    return [
      ...this.contentBlocking_(),
      group('fingerprint', L('settings.privacy.fingerprint.title'), [
        entry('fingerprintProtection', L('settings.privacy.fingerprintProtection.title'),
              menuControl(s.fingerprintProtection, [
                {value: 'off', title: L('privacy.fingerprint.off')},
                {value: 'privateBrowsing', title: L('privacy.fingerprint.privateBrowsing')},
                {value: 'allBrowsing', title: L('privacy.fingerprint.allBrowsing')},
              ], (value) => this.set_('fingerprintProtection', value)),
              {detail: L('settings.privacy.fingerprintProtection.detail'),
               keywords: L('settings.privacy.fingerprintProtection.keywords')}),
        entry('fingerprintExceptions', L('settings.privacy.fingerprintExceptions.title'),
              this.fingerprintExceptions_(s),
              {layout: 'block', detail: L('settings.privacy.fingerprintExceptions.detail'),
               keywords: L('settings.privacy.fingerprintExceptions.keywords')}),
      ], {footer: L('settings.privacy.fingerprint.footer')}),
      group('location', L('settings.privacy.location.title'), [
        entry('locationSimulation', L('settings.privacy.locationSimulation.title'),
              menuControl(s.locationSimulation, [
                {value: 'off', title: L('privacy.fingerprint.off')},
                {value: 'privateBrowsing', title: L('privacy.fingerprint.privateBrowsing')},
                {value: 'allBrowsing', title: L('privacy.fingerprint.allBrowsing')},
              ], (value) => this.setLocationMode_(value)),
              {detail: L('settings.privacy.locationSimulation.detail'),
               keywords: L('settings.privacy.locationSimulation.keywords')}),
        entry('simulatedLocation', L('settings.privacy.simulatedLocation.title'),
              detailButton(L('common.chooseEllipsis'), 'mappin', () => this.pickLocation_('')),
              {detail: s.simulatedLocation ? this.locationText_(s.simulatedLocation) :
                                             L('settings.privacy.simulatedLocation.none'),
               keywords: L('settings.privacy.simulatedLocation.keywords')}),
        entry('siteLocations', L('settings.privacy.siteLocations.title'), this.siteLocations_(s),
              {layout: 'block', detail: L('settings.privacy.siteLocations.detail'),
               keywords: L('settings.privacy.siteLocations.keywords')}),
      ], {footer: L('settings.privacy.location.footer')}),
      group('tracking', L('settings.privacy.tracking.title'), [
        ...(b ? [entry('stripTrackingParameters', L('settings.privacy.stripTrackingParameters.title'),
                       switchControl(b.stripTrackingParameters, (value) => {
                         this.blocking_ = {...b, stripTrackingParameters: value};
                         chrome.send('setContentBlocking', ['stripTrackingParameters', value]);
                       }, {label: L('settings.privacy.stripTrackingParameters.title')}),
                       {detail: L('settings.privacy.stripTrackingParameters.detail'),
                        keywords: L('settings.privacy.stripTrackingParameters.keywords')})] : []),
        entry('cookies', L('settings.privacy.thirdPartyCookies.title'),
              menuControl(s.cookies === 0 ? 2 : s.cookies, [
                {value: 1, title: L('settings.privacy.thirdPartyCookies.block')},
                {value: 2, title: L('settings.privacy.thirdPartyCookies.blockPrivate')},
              ], (value) => this.set_('cookies', value)),
              {detail: L('settings.privacy.thirdPartyCookies.detail'),
               keywords: L('settings.privacy.thirdPartyCookies.keywords')}),
        entry('doNotTrack', L('settings.privacy.doNotTrack.title'),
              switchControl(s.doNotTrack, (value) => this.set_('doNotTrack', value), {label: L('settings.privacy.doNotTrack.title')}),
              {detail: L('settings.privacy.doNotTrack.detail'),
               keywords: L('settings.privacy.doNotTrack.keywords')}),
      ]),
      group('history', L('settings.privacy.history.title'), [
        entry('history', L('settings.privacy.viewHistory.title'),
              detailButton(L('settings.privacy.history.view'), 'clock', () => openUrl('starry://history', 'current')),
              {detail: count === null ? '' : L('settings.privacy.history.count', {count}),
               keywords: L('settings.privacy.viewHistory.keywords')}),
        entry('clearHistory', L('settings.privacy.clearHistory.title'),
              detailButton(L('settings.privacy.clearHistory.menu'), 'trash', (event) => this.clearHistory_(event)),
              {keywords: L('settings.privacy.clearHistory.keywords')}),
        entry('clearBrowsingData', L('settings.privacy.clearBrowsingData.title'),
              detailButton(L('common.chooseEllipsis'), '', () => openUrl('starry://browser-settings/clearBrowserData', 'foreground')),
              {detail: L('settings.privacy.clearBrowsingData.detail'),
               keywords: L('settings.privacy.clearBrowsingData.keywords')}),
      ]),
      group('more', L('settings.privacy.more.title'), [
        entry('passwords', L('settings.privacy.passwords.title'),
              detailButton(L('common.manageEllipsis'), 'lock', () => openUrl('starry://password-manager', 'foreground')),
              {detail: L('settings.privacy.passwords.detail'), keywords: L('settings.privacy.passwords.keywords')}),
        entry('siteSettings', L('settings.privacy.siteSettings.title'),
              detailButton(L('common.manageEllipsis'), '', () => openUrl('starry://browser-settings/content', 'foreground')),
              {detail: L('settings.privacy.siteSettings.detail'),
               keywords: L('settings.privacy.siteSettings.keywords')}),
        entry('security', L('settings.privacy.safeBrowsing.title'),
              detailButton(L('common.manageEllipsis'), '', () => openUrl('starry://browser-settings/security', 'foreground')),
              {detail: L('settings.privacy.safeBrowsing.detail'),
               keywords: L('settings.privacy.safeBrowsing.keywords')}),
      ]),
    ];
  }

  developer_(s) {
    return [
      group('engine', L('settings.developer.engine.title'), [
        entry('engineVersion', L('settings.developer.engine.version'),
              html`<span class="value-text">${s.engineVersion ? `Chromium ${s.engineVersion}` : ''}</span>`,
              {keywords: L('settings.developer.engine.keywords')}),
        entry('flags', L('settings.developer.flags.title'),
              detailButton(L('common.openEllipsis'), '', () => openUrl('starry://flags', 'foreground')),
              {detail: L('settings.developer.flags.detail'), keywords: L('settings.developer.flags.keywords')}),
      ]),
      group('browser', L('internalPage.browserSettings'), [
        entry('browserSettings', L('settings.developer.browserSettings.title'),
              detailButton(L('common.openEllipsis'), 'arrow.up.right.square', () => openUrl('starry://browser-settings', 'foreground')),
              {detail: L('settings.developer.browserSettings.detail'),
               keywords: L('settings.developer.browserSettings.keywords')}),
      ]),
    ];
  }

  // ---- 导入浏览器数据 ----
  //
  // 三组：从哪里导入（本机找到的浏览器，最近用过的在前；书签文件）、导入哪几样（那个来源有的）、
  // 导入的按钮和进度、结果。导入本身在浏览器里（starry/browser/importer/import_service.h），
  // 离开这一页也会做完。

  loadImport_() {
    sendWithPromise('getImportState').then((state) => {
      this.import_ = state;
    });
  }

  // 选中的来源：用户点过的；没点过时是正在导入、刚导入完的那个，再不然是列表的第一个。
  importSelection_() {
    const state = this.import_;
    const ids = [...(state ? state.sources.map((source) => source.id) : []), 'bookmarksFile'];
    const job = state && state.job;
    return [this.importSource_, job && job.sourceId, ids[0]].find((id) => id && ids.includes(id));
  }

  selectImportSource_(id) {
    const job = this.import_ && this.import_.job;
    // 换了来源就收起上一次的结果。
    if (job && job.status !== 'running' && job.sourceId !== id) {
      chrome.send('clearImport');
    }
    this.importSource_ = id;
  }

  importItemTitle_(item) {
    switch (item) {
      case 'bookmarks':
        return L('common.bookmarks');
      case 'history':
        return L('common.history');
      case 'passwords':
        return L('settings.import.item.passwords');
      case 'searchEngines':
        return L('settings.import.item.searchEngines');
      case 'autofill':
        return L('settings.import.item.autofill');
    }
    return item;
  }

  importItemDetail_(item, source) {
    switch (item) {
      case 'bookmarks':
        return L('settings.import.item.bookmarks.detail');
      case 'history':
        return L('settings.import.item.history.detail');
      case 'passwords':
        return source.keychainItem ? L('settings.import.item.passwords.keychain', {item: source.keychainItem}) :
                                     L('settings.import.item.passwords.detail');
      case 'searchEngines':
        return L('settings.import.item.searchEngines.detail');
      case 'autofill':
        return L('settings.import.item.autofill.detail');
    }
    return '';
  }

  importCount_(item, count) {
    const n = formatCount(count);
    switch (item) {
      case 'bookmarks':
        return L('settings.import.count.bookmarks', {count: n});
      case 'history':
        return L('settings.import.count.history', {count: n});
      case 'passwords':
        return L('settings.import.count.passwords', {count: n});
      case 'searchEngines':
        return L('settings.import.count.searchEngines', {count: n});
      case 'autofill':
        return L('settings.import.count.autofill', {count: n});
    }
    return n;
  }

  // 每个来源要导入哪几样：用户关掉过的记着，默认它有的都导入。
  importItemsFor_(source) {
    return this.importItems_[source.id] ||
        Object.fromEntries(IMPORT_ITEMS.map((item) => [item, !!source.items[item]]));
  }

  importSources_(state, selected, busy) {
    const entries = [];
    const sources = state ? state.sources : [];
    if (!state || (state.detecting && !sources.length)) {
      entries.push(entry('importDetecting', L('settings.import.detecting'),
                         html`<span class="spinner" aria-hidden="true"></span>`, {leading: 'magnifyingglass'}));
    } else if (!sources.length) {
      entries.push(entry('importNone', L('settings.import.none.title'),
                         detailButton(L('settings.import.redetect'), 'arrow.clockwise',
                                      () => chrome.send('detectImportSources')),
                         {detail: L('settings.import.none.detail'), leading: 'magnifyingglass'}));
    }
    for (const source of sources) {
      const items = IMPORT_ITEMS.filter((item) => source.items[item]).map((item) => this.importItemTitle_(item));
      entries.push(entry(`source-${source.id}`, source.browser, nothing, {
        detail: [source.profile, joinList(items)].filter(Boolean).join(' · '),
        leading: source.safari ? 'safari' : 'globe',
        selected: source.id === selected,
        disabled: busy,
        onSelect: () => this.selectImportSource_(source.id),
      }));
    }
    // macOS：没有「完全磁盘访问权限」读不了 Safari 的书签。
    if (state && state.safariNeedsAccess) {
      entries.push(entry('safariAccess', 'Safari', html`
        <div class="inline-controls">
          ${detailButton(L('settings.import.safari.openSettings'), 'arrow.up.right.square',
                         () => chrome.send('openFullDiskAccess'))}
          ${detailButton(L('settings.import.redetect'), 'arrow.clockwise', () => chrome.send('detectImportSources'))}
        </div>`, {detail: L('settings.import.safari.needsAccess'), leading: 'safari'}));
    }
    entries.push(entry('source-bookmarksFile', L('settings.import.bookmarksFile.title'), nothing, {
      detail: L('settings.import.bookmarksFile.detail'),
      leading: 'doc.text',
      selected: selected === 'bookmarksFile',
      disabled: busy,
      onSelect: () => this.selectImportSource_('bookmarksFile'),
    }));
    return entries;
  }

  importItemsGroup_(source, busy) {
    const enabled = this.importItemsFor_(source);
    const entries = IMPORT_ITEMS.filter((item) => source.items[item]).map((item) => entry(
        `item-${item}`, this.importItemTitle_(item),
        switchControl(!!enabled[item], (value) => {
          this.importItems_ = {...this.importItems_, [source.id]: {...enabled, [item]: value}};
        }, {label: this.importItemTitle_(item), disabled: busy}),
        {detail: this.importItemDetail_(item, source)}));
    return group('importItems', L('settings.import.items.title'), entries,
                 {footer: L('settings.import.items.footer', {browser: importSourceName(source)})});
  }

  // 导入完了的提醒：密码没拿到密钥、有解不开的、有几样没读出来。
  importNotes_(job, source) {
    const notes = [];
    if (job.passwordKeyUnavailable) {
      notes.push(source && source.keychainItem ?
                     L('settings.import.note.keychain', {item: source.keychainItem}) :
                     L('settings.import.note.passwordKey'));
    }
    if (job.undecryptablePasswords > 0) {
      notes.push(L('settings.import.note.undecryptable', {count: job.undecryptablePasswords}));
    }
    const failed = IMPORT_ITEMS.filter((item) => job.items[item] && job.failed[item] &&
                                                 !(item === 'passwords' && job.passwordKeyUnavailable));
    if (failed.length) {
      notes.push(L('settings.import.note.failed', {
        items: joinList(failed.map((item) => this.importItemTitle_(item))), browser: job.name,
      }));
    }
    return notes;
  }

  importProgress_(job, source) {
    switch (job.current) {
      case 'bookmarks':
        return L('settings.import.progress.bookmarks');
      case 'history':
        return L('settings.import.progress.history');
      case 'passwords':
        return source && source.keychainItem ?
            L('settings.import.progress.passwordsKeychain', {item: source.keychainItem}) :
            L('settings.import.progress.passwords');
      case 'searchEngines':
        return L('settings.import.progress.searchEngines');
      case 'autofill':
        return L('settings.import.progress.autofill');
    }
    return L('settings.import.progress.preparing');
  }

  retryImport_(job) {
    if (job.sourceId === 'bookmarksFile') {
      chrome.send('importBookmarksFile');
    } else {
      chrome.send('startImport', [job.sourceId, job.items]);
    }
  }

  importJobEntry_(job, source) {
    if (job.status === 'running') {
      return entry('importJob', L('settings.import.running.title', {browser: job.name}),
                   html`<span class="spinner" aria-hidden="true"></span>`,
                   {detail: this.importProgress_(job, source), leading: 'square.and.arrow.down'});
    }
    const done = detailButton(L('common.done'), '', () => chrome.send('clearImport'));
    if (job.status === 'failed') {
      const reason = job.error === 'firefoxRunning' ? L('settings.import.error.firefoxRunning') :
                     job.sourceId === 'bookmarksFile' ? L('settings.import.error.bookmarksFile') :
                     L('settings.import.error.unreadable', {browser: job.name});
      return entry('importJob', L('settings.import.failed.title'), html`
        <div class="inline-controls">
          ${done}${detailButton(L('common.retry'), 'arrow.clockwise', () => this.retryImport_(job))}
        </div>`, {detail: reason, leading: 'exclamationmark.triangle.fill', tone: 'warning'});
    }
    const counts = IMPORT_ITEMS.filter((item) => job.items[item] && job.counts[item] > 0)
                       .map((item) => this.importCount_(item, job.counts[item]));
    // 几句话接在一起：中文、日文的句号后面不空格。
    const gap = ['zh-Hans', 'zh-Hant', 'ja'].includes(language) ? '' : ' ';
    const detail = [counts.length ? L('settings.import.done.imported', {items: joinList(counts)}) :
                                    L('settings.import.done.empty'),
                    ...this.importNotes_(job, source)].join(gap);
    return entry('importJob', L('settings.import.done.title'), html`
      <div class="inline-controls">
        ${job.counts.bookmarks > 0 ? detailButton(L('settings.import.done.showBookmarks'), 'star',
                                                  () => openUrl('starry://bookmarks', 'current')) : nothing}
        ${done}
      </div>`, {detail, leading: 'checkmark.circle.fill', tone: 'success'});
  }

  importAction_(state, selected, source) {
    const job = state && state.job;
    if (job && (job.status === 'running' || job.sourceId === selected)) {
      return this.importJobEntry_(job, source);
    }
    if (selected === 'bookmarksFile') {
      return entry('importStart', L('settings.import.bookmarksFile.action'),
                   detailButton(L('common.chooseEllipsis'), 'folder', () => chrome.send('importBookmarksFile'),
                                {prominent: true}),
                   {detail: L('settings.import.bookmarksFile.actionDetail')});
    }
    const items = this.importItemsFor_(source);
    const chosen = IMPORT_ITEMS.filter((item) => source.items[item] && items[item]);
    return entry('importStart', L('settings.import.action.title', {browser: importSourceName(source)}),
                 detailButton(L('settings.import.action.button'), '',
                              () => chrome.send('startImport', [source.id, items]),
                              {prominent: true, disabled: !chosen.length}),
                 {detail: chosen.length ? L('settings.import.action.detail') : L('settings.import.action.nothing')});
  }

  importData_() {
    const state = this.import_;
    const selected = this.importSelection_();
    const job = state && state.job;
    const busy = !!job && job.status === 'running';
    const source = state && state.sources.find((candidate) => candidate.id === selected);
    const groups = [group('importSource', L('settings.import.source.title'), this.importSources_(state, selected, busy),
                          {footer: L('settings.import.source.footer')})];
    if (source) {
      groups.push(this.importItemsGroup_(source, busy));
    }
    if (source || selected === 'bookmarksFile') {
      groups.push(group('importAction', '', [this.importAction_(state, selected, source)]));
    }
    return groups;
  }

  // ---- 显示 ----

  renderEntry_(item) {
    const query = this.query_;
    const state = item.disabled ? 'disabled' : '';
    const labels = html`
      <div class="entry-labels">
        <div class="entry-title ${item.tone}">${highlight(item.title, query)}</div>
        ${item.detail ? html`<div class="entry-detail">${highlight(item.detail, query)}</div>` : nothing}
      </div>`;
    const leading = item.leading ? html`<span class="entry-leading">${icon(item.leading, 15)}</span>` : nothing;
    if (item.onSelect) {
      return html`
        <button class="entry selectable ${item.selected ? 'selected' : ''}" data-key=${item.id} role="radio"
            aria-checked=${item.selected ? 'true' : 'false'} ?disabled=${item.disabled} @click=${item.onSelect}>
          ${leading}${labels}
          <span class="entry-control">${item.control || nothing}<span class="check">${icon('checkmark', 12)}</span></span>
        </button>`;
    }
    if (item.layout === 'block') {
      return html`<div class="entry block ${state}" data-key=${item.id}>${labels}${item.control}</div>`;
    }
    return html`
      <div class="entry ${state}" data-key=${item.id}>
        ${leading}${labels}
        <div class="entry-control">${item.control}</div>
      </div>`;
  }

  renderGroup_(item, header = null) {
    return html`
      <section class="group" data-key=${item.id}>
        ${header || (item.title ? html`<div class="group-title">${item.title}</div>` : nothing)}
        <div class="group-card">${repeat(item.entries, (each) => each.id, (each) => this.renderEntry_(each))}</div>
        ${item.footer ? html`<div class="group-footer">${item.footer}</div>` : nothing}
      </section>`;
  }

  renderPaneHeader_(pane, summary = pane.summary) {
    return html`
      <div class="pane-header">
        <div class="pane-tile">${icon(pane.icon, 19)}</div>
        <div class="pane-text">
          <h1 class="pane-title">${pane.title}</h1>
          <div class="pane-summary">${summary}</div>
        </div>
      </div>`;
  }

  renderPane_() {
    const pane = PANES.find((candidate) => candidate.id === this.pane_);
    if (pane.id === 'extensions') {
      return html`<starry-extensions .compact=${this.compact_} .incognito=${this.settings_.incognito}></starry-extensions>`;
    }
    if (pane.id === 'appearance') {
      return html`${this.renderPaneHeader_(pane)}${this.renderAppearance_()}`;
    }
    const parent = pane.parent && PANES.find((candidate) => candidate.id === pane.parent);
    return html`
      ${parent ? html`
        <button class="pane-link back-link" @click=${() => this.select_(parent.id)}>
          <span class="chevron">${icon('chevron.left', 8)}</span><span>${parent.title}</span>
        </button>` : nothing}
      ${this.renderPaneHeader_(pane)}
      ${repeat(this.groups_(pane.id), (item) => item.id, (item) => this.renderGroup_(item))}`;
  }

  // 外观：四段分层引导（appearance.js）。
  renderAppearance_() {
    return renderAppearance(this.settings_, {
      mockWindow,
      set: (key, value) => this.set_(key, value),
      toggleButton: (button) => this.toggleUtilityButton_(button),
    });
  }

  // 搜索：每个词都要出现在标题、说明或关键词里；结果按分类分组。
  searchResults_() {
    const terms = this.query_.toLowerCase().split(/\s+/).filter(Boolean);
    const sections = [];
    // 子页（导入）不搜：它的入口在「通用」里搜得到。
    for (const pane of PANES.filter((candidate) => !candidate.parent)) {
      for (const item of this.groups_(pane.id)) {
        const entries = item.entries.filter((each) => {
          const haystack = `${each.title} ${each.detail} ${each.keywords} ${item.title}`.toLowerCase();
          return terms.every((term) => haystack.includes(term));
        });
        if (entries.length) {
          sections.push({pane, group: {...item, id: `${pane.id}-${item.id}`, entries}});
        }
      }
    }
    return sections;
  }

  renderSearch_() {
    const sections = this.searchResults_();
    const count = sections.reduce((sum, section) => sum + section.group.entries.length, 0);
    return html`
      <div class="results-title">
        <h1>${L('settings.search.results')}</h1>
        ${count ? html`<span class="results-count">${L('settings.search.count', {count})}</span>` : nothing}
      </div>
      ${sections.length ? nothing : emptyState({iconName: 'magnifyingglass', title: L('settings.search.empty')})}
      ${repeat(sections, (section) => section.group.id, (section) => this.renderGroup_(section.group, html`
        <button class="pane-link" title=${L('settings.search.openPane', {pane: section.pane.title})}
            @click=${() => this.select_(section.pane.id)}>
          ${icon(section.pane.icon, 11)}<span>${section.pane.title}</span>
          <span class="chevron">${icon('chevron.right', 8)}</span>
        </button>`))}`;
  }

  renderSearchField_() {
    return searchField({
      value: this.query_,
      placeholder: L('settings.search.placeholder'),
      onInput: (value) => {
        this.query_ = value.trim() ? value : '';
        const content = this.querySelector('.content');
        if (content) {
          content.scrollTop = 0;
        }
      },
    });
  }

  renderRail_() {
    const items = [];
    let advanced = false;
    for (const pane of PANES.filter((candidate) => !candidate.parent)) {
      if (pane.advanced && !advanced) {
        advanced = true;
        items.push(html`<div class="rail-heading">${L('settings.rail.advanced')}</div>`);
      }
      const active = !this.query_ && pane.id === railPane(this.pane_);
      items.push(html`
        <button class="rail-item ${active ? 'active' : ''}" title=${pane.summary} @click=${() => this.select_(pane.id)}>
          ${icon(pane.icon, 14)}<span>${pane.title}</span>
        </button>`);
    }
    return html`
      <nav class="rail">
        <div class="rail-title">${L('common.settings')}</div>
        ${this.renderSearchField_()}
        <div class="rail-list"><div class="rail-highlight"></div>${items}</div>
        <button class="rail-footer" @click=${() => this.openUpdates_()}>
          <span>Starry Browser · ${this.settings_?.version || ''}</span>
          ${this.update_?.hasAttention ? html`<span class="update-dot" aria-hidden="true"></span>` : nothing}
        </button>
      </nav>`;
  }

  renderCompactBar_() {
    return html`
      <div class="compact-bar">
        <div class="title-line">
          <div class="rail-title">${L('common.settings')}</div>
          <span class="spacer"></span>
          ${this.renderSearchField_()}
        </div>
        <div class="pane-chips">
          ${PANES.filter((pane) => !pane.parent).map((pane) => html`
            <button class="pane-chip ${!this.query_ && pane.id === railPane(this.pane_) ? 'active' : ''}" title=${pane.summary}
                @click=${() => this.select_(pane.id)}>
              ${icon(pane.icon, 11)}<span>${pane.title}</span>
            </button>`)}
        </div>
      </div>`;
  }

  render() {
    if (!this.settings_) {
      return nothing;
    }
    return html`
      <div class="settings ${this.compact_ ? 'compact' : ''}">
        ${this.compact_ ? this.renderCompactBar_() : this.renderRail_()}
        <main class="content">
          <div class="content-inner">${this.query_ ? this.renderSearch_() : this.renderPane_()}</div>
        </main>
      </div>`;
  }
}

customElements.define('starry-settings', StarrySettings);
