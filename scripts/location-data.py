#!/usr/bin/env python3
# Copyright 2026 The Starry Browser Authors
"""生成位置模拟选点页的离线数据（starry/browser/resources/location/）：

  cities.tsv      人口一万五以上的城市（GeoNames cities15000，CC BY 4.0）：坐标、国家、
                  IANA 时区和五种界面语言的名字。离线搜索、点地图时取最近城市的名字和时区。
  countries.json  国家轮廓（Natural Earth 1:50m，公有领域）：在线地图载不出来时的底图。

用法：scripts/location-data.py <下载目录>
下载目录里要有（只读数据，不执行其中任何东西）：
  https://download.geonames.org/export/dump/cities15000.zip
  https://download.geonames.org/export/dump/alternateNamesV2.zip
  https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_admin_0_countries.geojson

cities.tsv 的格式：第一行是 # 开头的时区表（制表符分隔），之后每行一个城市：
  纬度 经度 国家 时区序号 人口 名字 英文 简体中文 繁体中文 日文 韩文
和「名字」相同的译名留空。按人口从多到少排。
"""

import io
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'starry', 'browser', 'resources', 'location')

# 界面语言 → GeoNames 的 isolanguage，按优先顺序。
LANGUAGES = [
    ('en', ['en']),
    ('zh-Hans', ['zh-CN', 'zh-Hans', 'zh-SG', 'zh']),
    ('zh-Hant', ['zh-TW', 'zh-Hant', 'zh-HK', 'zh']),
    ('ja', ['ja']),
    ('ko', ['ko']),
]
WANTED = {code for _, codes in LANGUAGES for code in codes}


def clean(text):
    return text.replace('\t', ' ').replace('\n', ' ').strip()


def read_cities(directory):
    cities = {}
    with zipfile.ZipFile(os.path.join(directory, 'cities15000.zip')) as archive:
        with archive.open('cities15000.txt') as raw:
            for line in io.TextIOWrapper(raw, encoding='utf-8'):
                f = line.rstrip('\n').split('\t')
                if len(f) < 19 or not f[17]:
                    continue
                cities[f[0]] = {
                    'name': clean(f[1]),
                    'ascii': clean(f[2]),
                    'lat': float(f[4]),
                    'lng': float(f[5]),
                    'cc': f[8],
                    'pop': int(f[14] or 0),
                    'tz': f[17],
                }
    return cities


def read_names(directory, ids):
    # geonameid → isolanguage → [(rank, name)]
    names = {}
    with zipfile.ZipFile(os.path.join(directory, 'alternateNamesV2.zip')) as archive:
        with archive.open('alternateNamesV2.txt') as raw:
            for line in io.TextIOWrapper(raw, encoding='utf-8'):
                f = line.rstrip('\n').split('\t')
                if len(f) < 8 or f[1] not in ids or f[2] not in WANTED:
                    continue
                if f[6] == '1' or f[7] == '1':  # colloquial / historic
                    continue
                preferred = f[4] == '1'
                short = f[5] == '1'
                rank = (0 if preferred else 1, 0 if short else 1)
                names.setdefault(f[1], {}).setdefault(f[2], []).append((rank, clean(f[3])))
    return names


def pick(by_language, codes):
    for code in codes:
        candidates = by_language.get(code)
        if candidates:
            return sorted(candidates, key=lambda item: item[0])[0][1]
    return ''


# GeoNames 的中日韩名字有的带行政区划的后缀（重庆市、湛江市、충칭 시），有的不带：
# 统一去掉，和地图上的地名一致。只剩一个字的不动。
def strip_suffix(language, name):
    suffixes = {'zh-Hans': ['市'], 'zh-Hant': ['市'], 'ja': ['市'], 'ko': [' 시', '시']}
    for suffix in suffixes.get(language, []):
        if name.endswith(suffix) and len(name) - len(suffix) >= 2:
            return name[:-len(suffix)].strip()
    return name


def write_cities(cities, names):
    zones = sorted({city['tz'] for city in cities.values()})
    zone_index = {zone: index for index, zone in enumerate(zones)}
    rows = []
    for geoname_id, city in cities.items():
        by_language = names.get(geoname_id, {})
        localized = []
        for language, codes in LANGUAGES:
            name = strip_suffix(language, pick(by_language, codes))
            if language == 'en' and not name:
                name = city['ascii']
            localized.append('' if name == city['name'] else name)
        rows.append((city['pop'], '\t'.join([
            '%.4f' % city['lat'], '%.4f' % city['lng'], city['cc'],
            str(zone_index[city['tz']]), str(city['pop']), city['name'], *localized,
        ])))
    rows.sort(key=lambda row: -row[0])
    with open(os.path.join(OUT, 'cities.tsv'), 'w', encoding='utf-8') as out:
        out.write('#' + '\t'.join(zones) + '\n')
        for _, row in rows:
            out.write(row + '\n')
    return len(rows)


def quantize_ring(ring):
    result = []
    for lng, lat in ring:
        point = [round(lng, 2), round(lat, 2)]
        if not result or result[-1] != point:
            result.append(point)
    return result if len(result) >= 4 else None


def write_countries(directory):
    with open(os.path.join(directory, 'ne_50m_admin_0_countries.geojson'), encoding='utf-8') as f:
        source = json.load(f)
    features = []
    for feature in source['features']:
        props = feature['properties']
        iso = props.get('ISO_A2_EH') or props.get('ISO_A2') or ''
        geometry = feature['geometry']
        polygons = geometry['coordinates'] if geometry['type'] == 'MultiPolygon' else [
            geometry['coordinates']]
        out = []
        for polygon in polygons:
            rings = [r for r in (quantize_ring(ring) for ring in polygon) if r]
            if rings:
                out.append(rings)
        if out:
            features.append({'type': 'Feature', 'properties': {'iso': '' if iso == '-99' else iso},
                             'geometry': {'type': 'MultiPolygon', 'coordinates': out}})
    with open(os.path.join(OUT, 'countries.json'), 'w', encoding='utf-8') as out:
        json.dump({'type': 'FeatureCollection', 'features': features}, out,
                  separators=(',', ':'), ensure_ascii=False)
    return len(features)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    directory = sys.argv[1]
    os.makedirs(OUT, exist_ok=True)
    cities = read_cities(directory)
    names = read_names(directory, set(cities))
    print('cities: %d' % write_cities(cities, names))
    print('countries: %d' % write_countries(directory))


if __name__ == '__main__':
    main()
