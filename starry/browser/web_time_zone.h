// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_WEB_TIME_ZONE_H_
#define STARRY_BROWSER_WEB_TIME_ZONE_H_

#include <string>
#include <string_view>

#include "base/values.h"

class Profile;

namespace starry {

// 原来的用户资料在本次启动使用的时区，空字符串是跟随系统。第一次读取时固定，
// 之后改偏好、建窗口、创建新的网页进程都不变；无痕共用原来的值。模拟位置的网站
// 改用那个位置的时区（privacy/process_time_zone.h）。
const std::string& EffectiveWebTimeZone(Profile* profile);

std::string SystemTimeZoneId();
// 现在的偏移：「GMT+9」、「GMT+5:45」、「GMT」。
std::u16string WebTimeZoneOffset(std::string_view id);

// 设置页的选择、当前标签、常用和按地区分组的菜单、是否需要重启。
void AddWebTimeZoneSettings(Profile* profile, base::DictValue& state);

}  // namespace starry

#endif  // STARRY_BROWSER_WEB_TIME_ZONE_H_
