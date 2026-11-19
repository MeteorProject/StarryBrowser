// 改网页里节点的文字，主要是内联脚本：解析器把 <script> 放进文档、执行之前会先通知
// MutationObserver，这时清空文字，脚本就不会跑。
// @scriptlet remove-node-text removeNodeText
// @alias rmnt remove-node-text
// @scriptlet trusted-replace-node-text trustedReplaceNodeText trusted
// @alias trusted-rpnt trusted-replace-node-text
// @alias replace-node-text trusted-replace-node-text
// @alias rpnt trusted-replace-node-text

// 节点名（script、#text，或者正则）对上 nodeName、文字对上 pattern 的清空。后面可以写
// `condition, 文字`（也可以写 includes，还要含有这个才动）、`excludes, 文字`（含有这个的不动）、
// `sedCount, 次数`、`quitAfter, 毫秒`、`stay, 1`。
function removeNodeText(nodeName = '', pattern = '', ...rest) {
  filterNodeText(nodeName, pattern, undefined, rest);
}

// 把 pattern 换成 replacement（正则的 $1 这些照用）；pattern 是空的时整段换成 replacement。
function trustedReplaceNodeText(nodeName = '', pattern = '', replacement = '', ...rest) {
  filterNodeText(nodeName, pattern, replacement, rest);
}

// 网页要求 Trusted Types 时（YouTube）给脚本写文字会被拒绝：脚本的文字经过一个自己的策略
// （同 uBlock Origin），也不经过网页的默认策略。用到时才建，建不了（网页限定了策略的名字）
// 时照写文字。
const nodeTextTypes = globalThis.trustedTypes;
const nodeTextCreatePolicy = isObject(nodeTextTypes) ? nodeTextTypes.createPolicy : undefined;
let nodeTextPolicy;

function setNodeText(node, text) {
  if (node.nodeName !== 'SCRIPT') {
    node.textContent = text;
    return;
  }
  if (nodeTextPolicy === undefined) {
    nodeTextPolicy = null;
    try {
      if (typeof nodeTextCreatePolicy === 'function') {
        nodeTextPolicy = $.apply(nodeTextCreatePolicy, nodeTextTypes,
            [randomText(12), {createScript: source => source}]);
      }
    } catch (e) {
    }
  }
  node.textContent = nodeTextPolicy !== null ? nodeTextPolicy.createScript(text) : text;
}

// 默认只管文档解析完以前加进来的节点（DOMContentLoaded 就停），stay 时一直看着。
function filterNodeText(nodeName, pattern, replacement, rest) {
  if (nodeName === '') {
    return;
  }
  const reNode = toRegex(nodeName, 'i', true);
  const rePattern = toRegex(pattern, 'gms');
  const options = namedArgs(rest);
  const reCondition = toRegex(options.condition || options.includes || '', 'ms');
  const reExcludes = options.excludes ? toRegex(options.excludes, 'ms') : null;
  let remaining = parseInt(options.sedCount || '0', 10) || 0;
  const quitAfter = parseInt(options.quitAfter || '0', 10) || 0;
  let observer;
  const stop = () => {
    if (observer !== undefined) {
      observer.disconnect();
    }
  };
  const handle = node => {
    const before = node.textContent;
    if (!reTest(rePattern, before) || !reTest(reCondition, before) ||
        (reExcludes !== null && reTest(reExcludes, before))) {
      return;
    }
    rePattern.lastIndex = 0;
    const after = replacement === undefined ? '' : pattern === '' ? replacement :
        $.apply($.stringReplace, before, [rePattern, replacement]);
    if (after !== before) {
      try {
        setNodeText(node, after);
      } catch (e) {
      }
    }
    if (remaining > 0 && --remaining === 0) {
      stop();
    }
  };
  const visit = (node, seen) => {
    // 脚本的文字是后加进来的子节点：看它所在的元素。
    if (node.nodeType === 3 && !reTest(reNode, '#text')) {
      node = node.parentNode;
      if (node === null) {
        return;
      }
    }
    if (seen.has(node) || !reTest(reNode, node.nodeName)) {
      return;
    }
    seen.add(node);
    handle(node);
  };
  observer = new $.MutationObserver(records => {
    const seen = new Set();
    for (const record of records) {
      for (const node of record.addedNodes) {
        visit(node, seen);
      }
    }
  });
  observer.observe($.document, {childList: true, subtree: true});
  const root = $.document.documentElement;
  if (root !== null) {
    const seen = new Set();
    const walker = $.document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
    for (let node = walker.currentNode; node !== null; node = walker.nextNode()) {
      visit(node, seen);
    }
  }
  if (options.stay) {
    return;
  }
  runAt(() => {
    if (quitAfter > 0) {
      later(stop, quitAfter);
    } else {
      stop();
    }
  }, 'interactive');
}
