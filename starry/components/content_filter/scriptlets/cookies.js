// 设、删 cookie（多是 Cookie 横幅「已经选过了」的记号）。
// @scriptlet set-cookie setCookie
// @scriptlet set-cookie-reload setCookieReload
// @scriptlet trusted-set-cookie trustedSetCookie trusted
// @scriptlet trusted-set-cookie-reload trustedSetCookieReload trusted
// @scriptlet remove-cookie removeCookie
// @alias cookie-remover remove-cookie

// 信不过的列表能设的值：同意、拒绝、关掉这类的词，加上不大的数。
const safeCookieValues = new Set([
  '', 'true', 'false', 'yes', 'no', 'y', 'n', 't', 'f', 'on', 'off', 'ok', 'all', 'none',
  'accept', 'accepted', 'reject', 'rejected', 'allow', 'allowed', 'deny', 'denied',
  'agree', 'agreed', 'decline', 'declined', 'dismiss', 'dismissed', 'approved', 'granted',
  'refused', 'blocked', 'enable', 'enabled', 'disable', 'disabled', 'essential',
  'nonessential', 'necessary', 'required', 'functional', 'checked', 'unchecked',
  'hide', 'hidden', 'closed', 'done', 'set', 'unset', 'optin', 'optout', 'opt-in',
  'opt-out', 'consent', 'consented', 'customized', 'forever', 'never', 'always',
  'visited', 'seen', 'shown',
]);

function isSafeCookieValue(value) {
  if (safeCookieValues.has(value.toLowerCase())) {
    return true;
  }
  return /^\d+$/.test(value) && Number(value) <= 0x7fff;
}

function cookieValue(name) {
  for (const part of readCookies().split(';')) {
    const eq = part.indexOf('=');
    const key = (eq === -1 ? part : part.slice(0, eq)).trim();
    if (key === name) {
      return eq === -1 ? '' : part.slice(eq + 1).trim();
    }
  }
  return undefined;
}

// 写一个 cookie。路径默认 `/`，`none` 是不写路径；信不过的列表只能用这两种。options.domain
// 要是这个网站自己或者它的上级；options.reload 时，原来没有、设上了就重新载入一次页面。
function writeCookie(trusted, name, value, expires, path, options) {
  if (name === '') {
    return;
  }
  if (!trusted && /[^!#$&+\-.0-9A-Z[\]^_`a-z|~]/.test(name)) {
    name = encodeURIComponent(name);
  }
  if (/[^ -:<-[\]-~]/.test(value)) {
    value = encodeURIComponent(value);
  }
  const before = cookieValue(name);
  if (before === value) {
    return;
  }
  if (options.dontOverwrite && before !== undefined) {
    return;
  }
  let text = name + '=' + value;
  if (expires !== '') {
    text += '; expires=' + expires;
  }
  if (path === '') {
    path = '/';
  } else if (path === 'none') {
    path = '';
  }
  if (path !== '' && path !== '/' && !(trusted && path.startsWith('/'))) {
    return;
  }
  if (path !== '') {
    text += '; path=' + path;
  }
  if (options.domain) {
    const host = $.window.location.hostname;
    const domain = String(options.domain).replace(/^\./, '');
    if (host !== domain && !host.endsWith('.' + domain)) {
      return;
    }
    text += '; domain=' + domain;
  }
  try {
    writeCookieText(text);
  } catch (e) {
    return;
  }
  if (options.reload && cookieValue(name) === value) {
    $.window.location.reload();
  }
}

function setCookie(name = '', value = '', path = '', ...rest) {
  if (!isSafeCookieValue(value)) {
    return;
  }
  writeCookie(false, name, value, '', path, namedArgs(rest));
}

function setCookieReload(name = '', value = '', path = '', ...rest) {
  setCookie(name, value, path, 'reload', '1', ...rest);
}

// 任意的值，能设过期时间（秒数、1day、1year）和别的路径。
function trustedSetCookie(name = '', value = '', offsetExpires = '', path = '', ...rest) {
  let expires = '';
  if (offsetExpires !== '') {
    let seconds = NaN;
    if (offsetExpires === '1day') {
      seconds = 86400;
    } else if (offsetExpires === '1year') {
      seconds = 31536000;
    } else if (/^\d+$/.test(offsetExpires)) {
      seconds = Number(offsetExpires);
    }
    if (Number.isFinite(seconds)) {
      expires = new Date(Date.now() + seconds * 1000).toUTCString();
    }
  }
  writeCookie(true, name, expandTimeTokens(value), expires, path, namedArgs(rest));
}

function trustedSetCookieReload(name = '', value = '', offsetExpires = '', path = '', ...rest) {
  trustedSetCookie(name, value, offsetExpires, path, 'reload', '1', ...rest);
}

// 删掉名字对上 needle 的 cookie（空的是全部）：现在删，离开页面、cookie 变了时再删。
// 不知道 cookie 设在哪一级网站、哪个路径上，每种都试。
function removeCookie(needle = '') {
  const matches = matcher(needle);
  const remove = () => {
    const host = $.window.location.hostname;
    for (const part of readCookies().split(';')) {
      const eq = part.indexOf('=');
      const name = (eq === -1 ? part : part.slice(0, eq)).trim();
      if (name === '' || !matches(name)) {
        continue;
      }
      const base = name + '=; expires=Thu, 01 Jan 1970 00:00:00 GMT';
      const domains = [''];
      for (let domain = host; domain.includes('.'); domain = domain.slice(domain.indexOf('.') + 1)) {
        domains.push('; domain=' + domain, '; domain=.' + domain);
      }
      for (const domain of domains) {
        for (const path of ['', '; path=/']) {
          try {
            writeCookieText(base + domain + path);
          } catch (e) {
          }
        }
      }
    }
  };
  remove();
  listen($.window, 'beforeunload', remove);
  const store = $.window.cookieStore;
  if (isObject(store) && typeof store.addEventListener === 'function') {
    let pending = false;
    listen(store, 'change', () => {
      if (pending) {
        return;
      }
      pending = true;
      later(() => {
        pending = false;
        remove();
      }, 250);
    });
  }
}
