// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_UPDATER_RELEASE_SOURCES_H_
#define STARRY_BROWSER_UPDATER_RELEASE_SOURCES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "starry/browser/updater/semver.h"
#include "url/gurl.h"

namespace starry::updater {

// 一条访问 GitHub Releases 的通道：用户自定义镜像、直连 GitHub 或内置国内加速节点。
struct UpdateRoute {
  enum class Kind { kCustom, kDirect, kMirror };

  Kind kind = Kind::kDirect;
  // 持久化用的稳定标识："custom"、"direct" 或镜像前缀。
  std::string id;
  // 仅 kCustom / kMirror 非空；支持前缀形式（"https://gh-proxy.org/"）或模板
  // （含 "{url}" / "{path}"）。
  std::string prefix;
  // 用于界面与日志展示的简短主机名（如 "github.com"、"gh-proxy.org"）。
  std::string display_host;
};

struct ReleaseAsset {
  ReleaseAsset();
  ReleaseAsset(const ReleaseAsset&);
  ReleaseAsset& operator=(const ReleaseAsset&);
  ~ReleaseAsset();

  std::string name;
  int64_t size = 0;
  GURL download_url;
  // GitHub API 提供的 sha256 摘要（若存在，64 位小写十六进制）。
  std::string sha256;
};

struct ReleaseInfo {
  ReleaseInfo();
  ReleaseInfo(const ReleaseInfo&);
  ReleaseInfo& operator=(const ReleaseInfo&);
  ~ReleaseInfo();

  std::string tag_name;
  SemVer version;
  std::string title;
  std::string body;
  GURL html_url;
  std::string published_at;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;

  const ReleaseAsset* FindAsset(std::string_view name) const;
};

// 发版仓库（默认取 GN 参数 starry_update_repo，可由 --starry-update-repo 覆盖）。
std::string UpdateRepo();

// 标准 GitHub Releases URL。
GURL ReleasePageUrl(std::string_view tag = {});
GURL DirectApiLatestUrl();
GURL DirectWebLatestUrl();
GURL DirectAssetUrl(std::string_view tag, std::string_view filename);

// 内置国内镜像列表（含 GN 参数 starry_update_mirrors 指定的条目）。
std::vector<std::string> BuiltInMirrors();

// 构造检查更新 / 下载更新的通道顺序。
// - 检查更新：自定义镜像 → 直连（优先，自动复用系统代理）→ 上次成功的镜像 → 其余内置镜像。
// - 下载更新：自定义镜像 → 本次检查成功的通道 → 其余镜像 → 直连（若未在前面）。
std::vector<UpdateRoute> BuildCheckRoutes(std::string_view custom_mirror,
                                          std::string_view last_working_route);
std::vector<UpdateRoute> BuildDownloadRoutes(
    std::string_view custom_mirror,
    std::string_view winning_check_route,
    std::string_view last_working_route);

// 通过镜像重写 github.com URL；要求输入与输出均为 HTTPS。
GURL ApplyMirror(std::string_view mirror_prefix, const GURL& github_url);

// 校验并规整用户输入的自定义镜像地址：空串表示清除；无效或非 HTTPS 返回 nullopt。
std::optional<std::string> SanitizeMirrorInput(std::string_view raw);

// 解析 GitHub REST API `/repos/<repo>/releases/latest` 的 JSON 响应。
std::optional<ReleaseInfo> ParseGitHubReleaseApiJson(std::string_view json);

// 从 `https://github.com/<repo>/releases/latest`（或经镜像代理后的响应）中提取最新 tag：
// 支持 JSON（Accept: application/json）、重定向后的最终 URL 路径以及 HTML 页面。
std::optional<std::string> ExtractTagFromWebLatest(const GURL& final_url,
                                                   std::string_view body);

// 解析 scripts/release.sh 生成的 `.sha256` 文件（`<64-hex>  <filename>`），
// 校验文件名与十六进制格式，返回 64 位小写 SHA-256。
std::optional<std::string> ParseSha256File(std::string_view content,
                                           std::string_view expected_filename);

// 校验是否为合法的 64 位十六进制 SHA-256 并转为小写。
std::optional<std::string> NormalizeSha256(std::string_view hex);

}  // namespace starry::updater

#endif  // STARRY_BROWSER_UPDATER_RELEASE_SOURCES_H_
