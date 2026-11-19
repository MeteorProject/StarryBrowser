// AI 助手和智能体使用的页面工具脚本。
// 运行在 Starry 自己的 isolated world：网页的脚本看不到这里的变量，这里也不改网页的全局对象。
// starry/browser/agent/page_commands.cc 每次把这个文件和一句 __starryAgent.call(命令, 参数) 一起发过来（装过的文档
// 不再装），返回 JSON 文本。
// 同源的 iframe 直接在这里展开。跨源的 iframe（Chromium 里是另一个进程的 frame）摘不到内容：给它发一条
// 带口令的 postMessage，在文字里留一行占位（\u0001frame:口令）；浏览器先在子 frame 里装好听口令的脚本
// （FRAME_LISTENER），凭口令认出是哪个 frame，在里面再跑一遍这些工具，把结果填回占位的地方。
(() => {
  'use strict';
  if (globalThis.__starryAgent) return;

  // 每个文档一个随机 ID：快照和后面的操作对不上（页面跳转、刷新过）就拒绝，免得点错元素。
  const documentID = Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);
  let nextRef = 1;
  const refs = new Map();
  const elementRefs = new WeakMap();

  const SKIP = new Set(['script', 'style', 'noscript', 'template', 'head', 'meta', 'link', 'title', 'base']);
  // 名字取自内容的角色：子元素的文字已经在名字里，不再逐条列出。
  const NAME_FROM_CONTENT = new Set(['button', 'link', 'heading', 'option', 'tab', 'menuitem', 'menuitemcheckbox', 'menuitemradio',
    'treeitem', 'checkbox', 'radio', 'switch', 'tooltip', 'clickable', 'draggable', 'columnheader', 'rowheader']);
  // clickable、draggable、dropzone 不是 ARIA 角色：没有角色、但看得出能点、能拖、能接住拖进来的东西的元素。
  const INTERACTIVE = new Set(['button', 'link', 'textbox', 'searchbox', 'checkbox', 'radio', 'combobox', 'listbox', 'option', 'slider',
    'spinbutton', 'switch', 'tab', 'menuitem', 'menuitemcheckbox', 'menuitemradio', 'treeitem', 'clickable', 'draggable', 'gridcell']);
  // 列表项、区域这些也给 ref：拖放的起点和落点常常是它们。
  const REF_ROLES = new Set([...INTERACTIVE, 'img', 'heading', 'iframe', 'dialog', 'alertdialog', 'video', 'audio',
    'listitem', 'row', 'region', 'article', 'figure', 'group', 'dropzone']);
  // 不往下列出子元素的角色（值已经说明了内容）。
  const LEAF = new Set(['textbox', 'searchbox', 'img', 'progressbar', 'meter', 'slider', 'separator', 'spinbutton']);
  const INPUT_ROLES = {
    button: 'button', submit: 'button', reset: 'button', image: 'button', file: 'button', color: 'button',
    checkbox: 'checkbox', radio: 'radio', range: 'slider', number: 'spinbutton', search: 'searchbox',
  };
  const NOT_TEXT_INPUTS = new Set(['checkbox', 'radio', 'button', 'submit', 'reset', 'image', 'file', 'range', 'color', 'hidden']);

  const norm = text => String(text ?? '').replace(/\s+/g, ' ').trim();
  const clip = (text, max) => text.length > max ? text.slice(0, max - 1) + '…' : text;
  const isElement = node => !!node && node.nodeType === 1;
  const styleOf = el => el.ownerDocument.defaultView.getComputedStyle(el);
  const quote = text => '"' + text.replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';

  function randomToken() {
    const bytes = new Uint32Array(3);
    crypto.getRandomValues(bytes);
    return 'f' + Array.from(bytes, n => n.toString(36)).join('');
  }

  // 跨源 iframe：下发口令并记入 list，子 frame 监听脚本将口令回报浏览器侧；withRef 为 true 时同步记录 iframe 的 ref 以供坐标换算。
  function crossFrame(el, list, withRef) {
    const win = el.contentWindow;
    if (!win || !list || list.length >= 20) return null;
    const token = randomToken();
    try { win.postMessage({ starryAgentFrame: token }, '*'); } catch { return null; }
    list.push(withRef ? { token, ref: refFor(el) } : { token });
    return token;
  }

  function refFor(el) {
    let ref = elementRefs.get(el);
    if (!ref) {
      ref = 'e' + nextRef++;
      elementRefs.set(el, ref);
      refs.set(ref, new WeakRef(el));
    }
    return ref;
  }

  function isFocusable(el) {
    return el.hasAttribute('tabindex') || ['a', 'button', 'input', 'select', 'textarea'].includes(el.localName) || el.isContentEditable;
  }

  function implicitRole(el) {
    const tag = el.localName;
    switch (tag) {
      case 'a': case 'area': return el.hasAttribute('href') ? 'link' : null;
      case 'button': return 'button';
      case 'input': {
        const type = (el.getAttribute('type') || 'text').toLowerCase();
        if (type === 'hidden') return null;
        if (el.hasAttribute('list') && ['text', 'search', 'email', 'tel', 'url'].includes(type)) return 'combobox';
        return INPUT_ROLES[type] || 'textbox';
      }
      case 'textarea': return 'textbox';
      case 'select': return el.multiple || el.size > 1 ? 'listbox' : 'combobox';
      case 'option': return 'option';
      case 'img': return el.getAttribute('alt') === '' ? null : 'img';
      case 'svg': return el.getAttribute('aria-label') || el.querySelector(':scope > title') ? 'img' : null;
      case 'h1': case 'h2': case 'h3': case 'h4': case 'h5': case 'h6': return 'heading';
      case 'ul': case 'ol': case 'menu': return 'list';
      case 'li': return 'listitem';
      case 'dl': return 'list';
      case 'dt': return 'term';
      case 'dd': return 'definition';
      case 'nav': return 'navigation';
      case 'main': return 'main';
      case 'header': return el.closest('article, aside, main, nav, section') ? null : 'banner';
      case 'footer': return el.closest('article, aside, main, nav, section') ? null : 'contentinfo';
      case 'aside': return 'complementary';
      case 'form': return 'form';
      case 'search': return 'search';
      case 'section': return el.hasAttribute('aria-label') || el.hasAttribute('aria-labelledby') ? 'region' : null;
      case 'article': return 'article';
      case 'dialog': return 'dialog';
      case 'table': return 'table';
      case 'tr': return 'row';
      case 'td': return 'cell';
      case 'th': return el.getAttribute('scope') === 'row' ? 'rowheader' : 'columnheader';
      case 'p': return 'paragraph';
      case 'blockquote': return 'blockquote';
      case 'pre': return 'code';
      case 'fieldset': case 'details': case 'optgroup': return 'group';
      case 'summary': return 'button';
      case 'figure': return 'figure';
      case 'progress': return 'progressbar';
      case 'meter': return 'meter';
      case 'hr': return 'separator';
      case 'iframe': case 'frame': return 'iframe';
      case 'video': return 'video';
      case 'audio': return 'audio';
    }
    if (el.isContentEditable && !(el.parentElement && el.parentElement.isContentEditable)) return 'textbox';
    return null;
  }

  function roleOf(el) {
    const explicit = (el.getAttribute('role') || '').trim().split(/\s+/)[0].toLowerCase();
    if (explicit === 'none' || explicit === 'presentation') return isFocusable(el) ? implicitRole(el) : null;
    if (explicit === 'generic') return null;
    return explicit || implicitRole(el);
  }

  // 没有 ARIA 角色的元素在快照里的叫法（同 visit 里的判断）：给定位结果和描述用，提示条才有名字。
  function pseudoRole(el) {
    if (el.getAttribute('draggable') === 'true') return 'draggable';
    if (el.hasAttribute('ondrop') || el.hasAttribute('aria-dropeffect')) return 'dropzone';
    if (el.hasAttribute('onclick') || (el.hasAttribute('tabindex') && el.tabIndex >= 0) || styleOf(el).cursor === 'pointer') return 'clickable';
    return null;
  }

  function contentText(el) {
    let text = norm(el.innerText ?? el.textContent);
    if (!text) {
      const inner = el.querySelector('img[alt]:not([alt=""]), [aria-label], svg > title');
      if (inner) text = norm(inner.getAttribute('alt') || inner.getAttribute('aria-label') || inner.textContent);
    }
    return text;
  }

  function accessibleName(el, role) {
    const doc = el.ownerDocument;
    const labelledBy = el.getAttribute('aria-labelledby');
    if (labelledBy) {
      const text = norm(labelledBy.split(/\s+/).map(id => doc.getElementById(id)).filter(Boolean).map(node => node.innerText ?? node.textContent).join(' '));
      if (text) return clip(text, 100);
    }
    const label = norm(el.getAttribute('aria-label'));
    if (label) return clip(label, 100);
    const tag = el.localName;
    if (tag === 'input' || tag === 'textarea' || tag === 'select' || tag === 'meter' || tag === 'progress') {
      const type = (el.type || '').toLowerCase();
      if (type === 'button' || type === 'submit' || type === 'reset') return clip(norm(el.value) || (type === 'submit' ? 'Submit' : type === 'reset' ? 'Reset' : ''), 100);
      if (type === 'image') return clip(norm(el.alt || el.value) || 'Submit', 100);
      const labels = el.labels ? [...el.labels].map(node => norm(node.innerText ?? node.textContent)).filter(Boolean) : [];
      if (labels.length) return clip(labels.join(' '), 100);
      return clip(norm(el.title) || norm(el.placeholder), 100);
    }
    if (tag === 'img') return clip(norm(el.alt) || norm(el.title), 100);
    if (tag === 'svg') {
      const title = el.querySelector(':scope > title');
      return clip(norm(title ? title.textContent : ''), 100);
    }
    if (tag === 'fieldset' || tag === 'table' || tag === 'figure') {
      const caption = el.querySelector(tag === 'fieldset' ? ':scope > legend' : tag === 'table' ? ':scope > caption' : ':scope > figcaption');
      if (caption) return clip(norm(caption.innerText), 100);
    }
    if (tag === 'iframe' || tag === 'frame') return clip(norm(el.title || el.name), 100);
    if (NAME_FROM_CONTENT.has(role) || role === 'dropzone') {
      const text = contentText(el);
      if (text) return clip(text, 100);
    }
    return clip(norm(el.title), 100);
  }

  function deepActiveElement() {
    let active = document.activeElement;
    for (let i = 0; i < 20 && active; i++) {
      if (active.shadowRoot && active.shadowRoot.activeElement) { active = active.shadowRoot.activeElement; continue; }
      if (active.localName === 'iframe' || active.localName === 'frame') {
        let doc = null;
        try { doc = active.contentDocument; } catch {}
        if (doc && doc.activeElement && doc.activeElement !== doc.body) { active = doc.activeElement; continue; }
      }
      break;
    }
    return active;
  }

  function statesOf(el, role, focused) {
    const out = [];
    if (role === 'heading') {
      const level = Number(el.getAttribute('aria-level')) || Number((/^h([1-6])$/.exec(el.localName) || [])[1]) || 2;
      out.push('level=' + level);
    }
    const nativeCheck = el.localName === 'input' && (el.type === 'checkbox' || el.type === 'radio');
    const checked = el.getAttribute('aria-checked') ?? (nativeCheck ? String(el.checked) : null);
    if (nativeCheck && el.indeterminate) out.push('checked=mixed');
    else if (checked === 'true') out.push('checked');
    else if (checked === 'mixed') out.push('checked=mixed');
    if (el.disabled || el.getAttribute('aria-disabled') === 'true') out.push('disabled');
    const expanded = el.getAttribute('aria-expanded');
    if (expanded === 'true') out.push('expanded');
    else if (expanded === 'false') out.push('collapsed');
    else if (el.localName === 'summary' && el.parentElement && el.parentElement.localName === 'details') out.push(el.parentElement.open ? 'expanded' : 'collapsed');
    if (el.getAttribute('aria-selected') === 'true' || (el.localName === 'option' && el.selected)) out.push('selected');
    const pressed = el.getAttribute('aria-pressed');
    if (pressed === 'true') out.push('pressed');
    else if (pressed === 'mixed') out.push('pressed=mixed');
    if (el.required || el.getAttribute('aria-required') === 'true') out.push('required');
    if (role !== 'draggable' && el.getAttribute('draggable') === 'true') out.push('draggable');
    if (el === focused) out.push('focused');
    return out;
  }

  function valueOf(el, role) {
    const tag = el.localName;
    if (tag === 'input') {
      const type = (el.type || '').toLowerCase();
      if (type === 'file') return norm([...(el.files || [])].map(file => file.name).join(', ')) || null;
      if (NOT_TEXT_INPUTS.has(type)) return null;
      if (type === 'password') return el.value ? '•'.repeat(Math.min(el.value.length, 8)) : null;
      return el.value ? clip(norm(el.value), 200) : null;
    }
    if (tag === 'textarea') return el.value ? clip(norm(el.value), 200) : null;
    if (tag === 'select') return norm([...el.selectedOptions].map(option => option.label || option.text).join(', ')) || null;
    if (role === 'textbox' && el.isContentEditable) return clip(norm(el.innerText), 200) || null;
    if (['slider', 'spinbutton', 'progressbar', 'meter', 'scrollbar'].includes(role)) {
      const text = el.getAttribute('aria-valuetext') || el.getAttribute('aria-valuenow');
      if (text) return text;
      return el.value != null && el.value !== '' ? String(el.value) : null;
    }
    return null;
  }

  function linkURL(el) {
    const href = el.getAttribute('href');
    if (!href || href.startsWith('#') || /^\s*javascript:/i.test(href)) return null;
    try {
      const url = new URL(href, el.baseURI);
      return clip(url.origin === location.origin ? url.pathname + url.search + url.hash : url.href, 120);
    } catch {
      return null;
    }
  }

  function childNodesOf(node) {
    if (isElement(node)) {
      if (node.shadowRoot) return node.shadowRoot.childNodes;
      if (node.localName === 'slot') {
        const assigned = node.assignedNodes({ flatten: true });
        if (assigned.length) return assigned;
      }
    }
    return node.childNodes;
  }

  function closeText(out) {
    const last = out[out.length - 1];
    if (last && last.text !== undefined) last.closed = true;
  }

  function addText(out, raw) {
    const last = out[out.length - 1];
    if (last && last.text !== undefined && !last.closed) { last.text += raw; return; }
    if (norm(raw)) out.push({ text: raw });
  }

  // ---- 快照 ----

  function build(parent, out, ctx, budget) {
    for (const node of childNodesOf(parent)) {
      if (budget.nodes <= 0) { budget.truncated = true; return; }
      if (node.nodeType === 3) { addText(out, node.textContent); continue; }
      if (isElement(node)) visit(node, out, ctx, budget);
    }
  }

  function visit(el, out, ctx, budget) {
    const tag = el.localName;
    if (SKIP.has(tag) || el.getAttribute('aria-hidden') === 'true') return;
    if (tag === 'br') { closeText(out); return; }
    const style = styleOf(el);
    if (style.display === 'none') return;
    if (style.display !== 'contents' && typeof el.checkVisibility === 'function' && !el.checkVisibility()) return;
    budget.nodes--;
    const visible = style.visibility === 'visible';
    const pointer = style.cursor === 'pointer';
    let role = roleOf(el);
    // 没有角色、但看得出能点的元素（onclick、tabindex、鼠标变手形的最外一层）：很多网页用 div 做按钮。
    // 能拖的（draggable）、声明了接住拖放的（ondrop、aria-dropeffect）也单独标出来。
    if (!role && visible && tag !== 'body' && tag !== 'html') {
      const focusable = el.hasAttribute('tabindex') && el.tabIndex >= 0;
      const rect = el.getBoundingClientRect();
      if (rect.width > 0 && rect.height > 0) {
        if (el.getAttribute('draggable') === 'true') role = 'draggable';
        else if (el.hasAttribute('ondrop') || el.hasAttribute('aria-dropeffect')) role = 'dropzone';
        else if (el.hasAttribute('onclick') || focusable || (pointer && !ctx.pointer)) role = 'clickable';
      }
    }
    const block = !style.display.startsWith('inline') && style.display !== 'contents';
    if (!role || !visible) {
      if (block) closeText(out);
      build(el, out, { pointer }, budget);
      if (block) closeText(out);
      return;
    }
    closeText(out);
    const item = { role, name: accessibleName(el, role), states: statesOf(el, role, budget.focused), value: valueOf(el, role), children: [] };
    if (role === 'link') item.url = linkURL(el);
    if (REF_ROLES.has(role) && !(role === 'option' && el.closest('select'))) item.ref = refFor(el);
    if (tag === 'select') {
      const options = [...el.options];
      for (const option of options.slice(0, 25)) {
        item.children.push({ role: 'option', name: clip(norm(option.label || option.text), 100), states: option.selected ? ['selected'] : [], children: [] });
      }
      if (options.length > 25) item.children.push({ text: `… ${options.length - 25} more options`, closed: true });
    } else if (tag === 'iframe' || tag === 'frame') {
      let doc = null;
      try { doc = el.contentDocument; } catch {}
      if (doc && doc.body) build(doc.body, item.children, { pointer: false }, budget);
      else if (doc) item.note = 'empty frame';
      else {
        const token = crossFrame(el, budget.frames, true);
        if (token) item.frame = token;
        else item.note = 'cross-origin frame, content not available';
      }
    } else if (NAME_FROM_CONTENT.has(role)) {
      // 文字已经在名字里，只留下里面还能单独操作的元素。
      const inner = [];
      build(el, inner, { pointer: true }, budget);
      item.children = inner.filter(child => child.ref && INTERACTIVE.has(child.role));
    } else if (!LEAF.has(role)) {
      build(el, item.children, { pointer }, budget);
    }
    out.push(item);
  }

  function serialize(items, depth, lines, budget) {
    for (const item of items) {
      if (budget.length > budget.maxLength) { budget.truncated = true; return; }
      const indent = '  '.repeat(depth);
      if (item.text !== undefined) {
        const text = clip(norm(item.text), 300);
        if (text) push(lines, budget, `${indent}- text: ${text}`);
        continue;
      }
      const kids = item.children || [];
      if (!item.ref && !item.name && !kids.length && item.value == null && !item.note) continue;
      let line = `${indent}- ${item.role}`;
      if (item.name) line += ' ' + quote(item.name);
      for (const state of item.states || []) line += ` [${state}]`;
      if (item.ref) line += ` [ref=${item.ref}]`;
      if (item.url) line += ` (${item.url})`;
      const onlyText = kids.length === 1 && kids[0].text !== undefined;
      let nested = kids;
      if (item.value != null && item.value !== '') line += ': ' + item.value;
      else if (item.note) line += ': (' + item.note + ')';
      else if (onlyText) {
        const text = clip(norm(kids[0].text), 300);
        if (text && text !== item.name) line += ': ' + text;
        nested = [];
      }
      if ((nested.length || item.frame) && !line.endsWith(':')) line += ':';
      push(lines, budget, line);
      if (item.frame) push(lines, budget, `${indent}  \u0001frame:${item.frame}`);
      if (nested.length) serialize(nested, depth + 1, lines, budget);
    }
  }

  function push(lines, budget, line) {
    budget.length += line.length + 1;
    if (budget.length > budget.maxLength) { budget.truncated = true; return; }
    lines.push(line);
  }

  // refBase：同一标签页内延续上一个文档的编号递增，避免旧文档与新文档的 ref 冲突。
  function snapshot({ maxLength = 40000, refBase = 1 }) {
    if (refBase > nextRef) nextRef = refBase;
    const firstRef = nextRef;
    const budget = { nodes: 8000, length: 0, maxLength, truncated: false, focused: deepActiveElement(), frames: [] };
    const root = [];
    build(document.body || document.documentElement, root, { pointer: false }, budget);
    const lines = [];
    serialize(root, 0, lines, budget);
    return { documentID, url: location.href, title: document.title, text: lines.join('\n'), truncated: budget.truncated, firstRef, nextRef, frames: budget.frames };
  }

  // ---- 定位和操作 ----

  function resolve(ref, expected) {
    if (expected && expected !== documentID) {
      throw new Error('The page has changed since that snapshot (it navigated or reloaded). Take a new browser_snapshot and use refs from it.');
    }
    const el = refs.get(ref)?.deref();
    if (!el) throw new Error(`Unknown ref "${ref}". Take a new browser_snapshot and use a ref from it.`);
    if (!el.isConnected) throw new Error(`Element ${ref} is no longer on the page. Take a new browser_snapshot.`);
    return el;
  }

  function describe(el) {
    if (!el) return 'nothing';
    const role = roleOf(el) || pseudoRole(el) || el.localName;
    const name = accessibleName(el, role);
    if (name) return `${role} ${quote(clip(name, 60))}`;
    const id = el.id ? '#' + el.id : '';
    const classes = typeof el.className === 'string' && el.className.trim() ? '.' + el.className.trim().split(/\s+/).slice(0, 2).join('.') : '';
    return `<${el.localName}${id}${classes}>`;
  }

  function frameOffset(el) {
    let x = 0, y = 0;
    let view = el.ownerDocument.defaultView;
    while (view && view !== window && view.frameElement) {
      const frame = view.frameElement;
      const rect = frame.getBoundingClientRect();
      const style = styleOf(frame);
      x += rect.left + parseFloat(style.borderLeftWidth) + parseFloat(style.paddingLeft);
      y += rect.top + parseFloat(style.borderTopWidth) + parseFloat(style.paddingTop);
      view = view.parent;
    }
    return { x, y };
  }

  function viewportRects(el) {
    const offset = frameOffset(el);
    const shift = rect => ({ left: rect.left + offset.x, top: rect.top + offset.y, right: rect.right + offset.x, bottom: rect.bottom + offset.y, width: rect.width, height: rect.height });
    return { bounds: shift(el.getBoundingClientRect()), parts: [...el.getClientRects()].slice(0, 6).map(shift) };
  }

  function deepHit(x, y) {
    let doc = document, dx = 0, dy = 0, hit = null;
    for (let depth = 0; depth < 10; depth++) {
      let node = doc.elementFromPoint(x - dx, y - dy);
      while (node && node.shadowRoot) {
        const inner = node.shadowRoot.elementFromPoint(x - dx, y - dy);
        if (!inner || inner === node) break;
        node = inner;
      }
      if (!node) break;
      hit = node;
      if (node.localName !== 'iframe' && node.localName !== 'frame') break;
      let inner = null;
      try { inner = node.contentDocument; } catch {}
      if (!inner) break;
      const rect = node.getBoundingClientRect();
      const style = styleOf(node);
      dx += rect.left + parseFloat(style.borderLeftWidth) + parseFloat(style.paddingLeft);
      dy += rect.top + parseFloat(style.borderTopWidth) + parseFloat(style.paddingTop);
      doc = inner;
    }
    return hit;
  }

  function composedContains(ancestor, node) {
    for (let current = node; current; current = current.parentNode || current.host) {
      if (current === ancestor) return true;
    }
    return false;
  }

  function hits(el, target, node) {
    if (!node) return true;
    if (composedContains(target, node) || composedContains(el, node)) return true;
    if (el.labels && [...el.labels].some(label => composedContains(label, node))) return true;
    return node.localName === 'label' && node.control === el;
  }

  function locate({ ref, documentID: expected, scroll = true }) {
    const el = resolve(ref, expected);
    let target = el;
    let rects = viewportRects(target);
    // 自己画样式的复选框：真的 input 常常缩成 0，点它的 label。
    if ((rects.bounds.width < 1 || rects.bounds.height < 1) && el.labels && el.labels.length) {
      target = el.labels[0];
      rects = viewportRects(target);
    }
    if (rects.bounds.width < 1 || rects.bounds.height < 1) throw new Error(`Element ${ref} (${describe(el)}) is not visible.`);
    const b = rects.bounds;
    if (scroll && (b.top < 0 || b.left < 0 || b.bottom > innerHeight || b.right > innerWidth)) {
      target.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' });
      rects = viewportRects(target);
    }
    const candidates = [rects.bounds, ...rects.parts];
    let chosen = null, covering = null;
    for (const rect of candidates) {
      const left = Math.max(rect.left, 0), top = Math.max(rect.top, 0);
      const right = Math.min(rect.right, innerWidth), bottom = Math.min(rect.bottom, innerHeight);
      if (right - left < 1 || bottom - top < 1) continue;
      const x = (left + right) / 2, y = (top + bottom) / 2;
      const node = deepHit(x, y);
      if (hits(el, target, node)) { chosen = { x, y }; break; }
      if (!covering) covering = { x, y, node };
    }
    if (!chosen && !covering) throw new Error(`Element ${ref} (${describe(el)}) is outside the visible area and could not be scrolled into view.`);
    const r = rects.bounds;
    const point = chosen || covering;
    return {
      x: point.x, y: point.y,
      rect: { x: r.left, y: r.top, width: r.width, height: r.height },
      description: describe(el),
      name: accessibleName(el, roleOf(el) || pseudoRole(el) || ''),
      role: roleOf(el) || pseudoRole(el) || el.localName,
      tag: el.localName,
      covered: chosen ? null : describe(covering.node),
      // 密码框：助手往里面输入前总要问用户，确认卡片上也不显示要输入的文字。
      password: el.localName === 'input' && (el.type || '').toLowerCase() === 'password',
    };
  }

  // 跨源 iframe 元素在这个文档视口里的位置（内容区的左上角）：它里面的坐标加上这个就是这里的坐标。
  // scroll：先把 iframe 滚进视口。point：iframe 里的一点，确认它在视口里（不在就滚过去）、没有被别的东西挡住。
  function frameBox({ ref, documentID: expected, scroll = false, point = null }) {
    const el = resolve(ref, expected);
    if (el.localName !== 'iframe' && el.localName !== 'frame') throw new Error(`${describe(el)} is not a frame.`);
    const bounds = el.getBoundingClientRect();
    if (bounds.width < 1 || bounds.height < 1) throw new Error(`The frame ${describe(el)} is not visible.`);
    if (scroll) {
      const b = viewportRects(el).bounds;
      if (b.top < 0 || b.left < 0 || b.bottom > innerHeight || b.right > innerWidth) el.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' });
    }
    const origin = () => {
      const offset = frameOffset(el);
      const rect = el.getBoundingClientRect();
      const style = styleOf(el);
      return {
        x: offset.x + rect.left + parseFloat(style.borderLeftWidth) + parseFloat(style.paddingLeft),
        y: offset.y + rect.top + parseFloat(style.borderTopWidth) + parseFloat(style.paddingTop),
      };
    };
    let box = origin();
    let covered = null;
    if (point) {
      const outside = () => box.x + point.x < 0 || box.y + point.y < 0 || box.x + point.x >= innerWidth || box.y + point.y >= innerHeight;
      if (outside()) {
        scrollBy({ left: box.x + point.x - innerWidth / 2, top: box.y + point.y - innerHeight / 2, behavior: 'instant' });
        box = origin();
      }
      if (outside()) throw new Error(`That part of the frame ${describe(el)} is outside the visible area.`);
      const node = deepHit(box.x + point.x, box.y + point.y);
      if (node && !composedContains(el, node)) covered = describe(node);
    }
    return { x: box.x, y: box.y, covered };
  }

  function editableTarget(el) {
    const active = deepActiveElement();
    if (active && composedContains(el, active)) return active;
    if (active && el.labels && [...el.labels].some(label => composedContains(label, active))) return active;
    return null;
  }

  function isEditable(node) {
    if (!node) return false;
    if (node.isContentEditable || node.localName === 'textarea') return true;
    return node.localName === 'input' && !NOT_TEXT_INPUTS.has((node.type || 'text').toLowerCase());
  }

  // 点过之后：焦点在不在这个元素（或它里面的编辑框）上，选中要替换的内容。
  function prepareType({ ref, documentID: expected, append = false }) {
    const el = resolve(ref, expected);
    let active = editableTarget(el);
    if (!isEditable(active)) {
      const focusable = isEditable(el) ? el : el.querySelector('input, textarea, [contenteditable=""], [contenteditable="true"]');
      if (focusable) focusable.focus({ preventScroll: true });
      active = editableTarget(el) || (focusable && deepActiveElement() === focusable ? focusable : null);
    }
    if (!isEditable(active)) throw new Error(`${describe(el)} is not an editable field.`);
    if (active.disabled || active.readOnly) throw new Error(`${describe(el)} is disabled or read-only.`);
    const doc = active.ownerDocument;
    if (append) {
      if (typeof active.value === 'string' && typeof active.setSelectionRange === 'function') {
        try { active.setSelectionRange(active.value.length, active.value.length); } catch {}
      } else {
        const selection = doc.getSelection();
        selection.selectAllChildren(active);
        selection.collapseToEnd();
      }
    } else if (typeof active.select === 'function') {
      active.select();
    } else {
      doc.execCommand('selectAll');
    }
    const content = typeof active.value === 'string' ? active.value : active.textContent;
    return { empty: !content, password: active.type === 'password' };
  }

  function selectOption({ ref, documentID: expected, values }) {
    const el = resolve(ref, expected);
    if (el.localName !== 'select') {
      throw new Error(`${describe(el)} is not a <select> element. Click it and choose the option from the next snapshot instead.`);
    }
    if (el.disabled) throw new Error(`${describe(el)} is disabled.`);
    const options = [...el.options];
    const label = option => norm(option.label || option.text);
    const chosen = [];
    for (const value of values) {
      const wanted = norm(value);
      const option = options.find(o => o.value === value) || options.find(o => label(o) === wanted)
        || options.find(o => label(o).toLowerCase() === wanted.toLowerCase());
      if (!option) throw new Error(`No option matches "${value}". Options: ${options.slice(0, 40).map(o => quote(label(o))).join(', ')}`);
      if (option.disabled) throw new Error(`Option "${label(option)}" is disabled.`);
      chosen.push(option);
    }
    if (!el.multiple && chosen.length > 1) throw new Error('This list allows only one option.');
    for (const option of options) option.selected = chosen.includes(option);
    el.dispatchEvent(new Event('input', { bubbles: true, composed: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
    return { selected: chosen.map(label) };
  }

  function scrollState() {
    const root = document.scrollingElement || document.documentElement;
    return { x: scrollX, y: scrollY, maxX: Math.max(0, root.scrollWidth - innerWidth), maxY: Math.max(0, root.scrollHeight - innerHeight), width: innerWidth, height: innerHeight };
  }

  // 检查主文档及同源 iframe 是否包含目标文本；未找到时返回跨源 iframe 口令列表，由浏览器侧继续检索。
  function hasText({ text }) {
    const frames = [];
    const search = doc => {
      if (doc.body && (doc.body.innerText || '').includes(text)) return true;
      for (const frame of doc.querySelectorAll('iframe, frame')) {
        let inner = null;
        try { inner = frame.contentDocument; } catch {}
        if (inner) { if (search(inner)) return true; }
        else crossFrame(frame, frames, false);
      }
      return false;
    };
    const found = search(document);
    return { found, frames: found ? [] : frames };
  }

  // ---- 正文（Markdown）----

  const BLOCK = new Set(['p', 'div', 'section', 'article', 'main', 'header', 'footer', 'aside', 'nav', 'form', 'fieldset', 'figure', 'figcaption',
    'blockquote', 'address', 'details', 'summary', 'dl', 'dt', 'dd', 'center', 'hgroup', 'search']);

  function hidden(el) {
    if (el.getAttribute('aria-hidden') === 'true') return true;
    if (typeof el.checkVisibility !== 'function') return false;
    return !el.checkVisibility() && styleOf(el).display !== 'contents';
  }

  function absoluteURL(el) {
    const href = el.getAttribute('href');
    if (!href || href.startsWith('#') || /^\s*javascript:/i.test(href)) return null;
    try { return new URL(href, el.baseURI).href; } catch { return null; }
  }

  function tableMarkdown(table) {
    const rows = [...table.rows].filter(row => !hidden(row));
    if (!rows.length) return '';
    const cell = node => norm(node.innerText).replace(/\|/g, '\\|');
    const lines = rows.slice(0, 100).map(row => '| ' + [...row.cells].map(cell).join(' | ') + ' |');
    const width = Math.max(...rows.slice(0, 100).map(row => row.cells.length), 1);
    lines.splice(1, 0, '|' + ' --- |'.repeat(width));
    if (rows.length > 100) lines.push(`… ${rows.length - 100} more rows`);
    return lines.join('\n');
  }

  function markdown(node, out, ctx) {
    for (const child of childNodesOf(node)) {
      if (child.nodeType === 3) {
        const text = ctx.pre ? child.textContent : child.textContent.replace(/\s+/g, ' ');
        if (text) out.push(text);
        continue;
      }
      if (!isElement(child)) continue;
      const tag = child.localName;
      if (SKIP.has(tag) || hidden(child)) continue;
      if (ctx.skipChrome && (tag === 'nav' || tag === 'footer' || tag === 'aside' || ['navigation', 'contentinfo', 'complementary'].includes(child.getAttribute('role')))) continue;
      switch (tag) {
        case 'h1': case 'h2': case 'h3': case 'h4': case 'h5': case 'h6': {
          const text = norm(child.innerText);
          if (text) out.push('\n\n' + '#'.repeat(Number(tag[1])) + ' ' + text + '\n\n');
          break;
        }
        case 'br': out.push('\n'); break;
        case 'hr': out.push('\n\n---\n\n'); break;
        case 'a': {
          const text = norm(child.innerText);
          const url = absoluteURL(child);
          if (text && url) out.push(`[${text}](${url})`);
          else if (text) out.push(text);
          break;
        }
        case 'img': {
          const alt = norm(child.getAttribute('alt'));
          if (alt) out.push(`![${alt}]`);
          break;
        }
        case 'pre': out.push('\n\n```\n' + (child.innerText || '').replace(/\n+$/, '') + '\n```\n\n'); break;
        case 'code': out.push(ctx.pre ? child.textContent : '`' + norm(child.textContent) + '`'); break;
        case 'ul': case 'ol': {
          let index = 1;
          out.push('\n');
          for (const item of child.children) {
            if (item.localName !== 'li' || hidden(item)) continue;
            out.push('\n' + '  '.repeat(ctx.depth) + (tag === 'ol' ? `${index++}. ` : '- '));
            markdown(item, out, { ...ctx, depth: ctx.depth + 1 });
          }
          out.push('\n\n');
          break;
        }
        case 'table': out.push('\n\n' + tableMarkdown(child) + '\n\n'); break;
        case 'iframe': case 'frame': {
          let doc = null;
          try { doc = child.contentDocument; } catch {}
          if (doc && doc.body) markdown(doc.body, out, ctx);
          else if (!doc) {
            const token = crossFrame(child, ctx.frames, false);
            if (token) out.push('\n\n\u0001frame:' + token + '\n\n');
          }
          break;
        }
        case 'button': {
          const text = norm(child.innerText);
          if (text) out.push(` [${text}] `);
          break;
        }
        case 'input': case 'textarea': case 'select': break;
        default:
          if (BLOCK.has(tag) || tag === 'li') {
            out.push('\n\n');
            markdown(child, out, ctx);
            out.push('\n\n');
          } else {
            markdown(child, out, ctx);
          }
      }
    }
  }

  function mainContent() {
    const body = document.body || document.documentElement;
    const bodyLength = (body.innerText || '').length;
    let best = null, bestLength = 0;
    for (const candidate of document.querySelectorAll('main, [role="main"], article')) {
      if (hidden(candidate)) continue;
      const length = (candidate.innerText || '').length;
      if (length > bestLength) { best = candidate; bestLength = length; }
    }
    return best && bestLength > bodyLength * 0.3 ? { root: best, skipChrome: false } : { root: body, skipChrome: true };
  }

  // 提取整篇正文；分页截断（offset、maxLength）在浏览器侧回填跨源 iframe 内容后执行。
  function readPage() {
    const { root, skipChrome } = mainContent();
    const out = [];
    const frames = [];
    markdown(root, out, { depth: 0, pre: false, skipChrome, frames });
    const text = out.join('')
      .replace(/[ \t]+\n/g, '\n').replace(/\n[ \t]+(?=[^ \t\-\d])/g, '\n').replace(/[ \t]{2,}/g, ' ')
      .replace(/\n{3,}/g, '\n\n').trim();
    return { title: document.title, url: location.href, text, frames };
  }

  // 用户选中的文字（助手面板把它带进对话）：输入框里的选区不在 getSelection 里，单独取；密码框不取。
  function selection() {
    let active = document.activeElement;
    while (active && active.shadowRoot && active.shadowRoot.activeElement) active = active.shadowRoot.activeElement;
    if (active && (active.localName === 'textarea' || (active.localName === 'input' && active.type !== 'password'))) {
      try {
        const { selectionStart: start, selectionEnd: end } = active;
        if (typeof start === 'number' && end > start) return { text: active.value.slice(start, end) };
      } catch (_) {}
    }
    return { text: String(getSelection() || '') };
  }

  // 拖放做得怎么样（browser_drag、把文件拖到上传区域）：开始前 watchDrag，松开后 dragState。
  // 网页收到的 dragstart、dragover、drop、dragend 是真的，这里只在捕获阶段看一眼，不拦。
  // 目标在 dragover 里 preventDefault 才算接受：记下最近一次 dragover，事件派发完以后它的
  // defaultPrevented 就是网页的回答（拖进文件时不接受就不放下，否则浏览器会自己打开文件）。
  let dragWatch = null;
  function watchDrag() {
    if (!dragWatch) {
      for (const type of ['dragstart', 'dragover', 'drop', 'dragend']) {
        addEventListener(type, event => {
          if (type === 'dragstart') dragWatch.started = true;
          else if (type === 'dragover') dragWatch.over = event;
          else if (type === 'drop') dragWatch.dropped = true;
          else dragWatch.effect = event.dataTransfer ? event.dataTransfer.dropEffect : null;
        }, true);
      }
    }
    dragWatch = { started: false, over: null, dropped: false, effect: null };
    return {};
  }

  function dragState() {
    const watch = dragWatch || { started: false, over: null, dropped: false, effect: null };
    return { started: watch.started, accepting: !!(watch.over && watch.over.defaultPrevented),
             dropped: watch.dropped, effect: watch.effect };
  }

  const commands = { snapshot, locate, frameBox, prepareType, selectOption, scrollState, hasText, readPage, selection, watchDrag, dragState };
  Object.defineProperty(globalThis, '__starryAgent', {
    value: {
      call(name, args) {
        const command = commands[name];
        if (!command) return JSON.stringify({ error: 'Unknown page command: ' + name });
        try {
          return JSON.stringify({ value: command(args || {}) });
        } catch (error) {
          return JSON.stringify({ error: String(error && error.message || error) });
        }
      },
    },
  });
})();
