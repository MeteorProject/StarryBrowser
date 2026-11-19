// Copyright 2026 The Starry Browser Authors
//
// 书签页（starry://bookmarks）：支持搜索、编辑、删除与拖拽排序，也支持拖入外部链接创建新书签。
// 按文件夹分组显示，仅有一个文件夹时隐藏分组标题。数据来源见
// starry/browser/ui/webui/bookmarks_handler.h。

import {html, LitElement, nothing, repeat} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {addWebUiListener, copyText, faviconUrl, openUrl, sendWithPromise, showMenu, targetFromEvent} from './common/browser.js';
import {displayUrl} from './common/format.js';
import {L, platform} from './common/l10n.js';
import {emptyState, icon, libraryHeader, revealHeader, searchField} from './common/library.js';
import {animateRows, captureRows, fadeOutRow, Motion, reduceMotion, reveal} from './common/motion.js';

// 页面里拖动书签时带的类型（值是书签的 id），和别处拖来的链接分开。
const DRAG_TYPE = 'application/x-starry-bookmark';

function hostOf(url) {
  try {
    const host = new URL(url).host;
    return host.startsWith('www.') ? host.slice(4) : host;
  } catch (e) {
    return url;
  }
}

function titleOf(bookmark) {
  return bookmark.title || hostOf(bookmark.url);
}

// 拖进来的是不是链接：别的网页、地址栏、书签页自己。
function draggedUrl(dataTransfer) {
  const list = dataTransfer.getData('text/uri-list');
  const url = (list.split(/\r?\n/).find((line) => line && !line.startsWith('#')) ||
               dataTransfer.getData('text/plain') || '').trim();
  return /^(https?|file):\/\//i.test(url) ? url : '';
}

function acceptsDrag(dataTransfer) {
  const types = Array.from(dataTransfer.types);
  return types.includes(DRAG_TYPE) || types.includes('text/uri-list');
}

class StarryBookmarks extends LitElement {
  static properties = {
    folders_: {state: true},
    query_: {state: true},
    loaded_: {state: true},
    editing_: {state: true},
    dropTarget_: {state: true},
    draggingId_: {state: true},
  };

  constructor() {
    super();
    this.folders_ = [];
    this.query_ = '';
    this.loaded_ = false;
    // 正在编辑的：{bookmark, top, left, title, address, error, saving}。
    this.editing_ = null;
    // 拖到哪里：{before: id} 或 {folder: id}（放到那组的最后）。
    this.dropTarget_ = null;
    this.draggingId_ = null;
  }

  createRenderRoot() {
    return this;
  }

  async connectedCallback() {
    super.connectedCallback();
    addWebUiListener('bookmarks-changed', (snapshot) => this.apply_(snapshot, true));
    const snapshot = await sendWithPromise('getBookmarks');
    await this.apply_(snapshot, false);
    reveal(this.querySelector('.bookmark-list'), {delay: 0.16, distance: 8});
    reveal(this.querySelector('.empty-state'), {delay: 0.16, distance: 8});
  }

  firstUpdated() {
    revealHeader(this);
  }

  async apply_(snapshot, animated) {
    const list = this.querySelector('.bookmark-list');
    const before = animated && list ? captureRows(list) : null;
    this.folders_ = snapshot.folders;
    this.loaded_ = snapshot.loaded;
    // 正在编辑的书签被别处删了：关掉浮层。
    if (this.editing_ && !this.all_.some((bookmark) => bookmark.id === this.editing_.bookmark.id)) {
      this.editing_ = null;
    }
    await this.updateComplete;
    if (before) {
      animateRows(this.querySelector('.bookmark-list'), before);
    }
  }

  // 全部书签，按显示的顺序，每个带上它所在的文件夹。
  get all_() {
    return this.folders_.flatMap(
        (folder) => folder.items.map((bookmark) => ({...bookmark, folder: folder.id, path: folder.path})));
  }

  // 每个词都要出现在名称或网址里，不分大小写。
  get filtered_() {
    const terms = this.query_.toLowerCase().split(/\s+/).filter(Boolean);
    return this.all_.filter((bookmark) => {
      const haystack = `${bookmark.title} ${displayUrl(bookmark.url)}`.toLowerCase();
      return terms.every((term) => haystack.includes(term));
    });
  }

  get subtitle_() {
    if (!this.loaded_) {
      return '';
    }
    if (this.query_) {
      const count = this.filtered_.length;
      return count === 0 ? L('bookmarks.subtitle.noMatches') : L('bookmarks.subtitle.found', {count});
    }
    const count = this.all_.length;
    return count === 0 ? L('bookmarks.subtitle.empty') : L('bookmarks.subtitle.total', {count});
  }

  // ---- 操作 ----

  async remove_(bookmark, row) {
    if (this.editing_ && this.editing_.bookmark.id === bookmark.id) {
      this.editing_ = null;
    }
    await fadeOutRow(row);
    const list = this.querySelector('.bookmark-list');
    const before = captureRows(list);
    this.folders_ = this.folders_
        .map((folder) => ({...folder, items: folder.items.filter((item) => item.id !== bookmark.id)}))
        .filter((folder) => folder.items.length);
    await this.updateComplete;
    animateRows(list, before);
    // 删完会收到 bookmarks-changed，那时和 Chrome 的书签对上。
    chrome.send('removeBookmark', [bookmark.id]);
  }

  onRowClick_(event, bookmark) {
    if (event.target.closest('.icon-button')) {
      return;
    }
    event.preventDefault();
    openUrl(bookmark.url, targetFromEvent(event));
  }

  async showRowMenu_(event, bookmark) {
    event.preventDefault();
    const row = event.currentTarget;
    const id = await showMenu([
      {id: 'open', title: L('common.open'), icon: 'arrow.up.forward'},
      {id: 'openTab', title: L('library.menu.openInNewTab'), icon: 'plus.square.on.square'},
      {id: 'openWindow', title: L('library.menu.openInNewWindow'), icon: 'macwindow'},
      {separator: true},
      {id: 'copy', title: L('library.menu.copyLink'), icon: 'link'},
      {id: 'edit', title: L('bookmarks.row.editMenu'), icon: 'pencil'},
      {separator: true},
      {id: 'remove', title: L('bookmarks.row.delete'), icon: 'trash'},
    ], {event});
    switch (id) {
      case 'open':
        openUrl(bookmark.url, 'current');
        break;
      case 'openTab':
        openUrl(bookmark.url, 'foreground');
        break;
      case 'openWindow':
        openUrl(bookmark.url, 'window');
        break;
      case 'copy':
        copyText(bookmark.url);
        break;
      case 'edit':
        this.edit_(bookmark, row);
        break;
      case 'remove':
        this.remove_(bookmark, row);
        break;
    }
  }

  // ---- 编辑 ----

  // 浮层放在这一行下面，下面放不下时放上面；横向和行的右边对齐。
  edit_(bookmark, row) {
    const rect = row.getBoundingClientRect();
    const width = 340;
    const height = 230;
    const below = rect.bottom + 6 + height <= innerHeight || rect.top - 6 - height < 0;
    const top = (below ? rect.bottom + 6 : rect.top - 6 - height) + scrollY;
    const left = Math.max(12, Math.min(rect.right - width, innerWidth - width - 12)) + scrollX;
    this.editing_ = {bookmark, top, left, below, title: bookmark.title, address: bookmark.url, error: '', saving: false};
    this.updateComplete.then(() => {
      const editor = this.querySelector('.bookmark-editor');
      if (!editor) {
        return;
      }
      if (!reduceMotion()) {
        editor.animate(
            [{opacity: 0, transform: `translateY(${below ? -4 : 4}px) scale(0.97)`}, {opacity: 1, transform: 'none'}],
            {duration: Motion.popover.duration, easing: Motion.popover.easing});
      }
      const field = editor.querySelector('input');
      field.focus();
      field.select();
    });
  }

  closeEditor_() {
    this.editing_ = null;
  }

  async saveEditor_() {
    const editing = this.editing_;
    if (!editing || editing.saving || !editing.address.trim()) {
      return;
    }
    this.editing_ = {...editing, saving: true};
    const result = await sendWithPromise('updateBookmark', editing.bookmark.id, editing.title, editing.address);
    if (this.editing_ === null || this.editing_.bookmark.id !== editing.bookmark.id) {
      return;
    }
    if (result.ok) {
      this.editing_ = null;
      return;
    }
    this.editing_ = {
      ...this.editing_,
      saving: false,
      error: result.error === 'invalid' ? L('bookmarks.editor.error.invalidURL') : L('bookmarks.editor.error.gone'),
    };
  }

  onEditorKey_(event) {
    if (event.key === 'Escape') {
      event.preventDefault();
      this.closeEditor_();
    } else if (event.key === 'Enter' && !event.isComposing) {
      event.preventDefault();
      this.saveEditor_();
    }
  }

  renderEditor_() {
    const editing = this.editing_;
    if (!editing) {
      return nothing;
    }
    const update = (key) => (event) => {
      this.editing_ = {...this.editing_, [key]: event.target.value, error: ''};
    };
    return html`
      <div class="editor-scrim" @mousedown=${() => this.closeEditor_()}></div>
      <div class="bookmark-editor" style="top: ${editing.top}px; left: ${editing.left}px"
          @keydown=${(event) => this.onEditorKey_(event)}>
        <h2>${L('bookmarks.editor.title')}</h2>
        <label class="editor-field">
          <span>${L('bookmarks.editor.name')}</span>
          <input type="text" .value=${editing.title} spellcheck="false" @input=${update('title')}>
        </label>
        <label class="editor-field">
          <span>${L('bookmarks.editor.address')}</span>
          <input type="text" .value=${editing.address} spellcheck="false" @input=${update('address')}>
        </label>
        ${editing.error ? html`<div class="editor-error">${editing.error}</div>` : nothing}
        <div class="editor-buttons">
          <button class="pill-button" @click=${() => this.closeEditor_()}>${L('common.cancel')}</button>
          <button class="filled-pill" ?disabled=${editing.saving || !editing.address.trim()}
              @click=${() => this.saveEditor_()}>${L('bookmarks.editor.save')}</button>
        </div>
      </div>`;
  }

  // ---- 拖动 ----

  onDragStart_(event, bookmark) {
    this.draggingId_ = bookmark.id;
    event.dataTransfer.effectAllowed = 'copyMove';
    event.dataTransfer.setData(DRAG_TYPE, bookmark.id);
    event.dataTransfer.setData('text/uri-list', bookmark.url);
    event.dataTransfer.setData('text/plain', bookmark.url);
  }

  onDragEnd_() {
    this.draggingId_ = null;
    this.dropTarget_ = null;
  }

  // 拖到一行上：放到这行前面。
  onRowDragOver_(event, bookmark) {
    if (this.query_ || !acceptsDrag(event.dataTransfer)) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    event.dataTransfer.dropEffect = Array.from(event.dataTransfer.types).includes(DRAG_TYPE) ? 'move' : 'copy';
    if (this.dropTarget_?.before !== bookmark.id) {
      this.dropTarget_ = {before: bookmark.id};
    }
  }

  // 拖到一组下面的空白（或者整个列表下面）：放到那组的最后。
  onSectionDragOver_(event, folderId) {
    if (this.query_ || !acceptsDrag(event.dataTransfer)) {
      return;
    }
    event.preventDefault();
    event.stopPropagation();
    event.dataTransfer.dropEffect = Array.from(event.dataTransfer.types).includes(DRAG_TYPE) ? 'move' : 'copy';
    if (this.dropTarget_?.folder !== folderId) {
      this.dropTarget_ = {folder: folderId};
    }
  }

  onDragLeave_(event) {
    const list = this.querySelector('.bookmark-list');
    if (!event.relatedTarget || !list.contains(event.relatedTarget)) {
      this.dropTarget_ = null;
    }
  }

  // 拖进来的是已有书签就挪位置；是别处的链接就添加（已经有这个网址的书签时挪那个）。
  onDrop_(event) {
    const target = this.dropTarget_;
    this.dropTarget_ = null;
    if (!target || this.query_) {
      return;
    }
    event.preventDefault();
    const before = target.before ?? null;
    const folder = target.folder ?? null;
    const id = event.dataTransfer.getData(DRAG_TYPE);
    const url = draggedUrl(event.dataTransfer);
    const existing = id || this.all_.find((bookmark) => bookmark.url === url)?.id;
    if (existing) {
      if (existing !== before) {
        chrome.send('moveBookmark', [existing, before, folder]);
      }
    } else if (url) {
      chrome.send('addBookmark', [url, '', before, folder]);
    }
  }

  // ---- 显示 ----

  renderRow_(bookmark) {
    const editing = this.editing_?.bookmark.id === bookmark.id;
    const classes = ['row', 'bookmark-row'];
    if (editing) {
      classes.push('editing');
    }
    if (this.draggingId_ === bookmark.id) {
      classes.push('dragging');
    }
    if (this.dropTarget_?.before === bookmark.id) {
      classes.push('drop-before');
    }
    return html`
      <a class=${classes.join(' ')} href=${bookmark.url} data-key=${bookmark.id}
          title=${this.query_ && bookmark.path ? `${displayUrl(bookmark.url)}\n${bookmark.path}` : displayUrl(bookmark.url)}
          draggable="true"
          @click=${(event) => this.onRowClick_(event, bookmark)}
          @auxclick=${(event) => event.button === 1 && this.onRowClick_(event, bookmark)}
          @contextmenu=${(event) => this.showRowMenu_(event, bookmark)}
          @dragstart=${(event) => this.onDragStart_(event, bookmark)}
          @dragend=${() => this.onDragEnd_()}
          @dragover=${(event) => this.onRowDragOver_(event, bookmark)}>
        <img class="favicon" src=${faviconUrl(bookmark.url)} alt="" draggable="false">
        <span class="bookmark-text">
          <span class="bookmark-title ellipsis">${titleOf(bookmark)}</span>
          <span class="bookmark-url ellipsis">${displayUrl(bookmark.url)}</span>
        </span>
        <span class="bookmark-actions reveal-on-hover">
          <button class="icon-button ${editing ? 'active' : ''}" title=${L('bookmarks.row.edit.help')}
              @click=${(event) => {
                event.preventDefault();
                this.edit_(bookmark, event.currentTarget.closest('.row'));
              }}>
            ${icon('pencil', 11)}
          </button>
          <button class="icon-button" title=${L('bookmarks.row.delete.help')}
              @click=${(event) => {
                event.preventDefault();
                this.remove_(bookmark, event.currentTarget.closest('.row'));
              }}>
            ${icon('xmark', 10)}
          </button>
        </span>
      </a>`;
  }

  renderList_() {
    if (this.query_) {
      return repeat(this.filtered_, (bookmark) => bookmark.id, (bookmark) => this.renderRow_(bookmark));
    }
    // 只有一组时不显示组名。
    const headers = this.folders_.length > 1;
    return repeat(this.folders_, (folder) => folder.id, (folder) => html`
      <section class="folder-section ${this.dropTarget_?.folder === folder.id ? 'drop-end' : ''}"
          data-key=${'folder-' + folder.id}
          @dragover=${(event) => this.onSectionDragOver_(event, folder.id)}>
        ${headers ? html`<div class="section-header">${icon('folder', 13)}<span>${folder.path}</span></div>` :
                    nothing}
        ${repeat(folder.items.map((item) => ({...item, folder: folder.id, path: folder.path})),
                 (bookmark) => bookmark.id, (bookmark) => this.renderRow_(bookmark))}
      </section>`);
  }

  renderEmpty_() {
    if (this.query_) {
      return emptyState({iconName: 'magnifyingglass', title: L('library.search.noResults', {query: this.query_})});
    }
    return emptyState({iconName: 'star', title: L('bookmarks.empty.title'), detail: platform === 'mac' ? L('bookmarks.empty.detail') : L('bookmarks.empty.detailCtrl')});
  }

  render() {
    const header = libraryHeader({
      title: L('common.bookmarks'),
      subtitle: this.subtitle_,
      search: searchField({
        value: this.query_,
        placeholder: L('bookmarks.search.placeholder'),
        onInput: (value) => {
          this.query_ = value;
        },
      }),
      actions: nothing,
    });
    const shown = this.query_ ? this.filtered_.length : this.all_.length;
    const lastFolder = this.folders_.length ? this.folders_[this.folders_.length - 1].id : null;
    return html`
      <div class="page">
        ${header}
        ${this.loaded_ && !shown ? this.renderEmpty_() : nothing}
        <div class="bookmark-list"
            @dragover=${(event) => this.onSectionDragOver_(event, lastFolder)}
            @dragleave=${(event) => this.onDragLeave_(event)}
            @drop=${(event) => this.onDrop_(event)}>
          ${this.renderList_()}
        </div>
      </div>
      ${this.renderEditor_()}`;
  }
}

customElements.define('starry-bookmarks', StarryBookmarks);
