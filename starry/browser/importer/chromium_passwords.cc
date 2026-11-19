// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/chromium_passwords.h"

#include <array>
#include <string_view>
#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/strings/string_view_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "crypto/aead.h"
#include "crypto/aes_cbc.h"
#include "starry/browser/importer/chromium_browsers.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/mem.h"

#if BUILDFLAG(IS_WIN)
#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#endif

#if defined(STARRY_DEVTOOLS)
#include "starry_internal/browser/devtools.h"
#endif

namespace starry {

namespace {

constexpr std::string_view kVersion10 = "v10";

bool HasPrefix(base::span<const uint8_t> value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.first(prefix.size()) == base::as_byte_span(prefix);
}

#if !BUILDFLAG(IS_WIN)

// 同 components/os_crypt（POSIX）：盐和 IV 都是固定的，密钥 128 位。
constexpr std::string_view kSalt = "saltysalt";
constexpr std::array<uint8_t, crypto::aes_cbc::kBlockSize> kIv = {
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
constexpr size_t kCbcKeySize = 16;

#if BUILDFLAG(IS_MAC)
constexpr uint32_t kIterations = 1003;
#else
constexpr uint32_t kIterations = 1;
// 没有钥匙环时 Chromium 用的口令（v10）。
constexpr char kLinuxPassword[] = "peanuts";
#endif

std::vector<uint8_t> DeriveKey(std::string_view password) {
  std::vector<uint8_t> key(kCbcKeySize);
  PKCS5_PBKDF2_HMAC_SHA1(password.data(), password.size(),
                         base::as_byte_span(kSalt).data(), kSalt.size(),
                         kIterations, key.size(), key.data());
  return key;
}

#else  // BUILDFLAG(IS_WIN)

constexpr std::string_view kVersion20 = "v20";
constexpr std::string_view kDpapiKeyPrefix = "DPAPI";
constexpr size_t kGcmNonceSize = 12;
constexpr size_t kGcmTagSize = 16;
constexpr size_t kMaxLocalStateSize = 32 * 1024 * 1024;

// Local State 的 os_crypt.encrypted_key：base64、前缀 DPAPI、DPAPI 加密的 256 位密钥。
std::vector<uint8_t> ReadWindowsKey(const base::FilePath& user_data_dir) {
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(
          user_data_dir.AppendASCII(kLocalStateFile), &text,
          kMaxLocalStateSize)) {
    return {};
  }
  const std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(text, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  const std::string* encoded =
      root ? root->FindStringByDottedPath("os_crypt.encrypted_key") : nullptr;
  if (!encoded) {
    return {};
  }
  const std::optional<std::vector<uint8_t>> decoded =
      base::Base64Decode(*encoded);
  if (!decoded || !HasPrefix(*decoded, kDpapiKeyPrefix)) {
    return {};
  }
  std::optional<std::vector<uint8_t>> key =
      DpapiUnprotect(base::span(*decoded).subspan(kDpapiKeyPrefix.size()));
  if (!key ||
      key->size() != crypto::aead::KeySizeFor(crypto::aead::AES_256_GCM)) {
    return {};
  }
  return std::move(*key);
}

#endif  // BUILDFLAG(IS_WIN)

}  // namespace

ChromiumPasswordDecryptor::ChromiumPasswordDecryptor() = default;
ChromiumPasswordDecryptor::ChromiumPasswordDecryptor(
    ChromiumPasswordDecryptor&&) = default;
ChromiumPasswordDecryptor& ChromiumPasswordDecryptor::operator=(
    ChromiumPasswordDecryptor&&) = default;

ChromiumPasswordDecryptor::~ChromiumPasswordDecryptor() {
  OPENSSL_cleanse(key_.data(), key_.size());
}

// static
ChromiumPasswordDecryptor ChromiumPasswordDecryptor::Create(
    const ChromiumBrowser& browser,
    const base::FilePath& user_data_dir) {
  ChromiumPasswordDecryptor decryptor;
#if BUILDFLAG(IS_MAC)
  std::optional<std::string> password;
#if defined(STARRY_DEVTOOLS)
  if (ImportHomeForTesting()) {
    password = devtools::TestSafeStoragePassword();
  } else
#endif
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("use-mock-keychain")) {
    // 同 os_crypt 的 --use-mock-keychain：说了不碰钥匙串（scripts/run.sh）就不读。
    password = ReadKeychainPassword(browser.keychain_service,
                                    browser.keychain_account);
  }
  if (password && !password->empty()) {
    decryptor.key_ = DeriveKey(*password);
    decryptor.status_ = Status::kReady;
  }
  if (password) {
    OPENSSL_cleanse(password->data(), password->size());
  }
#elif BUILDFLAG(IS_WIN)
  decryptor.key_ = ReadWindowsKey(user_data_dir);
  // 没有密钥时还能解旧格式（直接用 DPAPI 加密）的。
  decryptor.status_ =
      decryptor.key_.empty() ? Status::kKeyUnavailable : Status::kReady;
#else
  decryptor.key_ = DeriveKey(kLinuxPassword);
  decryptor.status_ = Status::kReady;
#endif
  return decryptor;
}

std::optional<std::u16string> ChromiumPasswordDecryptor::Decrypt(
    base::span<const uint8_t> value) const {
  std::optional<std::vector<uint8_t>> plaintext;
#if BUILDFLAG(IS_WIN)
  if (HasPrefix(value, kVersion10)) {
    const base::span<const uint8_t> payload = value.subspan(kVersion10.size());
    if (key_.empty() || payload.size() < kGcmNonceSize + kGcmTagSize) {
      return std::nullopt;
    }
    plaintext = crypto::aead::Open(
        crypto::aead::AES_256_GCM, key_, payload.subspan(kGcmNonceSize),
        payload.first(kGcmNonceSize), base::span<const uint8_t>());
  } else if (HasPrefix(value, kVersion20)) {
    return std::nullopt;
  } else {
    plaintext = DpapiUnprotect(value);
  }
#else
  if (status_ != Status::kReady || !HasPrefix(value, kVersion10)) {
    return std::nullopt;
  }
  plaintext =
      crypto::aes_cbc::Decrypt(key_, kIv, value.subspan(kVersion10.size()));
#endif
  if (!plaintext) {
    return std::nullopt;
  }
  std::u16string result;
  const bool valid = base::UTF8ToUTF16(base::as_string_view(*plaintext).data(),
                                       plaintext->size(), &result);
  OPENSSL_cleanse(plaintext->data(), plaintext->size());
  if (!valid) {
    return std::nullopt;
  }
  return result;
}

}  // namespace starry
