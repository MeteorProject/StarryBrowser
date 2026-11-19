// 读写某个属性时抛错，让用到它的广告、反屏蔽脚本停下来。
// @scriptlet abort-on-property-read abortOnPropertyRead
// @alias aopr abort-on-property-read
// @scriptlet abort-on-property-write abortOnPropertyWrite
// @alias aopw abort-on-property-write
// @scriptlet abort-current-script abortCurrentScript
// @alias acs abort-current-script
// @alias acis abort-current-script
// @alias abort-current-inline-script abort-current-script
// @scriptlet abort-on-stack-trace abortOnStackTrace
// @alias aost abort-on-stack-trace

// 读到 chain 就抛错（中间的对象还没有时等它出现）。
function abortOnPropertyRead(chain = '') {
  if (chain === '') {
    return;
  }
  const token = abortToken();
  trapChain($.window, chain, (owner, prop) => {
    try {
      $.defineProperty(owner, prop, {
        configurable: false,
        get() {
          throw new $.ReferenceError(token);
        },
        set() {},
      });
    } catch (e) {
    }
  });
}

// 给 chain 赋值就抛错（读到的是 undefined）。
function abortOnPropertyWrite(chain = '') {
  if (chain === '') {
    return;
  }
  const target = resolveChain(chain);
  if (target === undefined) {
    return;
  }
  const token = abortToken();
  try {
    delete target.owner[target.prop];
  } catch (e) {
  }
  try {
    $.defineProperty(target.owner, target.prop, {
      configurable: false,
      set() {
        throw new $.ReferenceError(token);
      },
    });
  } catch (e) {
  }
}

// 脚本的内容：内联的是文字，`data:` 网址的解出来；外部脚本是空的。
function scriptTextOf(script) {
  const text = script.textContent;
  if (text.trim() !== '') {
    return text;
  }
  const match = /^data:([^,]*),(.*)$/s.exec(String(script.src).trim());
  if (match !== null) {
    try {
      return match[1].endsWith(';base64') ? atob(match[2]) : decodeURIComponent(match[2]);
    } catch (e) {
    }
  }
  return text;
}

// 正在跑的脚本（document.currentScript）内容对上 needle、网址对上 context 时，读写 target
// 就抛错。别的脚本照常用。
function abortCurrentScript(target = '', needle = '', context = '') {
  if (target === '') {
    return;
  }
  const resolved = resolveChain(target);
  if (resolved === undefined) {
    return;
  }
  const {owner, prop} = resolved;
  const reNeedle = toRegex(needle);
  const reContext = toRegex(context);
  const token = abortToken();
  const access = propertyAccess(owner, prop);
  if (!access.configurable) {
    return;
  }
  const check = () => {
    const script = $.apply($.currentScript, $.document, []);
    if (!(script instanceof $.HTMLScriptElement)) {
      return;
    }
    if (context !== '' && !reTest(reContext, String(script.src))) {
      return;
    }
    if (!reTest(reNeedle, scriptTextOf(script))) {
      return;
    }
    throw new $.ReferenceError(token);
  };
  try {
    $.defineProperty(owner, prop, {
      configurable: true,
      enumerable: access.enumerable,
      get() {
        check();
        return access.get(this);
      },
      set(value) {
        check();
        access.set(this, value);
      },
    });
  } catch (e) {
  }
}

// 调用栈（stackText）对上 needle 时，读写 chain 就抛错。
function abortOnStackTrace(chain = '', needle = '') {
  if (chain === '') {
    return;
  }
  const matches = matcher(needle);
  const token = abortToken();
  trapChain($.window, chain, (owner, prop) => {
    const access = propertyAccess(owner, prop);
    if (!access.configurable) {
      return;
    }
    try {
      $.defineProperty(owner, prop, {
        configurable: true,
        enumerable: access.enumerable,
        get() {
          if (matches(stackText())) {
            throw new $.ReferenceError(token);
          }
          return access.get(this);
        },
        set(value) {
          if (matches(stackText())) {
            throw new $.ReferenceError(token);
          }
          access.set(this, value);
        },
      });
    } catch (e) {
    }
  });
}
