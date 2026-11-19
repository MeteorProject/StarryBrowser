// Copyright 2026 The Starry Browser Authors
//
// 助手回答的 Markdown 渲染：支持段落、标题、列表、代码块、引用、表格，以及粗体、斜体、
// 行内代码和链接。所有文本先做 HTML 转义；链接不直接跳转，由页面交由浏览器在新标签中打开。

function escape(text) {
  return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
}

function safeUrl(url) {
  try {
    const parsed = new URL(url);
    return parsed.protocol === 'http:' || parsed.protocol === 'https:' ? parsed.href : null;
  } catch {
    return null;
  }
}

// 行内：先把代码段拿出来（里面不再处理），再处理链接、粗体、斜体。
function inline(text) {
  const codes = [];
  let out = text.replace(/`([^`\n]+)`/g, (_, code) => {
    codes.push(`<code>${escape(code)}</code>`);
    return `\u0000${codes.length - 1}\u0000`;
  });
  out = escape(out);
  out = out.replace(/\[([^\]\n]+)\]\(([^)\s]+)\)/g, (match, label, url) => {
    const href = safeUrl(url.replace(/&amp;/g, '&'));
    return href ? `<a href="#" data-url="${escape(href)}">${label}</a>` : match;
  });
  out = out.replace(/(^|[\s(])(https?:\/\/[^\s<)]+)/g, (match, lead, url) => {
    const href = safeUrl(url.replace(/&amp;/g, '&'));
    return href ? `${lead}<a href="#" data-url="${escape(href)}">${url}</a>` : match;
  });
  out = out.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
  out = out.replace(/__([^_\n]+)__/g, '<strong>$1</strong>');
  out = out.replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, '$1<em>$2</em>');
  out = out.replace(/~~([^~\n]+)~~/g, '<del>$1</del>');
  return out.replace(/\u0000(\d+)\u0000/g, (_, index) => codes[Number(index)]);
}

function isTableRow(line) {
  return /^\s*\|.*\|\s*$/.test(line);
}

function cells(line) {
  return line.trim().replace(/^\||\|$/g, '').split('|').map((cell) => cell.trim());
}

export function renderMarkdown(source) {
  const lines = source.replace(/\r\n?/g, '\n').split('\n');
  const out = [];
  let index = 0;
  let paragraph = [];
  const flush = () => {
    if (paragraph.length) {
      out.push(`<p>${paragraph.map(inline).join('<br>')}</p>`);
      paragraph = [];
    }
  };
  while (index < lines.length) {
    const line = lines[index];
    // 代码块（没写完的也先画出来）。
    const fence = /^\s*(```|~~~)\s*([\w+-]*)\s*$/.exec(line);
    if (fence) {
      flush();
      const body = [];
      index++;
      while (index < lines.length && !new RegExp(`^\\s*${fence[1]}\\s*$`).test(lines[index])) {
        body.push(lines[index]);
        index++;
      }
      index++;
      // 悬停时右上角显示语言标签和复制按钮。
      out.push(`<div class="code"><div class="code-tools"><span class="code-lang">${escape(fence[2] || '')}</span>` +
               `<button class="copy-code" data-code="${escape(body.join('\n'))}"></button></div>` +
               `<pre><code>${escape(body.join('\n'))}</code></pre></div>`);
      continue;
    }
    if (!line.trim()) {
      flush();
      index++;
      continue;
    }
    const heading = /^(#{1,6})\s+(.*)$/.exec(line);
    if (heading) {
      flush();
      const level = Math.min(heading[1].length + 2, 6);
      out.push(`<h${level}>${inline(heading[2])}</h${level}>`);
      index++;
      continue;
    }
    if (/^\s*([-*_])\s*\1\s*\1[\s\1]*$/.test(line)) {
      flush();
      out.push('<hr>');
      index++;
      continue;
    }
    if (/^\s*>/.test(line)) {
      flush();
      const quote = [];
      while (index < lines.length && /^\s*>/.test(lines[index])) {
        quote.push(lines[index].replace(/^\s*>\s?/, ''));
        index++;
      }
      out.push(`<blockquote>${renderMarkdown(quote.join('\n'))}</blockquote>`);
      continue;
    }
    if (isTableRow(line) && index + 1 < lines.length && /^\s*\|?\s*:?-{2,}/.test(lines[index + 1])) {
      flush();
      const head = cells(line);
      index += 2;
      const rows = [];
      while (index < lines.length && isTableRow(lines[index])) {
        rows.push(cells(lines[index]));
        index++;
      }
      out.push('<div class="table"><table><thead><tr>' +
               head.map((cell) => `<th>${inline(cell)}</th>`).join('') + '</tr></thead><tbody>' +
               rows.map((row) => '<tr>' + row.map((cell) => `<td>${inline(cell)}</td>`).join('') + '</tr>').join('') +
               '</tbody></table></div>');
      continue;
    }
    const item = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(line);
    if (item) {
      flush();
      const ordered = /\d/.test(item[2]);
      const entries = [];
      while (index < lines.length) {
        const match = /^(\s*)([-*+]|\d+[.)])\s+(.*)$/.exec(lines[index]);
        if (match) {
          entries.push({indent: match[1].length, text: match[3]});
          index++;
        } else if (lines[index].trim() && /^\s{2,}/.test(lines[index]) && entries.length) {
          entries[entries.length - 1].text += '\n' + lines[index].trim();
          index++;
        } else {
          break;
        }
      }
      const tag = ordered ? 'ol' : 'ul';
      out.push(`<${tag}>` + entries.map((entry) =>
          `<li${entry.indent >= 2 ? ' class="nested"' : ''}>${entry.text.split('\n').map(inline).join('<br>')}</li>`)
          .join('') + `</${tag}>`);
      continue;
    }
    paragraph.push(line);
    index++;
  }
  flush();
  return out.join('');
}
