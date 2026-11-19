// Copyright 2026 The Starry Browser Authors
//
// 按界面语言格式化日期、时间、文件大小和时长。

import {L, locale} from './l10n.js';

const timeFormat = new Intl.DateTimeFormat(locale, {hour: '2-digit', minute: '2-digit', hourCycle: 'h23'});
const dayFormat = new Intl.DateTimeFormat(locale, {month: 'long', day: 'numeric', weekday: 'long'});
const dayYearFormat =
    new Intl.DateTimeFormat(locale, {year: 'numeric', month: 'long', day: 'numeric', weekday: 'long'});
const shortDayFormat = new Intl.DateTimeFormat(locale, {month: 'short', day: 'numeric'});
const shortDayYearFormat = new Intl.DateTimeFormat(locale, {year: 'numeric', month: 'short', day: 'numeric'});
const monthYearFormat = new Intl.DateTimeFormat(locale, {year: 'numeric', month: 'long'});
const weekdayFormat = new Intl.DateTimeFormat(locale, {weekday: 'narrow'});

export function startOfDay(time) {
  const date = new Date(time);
  date.setHours(0, 0, 0, 0);
  return date.getTime();
}

// 「09:05」。
export function timeLabel(time) {
  return timeFormat.format(new Date(time));
}

// 「11月3日 09:05」（下载详情的开始时间）。
const dateTimeFormat = new Intl.DateTimeFormat(
    locale, {month: 'long', day: 'numeric', hour: '2-digit', minute: '2-digit', hourCycle: 'h23'});

export function dateTimeLabel(time) {
  return dateTimeFormat.format(new Date(time));
}

// 今天、昨天，其余是「10月30日星期四」，不是今年的加上年份。
export function dayLabel(day, now = Date.now()) {
  const today = startOfDay(now);
  if (day === today) {
    return L('library.day.today');
  }
  const yesterday = startOfDay(today - 12 * 3600 * 1000);
  if (day === yesterday) {
    return L('library.day.yesterday');
  }
  const date = new Date(day);
  const sameYear = date.getFullYear() === new Date(now).getFullYear();
  return (sameYear ? dayFormat : dayYearFormat).format(date);
}

// 简短日期（时间线目录、活动条提示）：今天、昨天，其余是「10月30日」。
export function shortDayLabel(day, now = Date.now()) {
  const today = startOfDay(now);
  if (day === today) {
    return L('library.day.today');
  }
  const yesterday = startOfDay(today - 12 * 3600 * 1000);
  if (day === yesterday) {
    return L('library.day.yesterday');
  }
  const date = new Date(day);
  const sameYear = date.getFullYear() === new Date(now).getFullYear();
  return (sameYear ? shortDayFormat : shortDayYearFormat).format(date);
}

export function monthYearLabel(year, month) {
  return monthYearFormat.format(new Date(year, month, 1));
}

export function weekdayLabels() {
  // 以已知的星期日为基准，依次取周日到周六的简写。
  return Array.from({length: 7}, (_, index) => weekdayFormat.format(new Date(2026, 10, 1 + index)));
}

// 网址的显示：去掉协议和末尾的斜杠。
export function displayUrl(url) {
  try {
    const parsed = new URL(url);
    if (parsed.protocol === 'http:' || parsed.protocol === 'https:') {
      let text = parsed.host + parsed.pathname + parsed.search;
      if (text.endsWith('/')) {
        text = text.slice(0, -1);
      }
      return text;
    }
  } catch (e) {
    // 不是网址，原样显示。
  }
  return url;
}

// 文件大小（按十进制 1000 进位）：「512 字节」「12 KB」「1.2 MB」「3.45 GB」。
export function bytes(count) {
  const value = Math.max(0, count || 0);
  if (value < 1000) {
    return L('downloads.size.bytes', {count: Math.round(value)});
  }
  const units = ['KB', 'MB', 'GB', 'TB'];
  let scaled = value / 1000;
  let unit = 0;
  while (scaled >= 1000 && unit < units.length - 1) {
    scaled /= 1000;
    unit++;
  }
  const digits = unit === 0 ? 0 : unit === 1 ? 1 : 2;
  return `${scaled.toFixed(digits)} ${units[unit]}`;
}

export function speed(bytesPerSecond) {
  return `${bytes(bytesPerSecond)}/s`;
}

// 格式化时长，例如「3 秒」「2 分钟」「1 小时 5 分」。
export function duration(seconds) {
  const total = Math.ceil(seconds);
  if (total < 60) {
    return L('downloads.duration.seconds', {count: Math.max(total, 1)});
  }
  if (total < 3600) {
    return L('downloads.duration.minutes', {count: Math.floor((total + 30) / 60)});
  }
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  return minutes === 0 ? L('downloads.duration.hours', {count: hours}) :
                        L('downloads.duration.hoursMinutes', {hours, minutes});
}

// 替换模板中的 {key} 占位符。
export function fill(template, values) {
  return template.replace(/\{(\w+)\}/g, (_, key) => String(values[key] ?? ''));
}
