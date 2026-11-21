// Copyright 2026 The Starry Browser Authors
//
// 首次启动设置向导（starry://welcome）：欢迎 → 导入 → 外观 →（窗口按钮）→ 新标签页 → 隐私 → AI 助手 → 完成。
// 选项直接复用设置页 Handler 即时生效；AI 助手在点击「继续」时写入，跳过时保持不变。

import {html, LitElement, nothing} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, send, sendWithPromise, showMenu} from './common/browser.js';
import {joinList, L, locale, platform} from './common/l10n.js';
import {icon} from './common/library.js';
import {Motion, reduceMotion} from './common/motion.js';

const STEP_TITLE_KEYS = {
  import: 'welcome.step.import',
  appearance: 'welcome.step.appearance',
  windowControls: 'welcome.step.windowControls',
  newTab: 'welcome.step.newTab',
  privacy: 'welcome.step.privacy',
  ai: 'welcome.step.ai',
  done: 'welcome.step.done',
};
const IMPORT_ITEMS = ['bookmarks', 'history', 'passwords', 'searchEngines', 'autofill'];

// 隐私预设：cookies 1 = 阻止全部第三方 Cookie，2 = 仅无痕窗口阻止。
const ANNOYANCE_LISTS = ['easylist-cookie', 'fanboy-annoyance'];
const PRESETS = {
  standard: {blocking: true, annoyances: false, fingerprint: 'privateBrowsing', cookies: 2, doNotTrack: false},
  strict: {blocking: true, annoyances: true, fingerprint: 'allBrowsing', cookies: 1, doNotTrack: true},
  basic: {blocking: false, annoyances: false, fingerprint: 'privateBrowsing', cookies: 2, doNotTrack: false},
};

function shortcut(key) {
  return platform === 'mac' ? `⌘${key}` : `Ctrl+${key}`;
}

function kbd(key) {
  const parts = platform === 'mac' ? ['⌘', key] : ['Ctrl', key];
  return html`<span class="kbd">${parts.map((part) => html`<i>${part}</i>`)}</span>`;
}

function hash(n) {
  const x = Math.sin(n * 127.1 + 311.7) * 43758.5453;
  return x - Math.floor(x);
}

const STARS = Array.from({length: 110}, (_, i) => ({
  x: hash(i * 3.17) * 100,
  y: Math.pow(hash(i * 7.73), 1.4) * 88,
  size: 0.7 + Math.pow(hash(i * 1.31), 3) * 2.2,
  delay: hash(i * 5.1) * 3.6,
  opacity: 0.3 + hash(i * 9.2) * 0.65,
}));

function itemTitle(item) {
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

function itemCount(item, count) {
  const n = new Intl.NumberFormat(locale).format(count);
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

function importProgress(job, source) {
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

// 窗口布局小示意图（scheme: light/dark/system；highlight 高亮地址栏）。
function thumbOne(scheme, layout, address, highlight) {
  return html`
    <div class="th ${scheme} ${layout} addr-${address} ${highlight ? 'hl' : ''}">
      <div class="th-side"><i></i><i class="s"></i><i></i><i></i></div>
      <div class="th-top"><i class="s"></i><i></i><i></i></div>
      <div class="th-bar"></div>
      <div class="th-card"><s></s><b></b><u></u><u></u><u></u></div>
    </div>`;
}

function thumb(scheme, layout, address, highlight = false) {
  if (scheme === 'system') {
    return html`<div class="th-split">${thumbOne('light', layout, address, highlight)}${
        thumbOne('dark', layout, address, highlight)}</div>`;
  }
  return thumbOne(scheme, layout, address, highlight);
}

function windowControlGlyphs() {
  return html`
    <span class="wc-btn">
      <svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.35" stroke-linecap="round">
        <path d="M2.5 6h7"/>
      </svg>
    </span>
    <span class="wc-btn">
      <svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.25">
        <rect x="2.75" y="2.75" width="6.5" height="6.5" rx="1"/>
      </svg>
    </span>
    <span class="wc-btn close">
      <svg viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.35" stroke-linecap="round">
        <path d="M3 3l6 6M9 3l-6 6"/>
      </svg>
    </span>`;
}

function previewWindow(dark, layout, address, controls = '') {
  const tabs = [true, false, false, false];
  const win = !!controls;
  const lights = win ? nothing : html`<div class="pv-lights"><i></i><i></i><i></i></div>`;
  return html`
    <div class="pv ${dark ? '' : 'light'} ${layout} addr-${address} ${win ? `win-controls wc-${controls}` : ''}"
        aria-hidden="true">
      <div class="pv-side">
        ${lights}<div class="pv-bar"></div>
        <div class="pv-tiles"><i></i><i></i><i></i><i></i></div>
        ${tabs.map((selected) => html`<div class="pv-tab ${selected ? 'sel' : ''}"></div>`)}
      </div>
      <div class="pv-strip">${lights}${tabs.map((selected) => html`<div class="pv-tab ${selected ? 'sel' : ''}"></div>`)}</div>
      <div class="pv-tool"><div class="pv-bar"></div></div>
      ${win ? html`<div class="pv-rail"><span></span></div>` : nothing}
      <div class="pv-card">
        <div class="pv-compact"><span class="pv-nav"><i></i><i></i></span><div class="pv-bar"></div></div>
        <div class="pv-page">
          <div class="site-nav"><span class="site-logo"></span><i></i><i></i><i></i></div>
          <div class="site-body"><b></b><u></u><u></u><u style="width:80%"></u><em></em><u></u><u style="width:70%"></u></div>
        </div>
      </div>
      ${win ? html`<div class="pv-win-hint"></div><div class="pv-win">${windowControlGlyphs()}</div>` : nothing}
    </div>`;
}

// 窗口控制按钮局部放大预览（聚焦窗口右上角，按宽 200 缩放）。
function windowControlsPreview(mode, dark) {
  return html`
    <div class="wcv ${mode} ${dark ? '' : 'light'}" aria-hidden="true">
      <div class="wcv-rail">
        <span class="wcv-rail-title"></span>
        <span class="wcv-rail-btns">${windowControlGlyphs()}</span>
      </div>
      <div class="wcv-card">
        <div class="wcv-nav">
          <span class="wcv-links"><i></i><i></i></span>
          <span class="wcv-site-actions"><i></i><em class="wcv-avatar"></em></span>
        </div>
        <div class="wcv-body"><b></b><u></u><u class="s"></u><em></em></div>
      </div>
      ${mode === 'autoHide' ? html`<div class="wcv-hint"></div>` : nothing}
      ${mode !== 'rail' ? html`<div class="wcv-capsule">${windowControlGlyphs()}</div>` : nothing}
    </div>`;
}

function newTabPreview(style, dark, hasWindowControls = false) {
  const page = html`<div class="ntv-page"><b></b><u></u><u></u><u class="s"></u><em></em></div>`;
  return html`
    <div class="ntv ${style} ${dark ? '' : 'light'} ${hasWindowControls ? 'win-controls' : ''}" aria-hidden="true">
      <div class="ntv-side">
        ${hasWindowControls ? nothing : html`<span class="ntv-lights"><i></i><i></i><i></i></span>`}
        <i class="bar"></i><i></i><i class="s"></i><i></i><i></i>
      </div>
      <div class="ntv-card">
        ${style === 'card' ? html`
          <div class="ntv-hello"></div><div class="ntv-pill"></div>
          <div class="ntv-grid"><i class="w"></i><i></i><i></i><i></i><i></i></div>` : page}
      </div>
      ${hasWindowControls ? html`<div class="ntv-win"><i></i><i></i><i></i></div>` : nothing}
      ${style === 'minimal' ? html`<div class="ntv-dim"></div><div class="ntv-pill floating"></div>` : nothing}
      ${style === 'overlay' ? html`
        <div class="ntv-blur"></div><div class="ntv-pill floating"></div>
        <div class="ntv-tiles">${Array.from({length: 12}, () => html`<i></i>`)}</div>
        <div class="ntv-cards"><i></i><i></i><i></i></div>` : nothing}
    </div>`;
}

class StarryWelcome extends LitElement {
  static properties = {
    step_: {state: true},
    settings_: {state: true},
    blocking_: {state: true},
    import_: {state: true},
    assistant_: {state: true},
    agents_: {state: true},
    importSource_: {state: true},
    importItems_: {state: true},
    aiChoice_: {state: true},
    aiProvider_: {state: true},
    keyDraft_: {state: true},
    customUrl_: {state: true},
    intro_: {state: true},
    leaving_: {state: true},
  };

  constructor() {
    super();
    this.step_ = 0;
    this.settings_ = null;
    this.blocking_ = null;
    this.import_ = null;
    this.assistant_ = null;
    this.agents_ = null;
    this.importSource_ = '';
    this.importItems_ = {};
    this.aiChoice_ = '';
    this.aiProvider_ = '';
    this.keyDraft_ = '';
    this.customUrl_ = '';
    this.intro_ = true;
    this.leaving_ = false;
    this.direction_ = 1;
    this.aiChoiceTouched_ = false;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('settings-changed', (settings) => {
      const currentId = this.stepId_();
      this.settings_ = settings;
      const steps = this.steps_();
      const index = steps.indexOf(currentId);
      if (index >= 0) {
        this.step_ = index;
      } else if (this.step_ >= steps.length) {
        this.step_ = steps.length - 1;
      }
    });
    addWebUiListener('content-blocking-changed', (state) => this.blocking_ = state);
    addWebUiListener('import-changed', (state) => this.import_ = state);
    addWebUiListener('assistant-settings-changed', (state) => this.assistant_ = state);
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => this.requestUpdate());
    sendWithPromise('getContentBlocking').then((state) => this.blocking_ = state);
    sendWithPromise('getImportState').then((state) => this.import_ = state);
    sendWithPromise('getAssistantSettings').then((state) => {
      this.assistant_ = state;
      this.pickDefaultAi_();
    });
    // 本机智能体检测需启动子进程，页面加载时提前开始。
    sendWithPromise('detectLocalAgents').then((agents) => {
      this.agents_ = agents;
      this.pickDefaultAi_();
    });
    this.settings_ = await sendWithPromise('getSettings');
    send('checkDefaultBrowser');
  }

  updated(changed) {
    const initial = changed.has('settings_') && !changed.get('settings_') && !!this.settings_;
    if (initial) {
      this.playIntro_();
    }
    const entered = changed.has('step_') && changed.get('step_') !== undefined;
    if (entered) {
      this.enterStep_();
    }
    this.placeRings_(entered || initial);
  }

  // ---- 动效 ----

  playIntro_() {
    const blocks = [...this.querySelectorAll('.step .st')];
    if (!reduceMotion()) {
      blocks.forEach((block, index) => block.animate(
          [{opacity: 0, transform: 'translateY(14px)'}, {opacity: 1, transform: 'none'}],
          {duration: Motion.listEdit.duration, easing: Motion.listEdit.easing,
           delay: 250 + index * Motion.staggerStep, fill: 'backwards'}));
    }
    // 星星、地平线、图标的出场在 CSS 里（.stage.intro 拿掉时过渡）。
    requestAnimationFrame(() => requestAnimationFrame(() => this.intro_ = false));
    this.focusPrimary_();
  }

  steps_() {
    const steps = ['welcome', 'import', 'appearance'];
    if (this.settings_?.hasWindowControls) {
      steps.push('windowControls');
    }
    steps.push('newTab', 'privacy', 'ai', 'done');
    return steps;
  }

  stepId_() {
    const steps = this.steps_();
    return steps[this.step_] || steps[0];
  }

  // 换页：旧的一页拷一份盖在原处淡出，新的一页里的块错开 28 ms 弹进来（kListEdit）。
  go_(direction) {
    const steps = this.steps_();
    const next = this.step_ + direction;
    if (next < 0 || next >= steps.length || this.leaving_) {
      return;
    }
    const page = this.querySelector('.step');
    const ghosts = this.querySelector('.ghosts');
    if (page && ghosts && !reduceMotion()) {
      const rect = page.getBoundingClientRect();
      const base = ghosts.getBoundingClientRect();
      const ghost = page.cloneNode(true);
      ghost.style.left = `${rect.left - base.left}px`;
      ghost.style.top = `${rect.top - base.top}px`;
      ghost.style.width = `${rect.width}px`;
      ghosts.appendChild(ghost);
      ghost.animate([{opacity: 1, transform: 'none'}, {opacity: 0, transform: `translateX(${-28 * direction}px)`}],
                    {duration: 160, easing: 'ease-in', fill: 'forwards'})
          .finished.then(() => ghost.remove());
    }
    this.direction_ = direction;
    this.step_ = next;
  }

  enterStep_() {
    const pages = this.querySelector('.pages');
    if (pages) {
      pages.scrollTop = 0;
    }
    if (!reduceMotion()) {
      const blocks = [...this.querySelectorAll('.step .st')];
      blocks.forEach((block, index) => block.animate(
          [{opacity: 0, transform: `translateX(${32 * this.direction_}px)`}, {opacity: 1, transform: 'none'}],
          {duration: Motion.listEdit.duration, easing: Motion.listEdit.easing,
           delay: 50 + Math.min(index, Motion.staggerLimit) * Motion.staggerStep, fill: 'backwards'}));
    }
    this.focusPrimary_();
  }

  focusPrimary_() {
    this.querySelector('.step .btn.primary, .foot .btn.primary')?.focus({preventScroll: true});
  }

  // 视觉单选的选中框：移到选中的那一项（换了一页时直接放过去）。
  placeRings_(instant) {
    for (const pick of this.querySelectorAll('.pick')) {
      const ring = pick.querySelector(':scope > .ring');
      const selected = pick.querySelector(':scope > .sel');
      if (!ring) {
        continue;
      }
      ring.hidden = !selected;
      if (!selected) {
        continue;
      }
      if (instant) {
        ring.classList.add('noanim');
      }
      ring.style.transform = `translate(${selected.offsetLeft}px, ${selected.offsetTop}px)`;
      ring.style.width = `${selected.offsetWidth}px`;
      ring.style.height = `${selected.offsetHeight}px`;
      if (instant) {
        void ring.offsetWidth;
        ring.classList.remove('noanim');
      }
    }
  }

  // ---- 设置 ----

  set_(key, value) {
    this.settings_ = {...this.settings_, [key]: value};
    send('setSetting', key, value);
  }

  isDark_() {
    const scheme = this.settings_.colorScheme;
    return scheme === 'dark' || (scheme === 'system' && matchMedia('(prefers-color-scheme: dark)').matches);
  }

  // 地址栏实际放在哪：顶部标签布局下总在顶部（addressBarOnTop 只管侧栏布局），紧凑只在顶部时有效（同设置页）。
  addressPosition_() {
    const s = this.settings_;
    const onTop = s.sidebarLayout === 'top' || s.addressBarOnTop;
    return !onTop ? 'sidebar' : (s.compactAddressBar ? 'compact' : 'top');
  }

  setAddressPosition_(position) {
    if (this.settings_.sidebarLayout !== 'top') {
      this.set_('addressBarOnTop', position !== 'sidebar');
    }
    this.set_('compactAddressBar', position === 'compact');
  }

  windowControlsChoice_() {
    const s = this.settings_;
    if (s.windowsControlsStyle === 'rail') {
      return 'rail';
    }
    return s.windowsControlsAutoHide ? 'autoHide' : 'glass';
  }

  setWindowControlsChoice_(choice) {
    if (choice === 'rail') {
      this.set_('windowsControlsStyle', 'rail');
      return;
    }
    this.set_('windowsControlsStyle', 'glass');
    this.set_('windowsControlsAutoHide', choice === 'autoHide');
  }

  // 现在的设置是哪一档（自己在设置里调过、对不上的算标准）。
  currentPreset_() {
    const s = this.settings_;
    const blocking = this.blocking_;
    if (blocking && !blocking.enabled) {
      return 'basic';
    }
    const annoyances = blocking && ANNOYANCE_LISTS.every(
        (id) => blocking.subscriptions?.some((list) => list.id === id && list.enabled));
    if (annoyances && s.fingerprintProtection === 'allBrowsing' && s.cookies === 1) {
      return 'strict';
    }
    return 'standard';
  }

  applyPreset_(name) {
    const preset = PRESETS[name];
    send('setContentBlocking', 'enabled', preset.blocking);
    send('setContentBlocking', 'stripTrackingParameters', true);
    for (const id of ANNOYANCE_LISTS) {
      send('setFilterListEnabled', id, preset.annoyances);
    }
    if (this.blocking_) {
      this.blocking_ = {
        ...this.blocking_,
        enabled: preset.blocking,
        subscriptions: (this.blocking_.subscriptions || []).map(
            (list) => ANNOYANCE_LISTS.includes(list.id) ? {...list, enabled: preset.annoyances} : list),
      };
    }
    this.set_('fingerprintProtection', preset.fingerprint);
    this.set_('cookies', preset.cookies);
    this.set_('doNotTrack', preset.doNotTrack);
  }

  async chooseLanguage_(event) {
    const s = this.settings_;
    const id = await showMenu(s.interfaceLanguages.map((language) => ({
      id: language.id || 'system',
      title: language.name,
      checked: language.id === s.interfaceLanguage,
    })), {element: event.currentTarget});
    if (id) {
      this.set_('interfaceLanguage', id === 'system' ? '' : id);
    }
  }

  // ---- 导入 ----

  importSelection_() {
    const state = this.import_;
    const ids = state ? state.sources.map((source) => source.id) : [];
    const job = state && state.job;
    return [this.importSource_, job && job.sourceId, ids[0]].find((id) => id && ids.includes(id)) || '';
  }

  importItemsFor_(source) {
    return this.importItems_[source.id] ||
        Object.fromEntries(IMPORT_ITEMS.map((item) => [item, !!source.items[item]]));
  }

  toggleImportItem_(source, item) {
    const items = this.importItemsFor_(source);
    this.importItems_ = {...this.importItems_, [source.id]: {...items, [item]: !items[item]}};
  }

  startImport_() {
    const source = this.import_.sources.find((candidate) => candidate.id === this.importSelection_());
    if (source) {
      send('startImport', source.id, this.importItemsFor_(source));
    }
  }

  // ---- AI 助手 ----

  providers_() {
    return (this.assistant_ && this.assistant_.providers) || [];
  }

  provider_() {
    const providers = this.providers_();
    return providers.find((provider) => provider.id === this.aiProvider_) ||
        providers.find((provider) => !provider.local && !provider.custom) || providers[0];
  }

  // 默认选哪个：已经配过的照旧，否则找到的本机智能体（登录了的优先），都没有是模型服务。
  pickDefaultAi_() {
    const a = this.assistant_;
    if (!a || this.aiChoiceTouched_) {
      return;
    }
    if (!this.aiProvider_ && a.provider) {
      this.aiProvider_ = a.provider;
    }
    if (a.enabled === false) {
      this.aiChoice_ = 'off';
      return;
    }
    if (a.backend === 'claudeCode' || a.backend === 'codex') {
      this.aiChoice_ = a.backend;
      return;
    }
    if (a.provider) {
      this.aiChoice_ = 'service';
      return;
    }
    const agents = this.agents_;
    if (!agents) {
      return;
    }
    const found = ['claudeCode', 'codex'].filter((kind) => agents[kind] && agents[kind].found);
    this.aiChoice_ = found.find((kind) => agents[kind].loggedIn !== false) || found[0] || 'service';
  }

  chooseAi_(choice) {
    this.aiChoiceTouched_ = true;
    this.aiChoice_ = choice;
  }

  async chooseProvider_(event) {
    const current = this.provider_();
    const id = await showMenu(this.providers_().map((provider) => ({
      id: provider.id,
      title: provider.title,
      checked: current && provider.id === current.id,
    })), {element: event.currentTarget});
    if (id) {
      this.aiProvider_ = id;
      this.keyDraft_ = '';
    }
  }

  // 「继续」时写进设置。跳过时不动。
  commitAi_() {
    switch (this.aiChoice_) {
      case 'claudeCode':
      case 'codex':
        send('setAssistantSetting', 'enabled', true);
        send('setAssistantSetting', 'backend', this.aiChoice_);
        break;
      case 'service': {
        const provider = this.provider_();
        send('setAssistantSetting', 'enabled', true);
        send('setAssistantSetting', 'backend', 'chat');
        if (provider) {
          send('setAssistantSetting', 'provider', provider.id);
          if (provider.custom && this.customUrl_.trim()) {
            send('setAssistantSetting', 'customUrl', this.customUrl_.trim());
          }
          // Key 存在当前的服务下面：先换服务再存。
          if (this.keyDraft_.trim()) {
            sendWithPromise('setAssistantKey', this.keyDraft_.trim());
          }
        }
        break;
      }
      case 'off':
        send('setAssistantSetting', 'enabled', false);
        break;
    }
  }

  finish_() {
    if (this.leaving_) {
      return;
    }
    this.leaving_ = true;
    send('finishOnboarding');
  }

  // ---- 显示 ----

  render() {
    const steps = this.steps_();
    const stepId = this.stepId_();
    const atWelcome = stepId === 'welcome';
    const wide = stepId === 'appearance';
    return html`
      <div class="stage ${atWelcome ? 'at-welcome' : ''} ${this.intro_ ? 'intro' : ''} ${this.leaving_ ? 'leaving' : ''}">
        <div class="stars" aria-hidden="true">
          ${STARS.map((star) => html`<i style="left:${star.x}%;top:${star.y}%;width:${star.size}px;height:${star.size}px;--o:${star.opacity};animation-delay:-${star.delay}s"></i>`)}
        </div>
        <div class="horizon" aria-hidden="true"></div>
        <div class="top" aria-hidden=${atWelcome ? 'true' : 'false'}>${this.renderProgress_()}</div>
        <main class="pages">
          ${this.settings_ ? html`
            <div class="step ${wide ? 'wide' : ''}" data-step=${stepId}
                aria-label=${L('welcome.stepLabel', {step: this.step_, count: steps.length - 1})}>
              ${this.renderStep_()}
            </div>` : nothing}
        </main>
        <div class="ghosts"></div>
        <div class="foot"><div class="in ${wide ? 'wide' : ''}">${this.settings_ ? this.renderFoot_() : nothing}</div></div>
        <div class="fill"></div>
      </div>`;
  }

  renderProgress_() {
    const steps = this.steps_().slice(1);
    return steps.map((id, index) => {
      const step = index + 1;
      const state = step < this.step_ ? 'done' : step === this.step_ ? 'cur' : '';
      return html`<div class="pseg ${state}">${L(STEP_TITLE_KEYS[id])}</div>`;
    });
  }

  renderStep_() {
    switch (this.stepId_()) {
      case 'welcome':
        return this.renderWelcome_();
      case 'import':
        return this.renderImport_();
      case 'appearance':
        return this.renderAppearance_();
      case 'windowControls':
        return this.renderWindowControls_();
      case 'newTab':
        return this.renderNewTab_();
      case 'privacy':
        return this.renderPrivacy_();
      case 'ai':
        return this.renderAi_();
      case 'done':
        return this.renderDone_();
    }
    return nothing;
  }

  renderFoot_() {
    const stepId = this.stepId_();
    if (stepId === 'welcome') {
      return nothing;
    }
    const back = html`
      <button class="btn text" @click=${() => this.go_(-1)}>${icon('chevron.left', 11)}${L('welcome.back')}</button>`;
    const next = (label = L('welcome.continue'), action = () => this.go_(1), disabled = false) => html`
      <button class="btn primary" ?disabled=${disabled} @click=${action}>${label}</button>`;
    const skip = html`<button class="btn text" @click=${() => this.go_(1)}>${L('welcome.skip')}</button>`;
    let trailing;
    switch (stepId) {
      case 'import': {
        const state = this.import_;
        const job = state && state.job;
        const selected = this.importSelection_();
        const source = state && state.sources.find((candidate) => candidate.id === selected);
        // 在导入或者导入完了：接着往下（导入在后台做完）。
        if (!source || (job && (job.status === 'running' || (job.sourceId === selected && job.status === 'done')))) {
          trailing = next();
        } else {
          const items = this.importItemsFor_(source);
          const chosen = IMPORT_ITEMS.some((item) => source.items[item] && items[item]);
          trailing = html`${skip}${next(L('welcome.import.action'), () => this.startImport_(), !chosen)}`;
        }
        break;
      }
      case 'ai':
        trailing = html`${skip}${next(L('welcome.continue'), () => {
          this.commitAi_();
          this.go_(1);
        }, !this.aiChoice_)}`;
        break;
      case 'done':
        trailing = next(L('welcome.finish'), () => this.finish_());
        break;
      default:
        trailing = next();
    }
    return html`${back}<span class="sp"></span>${trailing}`;
  }

  switchRow_({key, iconName, title, detail, on, disabled = false, onChange}) {
    return html`
      <div class="line ${disabled ? 'off' : ''}" data-key=${key}>
        <span class="lead ${on && !disabled ? 'on' : ''}">${icon(iconName, 15)}</span>
        <div class="labels"><b>${title}</b>${detail ? html`<span>${detail}</span>` : nothing}</div>
        <button class="switch ${on ? 'on' : ''}" role="switch" aria-checked=${on ? 'true' : 'false'}
            aria-label=${title} ?disabled=${disabled} @click=${() => onChange(!on)}></button>
      </div>`;
  }

  renderWelcome_() {
    const s = this.settings_;
    const language = s.interfaceLanguages.find((candidate) => candidate.id === s.interfaceLanguage);
    return html`
      <div class="welcome">
        <img class="app-icon" src="icon.svg" alt="">
        <h1 class="st">${L('welcome.title')}</h1>
        <div class="actions st">
          <button class="btn lg primary" @click=${() => this.go_(1)}>${L('welcome.start')}</button>
        </div>
        <div class="language-row st">
          <button class="language" aria-label=${L('settings.general.interfaceLanguage.title')}
              @click=${(event) => this.chooseLanguage_(event)}>
            ${icon('globe', 13)}${language ? language.name : ''}${icon('chevron.down', 10)}
          </button>
          ${s.interfaceLanguageNeedsRelaunch ? html`
            <button class="language relaunch" title=${L('welcome.relaunchDetail')} @click=${() => send('relaunch')}>
              ${icon('arrow.clockwise', 12)}${L('welcome.relaunch')}
            </button>` : nothing}
        </div>
        ${s.interfaceLanguageNeedsRelaunch ? html`<div class="note st">${L('welcome.relaunchDetail')}</div>` : nothing}
      </div>`;
  }

  renderImport_() {
    const state = this.import_;
    const sources = state ? state.sources : [];
    const job = state && state.job;
    const busy = !!job && job.status === 'running';
    const selected = this.importSelection_();
    let list;
    if (!state || (state.detecting && !sources.length)) {
      list = html`
        <div class="line"><span class="spinner"></span><div class="labels"><b>${L('settings.import.detecting')}</b></div></div>`;
    } else if (!sources.length) {
      list = html`
        <div class="line">
          <span class="lead">${icon('magnifyingglass', 15)}</span>
          <div class="labels"><b>${L('settings.import.none.title')}</b><span>${L('welcome.import.noneDetail')}</span></div>
          <button class="btn secondary" @click=${() => send('detectImportSources')}>${L('settings.import.redetect')}</button>
        </div>`;
    } else {
      list = sources.map((source) => this.renderImportSource_(source, source.id === selected, job, busy));
    }
    const source = sources.find((candidate) => candidate.id === selected);
    return html`
      <h1 class="st">${L('welcome.import.title')}</h1>
      <div class="sub st">${L('welcome.import.subtitle')}</div>
      <div class="list st" role="radiogroup">${list}</div>
      ${state && state.safariNeedsAccess ? html`
        <div class="list st">
          <div class="line">
            <span class="browser">${icon('safari', 16)}</span>
            <div class="labels"><b>Safari</b><span>${L('settings.import.safari.needsAccess')}</span></div>
            <button class="btn secondary" @click=${() => send('openFullDiskAccess')}>${L('settings.import.safari.openSettings')}</button>
          </div>
        </div>` : nothing}
      ${this.renderImportJob_(job, source)}
      <div class="note st">
        ${icon('info.circle', 13)}
        <span>
          ${platform === 'mac' ? L('welcome.import.noteMac') : L('welcome.import.note')}
          <button class="link" ?disabled=${busy} @click=${() => send('importBookmarksFile')}>${L('welcome.import.bookmarksFile')}</button>
        </span>
      </div>`;
  }

  renderImportSource_(source, selected, job, busy) {
    const items = this.importItemsFor_(source);
    let status = nothing;
    if (job && job.sourceId === source.id) {
      if (job.status === 'running') {
        status = html`<span class="status"><span class="spinner"></span></span>`;
      } else if (job.status === 'done') {
        status = html`<span class="status ok pop">${icon('checkmark', 11)}${L('welcome.import.imported')}</span>`;
      } else {
        status = html`<span class="status warn">${icon('exclamationmark.triangle.fill', 11)}${L('settings.import.failed.title')}</span>`;
      }
    }
    return html`
      <div class="item">
        <button class="line" role="radio" aria-checked=${selected ? 'true' : 'false'} ?disabled=${busy}
            @click=${() => this.importSource_ = source.id}>
          <span class="radio ${selected ? 'on' : ''}"></span>
          <span class="browser">${icon(source.safari ? 'safari' : 'globe', 16)}</span>
          <div class="labels"><b>${source.browser}</b>${source.profile ? html`<span>${source.profile}</span>` : nothing}</div>
          ${status}
        </button>
        ${selected ? html`
          <div class="chips">
            ${IMPORT_ITEMS.filter((item) => source.items[item]).map((item) => html`
              <button class="chip ${items[item] ? '' : 'off'}" aria-pressed=${items[item] ? 'true' : 'false'}
                  ?disabled=${busy} @click=${() => this.toggleImportItem_(source, item)}>
                ${items[item] ? icon('checkmark', 10) : nothing}${itemTitle(item)}
              </button>`)}
          </div>` : nothing}
      </div>`;
  }

  renderImportJob_(job, source) {
    if (!job) {
      return nothing;
    }
    if (job.status === 'running') {
      return html`
        <div class="note st"><span class="spinner"></span>
          <span>${L('settings.import.running.title', {browser: job.name})} ${importProgress(job, source)}</span>
        </div>`;
    }
    if (job.status === 'failed') {
      const reason = job.error === 'firefoxRunning' ? L('settings.import.error.firefoxRunning') :
                     job.sourceId === 'bookmarksFile' ? L('settings.import.error.bookmarksFile') :
                     L('settings.import.error.unreadable', {browser: job.name});
      return html`<div class="note warn st">${icon('exclamationmark.triangle.fill', 13)}<span>${reason}</span></div>`;
    }
    const counts = IMPORT_ITEMS.filter((item) => job.items[item] && job.counts && job.counts[item] > 0)
                       .map((item) => itemCount(item, job.counts[item]));
    return html`
      <div class="note st">
        ${icon('checkmark.circle', 13)}
        <span>${counts.length ? L('settings.import.done.imported', {items: joinList(counts)}) :
                                L('settings.import.done.empty')}</span>
      </div>`;
  }

  renderAppearance_() {
    const s = this.settings_;
    const dark = this.isDark_();
    const sidebar = s.sidebarLayout !== 'top';
    const schemes = [
      ['system', L('appearance.system')],
      ['light', L('appearance.light')],
      ['dark', L('appearance.dark')],
    ];
    const layouts = [
      ['docked', L('layout.dockedSidebar')],
      ['floating', L('layout.floatingSidebar')],
      ['top', L('layout.top')],
    ];
    const address = this.addressPosition_();
    const addresses = [
      ['sidebar', L('welcome.appearance.address.sidebar')],
      ['top', L('welcome.appearance.address.top')],
      ['compact', L('welcome.appearance.address.compact')],
    ];
    const option = (value, title, selected, preview, onSelect, {disabled = false, tip = ''} = {}) => html`
      <button class="opt ${selected ? 'sel' : ''}" role="radio" aria-checked=${selected ? 'true' : 'false'}
          data-value=${value} ?disabled=${disabled} title=${tip || nothing} @click=${onSelect}>
        <div class="thumb">${preview}</div><div class="opt-t">${title}</div>
      </button>`;
    return html`
      <div class="two">
        <div class="col">
          <h1 class="st">${L('welcome.appearance.title')}</h1>
          <div class="sec st">${L('welcome.appearance.theme')}</div>
          <div class="st">
            <div class="pick" role="radiogroup" aria-label=${L('welcome.appearance.theme')} data-pick="scheme">
              <div class="ring"></div>
              ${schemes.map(([value, title]) => option(value, title, s.colorScheme === value,
                                                       thumb(value, 'docked', 'sidebar'),
                                                       () => this.set_('colorScheme', value)))}
            </div>
          </div>
          <div class="sec st">${L('welcome.appearance.tabs')}</div>
          <div class="st">
            <div class="pick" role="radiogroup" aria-label=${L('welcome.appearance.tabs')} data-pick="layout">
              <div class="ring"></div>
              ${layouts.map(([value, title]) => option(value, title, s.sidebarLayout === value,
                                                       thumb(dark ? 'dark' : 'light', value, address),
                                                       () => this.set_('sidebarLayout', value)))}
            </div>
          </div>
          ${s.sidebarLayout === 'floating' ? html`
            <div class="note">${icon('info.circle', 13)}<span>${L('settings.appearance.sidebarAutoHide.detail')}</span></div>` : nothing}
          <div class="sec st">${L('welcome.appearance.address')}</div>
          <div class="st">
            <div class="pick" role="radiogroup" aria-label=${L('welcome.appearance.address')} data-pick="address">
              <div class="ring"></div>
              ${addresses.map(([value, title]) => {
                // 顶部标签布局下地址栏不能放进侧栏：这一项变灰，示意图照固定侧栏画。
                const unavailable = value === 'sidebar' && !sidebar;
                return option(value, title, address === value,
                              thumb(dark ? 'dark' : 'light', unavailable ? 'docked' : s.sidebarLayout, value, true),
                              () => this.setAddressPosition_(value),
                              {disabled: unavailable, tip: unavailable ? L('settings.appearance.sidebarOnly') : ''});
              })}
            </div>
          </div>
          <div class="lines st">
            ${this.switchRow_({
              key: 'tint', iconName: 'paintpalette', title: L('settings.appearance.tintFromWebsite.title'),
              detail: L('welcome.appearance.tintDetail'), on: s.tintFromWebsite,
              onChange: (value) => this.set_('tintFromWebsite', value),
            })}
            ${this.switchRow_({
              key: 'tree', iconName: 'list.bullet.indent', title: L('settings.appearance.treeTabs.title'),
              detail: sidebar ? L('welcome.appearance.treeDetail') : L('settings.appearance.sidebarOnly'),
              on: s.treeTabs, disabled: !sidebar, onChange: (value) => this.set_('treeTabs', value),
            })}
          </div>
        </div>
        <div class="preview st">
          ${previewWindow(dark, s.sidebarLayout, address, s.hasWindowControls ? this.windowControlsChoice_() : '')}
          <div class="preview-caption">${icon('eye', 12)}${L('welcome.appearance.preview')}</div>
        </div>
      </div>`;
  }

  renderWindowControls_() {
    const s = this.settings_;
    const dark = this.isDark_();
    const current = this.windowControlsChoice_();
    const sidebarOnly = s.sidebarLayout !== 'top' && !s.addressBarOnTop;
    const choices = [
      {value: 'glass', title: L('welcome.windowControls.glass.title'),
       badge: L('welcome.privacy.recommended'),
       detail: L('welcome.windowControls.glass.detail')},
      {value: 'autoHide', title: L('welcome.windowControls.autoHide.title'),
       detail: L('welcome.windowControls.autoHide.detail')},
      {value: 'rail', title: L('welcome.windowControls.rail.title'),
       detail: L('welcome.windowControls.rail.detail')},
    ];
    return html`
      <h1 class="st">${L('welcome.windowControls.title')}</h1>
      <div class="sub st">${L('welcome.windowControls.subtitle')}</div>
      <div class="st" style="margin-top:20px">
        <div class="pick grid3" role="radiogroup" aria-label=${L('welcome.windowControls.title')}
            data-pick="windowControls">
          <div class="ring" style="border-radius:14px"></div>
          ${choices.map((choice) => html`
            <button class="preset wcv-option ${current === choice.value ? 'sel' : ''}" role="radio"
                aria-checked=${current === choice.value ? 'true' : 'false'} data-value=${choice.value}
                @click=${() => this.setWindowControlsChoice_(choice.value)}>
              ${windowControlsPreview(choice.value, dark)}
              <div class="preset-head">
                ${choice.title}
                ${choice.badge ? html`<span class="badge">${choice.badge}</span>` : nothing}
              </div>
              <div class="ntp-detail">${choice.detail}</div>
            </button>`)}
        </div>
      </div>
      <div class="note st">
        ${icon('info.circle', 13)}
        <span>${sidebarOnly ? L('welcome.windowControls.note') : L('welcome.windowControls.noteBarLayout')}</span>
      </div>`;
  }

  renderNewTab_() {
    const s = this.settings_;
    const dark = this.isDark_();
    const current = s.newTabStyle || 'minimal';
    const styles = [
      {value: 'minimal', title: L('newTab.style.minimal'), detail: L('newTab.style.minimal.detail')},
      {value: 'card', title: L('newTab.style.card'), detail: L('newTab.style.card.detail')},
      {value: 'overlay', title: L('newTab.style.overlay'), detail: L('newTab.style.overlay.detail')},
    ];
    return html`
      <h1 class="st">${L('welcome.newTab.title')}</h1>
      <div class="sub st">${L('welcome.newTab.subtitle', {shortcut: shortcut('T')})}</div>
      <div class="st" style="margin-top:20px">
        <div class="pick grid3" role="radiogroup" aria-label=${L('welcome.newTab.title')} data-pick="newTab">
          <div class="ring" style="border-radius:14px"></div>
          ${styles.map((style) => html`
            <button class="preset ntp-option ${current === style.value ? 'sel' : ''}" role="radio"
                aria-checked=${current === style.value ? 'true' : 'false'} data-value=${style.value}
                @click=${() => this.set_('newTabStyle', style.value)}>
              ${newTabPreview(style.value, dark, s.hasWindowControls)}
              <div class="preset-head">${style.title}</div>
              <div class="ntp-detail">${style.detail}</div>
            </button>`)}
        </div>
      </div>
      <div class="note st">${icon('info.circle', 13)}<span>${L('welcome.newTab.note')}</span></div>`;
  }

  renderPrivacy_() {
    const s = this.settings_;
    const current = this.currentPreset_();
    const point = (kind, text) => {
      const iconName = {yes: 'checkmark', part: 'circle', no: 'xmark', warn: 'exclamationmark.triangle.fill'}[kind];
      return html`<li class=${kind}>${icon(iconName, 11)}<span>${text}</span></li>`;
    };
    const presets = [
      {value: 'standard', iconName: 'checkmark.shield', title: L('welcome.privacy.standard'),
       badge: L('welcome.privacy.recommended'),
       points: [point('yes', L('welcome.privacy.point.blocking')), point('yes', L('welcome.privacy.point.stripParams')),
                point('part', L('welcome.privacy.point.privateOnly'))]},
      {value: 'strict', iconName: 'shield.lefthalf.filled', title: L('welcome.privacy.strict'),
       points: [point('yes', L('welcome.privacy.point.annoyances')), point('yes', L('welcome.privacy.point.fingerprintAll')),
                point('yes', L('welcome.privacy.point.cookiesAll')), point('yes', L('welcome.privacy.point.doNotTrack')),
                point('warn', L('welcome.privacy.point.mayBreak'))]},
      {value: 'basic', iconName: 'shield.slash', title: L('welcome.privacy.basic'),
       points: [point('no', L('welcome.privacy.point.noBlocking')), point('yes', L('welcome.privacy.point.stripParams')),
                point('part', L('welcome.privacy.point.privateKept'))]},
    ];
    const engines = (s.searchEngines || []).slice(0, 6);
    return html`
      <h1 class="st">${L('welcome.privacy.title')}</h1>
      <div class="st" style="margin-top:20px">
        <div class="pick grid3" role="radiogroup" aria-label=${L('welcome.privacy.title')} data-pick="privacy">
          <div class="ring" style="border-radius:14px"></div>
          ${presets.map((preset) => html`
            <button class="preset ${current === preset.value ? 'sel' : ''}" role="radio"
                aria-checked=${current === preset.value ? 'true' : 'false'} data-value=${preset.value}
                @click=${() => this.applyPreset_(preset.value)}>
              <div class="preset-head">
                ${icon(preset.iconName, 16)}${preset.title}
                ${preset.badge ? html`<span class="badge">${preset.badge}</span>` : nothing}
              </div>
              <ul class="points">${preset.points}</ul>
            </button>`)}
        </div>
      </div>
      <div class="sec st">${L('welcome.privacy.search')}</div>
      <div class="engines st" role="radiogroup" aria-label=${L('settings.general.searchEngine.title')}>
        ${engines.map((engine) => html`
          <button class="engine ${s.searchEngine === engine.id ? 'sel' : ''}" role="radio"
              aria-checked=${s.searchEngine === engine.id ? 'true' : 'false'} ?disabled=${s.searchEngineManaged}
              @click=${() => this.set_('searchEngine', engine.id)}>${engine.name}</button>`)}
      </div>
      <div class="lines st">
        ${this.switchRow_({
          key: 'suggest', iconName: 'magnifyingglass', title: L('settings.general.searchSuggestions.title'),
          detail: L('settings.general.searchSuggestions.detail'), on: s.searchSuggest,
          onChange: (value) => this.set_('searchSuggest', value),
        })}
      </div>`;
  }

  renderAi_() {
    const agents = this.agents_;
    const local = (value, name, command, loginCommand) => {
      const info = agents && agents[value];
      const found = !!(info && info.found);
      let detail;
      let status = nothing;
      if (!agents) {
        detail = L('welcome.ai.checking');
        status = html`<span class="status"><span class="spinner"></span></span>`;
      } else if (!found) {
        detail = L('welcome.ai.local.missing', {command});
      } else if (info.loggedIn === false) {
        detail = L('welcome.ai.local.loggedOut', {command: loginCommand});
        status = html`<span class="status warn">${L('welcome.ai.local.notLoggedIn')}</span>`;
      } else {
        detail = L('welcome.ai.local.ready', {name});
        status = html`<span class="status ok">${icon('checkmark', 11)}${L('welcome.ai.local.found')}</span>`;
      }
      return this.renderAiChoice_({value, iconName: 'brain', title: L('welcome.ai.local.title', {name}), detail,
                                   status, disabled: !found});
    };
    const provider = this.provider_();
    const a = this.assistant_ || {};
    const keySaved = a.hasKey && provider && provider.id === a.provider;
    const more = provider ? html`
      <div class="choice-more">
        <button class="menu-button" @click=${(event) => this.chooseProvider_(event)}>
          ${provider.title}${icon('chevron.down', 10)}
        </button>
        ${provider.custom ? html`
          <input class="input" .value=${this.customUrl_} placeholder=${L('settings.ai.assistant.custom.urlPlaceholder')}
              spellcheck="false" autocomplete="off" @input=${(event) => this.customUrl_ = event.target.value}>` : nothing}
        ${provider.requiresKey || provider.custom ? html`
          <input class="input" type="password" .value=${this.keyDraft_} spellcheck="false" autocomplete="off"
              placeholder=${keySaved ? L('settings.ai.assistant.key.saved') : L('settings.ai.assistant.key.placeholder')}
              @input=${(event) => this.keyDraft_ = event.target.value}>` : nothing}
      </div>` : nothing;
    return html`
      <div class="title-row st"><span class="ai-mark">${icon('sparkles', 15)}</span><h1>${L('welcome.ai.title')}</h1></div>
      <div class="sub st">${L('welcome.ai.subtitle', {shortcut: shortcut('E')})}</div>
      <div class="choices" role="radiogroup">
        ${local('claudeCode', 'Claude Code', 'claude', 'claude')}
        ${local('codex', 'Codex', 'codex', 'codex login')}
        ${this.renderAiChoice_({value: 'service', iconName: 'globe', title: L('welcome.ai.service.title'),
                                detail: L('welcome.ai.service.detail'), more})}
        ${this.renderAiChoice_({value: 'off', iconName: 'xmark', title: L('welcome.ai.off.title'),
                                detail: L('welcome.ai.off.detail', {shortcut: shortcut('E')})})}
      </div>
      <div class="note st">${icon('info.circle', 13)}<span>${L('welcome.ai.note')}</span></div>`;
  }

  renderAiChoice_({value, iconName, title, detail, status = nothing, disabled = false, more = nothing}) {
    const selected = this.aiChoice_ === value;
    return html`
      <div class="choice st ${selected ? 'sel' : ''} ${disabled ? 'disabled' : ''}" data-value=${value}>
        <button class="line" role="radio" aria-checked=${selected ? 'true' : 'false'} ?disabled=${disabled}
            @click=${() => this.chooseAi_(value)}>
          <span class="radio"></span>
          <span class="lead">${icon(iconName, 15)}</span>
          <div class="labels"><b>${title}</b><span>${detail}</span></div>
          ${status}
        </button>
        ${more !== nothing ? html`<div class="fold ${selected ? 'open' : ''}"><div>${more}</div></div>` : nothing}
      </div>`;
  }

  renderDone_() {
    const s = this.settings_;
    const browser = s.defaultBrowser || {};
    const isDefault = browser.state === 'default';
    return html`
      <h1 class="st">${L('welcome.done.title')}</h1>
      <div class="sub st">${L('welcome.done.subtitle')}</div>
      <div class="default-browser st">
        <img src="icon.svg" alt="">
        <div class="labels"><b>${L('welcome.done.defaultBrowser')}</b><span>${L('welcome.done.defaultBrowserDetail')}</span></div>
        ${isDefault ? html`
          <span class="status ok pop">${icon('checkmark', 11)}${L('welcome.done.isDefault')}</span>` : html`
          <button class="btn" ?disabled=${!browser.canSet || browser.state === 'checking'}
              @click=${() => send('setDefaultBrowser')}>${L('welcome.done.setDefault')}</button>`}
      </div>
      <div class="sec st">${L('welcome.done.defaults')}</div>
      <div class="lines st" style="margin-top:0">
        ${this.switchRow_({
          key: 'external', iconName: 'arrow.up.right.square', title: L('welcome.done.miniWindow'),
          detail: L('welcome.done.miniWindowDetail', {shortcut: shortcut('O')}), on: s.externalLinks !== 'browserWindow',
          onChange: (value) => this.set_('externalLinks', value ? 'miniWindow' : 'browserWindow'),
        })}
        ${this.switchRow_({
          key: 'archive', iconName: 'archivebox', title: L('welcome.done.archive'),
          detail: L('welcome.done.archiveDetail'), on: s.autoArchive,
          onChange: (value) => this.set_('autoArchive', value),
        })}
        ${this.switchRow_({
          key: 'pip', iconName: 'play.rectangle', title: L('welcome.done.pip'), on: s.mediaAutoPip,
          onChange: (value) => this.set_('mediaAutoPip', value),
        })}
      </div>
      <div class="sec st">${L('welcome.done.shortcuts')}</div>
      <div class="keys st">
        <div class="key">${L('welcome.done.key.newTab')}${kbd('T')}</div>
        <div class="key">${L('welcome.done.key.assistant')}${kbd('E')}</div>
        <div class="key">${L('welcome.done.key.pin')}${kbd('D')}</div>
        <div class="key">${L('welcome.done.key.back')}<span class="gesture">${L('welcome.done.key.swipe')}</span></div>
      </div>`;
  }
}

customElements.define('starry-welcome', StarryWelcome);
