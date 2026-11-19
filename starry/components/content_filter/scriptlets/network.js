// 网页用 fetch、XMLHttpRequest 发的请求：对上的不真发、给个假的响应（网页拿它判断广告
// 有没有被拦），或者改掉响应的内容（视频网站把广告的安排写在接口返回的 JSON 里）。
// @scriptlet no-fetch-if noFetchIf
// @alias prevent-fetch no-fetch-if
// @scriptlet trusted-prevent-fetch trustedPreventFetch trusted
// @scriptlet no-xhr-if noXhrIf
// @alias prevent-xhr no-xhr-if
// @scriptlet trusted-prevent-xhr trustedPreventXhr trusted
// @scriptlet trusted-replace-fetch-response trustedReplaceFetchResponse trusted
// @alias trusted-rpfr trusted-replace-fetch-response
// @scriptlet trusted-replace-xhr-response trustedReplaceXhrResponse trusted

const requestPropNames = new Set([
  'url', 'method', 'headers', 'body', 'mode', 'credentials', 'cache', 'redirect', 'referrer',
  'referrerPolicy', 'integrity', 'keepalive', 'signal', 'priority', 'duplex', 'destination',
]);

// 请求的条件：空格分开的 `属性:值`（值可以是 /正则/，`!` 开头是对不上），只写值的是
// 网址，`*` 是什么都算。全部对上才算。空的返回 null。
function propsMatcher(spec) {
  const needles = [];
  for (const token of String(spec).split(/\s+/)) {
    if (token === '') {
      continue;
    }
    if (token === '*') {
      needles.push({key: 'url', test: () => true});
      continue;
    }
    const colon = token.indexOf(':');
    const key = colon > 0 ? token.slice(0, colon) : '';
    if (requestPropNames.has(key)) {
      needles.push({key, test: matcher(token.slice(colon + 1))});
    } else {
      needles.push({key: 'url', test: matcher(token)});
    }
  }
  if (needles.length === 0) {
    return null;
  }
  return props => needles.every(needle => props[needle.key] !== undefined && needle.test(props[needle.key]));
}

const matchesAll = () => true;

// fetch 的参数整理成文字的属性（不是文字的转成 JSON）。
function fetchProps(args) {
  const [resource, init] = args;
  const props = {};
  if (resource instanceof $.Request) {
    for (const key of requestPropNames) {
      const value = resource[key];
      if (typeof value === 'string') {
        props[key] = value;
      }
    }
  } else {
    props.url = String(resource);
  }
  if (isObject(init)) {
    for (const key of $.keys(init)) {
      let value = init[key];
      if (typeof value !== 'string') {
        try {
          value = $.jsonStringify(value);
        } catch (e) {
          continue;
        }
      }
      if (typeof value === 'string') {
        props[key] = value;
      }
    }
  }
  return props;
}

function absoluteUrl(url) {
  try {
    return new URL(url, $.window.location.href).href;
  } catch (e) {
    return String(url);
  }
}

// 换掉 window.fetch：请求对上 matches 的交给 respond（返回响应或者 Promise），不然照常。
function hookFetch(matches, respond) {
  const original = $.window.fetch;
  if (typeof original !== 'function') {
    return;
  }
  $.window.fetch = new $.Proxy(original, {
    apply(target, thisArg, args) {
      let props;
      try {
        props = fetchProps(args);
        if (!matches(props)) {
          props = undefined;
        }
      } catch (e) {
        props = undefined;
      }
      if (props === undefined) {
        return $.apply(target, thisArg, args);
      }
      return respond(props, () => $.apply(target, thisArg, args));
    },
  });
}

// 照原来的响应（状态、头、类型、网址）做一个新内容的。
function rebuildResponse(original, body) {
  const response = new $.Response(body, {
    status: original.status,
    statusText: original.statusText,
    headers: original.headers,
  });
  for (const key of ['ok', 'redirected', 'type', 'url']) {
    try {
      $.defineProperty(response, key, {value: original[key]});
    } catch (e) {
    }
  }
  return response;
}

// 请求照发，响应交给 transform 改（返回新的响应；出错时用原来的）。
function hookFetchResponse(matches, transform) {
  hookFetch(matches, (props, send) => send().then(response =>
      $.Promise.resolve().then(() => transform(response)).catch(() => response)));
}

function preventFetch(trusted, propsToMatch, responseBody, responseType) {
  const matches = propsMatcher(propsToMatch);
  const body = fakeBody(responseBody, trusted);
  if (matches === null || body === undefined) {
    return;
  }
  hookFetch(matches, props => {
    const response = new $.Response(body, {
      status: 200,
      statusText: 'OK',
      headers: {'Content-Length': String(body.length)},
    });
    const type = ['basic', 'cors', 'opaque'].includes(responseType) ? responseType :
        props.mode === 'no-cors' ? 'opaque' : 'basic';
    const fields = {url: absoluteUrl(props.url), type, redirected: false};
    if (type === 'opaque') {
      Object.assign(fields, {url: '', status: 0, statusText: '', ok: false});
    }
    for (const key of $.keys(fields)) {
      try {
        $.defineProperty(response, key, {value: fields[key]});
      } catch (e) {
      }
    }
    return $.Promise.resolve(response);
  });
}

// 对上 propsToMatch 的 fetch 不发，返回 responseBody（空的、emptyObj、emptyArr、
// length:数、war:替身资源）。
function noFetchIf(propsToMatch = '', responseBody = '', responseType = '') {
  preventFetch(false, propsToMatch, responseBody, responseType);
}

function trustedPreventFetch(propsToMatch = '', responseBody = '') {
  preventFetch(true, propsToMatch, responseBody, '');
}

// 换掉 XMLHttpRequest：open 时对上 matches 的记下来（requests），members 返回的方法、
// 访问器装到子类上。base(xhr, 名字) 取原来的属性。
function hookXhr(matches, members) {
  const Base = $.window.XMLHttpRequest;
  if (typeof Base !== 'function') {
    return;
  }
  const requests = new $.WeakMap();
  const base = (xhr, name) => $.reflectGet(Base.prototype, name, xhr);
  const Hooked = class XMLHttpRequest extends Base {
    open(method, url, ...rest) {
      requests.delete(this);
      try {
        const props = {method: String(method), url: String(url)};
        if (matches(props)) {
          requests.set(this, {props});
        }
      } catch (e) {
      }
      return super.open(method, url, ...rest);
    }
  };
  const added = members(requests, base);
  for (const name of $.keys(added)) {
    const member = added[name];
    $.defineProperty(Hooked.prototype, name, typeof member === 'function' ?
        {value: member, writable: true, configurable: true} :
        {get: member.get, configurable: true, enumerable: true});
  }
  $.window.XMLHttpRequest = Hooked;
}

// 文字按 xhr.responseType 转成 response 的样子。
function typedResponse(type, text) {
  switch (type) {
    case 'json':
      try {
        return $.jsonParse(text);
      } catch (e) {
        return null;
      }
    case 'document':
      try {
        return new DOMParser().parseFromString(text, 'text/html');
      } catch (e) {
        return null;
      }
    case 'arraybuffer':
      return new TextEncoder().encode(text).buffer;
    case 'blob':
      return new Blob([text]);
    default:
      return text;
  }
}

function preventXhr(trusted, propsToMatch, directive) {
  const matches = propsMatcher(propsToMatch);
  const body = fakeBody(directive, trusted);
  if (matches === null || body === undefined) {
    return;
  }
  hookXhr(matches, (requests, base) => {
    const faked = xhr => {
      const request = requests.get(xhr);
      return request !== undefined && request.done === true ? request : undefined;
    };
    const fakedOr = (name, value) => ({
      get() {
        const request = faked(this);
        return request !== undefined ? value(request, this) : base(this, name);
      },
    });
    return {
      send(...args) {
        const request = requests.get(this);
        if (request === undefined) {
          return $.apply(base(this, 'send'), this, args);
        }
        later(() => {
          request.done = true;
          request.response = typedResponse(base(this, 'responseType'), body);
          dispatch(this, new Event('readystatechange'));
          dispatch(this, new ProgressEvent('load', {lengthComputable: true, loaded: body.length, total: body.length}));
          dispatch(this, new ProgressEvent('loadend', {lengthComputable: true, loaded: body.length, total: body.length}));
        }, 1);
      },
      getResponseHeader(name) {
        return faked(this) !== undefined ? null : $.apply(base(this, 'getResponseHeader'), this, [name]);
      },
      getAllResponseHeaders() {
        return faked(this) !== undefined ? '' : $.apply(base(this, 'getAllResponseHeaders'), this, []);
      },
      readyState: fakedOr('readyState', () => 4),
      status: fakedOr('status', () => 200),
      statusText: fakedOr('statusText', () => 'OK'),
      response: fakedOr('response', request => request.response),
      responseText: fakedOr('responseText', () => body),
      responseURL: fakedOr('responseURL', request => absoluteUrl(request.props.url)),
      responseXML: fakedOr('responseXML', () => null),
    };
  });
}

// 对上 propsToMatch 的 XMLHttpRequest 不发，响应是 directive（写法同 no-fetch-if 的
// responseBody）。
function noXhrIf(propsToMatch = '', directive = '') {
  preventXhr(false, propsToMatch, directive);
}

function trustedPreventXhr(propsToMatch = '', directive = '') {
  preventXhr(true, propsToMatch, directive);
}

// XMLHttpRequest 完成以后读到的文字（或者 JSON 的对象）交给 transform 改一次，之后读的
// 都是改过的。
function hookXhrResponse(matches, transform) {
  hookXhr(matches, (requests, base) => ({
    response: {
      get() {
        const inner = base(this, 'response');
        const request = requests.get(this);
        if (request === undefined || base(this, 'readyState') !== 4) {
          return inner;
        }
        if (request.source !== inner) {
          request.source = inner;
          try {
            request.result = transform(inner);
          } catch (e) {
            request.result = inner;
          }
        }
        return request.result;
      },
    },
    responseText: {
      get() {
        const response = this.response;
        return typeof response === 'string' ? response : base(this, 'responseText');
      },
    },
  }));
}

function replacementRegex(pattern) {
  return pattern === '*' ? /^[\s\S]*$/ : toRegex(pattern, 'gms');
}

// fetch 的响应里对上 pattern 的换成 replacement。
function trustedReplaceFetchResponse(pattern = '', replacement = '', propsToMatch = '') {
  if (pattern === '') {
    return;
  }
  const re = replacementRegex(pattern);
  hookFetchResponse(propsMatcher(propsToMatch) || matchesAll, response =>
      response.clone().text().then(text => {
        re.lastIndex = 0;
        const after = $.apply($.stringReplace, text, [re, replacement]);
        return after === text ? response : rebuildResponse(response, after);
      }));
}

function trustedReplaceXhrResponse(pattern = '', replacement = '', propsToMatch = '') {
  if (pattern === '') {
    return;
  }
  const re = replacementRegex(pattern);
  hookXhrResponse(propsMatcher(propsToMatch) || matchesAll, inner => {
    if (typeof inner !== 'string') {
      return inner;
    }
    re.lastIndex = 0;
    return $.apply($.stringReplace, inner, [re, replacement]);
  });
}
