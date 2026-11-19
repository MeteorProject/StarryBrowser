// uBlock Origin 写法的 JSONPath，和用它改 JSON 对象的几种：JSON.parse 的结果、函数的参数和
// 返回值、fetch / XMLHttpRequest 的请求体和响应。不带 trusted 的只能删掉找到的项，信得过的
// 列表里还能改值（YouTube 用它改发出去的播放请求，Facebook 用它删掉信息流里的广告）。
// @requires network
// @scriptlet json-edit jsonEdit
// @scriptlet trusted-json-edit trustedJsonEdit trusted
// @scriptlet edit-inbound-object editInboundObject
// @scriptlet trusted-edit-inbound-object trustedEditInboundObject trusted
// @scriptlet edit-outbound-object editOutboundObject
// @scriptlet trusted-edit-outbound-object trustedEditOutboundObject trusted
// @scriptlet json-edit-xhr-response jsonEditXhrResponse
// @scriptlet trusted-json-edit-xhr-response trustedJsonEditXhrResponse trusted
// @scriptlet json-edit-fetch-response jsonEditFetchResponse
// @scriptlet trusted-json-edit-fetch-response trustedJsonEditFetchResponse trusted
// @scriptlet json-edit-xhr-request jsonEditXhrRequest
// @scriptlet trusted-json-edit-xhr-request trustedJsonEditXhrRequest trusted
// @scriptlet json-edit-fetch-request jsonEditFetchRequest
// @scriptlet trusted-json-edit-fetch-request trustedJsonEditFetchRequest trusted
// @scriptlet jsonl-edit-xhr-response jsonlEditXhrResponse
// @scriptlet trusted-jsonl-edit-xhr-response trustedJsonlEditXhrResponse trusted
// @scriptlet jsonl-edit-fetch-response jsonlEditFetchResponse
// @scriptlet trusted-jsonl-edit-fetch-response trustedJsonlEditFetchResponse trusted

// JSONPath 的写法（同 uBlock Origin，RFC 9535 的一部分）：
//   $ 根（开头不写也是根），@ 当前的项；.名字、.*、./正则/ 下一级；..名字 下面所有级；
//   ['名字', 0, -1] 下一级的这些（数字是数组的下标，负数从后往前数）；[*] 下一级的每一项；
//   [?路径] 筛选：用在数组上留下有这条路径的项，用在对象上留下这个对象自己（v2: 开头时
//   对象也按每一项筛），[?!路径] 反过来；路径的最后一级可以带比较：== != < <= > >=
//   （右边是 JSON 或者 '文字'）、^= $= *=（转成文字后开头、结尾、含有）、=/正则/（可以带 i）；
//   `;$` 前面这段要找到才接着从根找后面的，`{n}`、`{n,m}`、`{n,}`、`{,m}` 后面跟 `;$` 是要
//   找到这么多个。
// 最后可以写 `=JSON 值`（换成这个值）、`+=JSON 对象或数组`（并进对象、加到数组后面）、
// `=repl({"regex":…,"flags":…,"replacement":…})`（文字里替换，也可以写 "pattern" 当字面的
// 文字）、`=call(["${obj}", "方法", 参数…])`（调方法，${obj}、${key}、${val} 是找到的项的
// 主人、名字、值）；值里的 ${now} 换成现在的时间（毫秒）。不写时删掉找到的项。
const jpRoot = 1;
const jpCurrent = 2;
const jpChildren = 3;
const jpDescendants = 4;
const jpCount = 5;
const jpIsArray = Array.isArray;
const jpStructuredClone = globalThis.structuredClone;

// 从 i 开始找到 end 这个字符（`\end` 是它本身）。返回中间的文字和 end 后面的位置；没有
// 返回 undefined。
function jpUntil(text, end, i) {
  let out = '';
  for (let j = i; j < text.length; j++) {
    const c = text[j];
    if (c === end) {
      return {text: out, end: j + 1};
    }
    if (c === '\\' && text[j + 1] === end) {
      out += end;
      j++;
      continue;
    }
    out += c;
  }
  return undefined;
}

// `.` 后面的名字：/正则/、字母数字下划线、*。
function jpName(query, i) {
  if (query[i] === '/') {
    const r = jpUntil(query, '/', i + 1);
    if (r === undefined) {
      return undefined;
    }
    try {
      return {key: new $.RegExp(r.text), end: r.end};
    } catch (e) {
      return undefined;
    }
  }
  const match = /^(?:[A-Za-z_]\w*|\*)/.exec(query.slice(i));
  return match === null ? undefined : {key: match[0], end: i + match[0].length};
}

// `[` 后面逗号分开的名字，到 `]` 为止：'文字'、"文字"、整数。返回一个（或者一组）名字和 `]`
// 的位置。不带引号的名字同 uBlock Origin 不算数（v2 时直接是写错了）。
function jpKeys(query, i, v2) {
  const keys = [];
  let needKey = true;
  while (i < query.length && query[i] !== ']') {
    const c = query[i];
    if (c === ' ') {
      i++;
      continue;
    }
    if (c === ',') {
      if (needKey) {
        return undefined;
      }
      needKey = true;
      i++;
      continue;
    }
    if (c === '"' || c === '\'') {
      const r = jpUntil(query, c, i + 1);
      if (r === undefined) {
        return undefined;
      }
      keys.push(r.text);
      i = r.end;
      needKey = false;
      continue;
    }
    const number = /^-?\d+/.exec(query.slice(i));
    if (number !== null) {
      keys.push(parseInt(number[0], 10));
      i += number[0].length;
      needKey = false;
      continue;
    }
    if (v2) {
      return undefined;
    }
    const r = jpName(query, i);
    if (r === undefined) {
      return undefined;
    }
    keys.push(r.key);
    i = r.end;
  }
  if (needKey || i >= query.length) {
    return undefined;
  }
  return {key: keys.length === 1 ? keys[0] : keys, end: i};
}

// 名字后面的比较，记在 step 上，返回比较后面的位置；不是比较时返回 undefined。
function jpTest(query, step, i) {
  if (query.startsWith('=/', i)) {
    const r = jpUntil(query, '/', i + 2);
    if (r === undefined) {
      return i;
    }
    const flags = query[r.end] === 'i' ? 'i' : '';
    try {
      step.value = new $.RegExp(r.text, flags);
    } catch (e) {
      return undefined;
    }
    step.op = 're';
    return r.end + flags.length;
  }
  const match = /^\s*([!=^$*]=|[<>]=?)\s*(.+?)\]/.exec(query.slice(i));
  if (match === null) {
    return undefined;
  }
  let value;
  if (match[2][0] === '\'') {
    const r = jpUntil(match[2], '\'', 1);
    if (r === undefined) {
      return undefined;
    }
    value = r.text;
  } else {
    try {
      value = $.jsonParse(match[2]);
    } catch (e) {
      return undefined;
    }
  }
  step.op = match[1];
  step.value = value;
  return i + match[0].length - 1;
}

// 从 i 开始的一段路径，编成一步步（筛选里的路径也是这样一段）。返回这些步和停下的位置
// （后面是比较、`]` 或者赋值）；写错了返回 undefined。
function jpSteps(query, i, v2) {
  const steps = [];
  if (query[i] === '$') {
    steps.push({mv: jpRoot});
    i++;
  } else if (query[i] === '@') {
    steps.push({mv: jpCurrent});
    i++;
  } else {
    steps.push({mv: i === 0 ? jpRoot : jpCurrent});
  }
  let mv = 0;
  while (i < query.length) {
    const c = query[i];
    if (c === ' ') {
      i++;
      continue;
    }
    if (c === '.') {
      if (mv !== 0) {
        return undefined;
      }
      mv = query[i + 1] === '.' ? jpDescendants : jpChildren;
      i += mv === jpDescendants ? 2 : 1;
      continue;
    }
    if (c === ';' || c === '{') {
      let min = 1;
      let max = 1e6;
      let length = 2;
      if (c === '{') {
        const match = /^\{(\d+|\d+,\d+|\d+,|,\d+)\};\$/.exec(query.slice(i));
        if (match === null) {
          return undefined;
        }
        const comma = match[1].indexOf(',');
        if (comma === -1) {
          min = max = parseInt(match[1], 10);
        } else {
          min = parseInt(match[1].slice(0, comma), 10) || 0;
          max = parseInt(match[1].slice(comma + 1), 10) || 1e6;
        }
        length = match[0].length;
      } else if (!query.startsWith(';$', i)) {
        return undefined;
      }
      steps.push({mv: jpCount, min, max}, {mv: jpRoot});
      i += length;
      mv = 0;
      continue;
    }
    if (c !== '[') {
      if (mv === 0) {
        const end = jpTest(query, steps[steps.length - 1], i);
        if (end !== undefined) {
          i = end;
        }
        break;
      }
      const r = jpName(query, i);
      if (r === undefined) {
        return undefined;
      }
      steps.push({mv, key: r.key});
      i = r.end;
      mv = 0;
      continue;
    }
    if (mv === jpChildren) {
      return undefined;
    }
    if (query.startsWith('[?', i)) {
      const not = query[i + 2] === '!';
      const r = jpSteps(query, i + (not ? 3 : 2), v2);
      if (r === undefined || query[r.end] !== ']') {
        return undefined;
      }
      if (not) {
        r.steps[r.steps.length - 1].not = true;
      }
      steps.push({mv: mv || jpChildren, steps: r.steps});
      i = r.end + 1;
      mv = 0;
      continue;
    }
    if (query.startsWith('[*]', i)) {
      steps.push({mv: mv || jpChildren, key: '*'});
      i += 3;
      mv = 0;
      continue;
    }
    const r = jpKeys(query, i + 1, v2);
    if (r === undefined) {
      return undefined;
    }
    steps.push({mv: mv || jpChildren, key: r.key});
    i = r.end + 1;
    mv = 0;
  }
  if (mv !== 0) {
    return undefined;
  }
  return {steps, end: i};
}

// 对象下面的名字 key（一组、数组下标、*、正则）展开成具体的名字。不是对象返回 undefined。
function jpExpandKey(owner, key) {
  if (typeof owner !== 'object' || owner === null) {
    return undefined;
  }
  if (jpIsArray(key)) {
    const out = [];
    for (const one of key) {
      const keys = jpExpandKey(owner, one);
      if (keys !== undefined) {
        out.push(...keys);
      }
    }
    return out;
  }
  if (typeof key === 'number') {
    if (!jpIsArray(owner)) {
      return undefined;
    }
    return [key >= 0 ? key : owner.length + key];
  }
  if (key === '*') {
    return jpIsArray(owner) ? [...owner.keys()] : $.keys(owner);
  }
  if (key instanceof $.RegExp) {
    return $.keys(owner).filter(name => reTest(key, name));
  }
  return [key];
}

// value 下面的每一项（recursive 时一直往下，先父后子，同一个对象只进一次）：所在的对象、
// 名字和从 value 开始的路径。
function* jpEntries(value, recursive) {
  const seen = new WeakSet();
  const stack = [];
  const path = [];
  const push = object => stack.push({object, keys: jpIsArray(object) ? object.keys() : $.keys(object).values()});
  if (typeof value === 'object' && value !== null) {
    push(value);
  }
  while (stack.length !== 0) {
    const top = stack[stack.length - 1];
    const entry = top.keys.next();
    if (entry.done) {
      stack.pop();
      path.pop();
      continue;
    }
    path[stack.length - 1] = entry.value;
    yield {object: top.object, key: entry.value, path: path.slice()};
    const child = top.object[entry.value];
    if (recursive && typeof child === 'object' && child !== null && !seen.has(child)) {
      seen.add(child);
      push(child);
    }
  }
}

// owner 的 key 这一项对不对得上 step（有没有、比较）。
function jpTestKey(step, owner, key) {
  if (owner === undefined || owner === null) {
    return false;
  }
  let value;
  let has;
  try {
    value = owner[key];
    has = value !== undefined || hasOwn(owner, key);
  } catch (e) {
    return false;
  }
  if (step.op !== undefined && !has) {
    return false;
  }
  const want = step.not !== true;
  try {
    switch (step.op) {
      case '==':
        return (value === step.value) === want;
      case '!=':
        return (value !== step.value) === want;
      case '<':
        return (value < step.value) === want;
      case '<=':
        return (value <= step.value) === want;
      case '>':
        return (value > step.value) === want;
      case '>=':
        return (value >= step.value) === want;
      case '^=':
        return String(value).startsWith(step.value) === want;
      case '$=':
        return String(value).endsWith(step.value) === want;
      case '*=':
        return String(value).includes(step.value) === want;
      case 're':
        // 同 uBlock Origin：正则不管 `!`。
        return reTest(step.value, String(value));
    }
  } catch (e) {
    return false;
  }
  return has === want;
}

// 编好的 JSONPath：apply(根) 删掉（写了值时改掉）找到的每一项，返回改过的根（根被删掉时
// 是 null）；什么也没找到时返回 undefined，根不变。test(根) 只看找不找得到。写错的返回 null。
function compileJsonPath(query) {
  query = String(query);
  let v2 = false;
  if (query.startsWith('v2:')) {
    v2 = true;
    query = query.slice(3);
  }
  if (query === '') {
    return null;
  }
  const parsed = jpSteps(query, 0, v2);
  if (parsed === undefined) {
    return null;
  }
  let modify;
  let raw;
  let value;
  if (parsed.end !== query.length) {
    const rest = query.slice(parsed.end);
    const call = /^=([a-z]+)\((.+)\)$/.exec(rest);
    if (call !== null) {
      modify = call[1];
      raw = call[2];
    } else if (rest.startsWith('=')) {
      raw = rest.slice(1);
    } else if (rest.startsWith('+=')) {
      modify = '+';
      raw = rest.slice(2);
    } else {
      return null;
    }
    try {
      value = $.jsonParse(raw);
    } catch (e) {
      return null;
    }
  }
  let replaceRegex;
  // 路径从 box 开始：box.$ 是根，删掉、换掉根也是改它。
  const resolve = (box, path) => {
    if (path.length === 0) {
      return {value: box};
    }
    let object = box;
    for (let i = 0; i < path.length - 1; i++) {
      object = object[path[i]];
      if (!isObject(object)) {
        return {};
      }
    }
    const key = path[path.length - 1];
    return {object, key, value: object[key]};
  };
  const evaluate = (box, steps, from) => {
    let found = [];
    for (const step of steps) {
      switch (step.mv) {
        case jpRoot:
          found = [['$']];
          break;
        case jpCurrent:
          if (step.op !== undefined) {
            const {object, key} = resolve(box, from);
            if (object === undefined) {
              return [];
            }
            if (!jpTestKey(step, object, key)) {
              break;
            }
          }
          found = [from];
          break;
        case jpChildren:
        case jpDescendants:
          if (found.length !== 0) {
            found = descend(box, found, step);
          }
          break;
        case jpCount:
          if (found.length < step.min || found.length > step.max) {
            return [];
          }
          found = [];
          break;
      }
    }
    return found;
  };
  const filter = (box, from, step, owner, out) => {
    const recursive = step.mv === jpDescendants;
    const each = v2 || recursive || jpIsArray(owner);
    for (const {path} of jpEntries(owner, recursive)) {
      const candidate = each ? [...from, ...path] : from;
      if (evaluate(box, step.steps, candidate).length === 0) {
        continue;
      }
      out.push(candidate);
      if (!each) {
        break;
      }
    }
  };
  const descend = (box, list, step) => {
    const out = [];
    for (const from of list) {
      const owner = resolve(box, from).value;
      if (owner === undefined) {
        continue;
      }
      if (step.steps !== undefined) {
        filter(box, from, step, owner, out);
        continue;
      }
      for (const key of jpExpandKey(owner, step.key) || []) {
        if (jpTestKey(step, owner, key)) {
          out.push([...from, key]);
        }
      }
      if (step.mv !== jpDescendants) {
        continue;
      }
      for (const {object, key: name, path} of jpEntries(owner, true)) {
        const child = object[name];
        for (const key of jpExpandKey(child, step.key) || []) {
          if (jpTestKey(step, child, key)) {
            out.push([...from, ...path, key]);
          }
        }
      }
    }
    return out;
  };
  // 每次用一份新的值（几处、几次改的不共用一个对象）。
  const change = (object, key) => {
    let next = modify === 'repl' ? value : $.jsonParse(raw);
    if (typeof next === 'string') {
      next = next.replace('${now}', String(Date.now()));
    }
    switch (modify) {
      case undefined:
        object[key] = next;
        break;
      case '+': {
        const current = object[key];
        if (!isObject(next) || !isObject(current)) {
          break;
        }
        if (jpIsArray(current)) {
          if (jpIsArray(next)) {
            current.push(...next);
          } else {
            current.push(next);
          }
          break;
        }
        for (const name of $.keys(next)) {
          current[name] = next[name];
        }
        break;
      }
      case 'call': {
        if (!jpIsArray(next) || next.length < 2) {
          break;
        }
        const args = next.map(arg => arg === '${obj}' ? object : arg === '${key}' ? key :
            arg === '${val}' ? object[key] : arg);
        const self = args[0] !== undefined && args[0] !== null ? args[0] : $.window;
        self[args[1]](...args.slice(2));
        break;
      }
      case 'repl': {
        const current = object[key];
        if (typeof current !== 'string' || !isObject(next)) {
          break;
        }
        if (replaceRegex === undefined) {
          replaceRegex = null;
          try {
            replaceRegex = next.regex !== undefined ? new $.RegExp(next.regex, next.flags) :
                new $.RegExp(String(next.pattern).replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
          } catch (e) {
          }
        }
        if (replaceRegex !== null) {
          replaceRegex.lastIndex = 0;
          object[key] = $.apply($.stringReplace, current, [replaceRegex, next.replacement]);
        }
        break;
      }
    }
  };
  // 从后往前改：同一个数组里删掉几项时前面的下标不变。
  const edit = (box, paths) => {
    for (let i = paths.length - 1; i >= 0; i--) {
      const {object, key} = resolve(box, paths[i]);
      if (object === undefined) {
        continue;
      }
      try {
        if (raw !== undefined) {
          change(object, key);
        } else if (jpIsArray(object) && typeof key === 'number') {
          object.splice(key, 1);
        } else {
          delete object[key];
        }
      } catch (e) {
      }
    }
  };
  return {
    assigns: raw !== undefined,
    test(root) {
      return evaluate({$: root}, parsed.steps, []).length !== 0;
    },
    apply(root) {
      const box = {$: root};
      const paths = evaluate(box, parsed.steps, []);
      if (paths.length === 0) {
        return undefined;
      }
      edit(box, paths);
      return box.$ === undefined || box.$ === null ? null : box.$;
    },
  };
}

// 列表里的 JSONPath：写错的、不信任却要改值的返回 null。
function jsonEditPath(trusted, query) {
  const path = compileJsonPath(query);
  return path === null || (path.assigns && !trusted) ? null : path;
}

// JSON 文字改过以后的文字；不是对象、没改返回 undefined。
function jsonEditText(path, text) {
  let data;
  try {
    data = $.jsonParse(text);
  } catch (e) {
    return undefined;
  }
  if (!isObject(data)) {
    return undefined;
  }
  const after = path.apply(data);
  return after === undefined ? undefined : $.jsonStringify(after);
}

// JSON.parse 的结果。后面可以写 `matches, 文字或者 /正则/`：只改原文对上的。
function jsonEditParse(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  if (path === null) {
    return;
  }
  const matches = namedArgs(rest).matches;
  const reMatches = matches !== undefined && matches !== '' ? toRegex(matches) : null;
  wrapMethod('JSON.parse', call => {
    const result = call.reflect();
    const text = call.args[0];
    if (reMatches !== null && typeof text === 'string' && !reTest(reMatches, text)) {
      return result;
    }
    try {
      const after = path.apply(result);
      return after === undefined ? result : after;
    } catch (e) {
      return result;
    }
  });
}

function jsonEdit(query = '', ...rest) {
  jsonEditParse(false, query, rest);
}

function trustedJsonEdit(query = '', ...rest) {
  jsonEditParse(true, query, rest);
}

// 调 chain 时第 argpos 个参数（负数从后往前数）是对象就改：改的是它的一份拷贝，网页手里的
// 不变（YouTube 的规则改 JSON.stringify 的参数，就是改发出去的请求）。
function editInbound(trusted, chain, argpos, query) {
  const path = jsonEditPath(trusted, query);
  const position = parseInt(argpos, 10);
  if (chain === '' || path === null || Number.isNaN(position) || typeof jpStructuredClone !== 'function') {
    return;
  }
  wrapMethod(chain, call => {
    const args = call.args;
    const index = position >= 0 ? position : args.length + position;
    const before = index >= 0 && index < args.length ? args[index] : undefined;
    if (typeof before === 'object' && before !== null) {
      try {
        if (path.test(before)) {
          const after = path.apply(jpStructuredClone(before));
          if (after !== undefined && after !== null) {
            args[index] = after;
          }
        }
      } catch (e) {
      }
    }
    return call.reflect();
  });
}

function editInboundObject(chain = '', argpos = '', query = '') {
  editInbound(false, chain, argpos, query);
}

function trustedEditInboundObject(chain = '', argpos = '', query = '') {
  editInbound(true, chain, argpos, query);
}

// chain 返回的对象。
function editOutbound(trusted, chain, query) {
  const path = jsonEditPath(trusted, query);
  if (chain === '' || path === null) {
    return;
  }
  wrapMethod(chain, call => {
    const result = call.reflect();
    try {
      const after = path.apply(result);
      return after === undefined ? result : after;
    } catch (e) {
      return result;
    }
  });
}

function editOutboundObject(chain = '', query = '') {
  editOutbound(false, chain, query);
}

function trustedEditOutboundObject(chain = '', query = '') {
  editOutbound(true, chain, query);
}

// 后面的 `propsToMatch, 条件`（写法同 no-fetch-if）：只改对上的请求，没写时全部。
function jsonEditMatcher(rest) {
  return propsMatcher(namedArgs(rest).propsToMatch || '') || matchesAll;
}

// XMLHttpRequest 的响应（文字或者 responseType 是 json 时的对象）。
function jsonEditXhr(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  if (path === null) {
    return;
  }
  hookXhrResponse(jsonEditMatcher(rest), inner => {
    if (typeof inner === 'string') {
      const after = jsonEditText(path, inner);
      return after === undefined ? inner : after;
    }
    if (typeof inner !== 'object' || inner === null) {
      return inner;
    }
    const after = path.apply(inner);
    return after === undefined ? inner : after;
  });
}

function jsonEditXhrResponse(query = '', ...rest) {
  jsonEditXhr(false, query, rest);
}

function trustedJsonEditXhrResponse(query = '', ...rest) {
  jsonEditXhr(true, query, rest);
}

// fetch 的响应。
function jsonEditFetch(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  if (path === null) {
    return;
  }
  hookFetchResponse(jsonEditMatcher(rest), response => response.clone().json().then(data => {
    if (typeof data !== 'object' || data === null) {
      return response;
    }
    const after = path.apply(data);
    return after === undefined ? response : rebuildResponse(response, $.jsonStringify(after));
  }));
}

function jsonEditFetchResponse(query = '', ...rest) {
  jsonEditFetch(false, query, rest);
}

function trustedJsonEditFetchResponse(query = '', ...rest) {
  jsonEditFetch(true, query, rest);
}

// XMLHttpRequest 发出去的 JSON 文字。
function jsonEditXhrSend(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  if (path === null) {
    return;
  }
  hookXhr(jsonEditMatcher(rest), (requests, base) => ({
    send(...args) {
      if (requests.get(this) !== undefined && typeof args[0] === 'string') {
        try {
          const after = jsonEditText(path, args[0]);
          if (after !== undefined) {
            args[0] = after;
          }
        } catch (e) {
        }
      }
      return $.apply(base(this, 'send'), this, args);
    },
  }));
}

function jsonEditXhrRequest(query = '', ...rest) {
  jsonEditXhrSend(false, query, rest);
}

function trustedJsonEditXhrRequest(query = '', ...rest) {
  jsonEditXhrSend(true, query, rest);
}

// fetch、new Request 的第二个参数里的 body（JSON 文字）。
function jsonEditFetchSend(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  if (path === null) {
    return;
  }
  const matches = jsonEditMatcher(rest);
  const edit = call => {
    const init = call.args[1];
    try {
      const body = isObject(init) ? init.body : undefined;
      if (typeof body === 'string' && body !== '') {
        const after = jsonEditText(path, body);
        if (after !== undefined && after !== body && matches(fetchProps(call.args))) {
          init.body = after;
        }
      }
    } catch (e) {
    }
    return call.reflect();
  };
  wrapMethod('fetch', edit);
  wrapMethod('Request', edit);
}

function jsonEditFetchRequest(query = '', ...rest) {
  jsonEditFetchSend(false, query, rest);
}

function trustedJsonEditFetchRequest(query = '', ...rest) {
  jsonEditFetchSend(true, query, rest);
}

// 一行一个 JSON 的文字（Facebook 的 graphql）：每行对上 extract 的第一组是 JSON，改过的
// 换回去。
function jsonlEditText(path, text, extract) {
  const separator = /\r?\n/.exec(text);
  const lines = text.split(/\r?\n/);
  let changed = false;
  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    extract.lastIndex = 0;
    const match = extract.exec(line);
    if (match === null || match[1] === undefined) {
      continue;
    }
    const after = jsonEditText(path, match[1]);
    if (after === undefined) {
      continue;
    }
    const [begin, end] = match.indices[1];
    lines[i] = line.slice(0, begin) + after + line.slice(end);
    changed = true;
  }
  return changed ? lines.join(separator !== null ? separator[0] : '\n') : text;
}

// `jsonExtract, 正则`：每行里 JSON 的那段（第一组），没写时是整行。
function jsonlExtract(rest) {
  const source = namedArgs(rest).jsonExtract;
  try {
    return new $.RegExp(source !== undefined && source !== '' ? source : '^(.*)$', 'd');
  } catch (e) {
    return null;
  }
}

function jsonlEditXhr(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  const extract = jsonlExtract(rest);
  if (path === null || extract === null) {
    return;
  }
  hookXhrResponse(jsonEditMatcher(rest), inner =>
      typeof inner === 'string' ? jsonlEditText(path, inner, extract) : inner);
}

function jsonlEditXhrResponse(query = '', ...rest) {
  jsonlEditXhr(false, query, rest);
}

function trustedJsonlEditXhrResponse(query = '', ...rest) {
  jsonlEditXhr(true, query, rest);
}

function jsonlEditFetch(trusted, query, rest) {
  const path = jsonEditPath(trusted, query);
  const extract = jsonlExtract(rest);
  if (path === null || extract === null) {
    return;
  }
  hookFetchResponse(jsonEditMatcher(rest), response => response.clone().text().then(text => {
    const after = jsonlEditText(path, text, extract);
    return after === text ? response : rebuildResponse(response, after);
  }));
}

function jsonlEditFetchResponse(query = '', ...rest) {
  jsonlEditFetch(false, query, rest);
}

function trustedJsonlEditFetchResponse(query = '', ...rest) {
  jsonlEditFetch(true, query, rest);
}
