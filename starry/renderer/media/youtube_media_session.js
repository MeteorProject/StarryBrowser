// Copyright 2026 The Starry Browser Authors
//
// YouTube 页面里的媒体会话只跟着「主」播放器。
//
// YouTube 页面里每个播放器（.html5-video-player：正在看的 #movie_player、鼠标停在推荐上
// 时出来的预览 #inline-preview-player、频道页的预告片……）各有一份媒体会话的代码，自己的
// 视频数据、状态一变，就把整个 navigator.mediaSession 写成自己的：标题、作者、封面、播放
// 状态、播放 / 暂停 / 快进这些按钮（绑在它自己身上），谁最后写算谁。小窗播放器里暂停着
// 歌、在搜索结果里悬停一下，浏览器（媒体岛、系统的「正在播放」、媒体键）看到的就成了推荐
// 的那个视频，而且是「在放」，按暂停去暂停的是预览。
//
// 这里包住 MediaSession 的写入：同一轮（一个任务里同步写的一串，到微任务时）算一批，
// 按标题认出是哪个播放器写的（YouTube 写的标题就是那个播放器 getVideoData().title），
// 页面里还有排得更高、已经载入的播放器时，这一批不交给浏览器。排名：不是预览、没静音的
// 最高；预览或者静音的其次；静音的预览最低。网页自己读回来的还是它写的值。
//
// 标题认不出的（广告）、页面里只有一个播放器的照常。播放状态单独写来时（YouTube 的
// 播放器状态一变就写）对不上主播放器的才不交；按钮、进度单独写来时，有排得低的播放器
// 在用就不交。按钮和进度先照常交给浏览器（原来的函数检查参数、抛同样的错），不要时再
// 改回去。
(function() {
'use strict';
const session = navigator.mediaSession;
if (!session || typeof MediaSession !== 'function' ||
    typeof MediaMetadata !== 'function') {
  return;
}
const proto = MediaSession.prototype;
const metadataProp = Object.getOwnPropertyDescriptor(proto, 'metadata');
const stateProp = Object.getOwnPropertyDescriptor(proto, 'playbackState');
const setHandler = proto.setActionHandler;
const setPosition = proto.setPositionState;
if (!metadataProp || !metadataProp.get || !metadataProp.set || !stateProp ||
    !stateProp.get || !stateProp.set || typeof setHandler !== 'function' ||
    typeof setPosition !== 'function') {
  return;
}
// 先拿好：之后网页换掉这些也不影响这里。
const apply = Reflect.apply;
const defer = queueMicrotask.bind(window);
const querySelectorAll = Document.prototype.querySelectorAll;
const elementQuery = Element.prototype.querySelector;
const elementClosest = Element.prototype.closest;
const STATES = ['none', 'paused', 'playing'];
// YouTube 的播放器会登记的操作（这几个总是合法的）。别的操作不经过这里。
const ACTIONS = ['play', 'pause', 'seekbackward', 'seekforward', 'seekto',
                 'previoustrack', 'nexttrack'];

// 网页写的（读回来给网页的就是这些）。
let pageMetadata = apply(metadataProp.get, session, []);
let pageState = apply(stateProp.get, session, []);
// 交给了浏览器的：要改回去时用。
const keptHandlers = new Map();
let keptPosition = [];
let pending = null;

function queue(op) {
  if (!pending) {
    pending = [];
    defer(flush);
  }
  pending.push(op);
}

function players() {
  const list = [];
  const nodes = apply(querySelectorAll, document, ['.html5-video-player']);
  for (let i = 0; i < nodes.length; i++) {
    const node = nodes[i];
    const video = apply(elementQuery, node, ['video']);
    let title = null;
    try {
      const data = typeof node.getVideoData === 'function' ?
          node.getVideoData() : null;
      if (data && typeof data.title === 'string') {
        title = data.title;
      }
    } catch (e) {
    }
    const preview = node.id === 'inline-preview-player' ||
        apply(elementClosest, node, ['ytd-video-preview']) !== null;
    const silent = !video || video.muted || video.volume === 0;
    list.push({
      title: title,
      video: video,
      loaded: !!video && video.readyState > 0,
      busy: !!video && (video.readyState > 0 || !video.paused),
      rank: (preview ? 0 : 1) + (silent ? 0 : 1),
    });
  }
  return list;
}

// 排得最高的、已经载入的播放器。
function topOf(list) {
  let top = null;
  for (const p of list) {
    if (p.loaded && (!top || p.rank > top.rank)) {
      top = p;
    }
  }
  return top;
}

// 有排得比 top 低的播放器在用（预览开着、预告片在放）。
function contested(list, top) {
  return !!top && list.some((p) => p !== top && p.busy && p.rank < top.rank);
}

// 这一批里写了标题时整批一起定：true 不交，false 交；没写标题时返回 null，一项一项定。
function decide(ops, list) {
  let meta = null;
  for (const op of ops) {
    if (op.kind === 'metadata') {
      meta = op;
    }
  }
  if (!meta) {
    return null;
  }
  if (!meta.value) {
    // 清空（播放器销毁时）：还有排得低的在用时算它的。
    return contested(list, topOf(list));
  }
  let writer = null;
  for (const p of list) {
    if (p.title === meta.value.title && (!writer || p.rank < writer.rank)) {
      writer = p;
    }
  }
  if (!writer) {
    return false;
  }
  return list.some((p) => p !== writer && p.loaded && p.rank > writer.rank);
}

function dropLone(op, list) {
  const top = topOf(list);
  if (!contested(list, top)) {
    return false;
  }
  if (op.kind === 'state') {
    return op.value !== (top.video.paused ? 'paused' : 'playing');
  }
  return op.kind === 'handler' || op.kind === 'position';
}

function keep(op) {
  if (op.kind === 'metadata') {
    apply(metadataProp.set, session, [op.value]);
  } else if (op.kind === 'state') {
    apply(stateProp.set, session, [op.value]);
  } else if (op.kind === 'handler') {
    keptHandlers.set(op.action, op.handler);
  } else if (op.kind === 'position') {
    keptPosition = op.args;
  }
}

// 标题、播放状态还没交出去，不用管；按钮、进度改回去。
function revert(op) {
  if (op.kind === 'handler') {
    apply(setHandler, session, [op.action, keptHandlers.get(op.action) || null]);
  } else if (op.kind === 'position') {
    apply(setPosition, session, keptPosition);
  }
}

function flush() {
  const ops = pending;
  pending = null;
  let list = null;
  let verdict = null;
  try {
    list = players();
    verdict = decide(ops, list);
  } catch (e) {
    list = null;
  }
  for (const op of ops) {
    let drop = false;
    try {
      if (list) {
        drop = verdict !== null ? verdict : dropLone(op, list);
      }
    } catch (e) {
      drop = false;
    }
    try {
      if (drop) {
        revert(op);
      } else {
        keep(op);
      }
    } catch (e) {
    }
  }
}

Object.defineProperty(proto, 'metadata', {
  configurable: true,
  enumerable: metadataProp.enumerable,
  get: function() {
    return this === session ? pageMetadata : apply(metadataProp.get, this, []);
  },
  set: function(value) {
    if (value === undefined) {
      value = null;
    }
    if (this !== session || (value !== null && !(value instanceof MediaMetadata))) {
      // 原来的 setter：类型不对时抛同样的错。
      apply(metadataProp.set, this, [value]);
      return;
    }
    pageMetadata = value;
    queue({kind: 'metadata', value: value});
  },
});

Object.defineProperty(proto, 'playbackState', {
  configurable: true,
  enumerable: stateProp.enumerable,
  get: function() {
    return this === session ? pageState : apply(stateProp.get, this, []);
  },
  set: function(value) {
    if (this !== session) {
      apply(stateProp.set, this, [value]);
      return;
    }
    // 同原来的：先转成字符串，不认识的值不理。
    const state = String(value);
    if (STATES.indexOf(state) < 0) {
      return;
    }
    pageState = state;
    queue({kind: 'state', value: state});
  },
});

proto.setActionHandler = function setActionHandler(action, handler) {
  const result = apply(setHandler, this, arguments);
  const name = String(action);
  if (this === session && ACTIONS.indexOf(name) >= 0) {
    queue({kind: 'handler', action: name, handler: handler || null});
  }
  return result;
};

proto.setPositionState = function setPositionState(state) {
  const result = apply(setPosition, this, arguments);
  if (this === session) {
    queue({kind: 'position', args: Array.prototype.slice.call(arguments)});
  }
  return result;
};
})();
