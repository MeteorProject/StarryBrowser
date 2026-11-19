// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_ASSISTANT_IMAGES_H_
#define STARRY_BROWSER_ASSISTANT_ASSISTANT_IMAGES_H_

#include <string>
#include <string_view>

#include "base/memory/ref_counted_memory.h"
#include "base/memory/scoped_refptr.h"
#include "starry/browser/agent/browser_tools.h"

// 对话里的图片（用户附上的、截取的页面）：开着的对话把它们放在这里，助手的网页用
// starry://assistant/image/<ID>.<扩展名> 显示（StarryPageUI 的请求过滤），不用把几 MB 的
// base64 塞进每一条消息。ID 是随机的；对话不留了就拿掉。
namespace starry::assistant {

// 加一张（ID 已经有了就换掉）。
void AddImage(const std::string& id, const agent::ToolImage& image);
void RemoveImage(const std::string& id);
// 图片在网页里的地址（相对 starry://assistant/）。
std::string ImagePath(const std::string& id, const std::string& mime_type);
// 网页请求 image/<ID>.<扩展名>：解码好的字节；不是这种地址或者没有这张时是 nullptr。
scoped_refptr<base::RefCountedMemory> FindImageForPath(const std::string& path);
bool IsImagePath(std::string_view path);

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_ASSISTANT_IMAGES_H_
