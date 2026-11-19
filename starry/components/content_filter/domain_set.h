// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_COMPONENTS_CONTENT_FILTER_DOMAIN_SET_H_
#define STARRY_COMPONENTS_CONTENT_FILTER_DOMAIN_SET_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace starry::content_filter {

// 一个网站的各级后缀的散列，从长到短（a.b.com → a.b.com、b.com、com）。entity 是去掉
// 公共后缀以后的「名字.*」写法（www.google.co.uk → www.google.*、google.*），给
// uBlock Origin 的 `example.*` 用。IP 地址只有它自己。
struct HostSuffixes {
  void Compute(std::string_view host);

  std::vector<uint64_t> plain;
  std::vector<uint64_t> entity;
};

// 去掉开头的 `www.`、末尾的点，转成小写的 ASCII（IDN 转成 punycode）。不是合法的
// 网站名时返回空。设置页的「不拦截的网站」、规则里的网站都用它。
std::string NormalizeHost(std::string_view host, bool strip_www);

// 规则里限定的一组网站，比如 `$domain=a.com|~b.a.com|example.*`、`a.com,~b.com##.ad`。
// 判断时从最长的后缀开始找，先碰到排除的就不算、先碰到包含的就算（越具体的越优先）；
// 一个都没碰到时，没有包含的网站就算（只写了排除的）。
class DomainSet {
 public:
  DomainSet();
  DomainSet(const DomainSet&);
  DomainSet& operator=(const DomainSet&);
  DomainSet(DomainSet&&);
  DomainSet& operator=(DomainSet&&);
  ~DomainSet();

  // separator：网络规则是 '|'，元素隐藏是 ','。有不支持的写法（正则网站、不合法的
  // 网站名）时返回 false。
  bool Parse(std::string_view list, char separator);
  void AddInclude(std::string_view host);
  void AddExclude(std::string_view host);

  bool Matches(const HostSuffixes& host) const;
  bool empty() const { return includes_.empty() && excludes_.empty(); }
  bool has_includes() const { return !includes_.empty(); }
  bool has_excludes() const { return !excludes_.empty(); }
  // 包含的网站（散列），建索引用。
  const std::vector<uint64_t>& includes() const { return includes_; }
  const std::vector<uint64_t>& excludes() const { return excludes_; }

 private:
  bool AddEntry(std::string_view entry, bool exclude);

  std::vector<uint64_t> includes_;
  std::vector<uint64_t> excludes_;
  bool has_entities_ = false;
};

}  // namespace starry::content_filter

#endif  // STARRY_COMPONENTS_CONTENT_FILTER_DOMAIN_SET_H_
