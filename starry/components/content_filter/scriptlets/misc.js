// 零散的几个：eval、WebRTC、页面自动刷新，几种常见反屏蔽脚本的假对象。
// @scriptlet noeval noEval
// @alias silent-noeval noeval
// @alias noeval-silent noeval
// @scriptlet noeval-if noEvalIf
// @alias prevent-eval-if noeval-if
// @scriptlet nowebrtc noWebrtc
// @scriptlet refresh-defuser refreshDefuser
// @scriptlet fuckadblock-defuser fuckAdBlockDefuser
// @alias nofab fuckadblock-defuser
// @alias fuckadblock.js-3.2.0 fuckadblock-defuser
// @alias prevent-fab-3.2.0 fuckadblock-defuser
// @scriptlet bab-defuser babDefuser
// @alias nobab bab-defuser
// @alias prevent-bab bab-defuser
// @scriptlet popads-dummy popadsDummy
// @alias set-popads-dummy popads-dummy
// @scriptlet popads.net popadsNet
// @alias prevent-popads-net popads.net

// eval 什么也不做。
function noEval() {
  $.window.eval = new $.Proxy($.window.eval, {
    apply() {
      return undefined;
    },
  });
}

// 要 eval 的文字对上 needle（空的是全部）时不做。
function noEvalIf(needle = '') {
  const matches = matcher(needle);
  $.window.eval = new $.Proxy($.window.eval, {
    apply(target, thisArg, args) {
      if (matches(String(args[0]))) {
        return undefined;
      }
      return $.apply(target, thisArg, args);
    },
  });
}

// WebRTC 的连接换成什么也不做的假对象（有的广告用它绕过拦截）。
function noWebrtc() {
  const noop = function() {};
  const FakePeerConnection = function() {};
  FakePeerConnection.prototype = {
    close: noop,
    createDataChannel() {
      return {close: noop, send: noop};
    },
    createOffer() {
      return $.Promise.resolve();
    },
    setLocalDescription() {
      return $.Promise.resolve();
    },
    setRemoteDescription() {
      return $.Promise.resolve();
    },
    addIceCandidate() {
      return $.Promise.resolve();
    },
    addEventListener: noop,
    removeEventListener: noop,
    toString() {
      return '[object RTCPeerConnection]';
    },
  };
  for (const name of ['RTCPeerConnection', 'webkitRTCPeerConnection']) {
    if (typeof $.window[name] === 'function') {
      $.window[name] = FakePeerConnection;
    }
  }
}

// `<meta http-equiv="refresh">` 到时间前停下页面的加载，不跳转。delay（秒）空的照
// meta 写的。
function refreshDefuser(delay = '') {
  runAt(() => {
    const meta = $.document.querySelector('meta[http-equiv="refresh" i][content]');
    if (meta === null) {
      return;
    }
    const seconds = parseFloat(delay === '' ? meta.getAttribute('content') : delay);
    later(() => $.window.stop(), Math.max(seconds || 0, 0) * 1000);
  }, 'interactive');
}

// FuckAdBlock、BlockAdBlock 这类检测脚本的假对象：总是报「没有拦截广告」。
function fuckAdBlockDefuser() {
  const Detector = function() {};
  Detector.prototype = {
    check() {
      return true;
    },
    clearEvent() {},
    emitEvent() {
      return this;
    },
    on(detected, fn) {
      if (!detected && typeof fn === 'function') {
        later(fn, 1);
      }
      return this;
    },
    onDetected() {
      return this;
    },
    onNotDetected(fn) {
      if (typeof fn === 'function') {
        later(fn, 1);
      }
      return this;
    },
    setOption() {
      return this;
    },
  };
  const instance = new Detector();
  const define = (name, value) => {
    try {
      $.defineProperty($.window, name, {value, configurable: false, writable: false});
    } catch (e) {
    }
  };
  for (const name of ['FuckAdBlock', 'BlockAdBlock', 'SniffAdBlock']) {
    define(name, Detector);
  }
  for (const name of ['fuckAdBlock', 'blockAdBlock', 'sniffAdBlock']) {
    define(name, instance);
  }
}

// BlockAdBlock 的混淆版本：认出它 eval 的那段代码、setTimeout 的回调，不让它跑，
// 顺手去掉它盖在页面上的提示。
function babDefuser() {
  const looksLikeBab = text =>
      text.includes('babasbm') || text.includes('blockadblock') || /getItem\('babn'\)/.test(text);
  $.window.eval = new $.Proxy($.window.eval, {
    apply(target, thisArg, args) {
      const code = args[0];
      if (typeof code !== 'string' || !looksLikeBab(code)) {
        return $.apply(target, thisArg, args);
      }
      const notice = $.document.getElementById('babasbmsgx');
      if (notice !== null) {
        notice.remove();
      }
      if ($.document.body !== null) {
        $.document.body.style.removeProperty('visibility');
      }
      return undefined;
    },
  });
  $.window.setTimeout = new $.Proxy($.window.setTimeout, {
    apply(target, thisArg, args) {
      if (typeof args[0] === 'string' && /\.bab_elementid.$/.test(args[0])) {
        return 0;
      }
      return $.apply(target, thisArg, args);
    },
  });
}

// PopAds 的弹窗脚本：给它要找的全局对象一个空的，它就不弹。
function popadsDummy() {
  for (const name of ['PopAds', 'popns']) {
    try {
      delete $.window[name];
      $.defineProperty($.window, name, {value: {}, configurable: false, writable: false});
    } catch (e) {
    }
  }
}

// 不让 PopAds 的脚本建它的全局对象（建的时候抛错）。
function popadsNet() {
  const token = abortToken();
  for (const name of ['PopAds', 'popns']) {
    try {
      delete $.window[name];
      $.defineProperty($.window, name, {
        configurable: false,
        set() {
          throw new $.ReferenceError(token);
        },
      });
    } catch (e) {
    }
  }
}
