// 脚本片段共用的部分，每段注入的脚本都带一份。
//
// 脚本片段是自己写的（不用 uBlock Origin 的 GPLv3 代码），名字、参数、行为和 uBlock Origin
// 的一样，过滤列表直接能用。浏览器在文档的根元素
// 刚建好、网页自己的脚本还没跑的时候，把这个页面用到的几个文件和每条规则的调用拼成一段
// 脚本，在网页的主 world 里跑一次（starry/components/content_filter/scriptlets.cc）。
// 整段包在一个函数里，网页看不到这里的名字；这里先取下的原生函数，之后网页再改也影响
// 不到。
//
// 文件开头 `// @` 的行是给 generate_scriptlets.py 看的：
//   @requires 模块        用到另一个文件里的函数（core 不用写，都带）
//   @scriptlet 名字 函数 [trusted]
//   @alias 别名 名字
// 生成时去掉只有注释的行和缩进，所以注释只写在单独的行上，代码里只能是 ASCII（别的
// 字符写成 \u 转义）。

const $ = {
  window: globalThis,
  document: globalThis.document,
  apply: Reflect.apply,
  construct: Reflect.construct,
  reflectGet: Reflect.get,
  defineProperty: Object.defineProperty,
  getOwnPropertyDescriptor: Object.getOwnPropertyDescriptor,
  hasOwnProperty: Object.prototype.hasOwnProperty,
  keys: Object.keys,
  Proxy: Proxy,
  Promise: Promise,
  WeakMap: WeakMap,
  RegExp: RegExp,
  regexTest: RegExp.prototype.test,
  stringReplace: String.prototype.replace,
  jsonParse: JSON.parse,
  jsonStringify: JSON.stringify,
  functionToString: Function.prototype.toString,
  Error: Error,
  ReferenceError: ReferenceError,
  setTimeout: globalThis.setTimeout,
  requestAnimationFrame: globalThis.requestAnimationFrame,
  addEventListener: EventTarget.prototype.addEventListener,
  removeEventListener: EventTarget.prototype.removeEventListener,
  dispatchEvent: EventTarget.prototype.dispatchEvent,
  MutationObserver: globalThis.MutationObserver,
  HTMLScriptElement: globalThis.HTMLScriptElement,
  Element: globalThis.Element,
  Response: globalThis.Response,
  Request: globalThis.Request,
  currentScript: Object.getOwnPropertyDescriptor(Document.prototype, 'currentScript').get,
  readyState: Object.getOwnPropertyDescriptor(Document.prototype, 'readyState').get,
  cookie: Object.getOwnPropertyDescriptor(Document.prototype, 'cookie'),
};

// 一条规则的调用：出错只影响它自己。
function $run(fn, args) {
  try {
    fn(...args);
  } catch (e) {
  }
}

const isObject = value =>
  value !== null && (typeof value === 'object' || typeof value === 'function');

const hasOwn = (object, key) => $.apply($.hasOwnProperty, object, [key]);

const reTest = (re, text) => {
  re.lastIndex = 0;
  return $.apply($.regexTest, re, [text]);
};

const later = (fn, ms) => $.apply($.setTimeout, $.window, [fn, ms]);

const listen = (target, type, fn, options) =>
  $.apply($.addEventListener, target, [type, fn, options]);

const unlisten = (target, type, fn, options) =>
  $.apply($.removeEventListener, target, [type, fn, options]);

const dispatch = (target, event) => $.apply($.dispatchEvent, target, [event]);

const readCookies = () => String($.apply($.cookie.get, $.document, []));

const writeCookieText = text => $.apply($.cookie.set, $.document, [text]);

// 函数的源码（网页常拿它认出广告脚本）；不是函数时转成文字。
function sourceOf(value) {
  if (typeof value === 'function') {
    try {
      return $.apply($.functionToString, value, []);
    } catch (e) {
    }
  }
  try {
    return String(value);
  } catch (e) {
    return '';
  }
}

// 列表里写的匹配：`/正则/标志` 是正则（只用它自己的标志），别的是字面的文字，用 flags；
// exact 时字面的文字要整个相同。空的匹配一切。
function toRegex(pattern, flags, exact) {
  pattern = String(pattern === undefined ? '' : pattern);
  if (pattern === '') {
    return /^/;
  }
  const match = /^\/(.+)\/([dgimsuvy]*)$/s.exec(pattern);
  if (match !== null) {
    try {
      return new $.RegExp(match[1], match[2]);
    } catch (e) {
      return /(?!)/;
    }
  }
  const escaped = pattern.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return new $.RegExp(exact ? '^' + escaped + '$' : escaped, flags);
}

// 同 toRegex，`!` 开头的反过来（不匹配的才算）。返回判断文字的函数。
function matcher(pattern, flags) {
  pattern = String(pattern === undefined ? '' : pattern);
  let negate = false;
  if (pattern.startsWith('!')) {
    negate = true;
    pattern = pattern.slice(1);
  }
  const re = toRegex(pattern, flags);
  return text => reTest(re, String(text)) !== negate;
}

// 位置参数后面成对的 `名字, 值`（`reload, 1`）。空着的跳过：列表里常有多写一个逗号的。
function namedArgs(args) {
  const list = args.filter(arg => arg !== '');
  const out = {};
  for (let i = 0; i + 1 < list.length; i += 2) {
    out[list[i]] = list[i + 1];
  }
  return out;
}

// 拦下读写时抛的错带一个随机记号：网页的 onerror 收不到，控制台也不报。在装拦截的时候
// 先调一次（监听要早于网页自己的）。
let abortTokenValue;
function abortToken() {
  if (abortTokenValue === undefined) {
    abortTokenValue = String.fromCharCode(97 + Math.floor(Math.random() * 26)) +
        Math.floor(Math.random() * 1e12).toString(36);
    listen($.window, 'error', event => {
      const message = event.message;
      if (typeof message === 'string' && message.includes(abortTokenValue)) {
        event.preventDefault();
        event.stopImmediatePropagation();
      }
    }, true);
  }
  return abortTokenValue;
}

// `a.b.c` 拆成最后一级的主人（a.b）和名字（c）。中间哪一级不是对象时返回 undefined。
function resolveChain(chain) {
  let owner = $.window;
  let prop = chain;
  for (;;) {
    const dot = prop.indexOf('.');
    if (dot === -1) {
      break;
    }
    let next;
    try {
      next = owner[prop.slice(0, dot)];
    } catch (e) {
      return undefined;
    }
    if (!isObject(next)) {
      return undefined;
    }
    owner = next;
    prop = prop.slice(dot + 1);
  }
  return {owner, prop};
}

// 在 `a.b.c` 的最后一级上调 install(主人, 名字)。中间某一级现在还没有时，先在那里装一个
// 访问器，等网页给它赋了对象再往下装（广告脚本常常之后才建这些对象）。同一级上已经有
// 别的规则装的访问器时接在它后面（赋值也传给它），几条 `x.a`、`x.b` 才都生效。
function trapChain(owner, chain, install) {
  const dot = chain.indexOf('.');
  if (dot === -1) {
    install(owner, chain);
    return;
  }
  const prop = chain.slice(0, dot);
  const rest = chain.slice(dot + 1);
  let value;
  try {
    value = owner[prop];
  } catch (e) {
    return;
  }
  if (isObject(value)) {
    trapChain(value, rest, install);
    return;
  }
  const desc = $.getOwnPropertyDescriptor(owner, prop);
  if (desc !== undefined && desc.configurable === false) {
    return;
  }
  const previousSet = desc !== undefined && typeof desc.set === 'function' ? desc.set : undefined;
  try {
    $.defineProperty(owner, prop, {
      configurable: true,
      enumerable: true,
      get() {
        return value;
      },
      set(next) {
        if (previousSet !== undefined) {
          try {
            $.apply(previousSet, this, [next]);
          } catch (e) {
          }
        }
        value = next;
        if (isObject(next)) {
          trapChain(next, rest, install);
        }
      },
    });
  } catch (e) {
  }
}

// 把 `a.b.c` 上的函数换成一层代理：调用（和 new）交给 handler(call)，call.args 是参数
// （可以改），call.thisArg 是 this，call.reflect() 照原来的调。找不到函数时返回 false。
function wrapMethod(chain, handler) {
  const target = resolveChain(chain);
  if (target === undefined) {
    return false;
  }
  const {owner, prop} = target;
  const original = owner[prop];
  if (typeof original !== 'function') {
    return false;
  }
  owner[prop] = new $.Proxy(original, {
    apply(fn, thisArg, args) {
      return handler({thisArg, args, reflect: () => $.apply(fn, thisArg, args)});
    },
    construct(fn, args, newTarget) {
      return handler({thisArg: undefined, args, reflect: () => $.construct(fn, args, newTarget)});
    },
  });
  return true;
}

// 属性原来的读写（自己的访问器，或者值）：拦截里照常转给它。
function propertyAccess(owner, prop) {
  const desc = $.getOwnPropertyDescriptor(owner, prop);
  let value;
  try {
    value = owner[prop];
  } catch (e) {
  }
  const getter = desc !== undefined && typeof desc.get === 'function' ? desc.get : undefined;
  const setter = desc !== undefined && typeof desc.set === 'function' ? desc.set : undefined;
  return {
    configurable: desc === undefined || desc.configurable === true,
    enumerable: desc === undefined || desc.enumerable === true,
    get(self) {
      return getter !== undefined ? $.apply(getter, self, []) : value;
    },
    set(self, next) {
      if (setter !== undefined) {
        $.apply(setter, self, [next]);
      } else {
        value = next;
      }
    },
  };
}

// 现在的调用栈，一帧一段、用 tab 隔开：`函数 网址:行:1`。网页自己的内联脚本的网址写成
// inlineScript，没有网址的（注入的脚本、eval）写成 injectedScript；第一段是
// stackDepth:层数（uBlock Origin 的写法，列表照这个写）。
function stackText() {
  const page = String($.window.location.href).split('#')[0];
  const lines = [];
  for (const raw of String(new $.Error().stack || '').split(/[\n\r]+/)) {
    const line = raw.trim();
    const match = /(.*?@)?(\S+)(:\d+):\d+\)?$/.exec(line);
    if (match === null) {
      continue;
    }
    let url = match[2];
    if (url.startsWith('(')) {
      url = url.slice(1);
    }
    if (url === page) {
      url = 'inlineScript';
    } else if (url.startsWith('<anonymous>')) {
      url = 'injectedScript';
    }
    let fn = match[1] !== undefined ? match[1].slice(0, -1) : line.slice(0, match.index).trim();
    if (fn.startsWith('at')) {
      fn = fn.slice(2).trim();
    }
    lines.push(' ' + (fn + ' ' + url + match[3] + ':1').trim());
  }
  lines[0] = 'stackDepth:' + (lines.length - 1);
  return lines.join('\t');
}

// set-constant 这类的值：认得的写法换成值，包在数组里（值可能是 undefined）。认不得的
// 返回 undefined；信得过的列表里，别的写法当文字（`json:` 开头的按 JSON）。
function constantValue(raw, trusted) {
  switch (raw) {
    case 'undefined':
      return [undefined];
    case 'null':
      return [null];
    case 'false':
      return [false];
    case 'true':
      return [true];
    case '':
    case '\'\'':
    case 'emptyStr':
      return [''];
    case '[]':
    case 'emptyArr':
      return [[]];
    case '{}':
    case 'emptyObj':
      return [{}];
    case 'noopFunc':
      return [function() {}];
    case 'noopCallbackFunc':
      return [function() {
        return function() {};
      }];
    case 'noopThisFunc':
      return [function() {
        return this;
      }];
    case 'trueFunc':
      return [function() {
        return true;
      }];
    case 'falseFunc':
      return [function() {
        return false;
      }];
    case 'throwFunc':
      return [function() {
        throw '';
      }];
    case 'noopPromiseResolve':
      return [function() {
        return $.Promise.resolve();
      }];
    case 'noopPromiseReject':
      return [function() {
        return $.Promise.reject();
      }];
    case 'NaN':
      return [NaN];
    case 'Infinity':
      return [Infinity];
    case 'yes':
    case 'no':
      return [raw];
  }
  if (/^-?\d+$/.test(raw)) {
    const number = Number(raw);
    return trusted || Math.abs(number) <= 0x7fff ? [number] : undefined;
  }
  if (!trusted) {
    return undefined;
  }
  if (raw.startsWith('json:')) {
    try {
      return [$.jsonParse(raw.slice(5))];
    } catch (e) {
      return undefined;
    }
  }
  if (raw.startsWith('{') && raw.endsWith('}')) {
    try {
      const parsed = $.jsonParse(raw);
      if (isObject(parsed) && hasOwn(parsed, 'value')) {
        return [parsed.value];
      }
    } catch (e) {
    }
  }
  return [raw];
}

const readyStates = {loading: 1, interactive: 2, complete: 3};
const runAtStates = {
  asap: 1,
  loading: 1,
  interactive: 2,
  end: 2,
  'document-end': 2,
  complete: 3,
  idle: 3,
  'document-idle': 3,
};

// 等文档到了某个阶段再跑：asap 马上，interactive（默认）是 DOMContentLoaded，complete 是
// load。
function runAt(fn, when) {
  const wanted = runAtStates[when] || 2;
  const state = () => readyStates[$.apply($.readyState, $.document, [])] || 1;
  if (state() >= wanted) {
    fn();
    return;
  }
  const onChange = () => {
    if (state() < wanted) {
      return;
    }
    unlisten($.document, 'readystatechange', onChange, true);
    fn();
  };
  listen($.document, 'readystatechange', onChange, true);
}

// 文档变了以后（合到下一帧）再调 fn。返回停下的函数。
function onDomChange(fn, options) {
  let pending = false;
  const run = () => {
    pending = false;
    fn();
  };
  const observer = new $.MutationObserver(() => {
    if (pending) {
      return;
    }
    pending = true;
    if (typeof $.requestAnimationFrame === 'function') {
      $.apply($.requestAnimationFrame, $.window, [run]);
    } else {
      later(run, 16);
    }
  });
  try {
    observer.observe($.document, options || {childList: true, subtree: true});
  } catch (e) {
    return () => {};
  }
  return () => observer.disconnect();
}

// 值里的 $now$、$currentDate$、$currentISODate$ 换成现在的时间（trusted-set-cookie 这类）。
function expandTimeTokens(value) {
  const now = new Date();
  return value.replace('$now$', String(now.getTime()))
      .replace('$currentDate$', now.toUTCString())
      .replace('$currentISODate$', now.toISOString());
}

function randomText(length) {
  let text = '';
  while (text.length < length) {
    text += Math.random().toString(36).slice(2);
  }
  return text.slice(0, length);
}

// 假的响应内容（no-fetch-if、no-xhr-if）：几种固定的写法；`war:名字` 是浏览器带来的替身
// 资源（$war）；信得过的列表照写的文字。认不得返回 undefined。
function fakeBody(spec, trusted) {
  switch (spec) {
    case '':
    case 'emptyStr':
      return '';
    case 'emptyObj':
    case '{}':
      return '{}';
    case 'emptyArr':
    case '[]':
      return '[]';
    case 'true':
      return randomText(10);
  }
  if (spec.startsWith('length:')) {
    const match = /^length:(\d+)(?:-(\d+))?$/.exec(spec);
    if (match === null) {
      return undefined;
    }
    const min = Number(match[1]);
    const max = match[2] !== undefined ? Number(match[2]) : min;
    const length = min + Math.floor(Math.random() * (Math.max(max, min) - min + 1));
    return randomText(Math.min(length, 500000));
  }
  if (spec.startsWith('war:')) {
    const name = spec.slice(4);
    return hasOwn($war, name) ? $war[name] : undefined;
  }
  return trusted ? spec : undefined;
}
