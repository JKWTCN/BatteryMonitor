#include "JsPluginLoader.h"

#include "JsPluginProvider.h"
#include "util/AppSettings.h"
#include "util/Logger.h"

#include <QCoreApplication>

#include <filesystem>
#include <algorithm>

#include <windows.h>

namespace
{
    // 仅接受不以 . / _ 开头、扩展名为小写 .js 的常规文件名，防止奇怪
    // 文件名混进日志前缀与设置键。插件 id 在扫描后由相对路径生成。
    bool endsWithJs(const std::wstring &name)
    {
        return name.size() > 3 && name.compare(name.size() - 3, 3, L".js") == 0;
    }

    bool isValidPluginFileName(const std::wstring &name)
    {
        return endsWithJs(name) && name[0] != L'.' && name[0] != L'_';
    }

    std::wstring pluginPathToId(const std::filesystem::path &root,
                                const std::filesystem::path &path)
    {
        std::filesystem::path relative = path.lexically_relative(root);
        relative.replace_extension();
        return relative.generic_wstring();
    }

    std::string wideToUtf8(const std::wstring &s)
    {
        if (s.empty())
        {
            return {};
        }
        const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                            static_cast<int>(s.size()), nullptr, 0,
                                            nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                            out.data(), len, nullptr, nullptr);
        return out;
    }
} // namespace

namespace JsPluginLoader
{

std::wstring defaultPluginDir()
{
    const std::filesystem::path exeDir(
        QCoreApplication::applicationDirPath().toStdWString());
    return (exeDir / L"plugins").wstring();
}

std::vector<std::unique_ptr<IBatteryProvider>> discover(const std::wstring &dir)
{
    std::vector<std::unique_ptr<IBatteryProvider>> providers;
    if (dir.empty())
    {
        return providers;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path(dir), ec))
    {
        // 目录不存在属常态(用户从未装过插件),不打扰。
        LOG_VERBOSE_W(L"[js] plugin directory not found: " + dir);
        return providers;
    }

    const std::filesystem::path root(dir);
    std::vector<std::filesystem::path> files;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    if (ec)
    {
        LOG_WARN_W(L"[js] cannot scan plugin directory: " + dir);
        return providers;
    }

    for (; it != end; it.increment(ec))
    {
        if (ec)
        {
            LOG_WARN_W(L"[js] skipped an unreadable path under: " + dir);
            ec.clear();
            continue;
        }

        std::error_code entryError;
        if (!it->is_regular_file(entryError) || entryError)
        {
            continue;
        }
        const std::wstring name = it->path().filename().wstring();
        if (isValidPluginFileName(name))
        {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end()); // 按相对路径稳定加载,便于日志对照

    for (const std::filesystem::path &path : files)
    {
        // ID 使用相对 plugins 根目录的无扩展路径，避免不同子目录中的同名
        // 插件冲突，例如 razer/mouse 与 logitech/mouse。
        const std::wstring id = pluginPathToId(root, path);
        if (AppSettings::pluginDisabled(QString::fromStdWString(id)))
        {
            LOG_W(L"[js] plugin '" + id + L"' is disabled by settings, skipping");
            continue;
        }
        LOG_W(L"[js] discovered plugin '" + id + L"' (" + path.wstring() + L")");
        providers.push_back(
            std::make_unique<JsPluginProvider>(wideToUtf8(id), path.wstring()));
    }
    return providers;
}

} // namespace JsPluginLoader
