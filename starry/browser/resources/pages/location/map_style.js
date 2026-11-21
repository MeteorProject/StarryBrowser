// Copyright 2026 The Starry Browser Authors

const ONLINE_TILES = 'https://tiles.openfreemap.org/planet';

const LIGHT = {
  land: '#f3f2ef',
  water: '#bcd5ec',
  waterLine: '#a9c8e6',
  green: '#dfeacf',
  park: '#d4e6c4',
  ice: '#fbfcfd',
  residential: '#ecebe7',
  building: '#e3e1dc',
  buildingLine: '#d8d5cf',
  road: '#ffffff',
  roadMajor: '#ffffff',
  casing: '#d9d5cc',
  casingMajor: '#cfc8ba',
  rail: '#c9c5bd',
  boundary: '#a49cb3',
  boundaryState: '#c3bccd',
  label: '#34343a',
  labelMinor: '#5e5e66',
  labelWater: '#4f7598',
  halo: 'rgba(255, 255, 255, 0.92)',
  country: '#ebe9e4',
  countryLine: '#c8c3b8',
};

const DARK = {
  land: '#1e1f22',
  water: '#16263a',
  waterLine: '#1d3149',
  green: '#1f2a21',
  park: '#223024',
  ice: '#2a2d31',
  residential: '#232427',
  building: '#2b2c30',
  buildingLine: '#323338',
  road: '#38393e',
  roadMajor: '#45464c',
  casing: '#26272b',
  casingMajor: '#2c2d31',
  rail: '#3c3d42',
  boundary: '#6f6880',
  boundaryState: '#4d4858',
  label: '#e7e7ec',
  labelMinor: '#b4b4bc',
  labelWater: '#7fa2c4',
  halo: 'rgba(24, 25, 28, 0.9)',
  country: '#26272b',
  countryLine: '#3d3e44',
};

// 地名用界面语言，没有时用当地的写法。
function nameExpression(language) {
  const keys = {
    'zh-Hans': ['name:zh-Hans', 'name:zh', 'name:zh-Hant'],
    'zh-Hant': ['name:zh-Hant', 'name:zh', 'name:zh-Hans'],
    'ja': ['name:ja'],
    'ko': ['name:ko'],
    'en': ['name:en', 'name:latin'],
  }[language] || ['name:en', 'name:latin'];
  return ['coalesce', ...keys.map((key) => ['get', key]), ['get', 'name']];
}

const FONT = ['Sans Regular'];
const FONT_MEDIUM = ['Sans Medium'];
const FONT_BOLD = ['Sans Bold'];

function label(id, layer, filter, {font = FONT, size, color, minzoom, maxzoom, spacing,
                                    sortKey, text, placement}) {
  return {
    id, type: 'symbol', source: 'omt', 'source-layer': layer, filter,
    ...(minzoom === undefined ? {} : {minzoom}),
    ...(maxzoom === undefined ? {} : {maxzoom}),
    layout: {
      'text-field': text,
      'text-font': font,
      'text-size': size,
      'text-max-width': 8,
      'text-padding': 2,
      ...(spacing ? {'text-letter-spacing': spacing} : {}),
      ...(sortKey ? {'symbol-sort-key': sortKey} : {}),
      ...(placement ? {'symbol-placement': placement} : {}),
    },
    paint: {'text-color': color, 'text-halo-color': 'HALO', 'text-halo-width': 1.4,
            'text-halo-blur': 0.4},
  };
}

// zoom 只能在最外层的 interpolate 里；干道的倍数放在每一档的输出里。
const ROAD_WIDTH = (scale, factor) => ['interpolate', ['exponential', 1.5], ['zoom'],
  5, ['*', factor, 0.4 * scale], 10, ['*', factor, 1.2 * scale],
  14, ['*', factor, 4 * scale], 18, ['*', factor, 18 * scale]];

function onlineLayers(c, name) {
  const cls = ['get', 'class'];
  const major = ['match', cls, ['motorway', 'trunk', 'primary'], true, false];
  const roads = ['all', ['match', cls,
    ['motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'minor', 'service'], true, false],
    ['!=', ['get', 'brunnel'], 'tunnel']];
  return [
    {id: 'ice', type: 'fill', source: 'omt', 'source-layer': 'landcover',
     filter: ['==', cls, 'ice'], paint: {'fill-color': c.ice}},
    {id: 'green', type: 'fill', source: 'omt', 'source-layer': 'landcover',
     filter: ['match', cls, ['wood', 'grass', 'wetland'], true, false],
     paint: {'fill-color': c.green, 'fill-opacity': ['interpolate', ['linear'], ['zoom'], 4, 0.5, 10, 1]}},
    {id: 'residential', type: 'fill', source: 'omt', 'source-layer': 'landuse', minzoom: 10,
     filter: ['match', cls, ['residential', 'suburb', 'neighbourhood'], true, false],
     paint: {'fill-color': c.residential}},
    {id: 'park', type: 'fill', source: 'omt', 'source-layer': 'park', paint: {'fill-color': c.park}},
    {id: 'water', type: 'fill', source: 'omt', 'source-layer': 'water',
     filter: ['!=', ['get', 'brunnel'], 'tunnel'], paint: {'fill-color': c.water}},
    {id: 'waterway', type: 'line', source: 'omt', 'source-layer': 'waterway', minzoom: 8,
     paint: {'line-color': c.waterLine,
             'line-width': ['interpolate', ['linear'], ['zoom'], 8, 0.5, 14, 2]}},
    {id: 'building', type: 'fill', source: 'omt', 'source-layer': 'building', minzoom: 14,
     paint: {'fill-color': c.building, 'fill-outline-color': c.buildingLine,
             'fill-opacity': ['interpolate', ['linear'], ['zoom'], 14, 0, 15, 1]}},
    {id: 'road-casing', type: 'line', source: 'omt', 'source-layer': 'transportation', minzoom: 6,
     filter: roads, layout: {'line-cap': 'round', 'line-join': 'round'},
     paint: {'line-color': ['case', major, c.casingMajor, c.casing],
             'line-width': ROAD_WIDTH(1, ['case', major, 1.6, 1.25])}},
    {id: 'road', type: 'line', source: 'omt', 'source-layer': 'transportation', minzoom: 6,
     filter: roads, layout: {'line-cap': 'round', 'line-join': 'round'},
     paint: {'line-color': ['case', major, c.roadMajor, c.road],
             'line-width': ROAD_WIDTH(0.82, ['case', major, 1.3, 1])}},
    {id: 'rail', type: 'line', source: 'omt', 'source-layer': 'transportation', minzoom: 11,
     filter: ['==', cls, 'rail'],
     paint: {'line-color': c.rail, 'line-width': 1, 'line-dasharray': [3, 2]}},
    {id: 'boundary-state', type: 'line', source: 'omt', 'source-layer': 'boundary', minzoom: 3,
     filter: ['all', ['==', ['get', 'admin_level'], 4], ['!=', ['get', 'maritime'], 1]],
     paint: {'line-color': c.boundaryState, 'line-width': 0.8, 'line-dasharray': [3, 2]}},
    {id: 'boundary-country', type: 'line', source: 'omt', 'source-layer': 'boundary',
     filter: ['all', ['==', ['get', 'admin_level'], 2], ['!=', ['get', 'maritime'], 1]],
     paint: {'line-color': c.boundary,
             'line-width': ['interpolate', ['linear'], ['zoom'], 1, 0.6, 6, 1.4, 12, 2]}},
    label('water-name', 'water_name', ['!=', ['get', 'class'], 'lake'], {
      text: name, size: ['interpolate', ['linear'], ['zoom'], 2, 11, 8, 13], color: c.labelWater,
      spacing: 0.1}),
    label('road-name', 'transportation_name', ['match', cls,
      ['motorway', 'trunk', 'primary', 'secondary', 'tertiary', 'minor'], true, false], {
      text: name, size: 11, color: c.labelMinor, minzoom: 13, placement: 'line'}),
    label('place-minor', 'place', ['match', cls,
      ['suburb', 'quarter', 'neighbourhood', 'village', 'hamlet'], true, false], {
      text: name, size: ['interpolate', ['linear'], ['zoom'], 10, 11, 15, 13], color: c.labelMinor,
      minzoom: 11, sortKey: ['get', 'rank']}),
    label('place-town', 'place', ['==', cls, 'town'], {
      text: name, size: ['interpolate', ['linear'], ['zoom'], 8, 11, 14, 14], color: c.label,
      minzoom: 8, sortKey: ['get', 'rank']}),
    label('place-city', 'place', ['==', cls, 'city'], {
      font: FONT_MEDIUM, text: name,
      size: ['interpolate', ['linear'], ['zoom'], 4, 11, 8, 14, 12, 18], color: c.label,
      minzoom: 3, sortKey: ['get', 'rank']}),
    label('place-state', 'place', ['==', cls, 'state'], {
      text: name, size: 11, color: c.labelMinor, minzoom: 4, maxzoom: 8, spacing: 0.08}),
    label('place-country', 'place', ['==', cls, 'country'], {
      font: FONT_BOLD, text: name,
      size: ['interpolate', ['linear'], ['zoom'], 1, 10, 4, 14, 6, 16], color: c.label,
      maxzoom: 8, sortKey: ['get', 'rank']}),
  ];
}

function offlineLayers(c) {
  return [
    {id: 'country', type: 'fill', source: 'countries', paint: {'fill-color': c.country}},
    {id: 'country-line', type: 'line', source: 'countries',
     paint: {'line-color': c.countryLine, 'line-width': 0.8}},
    {id: 'city', type: 'symbol', source: 'cities',
     layout: {
       'text-field': ['get', 'name'],
       'text-font': FONT,
       'text-size': ['interpolate', ['linear'], ['get', 'rank'], 0, 14, 1, 11],
       'text-max-width': 8,
       'text-padding': 3,
       'symbol-sort-key': ['get', 'rank'],
     },
     paint: {'text-color': c.label, 'text-halo-color': c.halo, 'text-halo-width': 1.4}},
  ];
}

// online 为 false 时用离线数据：countries（GeoJSON）和 cities（{name, rank} 的点）。
export function mapStyle({dark, language, online, countries, cities}) {
  const c = dark ? DARK : LIGHT;
  const sources = online ? {
    omt: {type: 'vector', url: ONLINE_TILES,
          attribution: '© OpenFreeMap © OpenMapTiles © OpenStreetMap'},
  } : {
    countries: {type: 'geojson', data: countries},
    cities: {type: 'geojson', data: cities},
  };
  const layers = [
    {id: 'background', type: 'background', paint: {'background-color': online ? c.land : c.water}},
    ...(online ? onlineLayers(c, nameExpression(language)) : offlineLayers(c)),
  ];
  for (const layer of layers) {
    if (layer.paint && layer.paint['text-halo-color'] === 'HALO') {
      layer.paint['text-halo-color'] = c.halo;
    }
  }
  return {version: 8, sources, layers};
}
