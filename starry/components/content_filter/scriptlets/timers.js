// 定时器：不让对上的回调跑（反屏蔽检测、倒计时后弹广告），或者把等待缩短（「广告还有
// 30 秒」）。回调对的是它的源码（字符串的回调就是那段文字）。
// @scriptlet no-setTimeout-if noSetTimeoutIf
// @alias nostif no-setTimeout-if
// @alias prevent-setTimeout no-setTimeout-if
// @alias setTimeout-defuser no-setTimeout-if
// @scriptlet no-setInterval-if noSetIntervalIf
// @alias nosiif no-setInterval-if
// @alias prevent-setInterval no-setInterval-if
// @alias setInterval-defuser no-setInterval-if
// @scriptlet no-requestAnimationFrame-if noRequestAnimationFrameIf
// @alias norafif no-requestAnimationFrame-if
// @alias prevent-requestAnimationFrame no-requestAnimationFrame-if
// @scriptlet nano-setTimeout-booster nanoSetTimeoutBooster
// @alias nano-stb nano-setTimeout-booster
// @alias adjust-setTimeout nano-setTimeout-booster
// @scriptlet nano-setInterval-booster nanoSetIntervalBooster
// @alias nano-sib nano-setInterval-booster
// @alias adjust-setInterval nano-setInterval-booster

// 换掉 window[name]：每次调用先让 change 改参数。
function wrapTimer(name, change) {
  const original = $.window[name];
  if (typeof original !== 'function') {
    return;
  }
  $.window[name] = new $.Proxy(original, {
    apply(target, thisArg, args) {
      try {
        change(args);
      } catch (e) {
      }
      return $.apply(target, thisArg, args);
    },
  });
}

// 等待时间：空的是什么都算，`!数` 是除了这个数。
function delayMatcher(raw) {
  const negate = raw.startsWith('!');
  const wanted = parseInt(negate ? raw.slice(1) : raw, 10);
  if (Number.isNaN(wanted)) {
    return () => true;
  }
  return delay => ((Number(delay) || 0) === wanted) !== negate;
}

// 对上的回调换成空函数（照样返回定时器的编号，网页清得掉）。
function noTimerIf(name, needle, delay) {
  const matchesSource = matcher(needle);
  const matchesDelay = delayMatcher(delay);
  const noop = function() {};
  wrapTimer(name, args => {
    if (matchesSource(sourceOf(args[0])) && matchesDelay(args[1])) {
      args[0] = noop;
    }
  });
}

function noSetTimeoutIf(needle = '', delay = '') {
  noTimerIf('setTimeout', needle, delay);
}

function noSetIntervalIf(needle = '', delay = '') {
  noTimerIf('setInterval', needle, delay);
}

function noRequestAnimationFrameIf(needle = '') {
  const matchesSource = matcher(needle);
  const noop = function() {};
  wrapTimer('requestAnimationFrame', args => {
    if (matchesSource(sourceOf(args[0]))) {
      args[0] = noop;
    }
  });
}

// 等待时间是 delay（默认 1000，`*` 是不管多少）、回调对上 needle 的，等待乘上 boost
// （默认 0.05，限在 0.001 到 50 之间）。
function boostTimer(name, needle, delay, boost) {
  const matchesSource = matcher(needle);
  let wanted = delay === '*' ? undefined : parseInt(delay, 10);
  if (wanted !== undefined && Number.isNaN(wanted)) {
    wanted = 1000;
  }
  let factor = parseFloat(boost);
  if (!Number.isFinite(factor)) {
    factor = 0.05;
  }
  factor = Math.min(Math.max(factor, 0.001), 50);
  wrapTimer(name, args => {
    const current = Number(args[1]) || 0;
    if (wanted !== undefined && current !== wanted) {
      return;
    }
    if (matchesSource(sourceOf(args[0]))) {
      args[1] = current * factor;
    }
  });
}

function nanoSetTimeoutBooster(needle = '', delay = '', boost = '') {
  boostTimer('setTimeout', needle, delay, boost);
}

function nanoSetIntervalBooster(needle = '', delay = '', boost = '') {
  boostTimer('setInterval', needle, delay, boost);
}
