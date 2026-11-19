// Copyright 2026 The Starry Browser Authors
//
// 动效参数，与 Views 界面的 motion.h 保持一致。弹簧曲线由阻尼谐振解析解采样 64 个点
// 生成 CSS / Web Animations 的 linear() 缓动，时长取位移衰减到千分之一以内的时刻。

function spring(response, dampingFraction) {
  const omega = (2 * Math.PI) / response;
  const zeta = dampingFraction;
  // 从 1 动到 0 的位移（初速度为零）。
  const displacement = (t) => {
    if (zeta < 1) {
      const damped = omega * Math.sqrt(1 - zeta * zeta);
      return Math.exp(-zeta * omega * t) *
          (Math.cos(damped * t) + ((zeta * omega) / damped) * Math.sin(damped * t));
    }
    return (1 + omega * t) * Math.exp(-omega * t);
  };
  let settle = 0;
  for (let t = 0; t < 4; t += 1 / 240) {
    if (Math.abs(displacement(t)) > 0.001) {
      settle = t;
    }
  }
  const duration = settle + 1 / 240;
  const samples = 64;
  const points = [];
  for (let i = 0; i <= samples; i++) {
    const progress = i === samples ? 1 : 1 - displacement((duration * i) / samples);
    points.push(progress.toFixed(4));
  }
  return {duration: Math.round(duration * 1000), easing: `linear(${points.join(', ')})`};
}

function timed(seconds, x1, y1, x2, y2) {
  return {duration: Math.round(seconds * 1000), easing: `cubic-bezier(${x1}, ${y1}, ${x2}, ${y2})`};
}

const easeOut = (seconds) => timed(seconds, 0, 0, 0.58, 1);

export const Motion = {
  reveal: spring(0.55, 0.86),
  listEdit: spring(0.42, 0.86),
  lift: spring(0.35, 0.78),
  searchResize: spring(0.3, 0.9),
  selection: spring(0.36, 0.8),
  fold: spring(0.34, 0.88),
  // AI 助手气泡入场与列表项动效。
  bubbleIn: spring(0.34, 0.78),
  item: spring(0.42, 0.86),
  // 自动隐藏侧栏滑出与标签预览卡片动效（与 motion.h 一致），用于外观设置预览。
  sidebarPeek: spring(0.21, 1),
  tabPreview: spring(0.26, 0.92),
  // 分类与筛选切换动效。
  tab: timed(0.36, 0.16, 1, 0.3, 1),
  popover: timed(0.2, 0.16, 1, 0.3, 1),
  hover: easeOut(0.18),
  progress: easeOut(0.25),
  fadeOutFast: easeOut(0.1),
  settingsSearch: easeOut(0.15),
  staggerStep: 28,
  staggerLimit: 14,
};

export function reduceMotion() {
  return matchMedia('(prefers-reduced-motion: reduce)').matches;
}

// CSS 里用的几条曲线（例如搜索框变宽）。
{
  const style = document.documentElement.style;
  for (const name of ['searchResize', 'listEdit', 'lift', 'selection', 'fold', 'tab', 'sidebarPeek', 'tabPreview']) {
    const kebab = name.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
    style.setProperty(`--ease-${kebab}`, Motion[name].easing);
    style.setProperty(`--duration-${kebab}`, `${Motion[name].duration}ms`);
  }
}

// 元素入场动效：淡入并自下方 distance 像素处上移。
export function reveal(element, {delay = 0, distance = 10, scale = 1} = {}) {
  if (!element) {
    return;
  }
  const from = reduceMotion() ? 'none' : `translateY(${distance}px) scale(${scale})`;
  element.animate(
      [{opacity: 0, transform: from}, {opacity: 1, transform: 'none'}],
      {duration: Motion.reveal.duration, easing: Motion.reveal.easing,
       delay: delay * 1000, fill: 'backwards'});
}

// 列表变了（删了几行、插进来几行）：先记下每行的位置，改完以后从原来的位置滑过去（FLIP），
// 新的行淡入。行用 data-key 认。
export function captureRows(container) {
  const rects = new Map();
  for (const row of container.querySelectorAll('[data-key]')) {
    rects.set(row.dataset.key, row.getBoundingClientRect());
  }
  return rects;
}

export function animateRows(container, before, motion = Motion.listEdit) {
  if (reduceMotion()) {
    return;
  }
  for (const row of container.querySelectorAll('[data-key]')) {
    const old = before.get(row.dataset.key);
    const now = row.getBoundingClientRect();
    if (!old) {
      row.animate([{opacity: 0, transform: 'scale(0.98)'}, {opacity: 1, transform: 'none'}],
                  {duration: motion.duration, easing: motion.easing});
      continue;
    }
    const dy = old.top - now.top;
    if (Math.abs(dy) > 0.5) {
      row.animate([{transform: `translateY(${dy}px)`}, {transform: 'none'}],
                  {duration: motion.duration, easing: motion.easing});
    }
  }
}

// 列表行退场动效：微缩至 0.98 并淡出。
export function fadeOutRow(row) {
  if (!row || reduceMotion()) {
    return Promise.resolve();
  }
  return row.animate([{opacity: 1, transform: 'none'}, {opacity: 0, transform: 'scale(0.98)'}],
                     {duration: 180, easing: 'ease-out', fill: 'forwards'}).finished;
}
