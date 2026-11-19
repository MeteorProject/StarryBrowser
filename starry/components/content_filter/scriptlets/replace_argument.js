// 换掉函数调用的一个参数（或者 this）：网页调 chain 时，位置 argpos 的参数换成 argval。
// @scriptlet trusted-replace-argument trustedReplaceArgument trusted

// argpos 是从 0 开始的位置（负数从后往前数），或者 this。argval 的写法同
// trusted-set-constant（`json:` 开头的按 JSON），`repl:/正则/替换/标志` 是在原来的文字上替换。
// 后面可以写 `condition, 文字`：原来的参数对上它才换。
function trustedReplaceArgument(chain = '', argpos = '', argval = '', ...rest) {
  if (chain === '' || argpos === '') {
    return;
  }
  const target = resolveChain(chain);
  if (target === undefined) {
    return;
  }
  const {owner, prop} = target;
  const original = owner[prop];
  if (typeof original !== 'function') {
    return;
  }
  let replacer;
  let value;
  const repl = /^repl:\/(.+)\/(.*)\/([dgimsuvy]*)$/s.exec(argval);
  if (repl !== null) {
    try {
      replacer = {re: new $.RegExp(repl[1], repl[3]), text: repl[2]};
    } catch (e) {
      return;
    }
  } else {
    const parsed = constantValue(argval, true);
    if (parsed === undefined) {
      return;
    }
    value = parsed[0];
  }
  const options = namedArgs(rest);
  const reCondition = options.condition !== undefined ? toRegex(options.condition) : null;
  const offset = parseInt(argpos, 10);
  const change = (thisArg, args) => {
    let index = -1;
    let before;
    if (argpos === 'this') {
      before = thisArg;
    } else {
      if (Number.isNaN(offset)) {
        return thisArg;
      }
      index = offset >= 0 ? offset : args.length + offset;
      if (index < 0 || index >= args.length) {
        return thisArg;
      }
      before = args[index];
    }
    if (reCondition !== null && !reTest(reCondition, String(before))) {
      return thisArg;
    }
    let after = value;
    if (replacer !== undefined) {
      if (typeof before !== 'string') {
        return thisArg;
      }
      replacer.re.lastIndex = 0;
      after = $.apply($.stringReplace, before, [replacer.re, replacer.text]);
    }
    if (index === -1) {
      return after;
    }
    args[index] = after;
    return thisArg;
  };
  owner[prop] = new $.Proxy(original, {
    apply(fn, thisArg, args) {
      let self = thisArg;
      try {
        self = change(thisArg, args);
      } catch (e) {
      }
      return $.apply(fn, self, args);
    },
    construct(fn, args, newTarget) {
      try {
        change(undefined, args);
      } catch (e) {
      }
      return $.construct(fn, args, newTarget);
    },
  });
}
