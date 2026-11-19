// 事件、弹出窗口：不让对上的监听装上，拦下打开新窗口。
// @scriptlet addEventListener-defuser addEventListenerDefuser
// @alias aeld addEventListener-defuser
// @alias prevent-addEventListener addEventListener-defuser
// @scriptlet no-window-open-if noWindowOpenIf
// @alias nowoif no-window-open-if
// @alias prevent-window-open no-window-open-if
// @alias window.open-defuser no-window-open-if
// @scriptlet disable-newtab-links disableNewtabLinks
// @scriptlet alert-buster alertBuster

// 事件类型整个对上 type（或者正则）、监听函数的源码对上 pattern 的不装。后面可以写
// `elements, 选择器`：只管这些元素上的（window、document 写名字）。
function addEventListenerDefuser(type = '', pattern = '', ...rest) {
  const reType = toRegex(type, undefined, true);
  const reHandler = toRegex(pattern);
  const elements = String(namedArgs(rest).elements || '');
  const targetMatches = target => {
    if (elements === '') {
      return true;
    }
    for (const part of elements.split(/\s*,\s*/)) {
      if ((part === 'window' && target === $.window) ||
          (part === 'document' && target === $.document)) {
        return true;
      }
      try {
        if (target instanceof $.Element && target.matches(part)) {
          return true;
        }
      } catch (e) {
      }
    }
    return false;
  };
  const proto = $.window.EventTarget.prototype;
  proto.addEventListener = new $.Proxy(proto.addEventListener, {
    apply(target, thisArg, args) {
      let defuse = false;
      try {
        const handler = args[1];
        const source = isObject(handler) && typeof handler !== 'function' &&
                typeof handler.handleEvent === 'function' ?
            sourceOf(handler.handleEvent) :
            sourceOf(handler);
        defuse = reTest(reType, String(args[0])) && reTest(reHandler, source) &&
            targetMatches(thisArg);
      } catch (e) {
      }
      if (defuse) {
        return undefined;
      }
      return $.apply(target, thisArg, args);
    },
  });
}

// 给拦下的 window.open 返回的假窗口：常用的属性、方法都有，什么也不做。
function fakeWindow() {
  const noop = function() {};
  let doc = null;
  try {
    doc = $.document.implementation.createHTMLDocument('');
  } catch (e) {
  }
  const fake = {
    closed: false,
    opener: $.window,
    name: '',
    length: 0,
    document: doc,
    location: {href: 'about:blank', assign: noop, replace: noop, reload: noop},
    close() {
      fake.closed = true;
    },
    focus: noop,
    blur: noop,
    print: noop,
    stop: noop,
    postMessage: noop,
    addEventListener: noop,
    removeEventListener: noop,
    moveTo: noop,
    moveBy: noop,
    resizeTo: noop,
    resizeBy: noop,
    scrollTo: noop,
  };
  fake.window = fake;
  fake.self = fake;
  return fake;
}

// 网址（window.open 的参数连在一起）对上 pattern 的不开（`!` 开头是对不上的不开）。
// 写了 delay（秒）或者 decoy 时给一个藏起来的空 iframe 当窗口，到时间删掉（有的网页要
// 「真的」窗口才往下走）；不然给一个假窗口。
function noWindowOpenIf(pattern = '', delay = '', decoy = '') {
  const matches = matcher(pattern);
  const seconds = /^\d+$/.test(delay) ? Math.max(Number(delay), 1) : 0;
  const original = $.window.open;
  if (typeof original !== 'function') {
    return;
  }
  const decoyWindow = () => {
    const frame = $.document.createElement('iframe');
    frame.src = 'about:blank';
    frame.style.setProperty('display', 'none', 'important');
    ($.document.body || $.document.documentElement).append(frame);
    later(() => frame.remove(), (seconds || 10) * 1000);
    return frame.contentWindow || fakeWindow();
  };
  $.window.open = new $.Proxy(original, {
    apply(target, thisArg, args) {
      let blocked = false;
      try {
        blocked = matches(args.map(arg => String(arg)).join(' '));
      } catch (e) {
      }
      if (!blocked) {
        return $.apply(target, thisArg, args);
      }
      return seconds > 0 || decoy !== '' ? decoyWindow() : fakeWindow();
    },
  });
}

// 链接不在新标签页里打开（点了没有反应），对付点哪里都开新标签页的广告。
function disableNewtabLinks() {
  listen($.document, 'click', event => {
    for (let node = event.target; node; node = node.parentNode) {
      if (node.localName === 'a' && node.hasAttribute('target')) {
        event.stopPropagation();
        event.preventDefault();
        break;
      }
    }
  }, true);
}

function alertBuster() {
  $.window.alert = new $.Proxy($.window.alert, {
    apply() {
      return undefined;
    },
  });
}
