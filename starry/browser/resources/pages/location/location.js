// Copyright 2026 The Starry Browser Authors

import {html, LitElement, nothing} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {send, sendWithPromise, showMenu} from './common/browser.js';
import {L, language, locale} from './common/l10n.js';
import {icon} from './common/library.js';
import * as maplibregl from './maplibre-gl.js';
import {mapStyle} from './map_style.js';

maplibregl.setWorkerUrl(new URL('maplibre-gl-worker.js', import.meta.url).href);

// WebUI 的网页不能自己发网络请求（会被结束进程）：地图要的 https 地址都改成 starrymap，
// 由浏览器去取（location_picker_handler.cc，只取 OpenFreeMap 的瓦片）。
const MAP_PROTOCOL = 'starrymap';

function decodeBase64(text) {
  if (Uint8Array.fromBase64) {
    return Uint8Array.fromBase64(text);
  }
  return Uint8Array.from(atob(text), (c) => c.charCodeAt(0));
}

maplibregl.addProtocol(MAP_PROTOCOL, async (params) => {
  const result = await sendWithPromise('fetchMapResource',
                                       params.url.replace(`${MAP_PROTOCOL}://`, 'https://'));
  if (result.error) {
    // HTTP 错误或取不到：MapLibre 报 error 事件，取不到时换离线底图。
    throw new maplibregl.AJAXError(result.status || 0, result.error, params.url, new Blob());
  }
  const bytes = decodeBase64(result.data);
  if (params.type === 'json') {
    return {data: JSON.parse(new TextDecoder().decode(bytes))};
  }
  if (params.type === 'string') {
    return {data: new TextDecoder().decode(bytes)};
  }
  return {data: bytes.buffer};
});

function transformRequest(url) {
  return {url: url.startsWith('https://') ? `${MAP_PROTOCOL}://${url.slice(8)}` : url};
}

// 离城市多远以内用它的名字；再远一些叫「…附近」；更远（海上）只写坐标。
const NAMED_KM = 40;
const NEAR_KM = 250;
const SEARCH_LIMIT = 8;
const ONLINE_TIMEOUT_MS = 6000;

const NAME_COLUMNS = {'en': 6, 'zh-Hans': 7, 'zh-Hant': 8, 'ja': 9, 'ko': 10};
const regionNames = new Intl.DisplayNames([locale], {type: 'region'});

function country(code) {
  try {
    return regionNames.of(code) || code;
  } catch (e) {
    return code;
  }
}

function fold(text) {
  return text.normalize('NFD').replace(/\p{M}/gu, '').toLowerCase();
}

// WebUI 的网页不能 fetch 自己的 chrome:// 地址：数据当作模块取（starry_pages.cc）。
async function loadCities() {
  const text = (await import('./cities.tsv.js')).default;
  const lines = text.split('\n');
  const zones = lines[0].slice(1).split('\t');
  const column = NAME_COLUMNS[language] ?? 6;
  // 简繁之间互相补，其余没有译名的用英文。
  const sibling = language === 'zh-Hans' ? 8 : language === 'zh-Hant' ? 7 : -1;
  const cities = [];
  for (let i = 1; i < lines.length; i++) {
    const f = lines[i].split('\t');
    if (f.length < 11) {
      continue;
    }
    const names = f.slice(5, 11).filter(Boolean);
    cities.push({
      lat: Number(f[0]),
      lng: Number(f[1]),
      cc: f[2],
      zone: zones[Number(f[3])],
      name: f[column] || (sibling > 0 && f[sibling]) || f[6] || f[5],
      keys: names.map(fold),
    });
  }
  return cities;
}

function distanceKm(lat1, lng1, lat2, lng2) {
  const rad = Math.PI / 180;
  const dLat = (lat2 - lat1) * rad;
  const dLng = (lng2 - lng1) * rad;
  const a = Math.sin(dLat / 2) ** 2 +
      Math.cos(lat1 * rad) * Math.cos(lat2 * rad) * Math.sin(dLng / 2) ** 2;
  return 12742 * Math.asin(Math.min(1, Math.sqrt(a)));
}

function wrapLng(lng) {
  return ((lng + 540) % 360) - 180;
}

function formatCoordinates(lat, lng) {
  const format = new Intl.NumberFormat(locale, {minimumFractionDigits: 4, maximumFractionDigits: 4});
  return `${format.format(lat)}, ${format.format(lng)}`;
}

// 「35.68, 139.76」「35.68 139.76」：直接输入的经纬度（中间用什么逗号、空格都行）。
function parseCoordinates(text) {
  if (/\p{L}/u.test(text)) {
    return null;
  }
  const parts = text.split(/[^\d.+-]+/).filter(Boolean);
  if (parts.length !== 2 || !parts.every((part) => /^[+-]?\d+(\.\d+)?$/.test(part))) {
    return null;
  }
  const lat = Number(parts[0]);
  const lng = Number(parts[1]);
  return Math.abs(lat) <= 90 && Math.abs(lng) <= 180 ? {lat, lng} : null;
}

function isDark() {
  return getComputedStyle(document.documentElement).getPropertyValue('--is-dark').trim() === '1';
}

class LocationPicker extends LitElement {
  static properties = {
    title_: {state: true},
    label_: {state: true},
    coordinates_: {state: true},
    zone_: {state: true},
    zoneManual_: {state: true},
    query_: {state: true},
    results_: {state: true},
    selected_: {state: true},
    moving_: {state: true},
    offline_: {state: true},
    ready_: {state: true},
  };

  constructor() {
    super();
    this.title_ = '';
    this.label_ = '';
    this.coordinates_ = '';
    this.zone_ = '';
    this.zoneManual_ = false;
    this.query_ = '';
    this.results_ = [];
    this.selected_ = 0;
    this.moving_ = false;
    this.offline_ = false;
    this.ready_ = false;
    this.cities_ = [];
    this.accuracy_ = 50;
    this.zoneTitles_ = {};
    this.zoneMenu_ = [];
  }

  // 地图要页面的 maplibre-gl.css，不用 shadow DOM。
  createRenderRoot() {
    return this;
  }

  async firstUpdated() {
    const [init, cities] = await Promise.all([sendWithPromise('getLocationPicker'), loadCities()]);
    if (!init) {
      return;
    }
    this.cities_ = cities;
    this.title_ = init.title;
    this.zoneMenu_ = init.timeZoneMenu || [];
    const collect = (items) => items.forEach((item) => {
      if (item.id) {
        this.zoneTitles_[item.id] = item.title;
      }
      collect(item.children || []);
    });
    collect(this.zoneMenu_);
    let center;
    let zoom;
    if (init.location) {
      center = [init.location.longitude, init.location.latitude];
      zoom = 11;
      this.accuracy_ = init.location.accuracy || this.accuracy_;
      if (init.location.timeZone) {
        this.zone_ = init.location.timeZone;
        this.zoneManual_ = true;
      }
    } else {
      // 没选过：从网页时区的最大城市开始（不读真实位置）。
      const zone = init.defaultZone || Intl.DateTimeFormat().resolvedOptions().timeZone;
      const city = cities.find((c) => c.zone === zone) || cities[0];
      center = [city.lng, city.lat];
      zoom = 4;
    }
    if (!navigator.onLine) {
      this.countries_ = (await import('./countries.json.js')).default;
    }
    this.createMap_(center, zoom, !navigator.onLine);
    this.updatePlace_(true, init.location && init.location.label);
    this.ready_ = true;
    this.querySelector('.search input')?.focus();
  }

  createMap_(center, zoom, offline) {
    this.offline_ = offline;
    this.map_ = new maplibregl.Map({
      container: this.querySelector('.map'),
      style: this.style_(),
      center,
      zoom,
      minZoom: 1,
      maxZoom: 18,
      attributionControl: false,
      dragRotate: false,
      pitchWithRotate: false,
      touchPitch: false,
      renderWorldCopies: true,
      fadeDuration: 150,
      transformRequest,
    });
    this.map_.touchZoomRotate.disableRotation();
    this.map_.keyboard.disableRotation();
    this.map_.on('movestart', () => {
      this.moving_ = true;
      this.closeResults_();
    });
    this.map_.on('move', () => this.updatePlace_(false));
    this.map_.on('moveend', () => {
      this.moving_ = false;
      this.updatePlace_(true);
    });
    this.map_.on('click', (event) => this.map_.easeTo({center: event.lngLat, duration: 450}));
    this.map_.on('error', (event) => {
      if (!this.offline_ && event.sourceId === 'omt') {
        this.goOffline_();
      }
    });
    if (!offline) {
      this.onlineTimer_ = setTimeout(() => {
        if (!this.map_.isSourceLoaded('omt')) {
          this.goOffline_();
        }
      }, ONLINE_TIMEOUT_MS);
    }
    matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
      this.map_.setStyle(this.style_());
    });
  }

  style_() {
    if (!this.offline_) {
      return mapStyle({dark: isDark(), language, online: true});
    }
    return mapStyle({dark: isDark(), language, online: false,
                     countries: this.countries_ || {type: 'FeatureCollection', features: []},
                     cities: this.cityPoints_()});
  }

  // 离线底图上的城市名：人口排序，前面的大些。
  cityPoints_() {
    if (!this.cityPointData_) {
      const count = this.cities_.length || 1;
      this.cityPointData_ = {
        type: 'FeatureCollection',
        features: this.cities_.map((city, index) => ({
          type: 'Feature',
          geometry: {type: 'Point', coordinates: [city.lng, city.lat]},
          properties: {name: city.name, rank: Math.min(1, index / Math.min(count, 2000))},
        })),
      };
    }
    return this.cityPointData_;
  }

  async goOffline_() {
    if (this.offline_) {
      return;
    }
    clearTimeout(this.onlineTimer_);
    this.offline_ = true;
    this.countries_ ??= (await import('./countries.json.js')).default;
    this.map_.setStyle(this.style_());
  }

  nearest_(lat, lng) {
    let best = null;
    let bestKm = Infinity;
    for (const city of this.cities_) {
      // 先用粗略的平面距离筛，近的才算球面距离。
      if (Math.abs(city.lat - lat) > 25 && best) {
        continue;
      }
      const km = distanceKm(lat, lng, city.lat, city.lng);
      if (km < bestKm) {
        best = city;
        bestKm = km;
      }
    }
    return {city: best, km: bestKm};
  }

  // final：地图停下来了，才取名字和时区（拖动时只更新坐标）。
  updatePlace_(final, label) {
    if (!this.map_) {
      return;
    }
    const center = this.map_.getCenter();
    const lat = center.lat;
    const lng = wrapLng(center.lng);
    this.coordinates_ = formatCoordinates(lat, lng);
    if (!final) {
      return;
    }
    // 动过以后，手动选的时区不再算数，按新位置重新取。
    if (this.lat_ !== undefined &&
        (Math.abs(lat - this.lat_) > 1e-7 || Math.abs(lng - this.lng_) > 1e-7)) {
      this.zoneManual_ = false;
    }
    this.lat_ = lat;
    this.lng_ = lng;
    const {city, km} = this.nearest_(lat, lng);
    if (label) {
      this.label_ = label;
    } else if (!city || km > NEAR_KM) {
      this.label_ = this.coordinates_;
    } else if (km > NAMED_KM) {
      this.label_ = L('location.picker.near', {place: city.name});
    } else {
      this.label_ = L('location.picker.place', {city: city.name, country: country(city.cc)});
    }
    if (!this.zoneManual_ && city) {
      this.zone_ = city.zone;
    }
  }

  search_(query) {
    this.query_ = query;
    this.selected_ = 0;
    const text = fold(query.trim());
    if (!text) {
      this.results_ = [];
      return;
    }
    const results = [];
    const point = parseCoordinates(query);
    if (point) {
      results.push({title: formatCoordinates(point.lat, point.lng),
                    detail: L('location.picker.coordinates'), lat: point.lat, lng: point.lng,
                    zoom: 12});
    }
    const contains = [];
    for (const city of this.cities_) {
      if (results.length >= SEARCH_LIMIT) {
        break;
      }
      if (city.keys.some((key) => key.startsWith(text))) {
        results.push(this.cityResult_(city));
      } else if (contains.length < SEARCH_LIMIT && text.length > 1 &&
                 city.keys.some((key) => key.includes(text))) {
        contains.push(this.cityResult_(city));
      }
    }
    this.results_ = [...results, ...contains].slice(0, SEARCH_LIMIT);
  }

  cityResult_(city) {
    return {title: city.name, detail: country(city.cc), lat: city.lat, lng: city.lng, zoom: 11};
  }

  choose_(result) {
    if (!result) {
      return;
    }
    this.closeResults_();
    this.query_ = '';
    this.zoneManual_ = false;
    this.map_.flyTo({center: [result.lng, result.lat], zoom: Math.max(result.zoom, 4),
                     speed: 2.2, curve: 1.42, maxDuration: 3000});
  }

  closeResults_() {
    this.results_ = [];
  }

  onSearchKey_(event) {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      const count = this.results_.length;
      if (count) {
        this.selected_ = (this.selected_ + (event.key === 'ArrowDown' ? 1 : count - 1)) % count;
      }
    } else if (event.key === 'Enter') {
      event.preventDefault();
      this.choose_(this.results_[this.selected_]);
    } else if (event.key === 'Escape' && (this.query_ || this.results_.length)) {
      event.stopPropagation();
      this.query_ = '';
      this.closeResults_();
    }
  }

  async chooseZone_(event) {
    const items = [
      {id: '', title: L('location.picker.timeZoneAuto'), checked: !this.zoneManual_},
      {separator: true},
      ...this.zoneMenu_,
    ];
    const mark = (list) => list.map((item) => ({
      ...item,
      ...(item.id !== undefined && item.id !== '' && !item.children ?
          {checked: this.zoneManual_ && item.id === this.zone_} : {}),
      ...(item.children ? {children: mark(item.children)} : {}),
    }));
    const id = await showMenu(mark(items), {element: event.currentTarget});
    if (id === null || id === undefined) {
      return;
    }
    if (id === '') {
      this.zoneManual_ = false;
      this.updatePlace_(true);
    } else {
      this.zone_ = id;
      this.zoneManual_ = true;
    }
  }

  zoneTitle_() {
    if (!this.zone_) {
      return '';
    }
    return this.zoneTitles_[this.zone_] || this.zone_.split('/').pop().replace(/_/g, ' ');
  }

  confirm_() {
    if (this.moving_ || this.lat_ === undefined) {
      return;
    }
    send('pickLocation', {
      latitude: Math.round(this.lat_ * 1e6) / 1e6,
      longitude: Math.round(this.lng_ * 1e6) / 1e6,
      accuracy: this.accuracy_,
      timeZone: this.zone_,
      label: this.label_,
    });
  }

  cancel_() {
    send('cancelLocationPicker');
  }

  connectedCallback() {
    super.connectedCallback();
    this.onKeyDown = (event) => {
      if (event.key === 'Escape') {
        this.cancel_();
      }
    };
    window.addEventListener('keydown', this.onKeyDown);
  }

  disconnectedCallback() {
    window.removeEventListener('keydown', this.onKeyDown);
    super.disconnectedCallback();
  }

  zoom_(delta) {
    this.map_?.zoomTo(Math.round(this.map_.getZoom()) + delta, {duration: 250});
  }

  render() {
    return html`
      <div class="map"></div>
      <div class="pin ${this.moving_ ? 'moving' : ''}" aria-hidden="true">
        <svg viewBox="0 0 30 40" width="30" height="40">
          <path class="pin-head" d="M15 1.5C7.5 1.5 1.5 7.4 1.5 14.8c0 9.6 11.4 21 12.2 21.8a1.8 1.8 0 0 0 2.6 0c.8-.8 12.2-12.2 12.2-21.8C28.5 7.4 22.5 1.5 15 1.5Z"/>
          <circle class="pin-dot" cx="15" cy="14.6" r="5"/>
        </svg>
        <span class="pin-shadow"></span>
      </div>
      <div class="top">
        <div class="search glass ${this.query_ ? 'has-text' : ''}">
          ${icon('magnifyingglass', 13)}
          <input type="search" spellcheck="false" autocomplete="off"
              placeholder=${L('location.picker.search')} aria-label=${L('location.picker.search')}
              .value=${this.query_} ?disabled=${!this.ready_}
              @input=${(event) => this.search_(event.target.value)}
              @keydown=${(event) => this.onSearchKey_(event)}>
        </div>
        ${this.offline_ ? html`<span class="chip glass">${L('location.picker.offline')}</span>` : nothing}
        <span class="spacer"></span>
        <div class="zoom glass">
          <button class="zoom-button" title=${L('location.picker.zoomIn')} @click=${() => this.zoom_(1)}>${icon('plus', 12)}</button>
          <button class="zoom-button" title=${L('location.picker.zoomOut')} @click=${() => this.zoom_(-1)}>
            <span class="icon"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-linecap="round"><path d="M2.75 8h10.5" stroke-width="1.75"/></svg></span>
          </button>
        </div>
      </div>
      ${this.results_.length || (this.query_.trim() && this.ready_) ? html`
        <div class="results glass" role="listbox">
          ${this.results_.length ? this.results_.map((result, index) => html`
            <button class="result ${index === this.selected_ ? 'selected' : ''}" role="option"
                @mouseenter=${() => this.selected_ = index}
                @click=${() => this.choose_(result)}>
              <span class="result-title">${result.title}</span>
              <span class="result-detail">${result.detail}</span>
            </button>`) : html`<div class="no-results">${L('location.picker.noResults')}</div>`}
        </div>` : nothing}
      <div class="card glass">
        <div class="place">
          <div class="eyebrow">${this.title_}</div>
          <div class="place-title">${this.label_ || ' '}</div>
          <div class="place-detail">
            <span class="coordinates">${this.coordinates_}</span>
            ${this.zone_ ? html`
              <span class="dot">·</span>
              <button class="zone" @click=${(event) => this.chooseZone_(event)}>
                ${L('location.picker.timeZone', {zone: this.zoneTitle_()})}
                ${icon('chevron.up.chevron.down', 8)}
              </button>` : nothing}
          </div>
        </div>
        <div class="actions">
          <button class="pill-button" @click=${() => this.cancel_()}>${L('common.cancel')}</button>
          <button class="filled-pill" ?disabled=${!this.ready_ || this.moving_}
              @click=${() => this.confirm_()}>${L('location.picker.use')}</button>
        </div>
      </div>
      <div class="attribution">${this.offline_ ? 'Natural Earth · GeoNames' :
          html`<a>© OpenFreeMap</a> <a>© OpenMapTiles</a> <a>© OpenStreetMap</a>`}</div>`;
  }
}

customElements.define('starry-location-picker', LocationPicker);
