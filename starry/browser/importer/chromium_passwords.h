// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_IMPORTER_CHROMIUM_PASSWORDS_H_
#define STARRY_BROWSER_IMPORTER_CHROMIUM_PASSWORDS_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "build/build_config.h"

namespace starry {

struct ChromiumBrowser;

// 解开 Chromium 系浏览器存的密码（Login Data 的 password_value），用那个浏览器自己的
// os_crypt 的密钥（格式同 components/os_crypt）：
// - macOS：钥匙串里「<浏览器> Safe Storage」的口令，PBKDF2-SHA1（盐 saltysalt，1003 次）
//   得到 128 位的 AES-CBC 密钥，IV 是 16 个空格，密文的前缀是 v10。读别的 App 的钥匙串
//   项目时系统会问用户（Arc、Edge 导入 Chrome 的密码时也是这样）。
// - Windows：Local State 的 os_crypt.encrypted_key 是 DPAPI 加密的 256 位密钥；前缀 v10
//   的是 AES-256-GCM（12 字节的 nonce，最后 16 字节是 tag），没有前缀的是更早的直接用
//   DPAPI 加密的。Chrome 127 起的 v20（App-Bound 加密）只有 Chrome 自己能解，不支持。
// - Linux：v10 的口令固定是 peanuts（没有钥匙环时用），PBKDF2 一次；v11 的口令在系统的
//   钥匙环（libsecret、KWallet）里，暂不支持。
class ChromiumPasswordDecryptor {
 public:
  enum class Status {
    kReady,
    // 拿不到密钥：macOS 上钥匙串里没有这一项、用户没有允许，或者用了
    // --use-mock-keychain；Windows 上 Local State 里没有密钥（这时只能解旧格式的）。
    kKeyUnavailable,
  };

  // 取密钥。macOS 上读钥匙串，系统可能弹出授权的对话框，一直等到用户回答：在能阻塞的
  // 线程上调用。
  static ChromiumPasswordDecryptor Create(const ChromiumBrowser& browser,
                                          const base::FilePath& user_data_dir);

  ChromiumPasswordDecryptor(ChromiumPasswordDecryptor&&);
  ChromiumPasswordDecryptor& operator=(ChromiumPasswordDecryptor&&);
  ~ChromiumPasswordDecryptor();

  Status status() const { return status_; }

  // 解开一个 password_value。解不开（密钥不对、v20、v11）时是 nullopt。
  std::optional<std::u16string> Decrypt(base::span<const uint8_t> value) const;

 private:
  ChromiumPasswordDecryptor();

  Status status_ = Status::kKeyUnavailable;
  // macOS、Linux：AES-128-CBC 的密钥。Windows：AES-256-GCM 的密钥（可能是空的）。
  std::vector<uint8_t> key_;
};

#if BUILDFLAG(IS_MAC)
// 钥匙串里这个通用密码项目的口令（chromium_passwords_mac.cc）。
std::optional<std::string> ReadKeychainPassword(const std::string& service,
                                                const std::string& account);
#elif BUILDFLAG(IS_WIN)
// CryptUnprotectData（chromium_passwords_win.cc）。
std::optional<std::vector<uint8_t>> DpapiUnprotect(
    base::span<const uint8_t> data);
#endif

}  // namespace starry

#endif  // STARRY_BROWSER_IMPORTER_CHROMIUM_PASSWORDS_H_
