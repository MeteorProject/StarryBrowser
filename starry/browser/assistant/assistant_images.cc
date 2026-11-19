// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/assistant_images.h"

#include <map>
#include <string_view>

#include "base/base64.h"
#include "base/no_destructor.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"

namespace starry::assistant {

namespace {

constexpr std::string_view kPrefix = "image/";

std::map<std::string, scoped_refptr<base::RefCountedMemory>>& Images() {
  static base::NoDestructor<
      std::map<std::string, scoped_refptr<base::RefCountedMemory>>>
      images;
  return *images;
}

std::string_view Extension(const std::string& mime_type) {
  if (mime_type == "image/png") {
    return "png";
  }
  if (mime_type == "image/gif") {
    return "gif";
  }
  if (mime_type == "image/webp") {
    return "webp";
  }
  return "jpg";
}

}  // namespace

void AddImage(const std::string& id, const agent::ToolImage& image) {
  std::string data;
  if (!base::Base64Decode(image.base64, &data)) {
    return;
  }
  Images()[id] = base::MakeRefCounted<base::RefCountedString>(std::move(data));
}

void RemoveImage(const std::string& id) {
  Images().erase(id);
}

std::string ImagePath(const std::string& id, const std::string& mime_type) {
  return base::StrCat({kPrefix, id, ".", Extension(mime_type)});
}

bool IsImagePath(std::string_view path) {
  return base::StartsWith(path, kPrefix);
}

scoped_refptr<base::RefCountedMemory> FindImageForPath(
    const std::string& path) {
  if (!IsImagePath(path)) {
    return nullptr;
  }
  std::string_view name = std::string_view(path).substr(kPrefix.size());
  name = name.substr(0, name.find_first_of(".?"));
  auto it = Images().find(std::string(name));
  return it == Images().end() ? nullptr : it->second;
}

}  // namespace starry::assistant
