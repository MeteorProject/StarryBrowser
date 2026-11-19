// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_REQUEST_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_REQUEST_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ref.h"
#include "starry/components/content_filter/content_filter_types.h"
#include "starry/components/content_filter/domain_set.h"
#include "url/gurl.h"

namespace starry::content_filter {

// 请求的方法（$method=），每种一位。
enum RequestMethod : uint16_t {
  kMethodGet = 1u << 0,
  kMethodPost = 1u << 1,
  kMethodHead = 1u << 2,
  kMethodPut = 1u << 3,
  kMethodDelete = 1u << 4,
  kMethodPatch = 1u << 5,
  kMethodOptions = 1u << 6,
  kMethodConnect = 1u << 7,
  kMethodTrace = 1u << 8,
};
// 不认识的方法名返回 0。
uint16_t MethodFromName(std::string_view name);

// 要判断的一个请求：网址、发出它的网页（判断第三方和 `$domain=`）、类型。网址的
// 小写、分词、网站的各级后缀第一次用到时才算，一个请求只算一次。
class Request {
 public:
  // source 是发出请求的文档（iframe 里就是 iframe 的网址；顶层页面是它自己）。没有
  // 网页的（浏览器自己发的）传空的，算第一方。
  Request(const GURL& url, const GURL& source, RequestType type);
  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;
  ~Request();

  void set_method(std::string_view method) { method_ = MethodFromName(method); }

  const GURL& url() const { return *url_; }
  const std::string& spec() const { return url_->spec(); }
  // 小写的网址（不区分大小写的规则用）。
  const std::string& lower() const { return lower_; }
  RequestType type() const { return type_; }
  uint16_t method() const { return method_; }
  // 网址里主机名的位置（spec 的下标）。
  size_t host_begin() const { return host_begin_; }
  size_t host_end() const { return host_end_; }
  std::string_view host() const;
  const std::string& source_host() const { return source_host_; }
  bool has_source() const { return !source_host_.empty(); }
  // 第三方：和网页不属于同一个可注册的网站（eTLD+1）。
  bool third_party() const { return third_party_; }
  // 主机名不同（strict3p）。
  bool strict_third_party() const { return strict_third_party_; }

  const std::vector<uint32_t>& tokens() const;
  const HostSuffixes& host_suffixes() const;
  const HostSuffixes& source_suffixes() const;

 private:
  // 调用方的 GURL，要比 Request 活得久。
  const raw_ref<const GURL> url_;
  std::string lower_;
  size_t host_begin_ = 0;
  size_t host_end_ = 0;
  std::string source_host_;
  RequestType type_;
  uint16_t method_ = 0;
  bool third_party_ = false;
  bool strict_third_party_ = false;

  mutable bool tokenized_ = false;
  mutable std::vector<uint32_t> tokens_;
  mutable bool host_suffixes_ready_ = false;
  mutable HostSuffixes host_suffixes_;
  mutable bool source_suffixes_ready_ = false;
  mutable HostSuffixes source_suffixes_;
};

// 把小写的文字切成分词（连续的字母、数字和 %，至少 kMinTokenLength 个字符），
// 返回散列。网址只看前 kMaxTokenizedUrlLength 个字符。
void Tokenize(std::string_view lower, std::vector<uint32_t>* out);

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_REQUEST_H_
