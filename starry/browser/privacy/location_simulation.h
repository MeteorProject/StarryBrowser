// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_PRIVACY_LOCATION_SIMULATION_H_
#define STARRY_BROWSER_PRIVACY_LOCATION_SIMULATION_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/callback_list.h"
#include "base/functional/callback_forward.h"
#include "base/values.h"

class GURL;
class Profile;

namespace starry {

inline constexpr double kDefaultLocationAccuracy = 50;

struct SimulatedLocation {
  double latitude = 0;
  double longitude = 0;
  // 米。
  double accuracy = kDefaultLocationAccuracy;
  // IANA 名字；空字符串或无效时用「网页时区」的设置。
  std::string time_zone;
  // 给人看的名字，例如「东京 · 新宿」。
  std::string label;
};

enum class SiteLocationMode {
  kDefault,    // 跟随全局
  kReal,       // 真实位置（全局模拟时的例外）
  kSimulated,  // 模拟到 location
};

struct SiteLocationRule {
  SiteLocationMode mode = SiteLocationMode::kDefault;
  SimulatedLocation location;
};

// 一个顶层网站看到的位置。simulated 为 false：真实位置（Chromium 原来的流程）。
// 要模拟、但还没有有效位置（全局模拟没选点）时 location 为空：报位置不可用，
// 不回退到真实位置。
struct LocationDecision {
  bool simulated = false;
  std::optional<SimulatedLocation> location;
};

bool IsLocationSimulationMode(std::string_view mode);
LocationDecision ResolveLocation(Profile* profile, const GURL& top_url);
// 还不知道是哪个网站时（空闲的网页进程）：全局的结果。
LocationDecision ResolveDefaultLocation(Profile* profile);

std::optional<SimulatedLocation> GetGlobalSimulatedLocation(Profile* profile);
void SetGlobalSimulatedLocation(Profile* profile,
                                const SimulatedLocation& location);

// 设置页和选点页输入的主机名或 HTTP(S) 网址。无效的网站返回空字符串。
std::string NormalizeLocationSiteInput(std::string_view input);

// site 用 NormalizeFingerprintSite 规范化（主站点）。无痕的用户资料先看内存里的。
SiteLocationRule GetSiteLocationRule(Profile* profile, std::string_view site);
// kDefault 删除这一条。
void SetSiteLocationRule(Profile* profile,
                         std::string_view site,
                         const SiteLocationRule& rule);
base::DictValue LocationToValue(const SimulatedLocation& location);
// 无效的经纬度返回 nullopt；精度超出范围时用默认值，无效时区清空。
std::optional<SimulatedLocation> LocationFromValue(
    const base::DictValue& value);

void AddLocationSimulationSettings(Profile* profile, base::DictValue& state);

// 全局或任何网站的设置变了（含无痕的内存设置）。在 UI 线程调用。
base::CallbackListSubscription AddLocationSimulationObserver(
    base::RepeatingClosure callback);

// 浏览器客户端创建时调用一次：把规则装进 //content 的定位接管。
void RegisterLocationSimulation();

}  // namespace starry

#endif  // STARRY_BROWSER_PRIVACY_LOCATION_SIMULATION_H_
