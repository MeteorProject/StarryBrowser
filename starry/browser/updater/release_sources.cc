// Copyright 2026 The Starry Browser Authors

#include "starry/browser/updater/release_sources.h"

#include <algorithm>
#include <array>
#include <set>

#include "base/command_line.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace starry::updater {

namespace {

#if !defined(STARRY_UPDATE_REPO)
#define STARRY_UPDATE_REPO "MeteorProject/StarryBrowser"
#endif

#if !defined(STARRY_UPDATE_MIRRORS)
#define STARRY_UPDATE_MIRRORS ""
#endif

constexpr char kUpdateRepoSwitch[] = "starry-update-repo";
// 限制更新日志展示长度，避免异常大响应撑爆设置页。
constexpr size_t kMaxReleaseBodyLength = 8192;

// 内置面向中国大陆网络环境的 HTTPS GitHub Release 加速节点（按优先级排序）。
// 所有节点均配合 SHA-256 校验与跨镜像二次比对。
constexpr std::array<const char*, 5> kDefaultChinaMirrors = {
    "https://gh-proxy.org/",
    "https://hk.gh-proxy.org/",
    "https://ghfast.top/",
    "https://ghproxy.net/",
    "https://mirror.ghproxy.com/",
};

std::string HostOf(std::string_view url_str) {
  const GURL url(url_str);
  return url.is_valid() ? std::string(url.host()) : std::string(url_str);
}

UpdateRoute MakeMirrorRoute(std::string_view prefix, UpdateRoute::Kind kind) {
  UpdateRoute route;
  route.kind = kind;
  route.id = kind == UpdateRoute::Kind::kCustom ? "custom" : std::string(prefix);
  route.prefix = std::string(prefix);
  route.display_host = HostOf(prefix);
  return route;
}

UpdateRoute MakeDirectRoute() {
  UpdateRoute route;
  route.kind = UpdateRoute::Kind::kDirect;
  route.id = "direct";
  route.display_host = "github.com";
  return route;
}

bool IsValidRepoSlug(std::string_view repo) {
  const size_t slash = repo.find('/');
  if (slash == std::string_view::npos || slash == 0 ||
      slash + 1 >= repo.size() || repo.find('/', slash + 1) != std::string_view::npos) {
    return false;
  }
  for (char ch : repo) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                    ch == '.' || ch == '/';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ExtractTagAfterMarker(std::string_view text,
                                                 std::string_view marker) {
  size_t pos = 0;
  while ((pos = text.find(marker, pos)) != std::string_view::npos) {
    pos += marker.size();
    size_t end = pos;
    while (end < text.size()) {
      const char ch = text[end];
      if (ch == '/' || ch == '?' || ch == '#' || ch == '"' || ch == '\'' ||
          ch == '<' || ch == '>' || ch == '&' ||
          base::IsAsciiWhitespace(ch)) {
        break;
      }
      ++end;
    }
    const std::string_view candidate = text.substr(pos, end - pos);
    if (!candidate.empty() && ParseSemVer(candidate).has_value()) {
      return std::string(candidate);
    }
    pos = end;
  }
  return std::nullopt;
}

}  // namespace

ReleaseAsset::ReleaseAsset() = default;
ReleaseAsset::ReleaseAsset(const ReleaseAsset&) = default;
ReleaseAsset& ReleaseAsset::operator=(const ReleaseAsset&) = default;
ReleaseAsset::~ReleaseAsset() = default;

ReleaseInfo::ReleaseInfo() = default;
ReleaseInfo::ReleaseInfo(const ReleaseInfo&) = default;
ReleaseInfo& ReleaseInfo::operator=(const ReleaseInfo&) = default;
ReleaseInfo::~ReleaseInfo() = default;

const ReleaseAsset* ReleaseInfo::FindAsset(std::string_view name) const {
  for (const ReleaseAsset& asset : assets) {
    if (asset.name == name) {
      return &asset;
    }
  }
  return nullptr;
}

std::string UpdateRepo() {
  if (base::CommandLine::InitializedForCurrentProcess()) {
    const std::string override_repo =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            kUpdateRepoSwitch);
    if (IsValidRepoSlug(override_repo)) {
      return override_repo;
    }
  }
  return STARRY_UPDATE_REPO;
}

GURL ReleasePageUrl(std::string_view tag) {
  if (tag.empty()) {
    return GURL(base::StrCat({"https://github.com/", UpdateRepo(), "/releases"}));
  }
  return GURL(base::StrCat(
      {"https://github.com/", UpdateRepo(), "/releases/tag/", tag}));
}

GURL DirectApiLatestUrl() {
  return GURL(base::StrCat(
      {"https://api.github.com/repos/", UpdateRepo(), "/releases/latest"}));
}

GURL DirectWebLatestUrl() {
  return GURL(
      base::StrCat({"https://github.com/", UpdateRepo(), "/releases/latest"}));
}

GURL DirectAssetUrl(std::string_view tag, std::string_view filename) {
  return GURL(base::StrCat({"https://github.com/", UpdateRepo(),
                            "/releases/download/", tag, "/", filename}));
}

std::vector<std::string> BuiltInMirrors() {
  std::vector<std::string> mirrors;
  std::set<std::string> seen;
  const auto push = [&](std::string_view raw) {
    std::optional<std::string> clean = SanitizeMirrorInput(raw);
    if (clean && !clean->empty() && seen.insert(*clean).second) {
      mirrors.push_back(std::move(*clean));
    }
  };
  for (std::string_view extra : base::SplitStringPiece(
           STARRY_UPDATE_MIRRORS, ",", base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    push(extra);
  }
  for (const char* mirror : kDefaultChinaMirrors) {
    push(mirror);
  }
  return mirrors;
}

std::vector<UpdateRoute> BuildCheckRoutes(std::string_view custom_mirror,
                                          std::string_view last_working_route) {
  std::vector<UpdateRoute> routes;
  std::set<std::string> seen;

  if (std::optional<std::string> custom = SanitizeMirrorInput(custom_mirror);
      custom && !custom->empty()) {
    routes.push_back(MakeMirrorRoute(*custom, UpdateRoute::Kind::kCustom));
    seen.insert(*custom);
  }

  // 直连排在内置镜像之前：海外用户以及国内开了系统代理的用户一次命中，
  // 且能拿到完整的 GitHub Release 说明与原始 SHA-256。
  routes.push_back(MakeDirectRoute());
  seen.insert("direct");

  const std::vector<std::string> builtins = BuiltInMirrors();
  if (!last_working_route.empty() && last_working_route != "direct" &&
      last_working_route != "custom") {
    if (std::ranges::find(builtins, last_working_route) != builtins.end() &&
        seen.insert(std::string(last_working_route)).second) {
      routes.push_back(
          MakeMirrorRoute(last_working_route, UpdateRoute::Kind::kMirror));
    }
  }
  for (const std::string& mirror : builtins) {
    if (seen.insert(mirror).second) {
      routes.push_back(MakeMirrorRoute(mirror, UpdateRoute::Kind::kMirror));
    }
  }
  return routes;
}

std::vector<UpdateRoute> BuildDownloadRoutes(
    std::string_view custom_mirror,
    std::string_view winning_check_route,
    std::string_view last_working_route) {
  std::vector<UpdateRoute> routes;
  std::set<std::string> seen;

  const auto add_custom = [&]() {
    if (std::optional<std::string> custom = SanitizeMirrorInput(custom_mirror);
        custom && !custom->empty() && seen.insert("custom").second) {
      routes.push_back(MakeMirrorRoute(*custom, UpdateRoute::Kind::kCustom));
      seen.insert(*custom);
    }
  };
  const auto add_direct = [&]() {
    if (seen.insert("direct").second) {
      routes.push_back(MakeDirectRoute());
    }
  };
  const auto add_builtin = [&](std::string_view mirror) {
    if (seen.insert(std::string(mirror)).second) {
      routes.push_back(MakeMirrorRoute(mirror, UpdateRoute::Kind::kMirror));
    }
  };

  add_custom();

  const std::vector<std::string> builtins = BuiltInMirrors();
  // 如果刚刚检查更新是通过国内镜像成功的（说明直连 GitHub 受阻），下载时直接从该镜像
  // 起步，避免再次等待直连超时；若检查走的是直连，下载同样先走直连（遇限速再切镜像）。
  if (winning_check_route == "direct") {
    add_direct();
  } else if (!winning_check_route.empty() && winning_check_route != "custom" &&
             std::ranges::find(builtins, winning_check_route) !=
                 builtins.end()) {
    add_builtin(winning_check_route);
  }

  if (!last_working_route.empty() && last_working_route != "direct" &&
      last_working_route != "custom" &&
      std::ranges::find(builtins, last_working_route) != builtins.end()) {
    add_builtin(last_working_route);
  }

  // 若检查未明确指出镜像，直连优先于其余镜像。
  if (winning_check_route.empty() || winning_check_route == "custom") {
    add_direct();
  }

  for (const std::string& mirror : builtins) {
    add_builtin(mirror);
  }
  // 兜底补上直连（以防镜像均挂但用户刚好开启了代理）。
  add_direct();
  return routes;
}

GURL ApplyMirror(std::string_view mirror_prefix, const GURL& github_url) {
  if (!github_url.is_valid() || !github_url.SchemeIs(url::kHttpsScheme)) {
    return GURL();
  }
  const std::optional<std::string> clean = SanitizeMirrorInput(mirror_prefix);
  if (!clean || clean->empty()) {
    return GURL();
  }

  std::string result;
  if (clean->find("{url}") != std::string::npos) {
    result = *clean;
    base::ReplaceSubstringsAfterOffset(&result, 0, "{url}", github_url.spec());
  } else if (clean->find("{path}") != std::string::npos) {
    std::string_view path = github_url.path();
    if (path.starts_with("/")) {
      path.remove_prefix(1);
    }
    result = *clean;
    base::ReplaceSubstringsAfterOffset(&result, 0, "{path}", path);
  } else {
    result = *clean;
    if (!result.ends_with('/')) {
      result.push_back('/');
    }
    result.append(github_url.spec());
  }

  const GURL out(result);
  if (!out.is_valid() || !out.SchemeIs(url::kHttpsScheme)) {
    return GURL();
  }
  return out;
}

std::optional<std::string> SanitizeMirrorInput(std::string_view raw) {
  const std::string_view trimmed =
      base::TrimWhitespaceASCII(raw, base::TRIM_ALL);
  if (trimmed.empty()) {
    return std::string();
  }
  if (!base::StartsWith(trimmed, "https://",
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return std::nullopt;
  }
  // 模板形式（含 {url} 或 {path}）用占位替换后再校验 GURL 合法性。
  std::string probe(trimmed);
  base::ReplaceSubstringsAfterOffset(&probe, 0, "{url}",
                                     "https://github.com/a/b");
  base::ReplaceSubstringsAfterOffset(&probe, 0, "{path}", "a/b");
  const GURL parsed(probe);
  if (!parsed.is_valid() || !parsed.SchemeIs(url::kHttpsScheme) ||
      parsed.host().empty() || parsed.has_username() || parsed.has_password() ||
      parsed.has_ref()) {
    return std::nullopt;
  }
  std::string normalized(trimmed);
  const bool is_template = normalized.find("{url}") != std::string::npos ||
                           normalized.find("{path}") != std::string::npos;
  if (!is_template && !normalized.ends_with('/')) {
    normalized.push_back('/');
  }
  return normalized;
}

std::optional<std::string> NormalizeSha256(std::string_view hex) {
  std::string_view trimmed = base::TrimWhitespaceASCII(hex, base::TRIM_ALL);
  if (base::StartsWith(trimmed, "sha256:",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    trimmed.remove_prefix(7);
  }
  if (trimmed.size() != 64) {
    return std::nullopt;
  }
  std::string lower;
  lower.reserve(64);
  for (char ch : trimmed) {
    if (!base::IsHexDigit(ch)) {
      return std::nullopt;
    }
    lower.push_back(base::ToLowerASCII(ch));
  }
  return lower;
}

std::optional<ReleaseInfo> ParseGitHubReleaseApiJson(std::string_view json) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  const base::DictValue& root = parsed->GetDict();
  const std::string* tag = root.FindString("tag_name");
  if (!tag || tag->empty()) {
    return std::nullopt;
  }
  std::optional<SemVer> version = ParseSemVer(*tag);
  if (!version) {
    return std::nullopt;
  }

  ReleaseInfo info;
  info.tag_name = *tag;
  info.version = std::move(*version);
  info.draft = root.FindBool("draft").value_or(false);
  info.prerelease = root.FindBool("prerelease").value_or(false) ||
                    info.version.IsPrerelease();
  if (const std::string* name = root.FindString("name")) {
    info.title = *name;
  }
  if (const std::string* body = root.FindString("body")) {
    info.body = body->size() > kMaxReleaseBodyLength
                    ? body->substr(0, kMaxReleaseBodyLength)
                    : *body;
  }
  if (const std::string* html_url = root.FindString("html_url")) {
    GURL parsed_url(*html_url);
    if (parsed_url.is_valid() && parsed_url.SchemeIs(url::kHttpsScheme)) {
      info.html_url = std::move(parsed_url);
    }
  }
  if (!info.html_url.is_valid()) {
    info.html_url = ReleasePageUrl(info.tag_name);
  }
  if (const std::string* published_at = root.FindString("published_at")) {
    info.published_at = *published_at;
  }

  if (const base::ListValue* assets = root.FindList("assets")) {
    for (const base::Value& item : *assets) {
      if (!item.is_dict()) {
        continue;
      }
      const base::DictValue& dict = item.GetDict();
      const std::string* name = dict.FindString("name");
      const std::string* download_url = dict.FindString("browser_download_url");
      if (!name || name->empty() || !download_url || download_url->empty()) {
        continue;
      }
      GURL url(*download_url);
      if (!url.is_valid() || !url.SchemeIs(url::kHttpsScheme)) {
        continue;
      }
      ReleaseAsset asset;
      asset.name = *name;
      asset.download_url = std::move(url);
      if (std::optional<double> size = dict.FindDouble("size");
          size && *size > 0) {
        asset.size = static_cast<int64_t>(*size);
      }
      if (const std::string* digest = dict.FindString("digest")) {
        if (std::optional<std::string> sha = NormalizeSha256(*digest)) {
          asset.sha256 = std::move(*sha);
        }
      }
      info.assets.push_back(std::move(asset));
    }
  }
  return info;
}

std::optional<std::string> ExtractTagFromWebLatest(const GURL& final_url,
                                                   std::string_view body) {
  // 1) GitHub 对带 Accept: application/json 的 /releases/latest 返回 JSON。
  if (const std::string_view trimmed =
          base::TrimWhitespaceASCII(body, base::TRIM_ALL);
      trimmed.starts_with('{')) {
    if (std::optional<base::Value> parsed =
            base::JSONReader::Read(trimmed, base::JSON_PARSE_RFC);
        parsed && parsed->is_dict()) {
      if (const std::string* tag = parsed->GetDict().FindString("tag_name");
          tag && ParseSemVer(*tag).has_value()) {
        return *tag;
      }
    }
  }

  // 2) 重定向后的最终 URL 路径中带有 /releases/tag/<tag>。
  if (final_url.is_valid()) {
    if (std::optional<std::string> tag =
            ExtractTagAfterMarker(final_url.path(), "/releases/tag/")) {
      return tag;
    }
  }

  // 3) 若镜像在服务端跟随了 302 并直接返回 HTML，从 canonical 或仓库链接中提取。
  const std::string repo_marker =
      base::StrCat({"/", UpdateRepo(), "/releases/tag/"});
  if (std::optional<std::string> tag =
          ExtractTagAfterMarker(body, repo_marker)) {
    return tag;
  }
  return ExtractTagAfterMarker(body, "/releases/tag/");
}

std::optional<std::string> ParseSha256File(std::string_view content,
                                           std::string_view expected_filename) {
  // 仅读取合理大小的纯文本，跳过 HTML 错误页。
  if (content.empty() || content.size() > 64 * 1024) {
    return std::nullopt;
  }
  std::vector<std::string_view> lines = base::SplitStringPiece(
      content, "\r\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  std::optional<std::string> bare_hash;
  int valid_lines = 0;

  for (std::string_view line : lines) {
    if (line.starts_with('#')) {
      continue;
    }
    const size_t sep = line.find_first_of(" \t");
    const std::string_view hash_part =
        sep == std::string_view::npos ? line : line.substr(0, sep);
    std::optional<std::string> sha = NormalizeSha256(hash_part);
    if (!sha) {
      continue;
    }
    ++valid_lines;
    if (sep == std::string_view::npos) {
      bare_hash = std::move(*sha);
      continue;
    }
    std::string_view file_part =
        base::TrimWhitespaceASCII(line.substr(sep + 1), base::TRIM_ALL);
    if (file_part.starts_with('*')) {
      file_part.remove_prefix(1);
    }
    if (file_part.starts_with("./")) {
      file_part.remove_prefix(2);
    }
    if (file_part == expected_filename) {
      return sha;
    }
  }

  if (valid_lines == 1 && bare_hash.has_value()) {
    return bare_hash;
  }
  return std::nullopt;
}

}  // namespace starry::updater
