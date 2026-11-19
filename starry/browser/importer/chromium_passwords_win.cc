// Copyright 2026 The Starry Browser Authors

#include "starry/browser/importer/chromium_passwords.h"

#include <windows.h>

#include <wincrypt.h>

#include <optional>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/numerics/safe_conversions.h"
#include "base/win/scoped_localalloc.h"
#include "third_party/boringssl/src/include/openssl/mem.h"

namespace starry {

// 同 components/os_crypt 的 DPAPIKeyProvider：用当前 Windows 用户的 DPAPI 解开（别的
// 浏览器也是用这个用户的 DPAPI 加密的）。
std::optional<std::vector<uint8_t>> DpapiUnprotect(
    base::span<const uint8_t> data) {
  DATA_BLOB input = {};
  input.pbData = const_cast<BYTE*>(data.data());
  input.cbData = base::checked_cast<DWORD>(data.size());
  DATA_BLOB output = {};
  if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  auto local_alloc = base::win::TakeLocalAlloc(output.pbData);
  // SAFETY: CryptUnprotectData 给的是 output.cbData 字节。
  const base::span<uint8_t> bytes =
      UNSAFE_BUFFERS(base::span(local_alloc.get(), output.cbData));
  std::vector<uint8_t> result(bytes.begin(), bytes.end());
  OPENSSL_cleanse(bytes.data(), bytes.size());
  return result;
}

}  // namespace starry
