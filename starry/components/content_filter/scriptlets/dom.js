// 改网页的元素：去掉、设上属性和 class，换掉跳转链接，自动点按钮。
// @scriptlet remove-attr removeAttr
// @alias ra remove-attr
// @scriptlet remove-class removeClass
// @alias rc remove-class
// @scriptlet set-attr setAttr
// @scriptlet trusted-set-attr trustedSetAttr trusted
// @scriptlet href-sanitizer hrefSanitizer
// @scriptlet trusted-click-element trustedClickElement trusted
// @scriptlet prevent-innerHTML preventInnerHTML
// @scriptlet trusted-prevent-dom-bypass trustedPreventDomBypass trusted

// behavior 里的词：asap（马上）、complete（load 以后），默认 DOMContentLoaded；stay 时之后
// 文档变了再做。fix 返回 false（选择器写错了）就不再做。
function fixElements(fix, behavior, observe) {
  const words = String(behavior).split(/\s+/);
  const when = words.includes('asap') ? 'asap' : words.includes('complete') ? 'complete' : 'interactive';
  runAt(() => {
    if (fix() === false || !words.includes('stay')) {
      return;
    }
    onDomChange(fix, observe);
  }, when);
}

function querySafely(selector) {
  try {
    return $.document.querySelectorAll(selector);
  } catch (e) {
    return null;
  }
}

// 属性名用 | 分开；selector 空的是带这些属性的元素。
function removeAttr(rawAttrs = '', selector = '', behavior = '') {
  const attrs = rawAttrs.split(/\s*\|\s*/).filter(Boolean);
  if (attrs.length === 0) {
    return;
  }
  if (selector === '') {
    selector = attrs.map(attr => '[' + CSS.escape(attr) + ']').join(',');
  }
  fixElements(() => {
    const nodes = querySafely(selector);
    if (nodes === null) {
      return false;
    }
    for (const node of nodes) {
      for (const attr of attrs) {
        node.removeAttribute(attr);
      }
    }
    return true;
  }, behavior, {attributes: true, attributeFilter: attrs, childList: true, subtree: true});
}

function removeClass(rawClasses = '', selector = '', behavior = '') {
  const classes = rawClasses.split(/\s*\|\s*/).filter(Boolean);
  if (classes.length === 0) {
    return;
  }
  if (selector === '') {
    selector = classes.map(name => '.' + CSS.escape(name)).join(',');
  }
  fixElements(() => {
    const nodes = querySafely(selector);
    if (nodes === null) {
      return false;
    }
    for (const node of nodes) {
      node.classList.remove(...classes);
    }
    return true;
  }, behavior, {attributes: true, attributeFilter: ['class'], childList: true, subtree: true});
}

// 信不过的列表能设的值：空的、true、false、不大的数，或者 `[别的属性]`（照抄它的值）。
// 不许设 on 开头的属性（那是脚本）。
function setAttribute(trusted, selector, attr, value) {
  if (selector === '' || attr === '' || /^on/i.test(attr)) {
    return;
  }
  let copyFrom = '';
  if (/^\[.+\]$/.test(value)) {
    copyFrom = value.slice(1, -1);
  } else if (!trusted && !['', 'true', 'false'].includes(value.toLowerCase()) &&
             !(/^\d+$/.test(value) && Number(value) <= 0x7fff)) {
    return;
  }
  fixElements(() => {
    const nodes = querySafely(selector);
    if (nodes === null) {
      return false;
    }
    for (const node of nodes) {
      const next = copyFrom !== '' ? node.getAttribute(copyFrom) || '' : value;
      if (node.getAttribute(attr) !== next) {
        node.setAttribute(attr, next);
      }
    }
    return true;
  }, 'complete stay', {childList: true, subtree: true});
}

function setAttr(selector = '', attr = '', value = '') {
  setAttribute(false, selector, attr, value);
}

function trustedSetAttr(selector = '', attr = '', value = '') {
  setAttribute(true, selector, attr, value);
}

// 跳转链接（经过广告、统计网站转一下的）换成真正的网址。source 是网址从哪里来：text
// （默认，链接的文字）、`[属性]`、`?参数`（href 里的参数，`?a?b` 是一层层往里取）。
// 只换成 http、https 的网址。
function hrefSanitizer(selector = '', source = '') {
  if (selector === '') {
    return;
  }
  if (source === '') {
    source = 'text';
  }
  const fromParam = (href, spec) => {
    const next = spec.indexOf('?', 1);
    const name = spec.slice(1, next === -1 ? spec.length : next);
    let value = null;
    try {
      value = new URL(href, $.window.location.href).searchParams.get(name);
    } catch (e) {
    }
    if (value === null) {
      return '';
    }
    return next === -1 ? value : fromParam(value, spec.slice(next));
  };
  const extract = link => {
    if (/^\[.+\]$/.test(source)) {
      return link.getAttribute(source.slice(1, -1).trim()) || '';
    }
    if (source.startsWith('?')) {
      return fromParam(link.href, source);
    }
    if (source === 'text') {
      return link.textContent.trim();
    }
    return '';
  };
  const validate = text => {
    if (text === '' || /[\x00-\x20\x7f]/.test(text)) {
      return '';
    }
    try {
      const url = new URL(text, $.window.location.href);
      return url.protocol === 'http:' || url.protocol === 'https:' ? url.href : '';
    } catch (e) {
      return '';
    }
  };
  fixElements(() => {
    const nodes = querySafely(selector);
    if (nodes === null) {
      return false;
    }
    for (const link of nodes) {
      if (link.localName !== 'a' || !link.hasAttribute('href')) {
        continue;
      }
      const href = validate(extract(link));
      if (href !== '' && href !== link.getAttribute('href')) {
        link.setAttribute('href', href);
      }
    }
    return true;
  }, 'interactive stay', {childList: true, subtree: true});
}

// 依次点 selectors 里的元素（逗号分开；还没出现时等它，最多等 10 秒），两次之间等 delay
// 毫秒。extraMatch 写了 `cookie:名字`、`localStorage:名字`（`!` 开头是没有）时，对上了
// 才点。
function trustedClickElement(selectors = '', extraMatch = '', delay = '') {
  const queue = selectors.split(/\s*,\s*/).filter(Boolean);
  if (queue.length === 0) {
    return;
  }
  for (const token of extraMatch.split(/\s*,\s*/).filter(Boolean)) {
    const negate = token.startsWith('!');
    const spec = negate ? token.slice(1) : token;
    const colon = spec.indexOf(':');
    if (colon === -1) {
      continue;
    }
    const kind = spec.slice(0, colon);
    const re = toRegex(spec.slice(colon + 1), undefined, true);
    let names = [];
    try {
      if (kind === 'cookie') {
        names = readCookies().split(';').map(part => part.split('=')[0].trim());
      } else if (kind === 'localStorage') {
        names = $.keys($.window.localStorage);
      }
    } catch (e) {
    }
    if (names.some(name => reTest(re, name)) === negate) {
      return;
    }
  }
  const pause = Math.min(Math.max(parseInt(delay, 10) || 0, 0), 10000);
  const deadline = Date.now() + 10000;
  let stop = () => {};
  let waiting = false;
  const step = () => {
    if (waiting) {
      return;
    }
    if (queue.length === 0 || Date.now() > deadline) {
      stop();
      return;
    }
    let element = null;
    try {
      element = $.document.querySelector(queue[0]);
    } catch (e) {
      queue.length = 0;
      stop();
      return;
    }
    if (element === null) {
      return;
    }
    queue.shift();
    waiting = true;
    later(() => {
      waiting = false;
      try {
        element.click();
      } catch (e) {
      }
      step();
    }, pause);
  };
  runAt(() => {
    stop = onDomChange(step, {childList: true, subtree: true, attributes: true});
    later(() => stop(), 10000);
    step();
  }, 'interactive');
}

// 选择器对上的元素（空的是所有元素），设的 innerHTML 对上 pattern 就不设。
function preventInnerHTML(selector = '', pattern = '') {
  const proto = $.window.Element.prototype;
  const desc = $.getOwnPropertyDescriptor(proto, 'innerHTML');
  if (desc === undefined || typeof desc.set !== 'function') {
    return;
  }
  const matches = matcher(pattern);
  $.defineProperty(proto, 'innerHTML', {
    configurable: true,
    enumerable: desc.enumerable,
    get: desc.get,
    set(value) {
      let prevent = false;
      try {
        prevent = (selector === '' || this.matches(selector)) && matches(String(value));
      } catch (e) {
      }
      if (!prevent) {
        $.apply(desc.set, this, [value]);
      }
    },
  });
}

// 网页插一个 about:blank 的 iframe，从它的窗口里拿没改过的 fetch、JSON.parse，绕过这一层的脚本
// 片段。调 method 插进去的元素是这样的 iframe 时，把它窗口里的 targets（空格分开，可以是
// `JSON.parse` 这样的链）换成这一层的；没写时 contentWindow 直接给这一层的窗口。
function trustedPreventDomBypass(method = '', targets = '') {
  if (method === '') {
    return;
  }
  const HTMLElementType = $.window.HTMLElement;
  const chains = String(targets).split(/\s+/).filter(Boolean);
  wrapMethod(method, call => {
    const elements = call.args.filter(arg => arg instanceof HTMLElementType);
    const result = call.reflect();
    for (const element of new Set(elements)) {
      try {
        const frame = element.contentWindow;
        if (String(frame) !== '[object Window]') {
          continue;
        }
        const href = frame.location.href;
        if (href !== 'about:blank' && href !== $.window.location.href) {
          continue;
        }
        if (chains.length === 0) {
          $.defineProperty(element, 'contentWindow', {value: $.window});
          continue;
        }
        for (const chain of chains) {
          let mine = $.window;
          let theirs = frame;
          let prop = chain;
          for (let dot = prop.indexOf('.'); dot !== -1; dot = prop.indexOf('.')) {
            mine = mine[prop.slice(0, dot)];
            theirs = theirs[prop.slice(0, dot)];
            prop = prop.slice(dot + 1);
          }
          theirs[prop] = mine[prop];
        }
      } catch (e) {
      }
    }
    return result;
  });
}
