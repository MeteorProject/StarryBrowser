// Copyright 2026 The Starry Browser Authors

#include "starry/components/content_filter/redirect_resources.h"

#include <string>
#include <vector>

#include "base/no_destructor.h"

namespace starry::content_filter {

namespace {

// 透明的图片（scratchpad 里用 zlib 生成的，RGBA 全 0）。
constexpr char k1x1Gif[] =
    "\x47\x49\x46\x38\x39\x61\x01\x00\x01\x00\x80\x00\x00\x00\x00\x00"
    "\xff\xff\xff\x21\xf9\x04\x01\x00\x00\x00\x00\x2c\x00\x00\x00\x00"
    "\x01\x00\x01\x00\x00\x02\x02\x44\x01\x00\x3b";
constexpr char k2x2Png[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x02\x00\x00\x00\x02\x08\x06\x00\x00\x00\x72\xb6\x0d"
    "\x24\x00\x00\x00\x0b\x49\x44\x41\x54\x78\xda\x63\x60\x40\x07\x00"
    "\x00\x12\x00\x01\xe4\x55\x8d\xe7\x00\x00\x00\x00\x49\x45\x4e\x44"
    "\xae\x42\x60\x82";
constexpr char k3x2Png[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x03\x00\x00\x00\x02\x08\x06\x00\x00\x00\x9d\x74\x66"
    "\x1a\x00\x00\x00\x0b\x49\x44\x41\x54\x78\xda\x63\x60\xc0\x05\x00"
    "\x00\x1a\x00\x01\x2f\x98\x97\xa6\x00\x00\x00\x00\x49\x45\x4e\x44"
    "\xae\x42\x60\x82";
constexpr char k32x32Png[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52"
    "\x00\x00\x00\x20\x00\x00\x00\x20\x08\x06\x00\x00\x00\x73\x7a\x7a"
    "\xf4\x00\x00\x00\x1a\x49\x44\x41\x54\x78\xda\xed\xc1\x01\x01\x00"
    "\x00\x00\x82\x20\xff\xaf\x6e\x48\x40\x01\x00\x00\x00\xef\x06\x10"
    "\x20\x00\x01\xc9\xb5\xc3\xb1\x00\x00\x00\x00\x49\x45\x4e\x44\xae"
    "\x42\x60\x82";

constexpr char kNoopHtml[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body></body>"
    "</html>";
constexpr char kNoopJs[] = "(function() {})();";
constexpr char kNoopVmap[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<vmap:VMAP xmlns:vmap=\"http://www.iab.net/videosuite/vmap\" "
    "version=\"1.0\"></vmap:VMAP>";
constexpr char kNoopVast2[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"2.0\"></VAST>";
constexpr char kNoopVast3[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"3.0\"></VAST>";
constexpr char kNoopVast4[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><VAST version=\"4.0\"></VAST>";

// Google Analytics（analytics.js）：`ga()` 什么也不做，但调用 hitCallback 和就绪的
// 回调，结束「防闪烁」的隐藏（dataLayer.hide），网页等着统计发完再跳转的照样跳。
constexpr char kGoogleAnalytics[] = R"JS((function() {
  'use strict';
  var noop = function() {};
  var Tracker = function() {};
  Tracker.prototype.get = function() {};
  Tracker.prototype.set = noop;
  Tracker.prototype.send = noop;
  var callback = function(args) {
    var last = args[args.length - 1];
    try {
      if (last && typeof last === 'object' && typeof last.hitCallback === 'function') {
        last.hitCallback();
      } else if (typeof last === 'function') {
        last(new Tracker());
      }
    } catch (e) {}
  };
  var ga = function() { callback(arguments); };
  ga.create = function() { return new Tracker(); };
  ga.getByName = function() { return new Tracker(); };
  ga.getAll = function() { return []; };
  ga.remove = noop;
  ga.loaded = true;
  var name = window.GoogleAnalyticsObject || 'ga';
  var queue = window[name] && window[name].q;
  window[name] = ga;
  if (Array.isArray(queue)) {
    for (var i = 0; i < queue.length; i++) { callback(queue[i]); }
  }
  var dataLayer = window.dataLayer;
  if (dataLayer && dataLayer.hide && typeof dataLayer.hide.end === 'function') {
    try { dataLayer.hide.end(); } catch (e) {}
  }
})();
)JS";

// 旧版的 ga.js：`_gaq.push` 执行里面的函数，其余不做。
constexpr char kGoogleAnalyticsGa[] = R"JS((function() {
  'use strict';
  var noop = function() {};
  var tracker = new Proxy({}, { get: function() { return noop; } });
  var gat = { _createTracker: function() { return tracker; },
              _getTracker: function() { return tracker; },
              _getTrackerByName: function() { return tracker; },
              _anonymizeIp: noop, _forceSSL: noop };
  window._gat = gat;
  var queue = window._gaq;
  window._gaq = { push: function() {
    for (var i = 0; i < arguments.length; i++) {
      if (typeof arguments[i] === 'function') { try { arguments[i](); } catch (e) {} }
    }
    return 0;
  } };
  if (Array.isArray(queue)) { window._gaq.push.apply(window._gaq, queue); }
})();
)JS";

// Google Tag Manager：结束 dataLayer.hide，执行事件里的 eventCallback。
constexpr char kGoogleTagManager[] = R"JS((function() {
  'use strict';
  var noop = function() {};
  var w = window;
  w.ga = w.ga || noop;
  var dataLayer = w.dataLayer;
  if (!(dataLayer instanceof Object)) { return; }
  if (dataLayer.hide instanceof Object && typeof dataLayer.hide.end === 'function') {
    try { dataLayer.hide.end(); } catch (e) {}
    dataLayer.hide.end = noop;
  }
  var run = function(event) {
    if (event && typeof event.eventCallback === 'function') {
      var callback = event.eventCallback;
      event.eventCallback = noop;
      setTimeout(function() { try { callback(); } catch (e) {} }, 1);
    }
  };
  if (Array.isArray(dataLayer)) {
    for (var i = 0; i < dataLayer.length; i++) { run(dataLayer[i]); }
  }
  if (typeof dataLayer.push === 'function') {
    var push = dataLayer.push;
    dataLayer.push = function(event) { run(event); return push.apply(this, arguments); };
  }
})();
)JS";

// Google Publisher Tag（googletag）：定义广告位、显示都不做事，`cmd.push` 马上执行。
constexpr char kGooglePublisherTag[] = R"JS((function() {
  'use strict';
  var noop = function() {};
  var chain = function() { return this; };
  var empty = function() { return []; };
  var Slot = function(path, id) { this.path_ = path; this.id_ = id; };
  ['addService', 'clearCategoryExclusions', 'clearTargeting', 'defineSizeMapping',
   'set', 'setCategoryExclusion', 'setClickUrl', 'setCollapseEmptyDiv', 'setConfig',
   'setForceSafeFrame', 'setSafeFrameConfig', 'setTargeting',
   'updateTargetingFromMap'].forEach(function(name) { Slot.prototype[name] = chain; });
  Slot.prototype.get = function() { return null; };
  Slot.prototype.getAdUnitPath = function() { return this.path_; };
  Slot.prototype.getSlotElementId = function() { return this.id_; };
  Slot.prototype.getResponseInformation = function() { return null; };
  ['getAttributeKeys', 'getCategoryExclusions', 'getSizes', 'getTargeting',
   'getTargetingKeys'].forEach(function(name) { Slot.prototype[name] = empty; });
  var service = {};
  ['addEventListener', 'clear', 'clearCategoryExclusions', 'clearTagForChildDirectedTreatment',
   'clearTargeting', 'collapseEmptyDivs', 'disableInitialLoad', 'display',
   'enableAsyncRendering', 'enableLazyLoad', 'enableSingleRequest', 'enableSyncRendering',
   'enableVideoAds', 'refresh', 'removeEventListener', 'set', 'setCategoryExclusion',
   'setCentering', 'setCookieOptions', 'setForceSafeFrame', 'setLocation',
   'setPrivacySettings', 'setPublisherProvidedId', 'setRequestNonPersonalizedAds',
   'setSafeFrameConfig', 'setTagForChildDirectedTreatment', 'setTargeting',
   'setVideoContent', 'updateCorrelator'].forEach(function(name) { service[name] = chain; });
  service.get = function() { return null; };
  ['getAttributeKeys', 'getSlots', 'getTargeting', 'getTargetingKeys'].forEach(
      function(name) { service[name] = empty; });
  service.isInitialLoadDisabled = function() { return true; };
  var sizeMapping = function() {
    var builder = { addSize: function() { return builder; }, build: empty };
    return builder;
  };
  var gt = window.googletag || {};
  var queue = Array.isArray(gt.cmd) ? gt.cmd : [];
  gt.apiReady = true;
  gt.pubadsReady = true;
  gt.cmd = { push: function(fn) { try { fn.call(window); } catch (e) {} return 1; } };
  gt.companionAds = function() { return service; };
  gt.content = function() { return service; };
  gt.pubads = function() { return service; };
  gt.defineOutOfPageSlot = function(path, id) { return new Slot(path, id); };
  gt.defineSlot = function(path, size, id) { return new Slot(path, id); };
  gt.destroySlots = function() { return true; };
  gt.disablePublisherConsole = noop;
  gt.display = noop;
  gt.enableServices = noop;
  gt.getVersion = function() { return ''; };
  gt.setAdIframeTitle = noop;
  gt.setConfig = noop;
  gt.sizeMapping = sizeMapping;
  gt.enums = { OutOfPageFormat: {} };
  window.googletag = gt;
  for (var i = 0; i < queue.length; i++) {
    try { queue[i].call(window); } catch (e) {}
  }
})();
)JS";

// AdSense：push 不做事。
constexpr char kAdsByGoogle[] = R"JS((function() {
  'use strict';
  var existing = window.adsbygoogle;
  window.adsbygoogle = { loaded: true, push: function() {} };
  if (Array.isArray(existing)) { existing.length = 0; }
})();
)JS";

// Amazon 的 apstag：fetchBids 马上回一个空的结果。
constexpr char kAmazonApstag[] = R"JS((function() {
  'use strict';
  var noop = function() {};
  window.apstag = {
    _Q: [],
    fetchBids: function(config, callback) {
      if (typeof callback === 'function') {
        setTimeout(function() { try { callback([]); } catch (e) {} }, 1);
      }
    },
    init: noop,
    setDisplayBids: noop,
    targetingKeys: function() { return []; },
  };
})();
)JS";

// comScore 的 beacon。
constexpr char kScorecardBeacon[] = R"JS((function() {
  'use strict';
  window.COMSCORE = { beacon: function() {}, purge: function() { window._comscore = []; } };
})();
)JS";

std::string_view Bytes(const char* data, size_t size_with_terminator) {
  return std::string_view(data, size_with_terminator - 1);
}

// 几帧静音的 MP3（MPEG-1 Layer III、128 kbps、44.1 kHz、单声道，每帧 417 字节）。
const std::string& NoopMp3() {
  static const base::NoDestructor<std::string> mp3([] {
    std::string frame("\xff\xfb\x90\xc4", 4);
    frame.append(413, '\0');
    std::string result;
    for (int i = 0; i < 4; ++i) {
      result += frame;
    }
    return result;
  }());
  return *mp3;
}

const std::vector<RedirectResource>& Resources() {
  static const base::NoDestructor<std::vector<RedirectResource>> resources(
      std::vector<RedirectResource>{
          {"1x1.gif", "image/gif", Bytes(k1x1Gif, sizeof(k1x1Gif))},
          {"2x2.png", "image/png", Bytes(k2x2Png, sizeof(k2x2Png))},
          {"3x2.png", "image/png", Bytes(k3x2Png, sizeof(k3x2Png))},
          {"32x32.png", "image/png", Bytes(k32x32Png, sizeof(k32x32Png))},
          {"noop.html", "text/html", kNoopHtml},
          {"noop.js", "application/javascript", kNoopJs},
          {"noop.css", "text/css", ""},
          {"noop.txt", "text/plain", ""},
          {"noop.json", "application/json", "{}"},
          {"noop-0.1s.mp3", "audio/mpeg", NoopMp3()},
          {"noop-vmap1.xml", "application/xml", kNoopVmap},
          {"noop-vast2.xml", "application/xml", kNoopVast2},
          {"noop-vast3.xml", "application/xml", kNoopVast3},
          {"noop-vast4.xml", "application/xml", kNoopVast4},
          {"google-analytics_analytics.js", "application/javascript",
           kGoogleAnalytics},
          {"google-analytics_ga.js", "application/javascript",
           kGoogleAnalyticsGa},
          {"googletagmanager_gtm.js", "application/javascript",
           kGoogleTagManager},
          {"googletagservices_gpt.js", "application/javascript",
           kGooglePublisherTag},
          {"googlesyndication_adsbygoogle.js", "application/javascript",
           kAdsByGoogle},
          {"amazon_apstag.js", "application/javascript", kAmazonApstag},
          {"scorecardresearch_beacon.js", "application/javascript",
           kScorecardBeacon},
      });
  return *resources;
}

struct Alias {
  std::string_view alias;
  std::string_view name;
};

// uBlock Origin 的别名和 Adblock Plus 的 `abp-resource:` 名字。
constexpr Alias kAliases[] = {
    {"1x1-transparent.gif", "1x1.gif"},
    {"abp-resource:1x1-transparent-gif", "1x1.gif"},
    {"2x2-transparent.png", "2x2.png"},
    {"abp-resource:2x2-transparent-png", "2x2.png"},
    {"3x2-transparent.png", "3x2.png"},
    {"abp-resource:3x2-transparent-png", "3x2.png"},
    {"32x32-transparent.png", "32x32.png"},
    {"abp-resource:32x32-transparent-png", "32x32.png"},
    {"noopframe", "noop.html"},
    {"abp-resource:blank-html", "noop.html"},
    {"noopjs", "noop.js"},
    {"abp-resource:blank-js", "noop.js"},
    {"noopcss", "noop.css"},
    {"abp-resource:blank-css", "noop.css"},
    {"nooptext", "noop.txt"},
    {"abp-resource:blank-text", "noop.txt"},
    {"empty", "noop.txt"},
    {"noopjson", "noop.json"},
    {"noopmp3-0.1s", "noop-0.1s.mp3"},
    {"abp-resource:blank-mp3", "noop-0.1s.mp3"},
    {"noopvmap-1.0", "noop-vmap1.xml"},
    {"noop-vmap1.0.xml", "noop-vmap1.xml"},
    {"noopvast-2.0", "noop-vast2.xml"},
    {"noopvast-3.0", "noop-vast3.xml"},
    {"noopvast-4.0", "noop-vast4.xml"},
    {"google-analytics.com/analytics.js", "google-analytics_analytics.js"},
    {"googletagmanager_gtm.js", "googletagmanager_gtm.js"},
    {"googletagmanager.com/gtm.js", "googletagmanager_gtm.js"},
    {"google-analytics.com/ga.js", "google-analytics_ga.js"},
    {"googletagservices.com/gpt.js", "googletagservices_gpt.js"},
    {"googlesyndication.com/adsbygoogle.js",
     "googlesyndication_adsbygoogle.js"},
    {"amazon-adsystem.com/aax2/amzn_ads.js", "amazon_apstag.js"},
    {"scorecardresearch.com/beacon.js", "scorecardresearch_beacon.js"},
};

}  // namespace

const RedirectResource* FindRedirectResource(std::string_view name) {
  for (const RedirectResource& resource : Resources()) {
    if (resource.name == name) {
      return &resource;
    }
  }
  return nullptr;
}

std::string_view CanonicalRedirectName(std::string_view name) {
  // uBlock Origin 的 `名字:优先级`。
  if (const size_t colon = name.rfind(':');
      colon != std::string_view::npos && colon + 1 < name.size() &&
      !name.starts_with("abp-resource:")) {
    bool digits = true;
    for (char c : name.substr(colon + 1)) {
      digits &= (c >= '0' && c <= '9') || c == '-';
    }
    if (digits) {
      name = name.substr(0, colon);
    }
  }
  for (const Alias& alias : kAliases) {
    if (alias.alias == name) {
      return alias.name;
    }
  }
  if (const RedirectResource* resource = FindRedirectResource(name)) {
    return resource->name;
  }
  return std::string_view();
}

}  // namespace starry::content_filter
