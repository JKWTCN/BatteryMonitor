#pragma once

#include "src/core/IBatteryProvider.h"

#include <memory>
#include <string>
#include <vector>

// JS 插件发现与注册(使用说明见 docs/js-plugin.md)。
//
// 插件目录默认为 <exe目录>/plugins/,递归扫描到的每个 *.js 即一个插件。
// 插件 id = 相对 plugins 根目录的无扩展路径。坏文件不会阻断其它插件:
// 文件层面只做名字过滤,加载 / 校验在 provider 首次刷新时进行,失败会写
// ERROR 日志并停用该插件。
namespace JsPluginLoader
{
    // 默认插件目录 <exe目录>/plugins。
    std::wstring defaultPluginDir();

    // 递归扫描目录下的 *.js,逐个构造 JsPluginProvider。
    // 目录不存在 / 不可读时返回空列表并写一条 VERBOSE 日志。
    std::vector<std::unique_ptr<IBatteryProvider>> discover(const std::wstring &dir);
} // namespace JsPluginLoader
