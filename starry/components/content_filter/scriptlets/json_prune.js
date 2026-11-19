// 删掉 JSON 里的项（接口返回的广告安排、反屏蔽的开关）：JSON.parse、Response.json()
// 的结果，或者对上条件的 fetch、XMLHttpRequest 的响应。
// @requires network
// @scriptlet json-prune jsonPrune
// @scriptlet json-prune-fetch-response jsonPruneFetchResponse
// @scriptlet json-prune-xhr-response jsonPruneXhrResponse

// 按路径找（prune 时删掉）：`.` 分开每一级；`[]`、`*`、`{}` 是这一级的每一项；`[-]`、
// `{-}` 是删掉这一级里有后面那条路径的项。找到（删了）返回 true。
function findJsonPath(owner, chain, prune) {
  for (;;) {
    if (!isObject(owner)) {
      return false;
    }
    const dot = chain.indexOf('.');
    if (dot === -1) {
      if (chain === '*' || chain === '[]') {
        const keys = $.keys(owner);
        if (prune) {
          for (const key of keys) {
            delete owner[key];
          }
        }
        return keys.length !== 0;
      }
      if (!hasOwn(owner, chain)) {
        return false;
      }
      if (prune) {
        delete owner[chain];
      }
      return true;
    }
    const prop = chain.slice(0, dot);
    const rest = chain.slice(dot + 1);
    if (prop === '[-]' || prop === '{-}') {
      let found = false;
      if (Array.isArray(owner)) {
        for (let i = owner.length - 1; i >= 0; i--) {
          if (findJsonPath(owner[i], rest, false)) {
            if (prune) {
              owner.splice(i, 1);
            }
            found = true;
          }
        }
      } else {
        for (const key of $.keys(owner)) {
          if (findJsonPath(owner[key], rest, false)) {
            if (prune) {
              delete owner[key];
            }
            found = true;
          }
        }
      }
      return found;
    }
    if (prop === '[]' || prop === '*' || prop === '{}') {
      let found = false;
      for (const key of $.keys(owner)) {
        if (findJsonPath(owner[key], rest, prune)) {
          found = true;
        }
      }
      return found;
    }
    if (!hasOwn(owner, prop)) {
      return false;
    }
    owner = owner[prop];
    chain = rest;
  }
}

// 要删的路径、要求有的路径都用空格分开。needle 路径都有时（没写就不要求）才删；写了
// stackNeedle 时调用栈还要对上。返回删没删；没写要删的路径时返回 null。
function jsonPruner(rawPrunePaths, rawNeedlePaths, stackNeedle) {
  const prunePaths = String(rawPrunePaths).split(/ +/).filter(Boolean);
  const needlePaths = String(rawNeedlePaths).split(/ +/).filter(Boolean);
  if (prunePaths.length === 0) {
    return null;
  }
  const stack = stackNeedle ? matcher(stackNeedle) : null;
  return data => {
    if (!isObject(data)) {
      return false;
    }
    if (stack !== null && !stack(stackText())) {
      return false;
    }
    for (const path of needlePaths) {
      if (!findJsonPath(data, path, false)) {
        return false;
      }
    }
    let changed = false;
    for (const path of prunePaths) {
      if (findJsonPath(data, path, true)) {
        changed = true;
      }
    }
    return changed;
  };
}

function jsonPrune(rawPrunePaths = '', rawNeedlePaths = '', stackNeedle = '') {
  const prune = jsonPruner(rawPrunePaths, rawNeedlePaths, stackNeedle);
  if (prune === null) {
    return;
  }
  JSON.parse = new $.Proxy(JSON.parse, {
    apply(target, thisArg, args) {
      const result = $.apply(target, thisArg, args);
      try {
        prune(result);
      } catch (e) {
      }
      return result;
    },
  });
  const proto = $.Response !== undefined ? $.Response.prototype : undefined;
  if (proto !== undefined && typeof proto.json === 'function') {
    proto.json = new $.Proxy(proto.json, {
      apply(target, thisArg, args) {
        return $.apply(target, thisArg, args).then(result => {
          try {
            prune(result);
          } catch (e) {
          }
          return result;
        });
      },
    });
  }
}

// 后面可以写 `propsToMatch, 条件`（写法同 no-fetch-if），只改对上的请求。
function jsonPruneFetchResponse(rawPrunePaths = '', rawNeedlePaths = '', ...rest) {
  const prune = jsonPruner(rawPrunePaths, rawNeedlePaths, '');
  if (prune === null) {
    return;
  }
  const matches = propsMatcher(namedArgs(rest).propsToMatch || '') || matchesAll;
  hookFetchResponse(matches, response => response.clone().json().then(data =>
      prune(data) ? rebuildResponse(response, $.jsonStringify(data)) : response));
}

function jsonPruneXhrResponse(rawPrunePaths = '', rawNeedlePaths = '', ...rest) {
  const prune = jsonPruner(rawPrunePaths, rawNeedlePaths, '');
  if (prune === null) {
    return;
  }
  const matches = propsMatcher(namedArgs(rest).propsToMatch || '') || matchesAll;
  hookXhrResponse(matches, inner => {
    if (typeof inner === 'string') {
      let data;
      try {
        data = $.jsonParse(inner);
      } catch (e) {
        return inner;
      }
      return prune(data) ? $.jsonStringify(data) : inner;
    }
    prune(inner);
    return inner;
  });
}
