// Copyright 2026 The Starry Browser Authors
//
// 扩展选择器。浏览器把这个文档用的规则编成 JSON
// （starry/components/content_filter/procedural_selector.h），网页进程在文档的根元素建好时、
// 在隔离的 world 里调这个函数（starry/renderer/content_filter/content_filter_agent.cc）：
// 网页改不了这里用的 DOM 函数，也看不到这里的变量。先找一遍，之后页面每次变化
// （MutationObserver）再找；找一遍花的时间越长，隔得越久（最多占五分之一的时间）。
//
// filters：[[选择器, 动作, 参数…], …]。选择器是 [开头的原生选择器, [[操作, 参数…], …]]，
// 开头的是空的时从整个文档开始（`:xpath(…)`、`:matches-path(…) .x`）。
// 动作：0 藏起来（加上属性 attr，样式表里已经写了 display:none），1 删掉，2 加样式（参数是
// 第几种样式，加上属性 attr-第几种），3 去掉属性、4 去掉 class（参数是名字的正则和标志）。
// 藏起来、加样式的元素后来不符合了（文字变了）会去掉属性。
//
// 只能写 ASCII（generate_procedural_script.py 检查），注释写在单独的行上。
(function(filters, attr) {
'use strict';
const doc = document;
const ELEMENT = 1;
const SNAPSHOT = XPathResult.ORDERED_NODE_SNAPSHOT_TYPE;

const unique = (nodes) => nodes.length < 2 ? nodes : Array.from(new Set(nodes));
const isElement = (node) => node.nodeType === ELEMENT;
const textOf = (node) => node.textContent || '';

// `+ x`、`~ x`：node 后面的兄弟。用 :nth-child() 定位 node（不改网页）。
const siblings = (node, selector) => {
  const parent = node.parentElement;
  if (parent === null) {
    return [];
  }
  let index = 1;
  for (let e = node.previousElementSibling; e !== null; e = e.previousElementSibling) {
    index++;
  }
  return Array.from(parent.querySelectorAll(':scope > :nth-child(' + index + ') ' + selector));
};

// 开头的原生选择器，从 input（文档，或者 :has() 外面的元素）找。
const prime = (base, input) => {
  if (base === '') {
    return [input];
  }
  if (input === doc) {
    return Array.from(doc.querySelectorAll(base));
  }
  const c = base.charAt(0);
  if (c === '>') {
    return Array.from(input.querySelectorAll(':scope ' + base));
  }
  if (c === '+' || c === '~') {
    return siblings(input, base);
  }
  return Array.from(input.querySelectorAll(base));
};

const exec = (selector, input) => {
  let nodes = prime(selector.base, input);
  for (const task of selector.tasks) {
    if (nodes.length === 0) {
      break;
    }
    nodes = task(nodes);
  }
  return nodes;
};

// 要监视的属性（:watch-attr()）。
const watched = new Set();
let watchAll = false;
// :matches-media() 的媒体查询变了也重新找。
const mediaLists = [];

let compileSelector;

const tasks = {
  // 操作符后面接着的原生部分：` span`、`> span`（往里）、`+ span`、`~ span`（往后）。
  spath: (selector) => {
    const c = selector.charAt(0);
    return (nodes) => {
      const out = [];
      for (const node of nodes) {
        const found = c === '+' || c === '~'
            ? (node === doc ? [] : siblings(node, selector))
            : node.querySelectorAll(':scope' + selector);
        for (const e of found) {
          out.push(e);
        }
      }
      return unique(out);
    };
  },
  // 接着的复合选择器（`.x`、`:not(.y)`）：找到的元素还要符合。
  match: (selector) => (nodes) => nodes.filter((n) => isElement(n) && n.matches(selector)),
  text: (source, flags) => {
    const re = new RegExp(source, flags);
    return (nodes) => nodes.filter((n) => re.test(textOf(n)));
  },
  minlen: (n) => (nodes) => nodes.filter((node) => textOf(node).length >= n),
  has: (spec) => {
    const sub = compileSelector(spec);
    return (nodes) => nodes.filter((n) => exec(sub, n).length !== 0);
  },
  not: (spec) => {
    const sub = compileSelector(spec);
    return (nodes) => nodes.filter((n) => exec(sub, n).length === 0);
  },
  up: (n) => (nodes) => {
    const out = [];
    for (const node of nodes) {
      let e = node;
      for (let i = 0; i < n && e !== null; i++) {
        e = e.parentElement;
      }
      if (e !== null) {
        out.push(e);
      }
    }
    return unique(out);
  },
  upsel: (selector) => {
    // 先试一次，写错了在这里抛出（整条不用）。
    doc.documentElement.closest(selector);
    return (nodes) => {
      const out = [];
      for (const node of nodes) {
        const parent = node.parentElement;
        const e = parent === null ? null : parent.closest(selector);
        if (e !== null) {
          out.push(e);
        }
      }
      return unique(out);
    };
  },
  xpath: (expression) => {
    const compiled = doc.createExpression(expression, null);
    return (nodes) => {
      const out = [];
      for (const node of nodes) {
        const result = compiled.evaluate(node, SNAPSHOT, null);
        for (let i = 0; i < result.snapshotLength; i++) {
          const e = result.snapshotItem(i);
          if (isElement(e)) {
            out.push(e);
          }
        }
      }
      return unique(out);
    };
  },
  css: (pseudo, property, source, flags) => {
    const re = new RegExp(source, flags);
    const target = pseudo === '' ? null : pseudo;
    return (nodes) => nodes.filter((n) => isElement(n) &&
        re.test(getComputedStyle(n, target).getPropertyValue(property)));
  },
  attr: (nameSource, nameFlags, valueSource, valueFlags) => {
    const name = new RegExp(nameSource, nameFlags);
    const value = valueSource === undefined ? null : new RegExp(valueSource, valueFlags);
    return (nodes) => nodes.filter((n) => {
      if (!isElement(n)) {
        return false;
      }
      for (const a of n.attributes) {
        if (name.test(a.name) && (value === null || value.test(a.value))) {
          return true;
        }
      }
      return false;
    });
  },
  // 网址的路径加查询串（不管哪些元素）。网址里的中文这些是 %XX，规则里可能直接写：
  // 两种都试。
  path: (source, flags) => {
    const re = new RegExp(source, flags);
    const matches = () => {
      const path = location.pathname + location.search;
      if (re.test(path)) {
        return true;
      }
      try {
        return re.test(decodeURIComponent(path));
      } catch (e) {
        return false;
      }
    };
    return (nodes) => matches() ? nodes : [];
  },
  media: (query) => {
    const list = matchMedia(query);
    mediaLists.push(list);
    return (nodes) => list.matches ? nodes : [];
  },
  watch: (names) => {
    if (names.length === 0) {
      watchAll = true;
    }
    for (const name of names) {
      watched.add(name);
    }
    return (nodes) => nodes;
  },
  // 除了找到的元素、它们的上级和里面的，别的都要（藏起来）：上级的其他孩子。
  others: () => (nodes) => {
    const selected = new Set(nodes);
    const keep = new Set();
    for (const node of nodes) {
      for (let e = node; e !== null && !keep.has(e); e = e.parentElement) {
        keep.add(e);
      }
    }
    const out = [];
    for (const e of keep) {
      if (selected.has(e) || !isElement(e)) {
        continue;
      }
      for (const child of e.children) {
        if (!keep.has(child)) {
          out.push(child);
        }
      }
    }
    return out;
  },
};

compileSelector = (spec) => ({
  base: spec[0],
  tasks: spec[1].map((task) => tasks[task[0]].apply(null, task.slice(1))),
});

// 属性名 -> (元素 -> 几条规则在用)。
const marks = new Map();
const mark = (name, e) => {
  let counts = marks.get(name);
  if (counts === undefined) {
    counts = new Map();
    marks.set(name, counts);
  }
  counts.set(e, (counts.get(e) || 0) + 1);
  if (!e.hasAttribute(name)) {
    e.setAttribute(name, '');
  }
};
const unmark = (name, e) => {
  const counts = marks.get(name);
  const n = counts.get(e) - 1;
  if (n > 0) {
    counts.set(e, n);
    return;
  }
  counts.delete(e);
  e.removeAttribute(name);
};

const rules = [];
for (const filter of filters) {
  try {
    const rule = {
      selector: compileSelector(filter[0]),
      action: filter[1],
      name: null,
      re: null,
      marked: new Set(),
      dead: false,
    };
    if (rule.action === 0) {
      rule.name = attr;
    } else if (rule.action === 2) {
      rule.name = attr + '-' + filter[2];
    } else if (rule.action === 3 || rule.action === 4) {
      rule.re = new RegExp(filter[2], filter[3]);
    }
    rules.push(rule);
  } catch (e) {
    // 写错了的（正则、XPath）不用。
  }
}
if (rules.length === 0) {
  return;
}

const apply = (rule, elements) => {
  switch (rule.action) {
    case 0:
    case 2: {
      const next = new Set(elements);
      for (const e of rule.marked) {
        if (!next.has(e)) {
          unmark(rule.name, e);
        }
      }
      for (const e of next) {
        if (!rule.marked.has(e)) {
          mark(rule.name, e);
        } else if (!e.hasAttribute(rule.name)) {
          e.setAttribute(rule.name, '');
        }
      }
      rule.marked = next;
      break;
    }
    case 1:
      for (const e of elements) {
        if (e !== doc.documentElement) {
          e.remove();
        }
      }
      break;
    case 3:
      for (const e of elements) {
        for (const a of Array.from(e.attributes)) {
          if (rule.re.test(a.name)) {
            e.removeAttribute(a.name);
          }
        }
      }
      break;
    case 4:
      for (const e of elements) {
        for (const name of Array.from(e.classList)) {
          if (rule.re.test(name)) {
            e.classList.remove(name);
          }
        }
      }
      break;
  }
};

let observer = null;
let timer = 0;
let nextRun = 0;

const run = () => {
  const start = performance.now();
  for (const rule of rules) {
    if (rule.dead) {
      continue;
    }
    try {
      apply(rule, exec(rule.selector, doc).filter(isElement));
    } catch (e) {
      // 选择器写错了（浏览器不认）：之后不再找。
      rule.dead = true;
    }
  }
  // 自己改的（删掉的元素、去掉的属性）不用再找一遍。
  if (observer !== null) {
    observer.takeRecords();
  }
  const end = performance.now();
  nextRun = end + Math.min((end - start) * 4, 500);
};

const request = () => {
  if (timer !== 0) {
    return;
  }
  const wait = nextRun - performance.now();
  if (wait <= 0) {
    run();
    return;
  }
  timer = setTimeout(() => {
    timer = 0;
    run();
  }, wait);
};

run();
observer = new MutationObserver((records) => {
  for (const record of records) {
    if (record.type === 'childList' || !record.attributeName.startsWith(attr)) {
      request();
      return;
    }
  }
});
const options = {childList: true, subtree: true};
if (watchAll) {
  options.attributes = true;
} else if (watched.size !== 0) {
  options.attributes = true;
  options.attributeFilter = Array.from(watched);
}
observer.observe(doc, options);
for (const list of mediaLists) {
  list.addEventListener('change', request);
}
// 只改了文字（不加减节点）的变化 MutationObserver 不管，文档加载完再找一遍。
doc.addEventListener('DOMContentLoaded', request);
})
