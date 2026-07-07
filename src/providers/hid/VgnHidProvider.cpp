#include "VgnHidProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <hidapi.h>

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>

namespace
{
// —— 协议族枚举 ——
// 每个族对应一种独立的 HID 报文格式 / 命令号 / 校验 / 字段布局。readDevices() 按
// 设备表把 (VID,PID) 映射到族，再分派到对应的查询函数。
enum class Family
{
    ThreeMode,   // flashextreme / flash75 / rongyana 键盘，64B Output/Input Report
    Weisheng,    // VGN S99 / V98pro / VXE75 / VXE V87 / S87 键盘，7B Output Report
    Beiying,     // VGN V98pro V3 / V87 V2 / N75 V2 / V108 / VXE V87pro 键盘，19B+CRC
    VgnRyMouse,  // Dragonfly 3 Pro Max 等，64B Feature Report，反码 CRC
    Yongjiaxin,  // Dragonfly 3 SE/Pro 等，64B Output Report，0x55 头 + 反码 CRC
    VgnLdMs,     // Dragonfly F1 V2 Extreme / Falcon3 Extreme(+)，32B Output Report，反码 CRC
    Arbit,       // VGN FLASH68 / FLASH68+，64B Output Report，0x55 帧 + 加和校验
    ByKeyboard,  // VGN V98Pro V4 8K/1K，Feature/Output Report，结构化帧
    VgnKc2,      // Dragonfly F1 V5 Turbo+，20B Output Report（reportId=179）
    MouseEnc,    // VGN F2 Pro Max / Y2 / F1 / Dragonfly King 等，16B Output Report，0x55 CRC
};

// (VID,PID) → 协议族 + reportId + 默认显示名。
// reportId 来自型号表条目（部分族查询时会临时改写，见各查询函数）。
struct VgnDeviceEntry
{
    uint16_t vid;
    uint16_t pid;
    Family family;
    uint8_t reportId;     // Output/Input/Feature Report 编号
    const wchar_t *name;  // 默认设备名（hidapi product_string 取不到时兜底）
    bool receiver = false;      // 是否为 2.4G 接收器 PID
    uint16_t usagePage = 0;     // 非 0 时仅匹配指定 HID usage_page
    uint16_t usage = 0;         // 非 0 时仅匹配指定 HID usage
};

// —— 设备表 ——
// 同一物理设备常以「有线 + 2.4G Dongle」两条记录出现，PID 不同；本表都纳入。
constexpr VgnDeviceEntry kVgnDevices[] = {
    // —— ThreeMode 键盘族（reportId=0，64B）——
    { 0x25AA, 0x1001, Family::ThreeMode, 0, L"VGN Flash75 keyboard" },
    { 0x1CA3, 0x0206, Family::ThreeMode, 0, L"Spark Game keyboard" },
    { 0x1CA6, 0x0104, Family::ThreeMode, 0, L"VGN Neon75 Keyboard" },
    { 0x1CA6, 0x0105, Family::ThreeMode, 0, L"VGN Neon75 Extreme Keyboard" },
    { 0x1CA6, 0x0103, Family::ThreeMode, 0, L"VGN Flash75 keyboard" },
    { 0x391D, 0x2002, Family::ThreeMode, 0, L"VGN Neon68 Extreme+" },
    { 0x1CA6, 0x0534, Family::ThreeMode, 0, L"VGN Neon68 Extreme+" },
    { 0x3151, 0x502F, Family::ThreeMode, 0, L"VGN Neon75" },
    { 0x3151, 0x5038, Family::ThreeMode, 0, L"VGN Neon75 Dongle" },
    { 0x3151, 0x5054, Family::ThreeMode, 0, L"VGN Neon75 Extreme" },
    { 0x3151, 0x5055, Family::ThreeMode, 0, L"VGN Neon75 Extreme Dongle" },
    { 0x3151, 0x502D, Family::ThreeMode, 0, L"VGN Neon75 He" },
    { 0x391D, 0x2001, Family::ThreeMode, 0, L"VGN Neon68 Extreme" },
    { 0x391D, 0x200D, Family::ThreeMode, 0, L"VGN Neon75 Air" },
    { 0x391D, 0x2A05, Family::ThreeMode, 0, L"VGN Neon75 Air 2.4G" },
    { 0x391D, 0x200F, Family::ThreeMode, 0, L"VGN Neon75 Air" },

    // —— Weisheng 键盘族（reportId=4，7B）——
    { 0x320F, 0x5055, Family::Weisheng, 4, L"VGN S99" },
    { 0x320F, 0x5088, Family::Weisheng, 4, L"VGN S99 2.4G Dongle" },
    { 0x373B, 0x1028, Family::Weisheng, 4, L"VGN S87" },
    { 0x373B, 0x1029, Family::Weisheng, 4, L"VGN S87 Dongle" },

    // —— Beiying 键盘族（reportId=9/19/6，19B+CRC）——
    { 0x258A, 0x024E, Family::Beiying, 9, L"VGN V98pro V3" },
    { 0x258A, 0x024F, Family::Beiying, 19, L"VGN V98pro V3 2.4G Dongle" },
    { 0x258A, 0x027E, Family::Beiying, 9, L"VGN V98pro V3" },
    { 0x258A, 0x027F, Family::Beiying, 19, L"VGN V98pro V3 2.4G Dongle" },
    { 0x258A, 0x010C, Family::Beiying, 6, L"VXE V87pro Keyboard" },
    { 0x3554, 0xFA09, Family::Beiying, 19, L"VXE V87pro 2.4G Dongle" },
    { 0x258A, 0x026A, Family::Beiying, 9, L"VGN V87 V2" },
    { 0x258A, 0x027B, Family::Beiying, 19, L"VGN V87 V2 2.4G Dongle" },
    { 0x391D, 0x2003, Family::Beiying, 9, L"VGN N75 V2" },
    { 0x391D, 0x2A01, Family::Beiying, 19, L"VGN N75 V2 2.4G Dongle" },
    { 0x391D, 0x2004, Family::Beiying, 9, L"VGN N75 V2" },
    { 0x391D, 0x2005, Family::Beiying, 9, L"VGN V108" },
    { 0x391D, 0x2006, Family::Beiying, 9, L"VGN V108" },
    { 0x391D, 0x2A02, Family::Beiying, 19, L"VGN V108 2.4G Dongle" },

    // —— VgnRyMouse 鼠标族（reportId=0，64B Feature Report）——
    { 0x391D, 0x1004, Family::VgnRyMouse, 0, L"Dragonfly 3 Pro Max" },
    { 0x391D, 0x1A04, Family::VgnRyMouse, 0, L"Dragonfly 3 Pro Max" },
    { 0x391D, 0x1008, Family::VgnRyMouse, 0, L"Dragonfly Y2 V2 Pro Max" },
    { 0x391D, 0x1A08, Family::VgnRyMouse, 0, L"Dragonfly Y2 V2 Pro Max" },
    { 0x391D, 0x1028, Family::VgnRyMouse, 0, L"Dragonfly 3 Ultra" },
    { 0x391D, 0x1A25, Family::VgnRyMouse, 0, L"Dragonfly 3 Ultra" },
    { 0x391D, 0x102F, Family::VgnRyMouse, 0, L"Dragonfly F1 V5 Ultra" },
    { 0x391D, 0x1A2B, Family::VgnRyMouse, 0, L"Dragonfly F1 V5 Ultra" },

    // —— Yongjiaxin 鼠标族（reportId=0，64B Output Report，0x55 头）——
    { 0x391D, 0x1001, Family::Yongjiaxin, 0, L"VGN Dragonfly 3 SE" },
    { 0x391D, 0x1A01, Family::Yongjiaxin, 0, L"VGN Dragonfly 3 SE 2.4G" },
    { 0x391D, 0x1003, Family::Yongjiaxin, 0, L"VGN Dragonfly 3 Pro" },
    { 0x391D, 0x1A03, Family::Yongjiaxin, 0, L"VGN Dragonfly 3 Pro 2.4G" },
    { 0x3837, 0x4042, Family::Yongjiaxin, 0, L"VGN Dragonfly 3 Pro" },
    { 0x391D, 0x100A, Family::Yongjiaxin, 0, L"Dragonfly Y2 V2 Pro" },
    { 0x391D, 0x1A09, Family::Yongjiaxin, 0, L"Dragonfly Y2 V2 Pro 2.4G" },
    { 0x391D, 0x100B, Family::Yongjiaxin, 0, L"Dragonfly Y2 V2 Plus" },
    { 0x391D, 0x1A0A, Family::Yongjiaxin, 0, L"Dragonfly Y2 V2 Plus 2.4G" },
    { 0x391D, 0x1017, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 Pro" },
    { 0x391D, 0x1A14, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 Pro 2.4G" },
    { 0x391D, 0x1019, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 SE" },
    { 0x391D, 0x1A16, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 SE 2.4G" },
    { 0x391D, 0x1018, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 Plus" },
    { 0x391D, 0x1A15, Family::Yongjiaxin, 0, L"Dragonfly F1 V2 Plus 2.4G" },

    // —— VgnLdMs 鼠标族（reportId=160，32B Output Report）——
    { 0x391D, 0x101B, Family::VgnLdMs, 160, L"Dragonfly F1 V2 Extreme" },
    { 0x391D, 0x1A18, Family::VgnLdMs, 160, L"Dragonfly F1 V2 Extreme 2.4G", true },
    { 0x391D, 0x102E, Family::VgnLdMs, 160, L"Falcon3 Extreme" },
    { 0x391D, 0x1A2A, Family::VgnLdMs, 160, L"Falcon3 Extreme 2.4G", true },
    { 0x391D, 0x1037, Family::VgnLdMs, 160, L"Falcon3 Extreme+" },
    { 0x391D, 0x1A29, Family::VgnLdMs, 160, L"Falcon3 Extreme+ 2.4G", true },

    // —— Arbit 键盘族（reportId=0，64B Output Report，0x55 帧）——
    { 0x391D, 0x2007, Family::Arbit, 0, L"VGN FLASH68" },
    { 0x391D, 0x2011, Family::Arbit, 0, L"VGN FLASH68" },
    { 0x391D, 0x2009, Family::Arbit, 0, L"VGN FLASH68+" },

    // —— VgnKc2 鼠标族（reportId=179，20B Output Report）——
    { 0x391D, 0x1026, Family::VgnKc2, 179, L"Dragonfly F1 V5 Turbo+" },
    { 0x391D, 0x1A23, Family::VgnKc2, 179, L"Dragonfly F1 V5 Turbo+ 2.4G", true },

    // —— MouseEnc 鼠标族（reportId=8/32，16B Output Report）——
    // 型号持续扩充，故此处仅登记已知条目；表外的 PID 由 readDevices()
    // 的 VID 兜底逻辑归入 MouseEnc 尝试查询（无响应即跳过，不会误报）。
    // VID=0x3554（F2/Y2/F1 系列）
    { 0x3554, 0xFB3D, Family::MouseEnc, 8, L"VGN Gaming Mouse Y2 Ultra" },
    { 0x3554, 0xFB3E, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xFB53, Family::MouseEnc, 8, L"VGN Gaming Mouse Y2 Master" },
    { 0x3554, 0xFB52, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xFB55, Family::MouseEnc, 8, L"VGN Gaming Mouse Y2 Pro Max" },
    { 0x3554, 0xFB54, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xFB57, Family::MouseEnc, 8, L"VGN F2 Master" },         // F2 系列同族
    { 0x3554, 0xFB56, Family::MouseEnc, 8, L"VGN F2 Master" },
    { 0x3554, 0xFB49, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xFB4A, Family::MouseEnc, 8, L"UFO Pro" },
    { 0x3554, 0xFB4B, Family::MouseEnc, 8, L"UFO Ultra" },
    { 0x3554, 0xF502, Family::MouseEnc, 8, L"VGN Mouse" },
    { 0x3554, 0xF50A, Family::MouseEnc, 8, L"VGN Mouse" },
    { 0x3554, 0xF50B, Family::MouseEnc, 8, L"VGN Mouse" },
    { 0x3554, 0xF503, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xF505, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x3554, 0xF506, Family::MouseEnc, 8, L"VGN F1 MOBA" },
    { 0x3554, 0xFB51, Family::MouseEnc, 8, L"VGN F1 SE" },
    { 0x3554, 0xFB50, Family::MouseEnc, 8, L"VGN F1 SE" },
    { 0x3554, 0xFB59, Family::MouseEnc, 8, L"Dragonfly 3 Master" },
    { 0x3554, 0xFB58, Family::MouseEnc, 8, L"VGN USB Receiver" },
    // VID=0x391D（Dragonfly MasterGT 系列）
    { 0x391D, 0x1005, Family::MouseEnc, 8, L"Dragonfly 3 Master" },
    { 0x391D, 0x1A05, Family::MouseEnc, 8, L"VGN USB Receiver" },
    { 0x391D, 0x101D, Family::MouseEnc, 8, L"Dragonfly 3 MasterGT" },
    { 0x391D, 0x1A1A, Family::MouseEnc, 8, L"Dragonfly 3 MasterGT" },
    { 0x391D, 0x1034, Family::MouseEnc, 8, L"Dragonfly Y2 V2 MasterGT" },
    { 0x391D, 0x1A2F, Family::MouseEnc, 8, L"Dragonfly Y2 V2 MasterGT" },
    { 0x391D, 0x1032, Family::MouseEnc, 8, L"Dragonfly F2 MasterGT" },
    { 0x391D, 0x102D, Family::MouseEnc, 8, L"Dragonfly F2 MasterGT" },
    // VID=0x9981 / 0x372E（Dragonfly King，reportId=32）
    { 0x9981, 0x6002, Family::MouseEnc, 32, L"VGN Dragonfly King" },
    { 0x9981, 0x7002, Family::MouseEnc, 32, L"VGN Dragonfly King" },
    { 0x372E, 0x10AC, Family::MouseEnc, 32, L"VGN Dragonfly King" },
    { 0x372E, 0x10AD, Family::MouseEnc, 32, L"VGN Dragonfly King" },

    // —— ByKeyboard 族（VGN V98Pro V4 8K/1K，usagePage=0xFF60, usage=0x61）——
    { 0x391D, 0x200B, Family::ByKeyboard, 0, L"VGN V98Pro V4 8K", false, 0xFF60, 0x0061 },
    { 0x391D, 0x2A03, Family::ByKeyboard, 0, L"VGN V98Pro V4 8K 2.4G", true, 0xFF60, 0x0061 },
    { 0x391D, 0x200C, Family::ByKeyboard, 0, L"VGN V98Pro V4 1K", false, 0xFF60, 0x0061 },
    { 0x391D, 0x2A04, Family::ByKeyboard, 0, L"VGN V98Pro V4 1K 2.4G", true, 0xFF60, 0x0061 },
};

// VGN 设备涉及的 VID 集合（用于 hid_enumerate 时只枚举这些 VID，减少无关接口干扰）。
// 注：0x391D 是 VGN 关联品牌的核心 VID，多个族共用；其余 VID 各族分散。
constexpr uint16_t kVgnVendorIds[] = {
    0x1CA3, 0x1CA6, 0x258A, 0x25AA, 0x3151, 0x320F, 0x3554,
    0x372E, 0x373B, 0x3837, 0x391D, 0x9981,
};

// 判定一个 VID 是否属于 VGN 系（用于 hid_enumerate 二次过滤）。
bool isVgnVendor(uint16_t vid)
{
    for (uint16_t v : kVgnVendorIds) {
        if (v == vid) {
            return true;
        }
    }
    return false;
}

// 在设备表中查找 (VID,PID)；返回 nullptr 表示不支持。
const VgnDeviceEntry *findVgnEntry(uint16_t vid, uint16_t pid)
{
    for (const VgnDeviceEntry &e : kVgnDevices) {
        if (e.vid == vid && e.pid == pid) {
            return &e;
        }
    }
    return nullptr;
}

// —— VID 兜底匹配 ——
// 设备型号表随新型号出现持续扩充。对 MouseEnc 族相关的 VID，精确查表失败时
// 按 VID 兜底归入 MouseEnc，避免漏识别。reportId 取该 VID 在表中常见的值
// （0x3554/0x391D→8，0x9981/0x372E→32）。
//
// 注意：此兜底是「尝试性」的——若该接口其实不属于 MouseEnc 协议，查询会因无响应 /
// 校验失败被丢弃，不会误报电量。仅在设备确实走该协议时才产出结果。
bool tryMouseEncFallback(uint16_t vid, VgnDeviceEntry &out)
{
    switch (vid) {
    case 0x3554:  // VGN 鼠标主 VID（F2/Y2/F1 系列），reportId=8
    case 0x391D:  // Dragonfly MasterGT 系列，reportId=8
        out = { vid, 0, Family::MouseEnc, 8, L"VGN Mouse" };
        return true;
    case 0x9981:  // Dragonfly King，reportId=32
    case 0x372E:  // Dragonfly King，reportId=32
        out = { vid, 0, Family::MouseEnc, 32, L"VGN Dragonfly Mouse" };
        return true;
    default:
        return false;
    }
}

bool entryMatchesInterface(const VgnDeviceEntry &entry, const hid_device_info *info)
{
    if (!info) {
        return false;
    }
    if (entry.usagePage != 0 && info->usage_page != entry.usagePage) {
        return false;
    }
    if (entry.usage != 0 && info->usage != entry.usage) {
        return false;
    }
    return true;
}

// —— HID 读写工具 ——
//
// 查询某个设备路径下输出 / 输入 / Feature 报告的数据字节长度（均不含 Report ID 字节）。
// 通过 SetupAPI 打开设备路径对应的 HID 句柄，再用 HidD_GetPreparsedData + HidP_GetCaps
// 读取报告描述符里的 Output/Input/FeatureReportByteLength，这是取得真实报告大小的唯一
// 可靠途径——hidapi 的 hid_device_info 不暴露报告长度字段。
// 三个 out 参数在失败时均回退到 kDefaultReportDataSize。
// 返回 false 表示该接口「既无输出报告也无 Feature 报告」（纯输入接口）——这种接口无法
// 承载任何 VGN 写入式查询（hid_write / hid_send_feature_report 必然被拒，
// ERROR_INVALID_FUNCTION），调用方应直接跳过，避免无谓的打开 / 写入 / 报错。
bool queryReportByteLengths(const char *path, size_t &outDataLen,
                            size_t &inDataLen, size_t &featureDataLen);

// hid_write / hid_send_feature_report 的报文布局：
//   - 编号报告设备要求首字节为 Report ID，之后才是 data。
//   - 对 reportId==0 的设备，首字节填 0，之后是 data（编号报告设备的 0 号报告）。
//   - 对 reportId!=0 的设备，首字节填该 reportId，之后是 data。
//   因此「构造 data → hid_write([reportId] + data)」即可统一处理。
//
// 读响应：hid_read_timeout 同步阻塞读一帧 Input Report，编号报告设备首字节为 reportId。

// 输入报告读取超时（毫秒）。桌面单线程轮询，统一放宽到 500ms 以兼容响应较慢的接收器。
constexpr int kReadTimeoutMs = 500;

// 报告数据字节长度的默认回退值（不含 Report ID 字节）。VGN 多数族为 16B/20B/32B/64B，
// 此值仅在无法从报告描述符读取时兜底；各族的包大小由协议本身决定，不直接用此值构造包。
constexpr size_t kDefaultReportDataSize = 64;

// 把 unsigned short 格式化为 4 位十六进制 wchar（如 0x3554），用于日志。
std::wstring toHex4(unsigned short v)
{
    wchar_t buf[8];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%04X", v);
    return std::wstring(buf);
}

// 把 char*（hidapi 路径，ASCII）转 std::wstring。
std::wstring pathToWString(const char *path)
{
    if (!path) {
        return {};
    }
    return std::wstring(path, path + std::strlen(path));
}

// 安全获取 hidapi 错误信息：只调用 hid_error 一次，并立即拷贝成 std::wstring，
// 脱离 hidapi 内部 buffer 的生命周期。
// hid_error 返回的指针归 hidapi 所有（仅在 hid_close 前/进程堆上有效），
// 对处于失败/半残状态的句柄重复调用会反复触发其内部错误格式化路径（FormatMessageW
// 操作进程堆），是历史上堆损坏崩溃（0xc0000374）的根因，故此处务必只取一次。
std::wstring hidErrorString(hid_device *dev)
{
    const wchar_t *msg = hid_error(dev); // 仅调用一次
    return std::wstring(msg ? msg : L"unknown");
}

// hidapi 返回的设备路径形如 "\\\\?\\hid#vid_391d&pid_... ..."，可直接用于 CreateFileW
// 打开（hidapi 内部也是这么做的）。打开后用 HidD_GetPreparsedData 取报告描述符预处理
// 数据，再 HidP_GetCaps 读出 caps.Output/Input/FeatureReportByteLength。这三个值均已
// 包含 Report ID 字节，故各减 1 得到纯数据长度；不使用 Report ID 的设备减 1 会偏小，
// 但各族的包大小由协议本身决定、不依赖此值，偏小仅影响读取缓冲区大小，安全。
bool queryReportByteLengths(const char *path, size_t &outDataLen,
                            size_t &inDataLen, size_t &featureDataLen)
{
    outDataLen = kDefaultReportDataSize;
    inDataLen = kDefaultReportDataSize;
    featureDataLen = kDefaultReportDataSize;
    if (!path) {
        return false;
    }

    const std::wstring wpath(path, path + std::strlen(path));
    // hidapi 已用共享读方式打开设备；这里仅需读取报告描述符，故以只读 + 共享读写方式打开，
    // 避免与 hidapi 句柄产生独占冲突。
    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(h, &preparsed) || !preparsed) {
        CloseHandle(h);
        return false;
    }

    bool usable = false;
    HIDP_CAPS caps{};
    if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
        if (caps.OutputReportByteLength > 1) {
            outDataLen = caps.OutputReportByteLength - 1;
        }
        if (caps.InputReportByteLength > 1) {
            inDataLen = caps.InputReportByteLength - 1;
        }
        if (caps.FeatureReportByteLength > 1) {
            featureDataLen = caps.FeatureReportByteLength - 1;
        }
        // 该接口既无输出报告也无 Feature 报告（纯输入接口）时无法承载任何写入式查询，
        // hid_write / hid_send_feature_report 必然被拒（ERROR_INVALID_FUNCTION）。
        // 标记为不可用，调用方直接跳过。
        usable = caps.OutputReportByteLength > 0 || caps.FeatureReportByteLength > 0;
    }

    HidD_FreePreparsedData(preparsed);
    CloseHandle(h);
    return usable;
}

// 精确百分比 -> 离散档位（与其它 provider 一致）。
BatteryLevel levelFromPercentage(int percentage)
{
    if (percentage < 0) {
        return BatteryLevel::Unknown;
    }
    if (percentage >= 80) {
        return BatteryLevel::Full;
    }
    if (percentage >= 50) {
        return BatteryLevel::Medium;
    }
    if (percentage >= 20) {
        return BatteryLevel::Low;
    }
    return BatteryLevel::Empty;
}

int percentageFromVoltage(int millivolts)
{
    constexpr int kVoltageLut[] = {
        3050, 3420, 3480, 3540, 3600, 3660, 3720, 3760, 3800, 3840, 3880,
        3920, 3940, 3960, 3980, 4000, 4020, 4040, 4060, 4080, 4110,
    };

    if (millivolts <= 0) {
        return -1;
    }
    if (millivolts <= kVoltageLut[0]) {
        return 0;
    }
    constexpr size_t kLast = sizeof(kVoltageLut) / sizeof(kVoltageLut[0]) - 1;
    if (millivolts >= kVoltageLut[kLast]) {
        return 100;
    }
    for (size_t i = 1; i <= kLast; ++i) {
        if (millivolts <= kVoltageLut[i]) {
            const int lowMv = kVoltageLut[i - 1];
            const int highMv = kVoltageLut[i];
            const int lowPct = static_cast<int>((i - 1) * 5);
            const int span = std::max(1, highMv - lowMv);
            return lowPct + ((millivolts - lowMv) * 5 + span / 2) / span;
        }
    }
    return 100;
}

// 发送一帧 Output Report（reportId + data），返回 hid_write 字节数；<0 表示失败。
// hidapi 的 hid_write 首字节为 Report ID，故实际写入 1 + data.size() 字节。
int writeOutputReport(hid_device *dev, uint8_t reportId, const std::vector<unsigned char> &data)
{
    std::vector<unsigned char> buf;
    buf.reserve(data.size() + 1);
    buf.push_back(reportId);
    buf.insert(buf.end(), data.begin(), data.end());
    return hid_write(dev, buf.data(), buf.size());
}

// 同步发送 / 接收一帧 Feature Report。
// hidapi：hid_send_feature_report 写「[reportId] + data」；
//         hid_get_feature_report 读「[reportId] + 返回数据」（首字节回填 reportId）。
// 返回去掉首字节 reportId 后的 payload；失败返回 std::nullopt。
std::optional<std::vector<unsigned char>> featureReportXfer(hid_device *dev,
                                                             uint8_t reportId,
                                                             const std::vector<unsigned char> &data,
                                                             size_t responseSize)
{
    std::vector<unsigned char> req;
    req.reserve(data.size() + 1);
    req.push_back(reportId);
    req.insert(req.end(), data.begin(), data.end());
    if (hid_send_feature_report(dev, req.data(), req.size()) < 0) {
        LOG_VERBOSE_W(L"[VGN] hid_send_feature_report failed: " + hidErrorString(dev));
        return std::nullopt;
    }
    std::vector<unsigned char> resp(responseSize + 1, 0);
    resp[0] = reportId;
    int n = hid_get_feature_report(dev, resp.data(), resp.size());
    if (n <= 0) {
        LOG_VERBOSE_W(L"[VGN] hid_get_feature_report failed: " + hidErrorString(dev));
        return std::nullopt;
    }
    // 去掉首字节 reportId，返回 payload。
    return std::vector<unsigned char>(resp.begin() + 1, resp.begin() + std::min<size_t>(resp.size(), static_cast<size_t>(n)));
}

// 读一帧 Input Report（hid_read_timeout），返回去掉首字节 reportId 后的 payload。
// 返回 std::nullopt 表示超时或读取失败。bufSize 需包含 reportId 字节。
std::optional<std::vector<unsigned char>> readInputReport(hid_device *dev, size_t bufSize)
{
    std::vector<unsigned char> buf(bufSize, 0);
    int n = hid_read_timeout(dev, buf.data(), buf.size(), kReadTimeoutMs);
    if (n <= 0) {
        if (n < 0) {
            LOG_VERBOSE_W(L"[VGN] hid_read_timeout failed: " + hidErrorString(dev));
        }
        return std::nullopt;
    }
    buf.resize(static_cast<size_t>(n));
    return buf;
}

// —— 通用结果组装 ——
BatteryDevice makeDevice(const std::wstring &id, const std::wstring &name,
                         int percentage, bool charging, bool wired = false)
{
    BatteryDevice device;
    device.id = id;
    device.name = name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = percentage;
    device.level = levelFromPercentage(percentage);
    device.charging = charging;
    device.wired = wired;
    device.connected = true;
    return device;
}

// —— 协议族查询函数 ——
// 每个函数：打开设备 → 发电量查询 → 读响应 → 解析 → 返回 BatteryDevice。
// 返回 std::nullopt 表示本次查询失败（设备休眠 / 无响应 / 校验失败），上层跳过。

// ThreeMode 键盘族：[13,1,...] → 响应 [2]=mode,[3]=charge,[4]=battery。
// Output Report(reportId=0, 64B) + Input Report(64B)。无校验。
std::optional<BatteryDevice> queryThreeMode(hid_device *dev, const VgnDeviceEntry &entry,
                                             const std::wstring &id, const std::wstring &name)
{
    std::vector<unsigned char> req(64, 0);
    req[0] = 13; // ThreeMode 命令
    req[1] = 1;  // GetBasicInfo
    if (writeOutputReport(dev, entry.reportId, req) < 0) {
        LOG_VERBOSE_W(L"[VGN] ThreeMode hid_write failed for " + name);
        return std::nullopt;
    }
    auto resp = readInputReport(dev, 65);
    if (!resp || resp->size() < 5) {
        LOG_VERBOSE_W(L"[VGN] ThreeMode no/short response for " + name);
        return std::nullopt;
    }
    // Input Report 首字节为 reportId（编号报告设备），从第 1 字节起才是 payload。
    // 部分设备 hid_read 不回填 reportId，保守地先定位 payload 起点。
    const auto &r = *resp;
    size_t base = 0;
    if (r.size() >= 2 && r[0] == entry.reportId) {
        base = 1; // 跳过 reportId
    }
    if (base + 5 > r.size()) {
        LOG_VERBOSE_W(L"[VGN] ThreeMode payload too short for " + name);
        return std::nullopt;
    }
    // getBasicInfo 解析：{mode:e[2], charge:e[3], battery:e[4]}。
    // payload[0]=13(ThreeMode), payload[1]=1(GetBasicInfo)。
    const int battery = r[base + 4];
    const int charge = r[base + 3];
    LOG_VERBOSE_W(L"[VGN] ThreeMode " + name + L" battery=" + std::to_wstring(battery) +
          L"% charge=" + std::to_wstring(charge));
    return makeDevice(id, name, battery, charge != 0);
}

// Weisheng 键盘族：[0,0,26,6,0,0,0] → 响应 [2]==26 时 o=slice(7)，level=o[0]、charge=o[1]。
// Output Report(reportId=4, 7B) + Input Report。无校验。响应帧带 7 字节头，o 才是 payload。
std::optional<BatteryDevice> queryWeisheng(hid_device *dev, const VgnDeviceEntry &entry,
                                            const std::wstring &id, const std::wstring &name)
{
    // 失败时最多重试 5 次，每次发完等响应。
    for (int attempt = 0; attempt < 5; ++attempt) {
        std::vector<unsigned char> req = {0, 0, 26, 6, 0, 0, 0}; // GetBatterLevel=26
        if (writeOutputReport(dev, entry.reportId, req) < 0) {
            LOG_VERBOSE_W(L"[VGN] Weisheng hid_write failed for " + name);
            return std::nullopt;
        }
        auto resp = readInputReport(dev, 16); // reportId(1) + 报文（≥8B）
        if (!resp || resp->size() < 8) {
            continue;
        }
        const auto &r = *resp;
        size_t base = 0;
        if (!r.empty() && r[0] == entry.reportId) {
            base = 1;
        }
        if (base + 9 > r.size()) {
            continue;
        }
        // readBuffer 解析：i[2]==26 → o=i.slice(7)，battery.level=o[0]、charging=o[1]。
        if (r[base + 2] != 26) {
            continue;
        }
        const int battery = r[base + 7];
        const int charge = r[base + 8];
        LOG_VERBOSE_W(L"[VGN] Weisheng " + name + L" battery=" + std::to_wstring(battery) +
              L"% charge=" + std::to_wstring(charge));
        return makeDevice(id, name, battery, charge != 0);
    }
    LOG_VERBOSE_W(L"[VGN] Weisheng no battery response after retries for " + name);
    return std::nullopt;
}

// Beiying 键盘族：[74,1,0,0,0] pad 到 19B，末字节 CRC=(reportId+Σbytes)&0xFF。
// Output Report + Input Report。响应：n[0]==74 且 n.length>4 → level=n[4]、
// charging=((n[5]>>4)&0x0F)==1。
std::optional<BatteryDevice> queryBeiying(hid_device *dev, const VgnDeviceEntry &entry,
                                          const std::wstring &id, const std::wstring &name)
{
    std::vector<unsigned char> req = {74, 1, 0, 0, 0};
    req.resize(19, 0);
    // CRC = (reportId + Σbytes[0..18]) & 0xFF。
    unsigned crc = entry.reportId;
    for (unsigned char b : req) {
        crc += b;
    }
    req.push_back(static_cast<unsigned char>(crc & 0xFF)); // 第 20 字节（索引 19）为 CRC
    if (writeOutputReport(dev, entry.reportId, req) < 0) {
        LOG_VERBOSE_W(L"[VGN] Beiying hid_write failed for " + name);
        return std::nullopt;
    }
    auto resp = readInputReport(dev, 32);
    if (!resp || resp->size() < 6) {
        LOG_VERBOSE_W(L"[VGN] Beiying no/short response for " + name);
        return std::nullopt;
    }
    const auto &r = *resp;
    size_t base = 0;
    if (!r.empty() && r[0] == entry.reportId) {
        base = 1;
    }
    if (base + 6 > r.size()) {
        return std::nullopt;
    }
    if (r[base] != 74) {
        LOG_VERBOSE_W(L"[VGN] Beiying response cmd mismatch (" +
                   std::to_wstring(r[base]) + L"!=74) for " + name);
        return std::nullopt;
    }
    const int battery = r[base + 4];
    const bool charging = ((r[base + 5] >> 4) & 0x0F) == 1;
    LOG_VERBOSE_W(L"[VGN] Beiying " + name + L" battery=" + std::to_wstring(battery) +
          L"% charging=" + (charging ? L"1" : L"0"));
    return makeDevice(id, name, battery, charging);
}

// VgnRyMouse 鼠标族：Feature Report(reportId=0, 64B)，反码 CRC=255-(Σ&0xFF)。
// 查询前需初始化：发 [247,...] 探测，再发 [246,5,0,0,0,0,0] 切到 2.4G。
// 电量命令 [247,0,0,0,0,0,0]；响应 i[4]==1 或 i[0]==0 失败，否则 level=i[2]、charging=false。
std::optional<BatteryDevice> queryVgnRyMouse(hid_device *dev, const VgnDeviceEntry &entry,
                                              const std::wstring &id, const std::wstring &name)
{
    // 初始化：状态探测（响应 i[4]==1 视为未连 2.4G）。
    auto crcPad = [](std::vector<unsigned char> pkg) {
        pkg.resize(64, 0);
        unsigned sum = 0;
        for (unsigned char b : pkg) {
            sum += b;
        }
        pkg.back() = static_cast<unsigned char>(255 - (sum & 0xFF));
        return pkg;
    };

    // 1) 先探测连接状态，未连接时不继续切模式。
    {
        std::vector<unsigned char> probe = {247, 0, 0, 0, 0, 0, 0};
        probe = crcPad(std::move(probe));
        auto status = featureReportXfer(dev, entry.reportId, probe, 64);
        if (status && status->size() >= 5 && ((*status)[4] == 1 || (*status)[0] == 0)) {
            LOG_VERBOSE_W(L"[VGN] VgnRyMouse " + name + L" 2.4G not connected on probe");
            return std::nullopt;
        }
    }

    // 2) 切换到 2.4G 模式。
    {
        std::vector<unsigned char> sw = {246, 5, 0, 0, 0, 0, 0};
        sw = crcPad(std::move(sw));
        featureReportXfer(dev, entry.reportId, sw, 64);
    }

    // 3) 查询电量。
    std::vector<unsigned char> req = {247, 0, 0, 0, 0, 0, 0};
    req = crcPad(std::move(req));
    auto resp = featureReportXfer(dev, entry.reportId, req, 64);
    if (!resp || resp->size() < 5) {
        LOG_VERBOSE_W(L"[VGN] VgnRyMouse no/short response for " + name);
        return std::nullopt;
    }
    const auto &i = *resp;
    // i[4]==1 或 i[0]==0 视为「2.4G 请求数据失败」。
    if (i[4] == 1 || i[0] == 0) {
        LOG_VERBOSE_W(L"[VGN] VgnRyMouse " + name + L" 2.4G not ready (i[4]=" +
              std::to_wstring(i[4]) + L")");
        return std::nullopt;
    }
    const int battery = i[2];
    LOG_VERBOSE_W(L"[VGN] VgnRyMouse " + name + L" battery=" + std::to_wstring(battery) +
          L"% (charging fixed false)");
    return makeDevice(id, name, battery, false);
}

// Yongjiaxin 鼠标族：Output Report(reportId=0, 64B)，[0x55,48,...] + CRC=85-(Σ&0xFF)。
// 响应 Input Report：n[1]==48 时 receivedData=slice(8)；[1]==1 充电，否则 level=[0]。
// 查询前需唤醒；桌面轮询直接查，无响应即视为休眠跳过。
std::optional<BatteryDevice> queryYongjiaxin(hid_device *dev, const VgnDeviceEntry &entry,
                                              const std::wstring &id, const std::wstring &name)
{
    auto build = [](uint8_t cmd) {
        std::vector<unsigned char> pkg(64, 0);
        pkg[0] = 0x55; // 头
        pkg[1] = cmd;
        unsigned sum = 0;
        for (unsigned char b : pkg) {
            sum += b;
        }
        pkg.back() = static_cast<unsigned char>(85 - (sum & 0xFF)); // CRC
        return pkg;
    };

    // 先唤醒（MouseOnline=237）。部分设备会先回 Online 帧，所以后面最多读两帧，
    // 直到拿到 GetBattery(48) 响应。
    writeOutputReport(dev, entry.reportId, build(237));

    // 查电量（GetBattery=48）。
    if (writeOutputReport(dev, entry.reportId, build(48)) < 0) {
        LOG_VERBOSE_W(L"[VGN] Yongjiaxin hid_write failed for " + name);
        return std::nullopt;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto resp = readInputReport(dev, 65);
        if (!resp || resp->size() < 10) {
            continue;
        }
        const auto &n = *resp;
        size_t base = 0;
        if (!n.empty() && n[0] == entry.reportId) {
            base = 1;
        }
        // n[1]==48（GetBattery）。其它命令响应（如 MouseOnline）跳过。
        if (base + 1 >= n.size() || n[base + 1] != 48) {
            continue;
        }
        // receivedData = n.slice(8)；即从 base+8 起。
        if (base + 10 > n.size()) {
            return std::nullopt;
        }
        const int flag = n[base + 9];  // receivedData[1]
        if (flag == 1) {
            LOG_VERBOSE_W(L"[VGN] Yongjiaxin " + name + L" charging/wired");
            return makeDevice(id, name, -1, true, true);
        }
        const int battery = n[base + 8]; // receivedData[0]
        LOG_VERBOSE_W(L"[VGN] Yongjiaxin " + name + L" battery=" + std::to_wstring(battery) + L"%");
        return makeDevice(id, name, battery, false);
    }

    LOG_VERBOSE_W(L"[VGN] Yongjiaxin " + name + L" no GetBattery response (likely asleep)");
    return std::nullopt;
}

// VgnLdMs 鼠标族：Output Report(reportId=160, 32B)，[2,37,...] + CRC=255-(Σ&0xFF)。
// 响应 Input Report：i==2(GetDeviceInfo) 且 n[1]==37 → level=n[2]、charging=isWired。
// 该族「充电」以是否接有线近似，统一按 charging=false（无线场景）处理。
std::optional<BatteryDevice> queryVgnLdMs(hid_device *dev, const VgnDeviceEntry &entry,
                                          const std::wstring &id, const std::wstring &name)
{
    std::vector<unsigned char> req = {2, 37, 0, 0, 0, 0, 0}; // GetDeviceInfo=2，子参 37
    req.resize(32, 0);
    unsigned sum = 0;
    for (unsigned char b : req) {
        sum += b;
    }
    req.push_back(static_cast<unsigned char>(255 - (sum & 0xFF))); // 第 33 字节 CRC
    if (writeOutputReport(dev, entry.reportId, req) < 0) {
        LOG_VERBOSE_W(L"[VGN] VgnLdMs hid_write failed for " + name);
        return std::nullopt;
    }
    auto resp = readInputReport(dev, 33);
    if (!resp || resp->size() < 4) {
        LOG_VERBOSE_W(L"[VGN] VgnLdMs no/short response for " + name);
        return std::nullopt;
    }
    const auto &n = *resp;
    size_t base = 0;
    if (!n.empty() && n[0] == entry.reportId) {
        base = 1;
    }
    // i==GetDeviceInfo(2) 且 n[1]==37。
    if (base + 3 > n.size() || n[base] != 2 || n[base + 1] != 37) {
        LOG_VERBOSE_W(L"[VGN] VgnLdMs response mismatch for " + name);
        return std::nullopt;
    }
    const int battery = n[base + 2];
    const bool wired = !entry.receiver;
    LOG_VERBOSE_W(L"[VGN] VgnLdMs " + name + L" battery=" + std::to_wstring(battery) +
          L"% wired=" + (wired ? L"1" : L"0"));
    return makeDevice(id, name, battery, wired, wired);
}

// Arbit 键盘族：Output Report(reportId=0, 64B)，0x55 帧 + 加和校验。
// 电量随「功能配置块」读出：cmd 5(GetFunc) 分两次读 56B chunk（偏移 0/56），拼成
// funcBuf 后解析：level=funcBuf[32]&0x7F、charging=(funcBuf[37]!=0)。
std::optional<BatteryDevice> queryArbit(hid_device *dev, const VgnDeviceEntry &entry,
                                         const std::wstring &id, const std::wstring &name)
{
    constexpr size_t kPkt = 64;          // 包大小 64
    constexpr unsigned char kHeader = 0x55; // 帧头
    constexpr unsigned char kRespHead = 0xAA; // 响应头
    constexpr unsigned char kGetFunc = 5;    // 命令号

    // sendFormattedReport(cmd, offset, payload)：
    //   o[0]=0x55, o[1]=cmd, o[2]=0, o[3]=CRC(Σbytes[4..63]&0xFF),
    //   o[4]=payloadLen, o[5]=offsetLo, o[6]=offsetHi, o[7]=0, payload@o[8..]
    auto sendFormatted = [&](unsigned char cmd, uint16_t offset,
                             const std::vector<unsigned char> &payload) -> bool {
        std::vector<unsigned char> o(kPkt, 0);
        o[0] = kHeader;
        o[1] = cmd;
        o[4] = static_cast<unsigned char>(payload.size());
        o[5] = static_cast<unsigned char>(offset & 0xFF);
        o[6] = static_cast<unsigned char>((offset >> 8) & 0xFF);
        for (size_t i = 0; i < payload.size() && 8 + i < kPkt; ++i) {
            o[8 + i] = payload[i];
        }
        unsigned crc = 0;
        for (size_t a = 4; a < kPkt; ++a) {
            crc = (crc + o[a]) & 0xFF;
        }
        o[3] = static_cast<unsigned char>(crc);
        return writeOutputReport(dev, entry.reportId, o) >= 0;
    };

    // GetFunc：profile 偏移 0；chunk 偏移 0、56，各读 56B，拼到 funcBuf[0..111]。
    std::vector<unsigned char> funcBuf(112, 0);
    bool gotAny = false;
    for (size_t off = 0; off < funcBuf.size(); off += 56) {
        const size_t want = std::min<size_t>(56, funcBuf.size() - off);
        if (!sendFormatted(kGetFunc, static_cast<uint16_t>(off),
                           std::vector<unsigned char>(want, 0))) {
            break;
        }
        auto resp = readInputReport(dev, kPkt + 1);
        if (!resp || resp->size() < 9) {
            break;
        }
        const auto &c = *resp;
        size_t cb = 0;
        if (!c.empty() && c[0] == entry.reportId) {
            cb = 1;
        }
        // 响应头 0xAA 且 cmd 一致。
        if (cb + 5 > c.size() || c[cb] != kRespHead || c[cb + 1] != kGetFunc) {
            break;
        }
        const size_t len = c[cb + 4];
        const size_t avail = std::min({len, want, c.size() - (cb + 8)});
        if (avail == 0) {
            break;
        }
        std::copy(c.begin() + cb + 8, c.begin() + cb + 8 + avail, funcBuf.begin() + off);
        gotAny = true;
    }
    if (!gotAny) {
        LOG_VERBOSE_W(L"[VGN] Arbit GetFunc no response for " + name);
        return std::nullopt;
    }
    const int battery = funcBuf[32] & 0x7F;
    const bool charging = funcBuf[37] != 0;
    LOG_VERBOSE_W(L"[VGN] Arbit " + name + L" battery=" + std::to_wstring(battery) +
          L"% charging=" + (charging ? L"1" : L"0") +
          L" funcBuf[37]=" + std::to_wstring(funcBuf[37]));
    return makeDevice(id, name, battery, charging);
}

// ByKeyboard 键盘族：电量查询最终返回 [level,statusByte]。
// 这里优先走 520B Feature Report；若设备/接口不支持 Feature，则回退 63B Output/Input。
std::optional<BatteryDevice> queryByKeyboard(hid_device *dev, const VgnDeviceEntry &entry,
                                             const std::wstring &id, const std::wstring &name)
{
    auto parseBattery = [&](const std::vector<unsigned char> &resp)
        -> std::optional<BatteryDevice> {
        if (std::none_of(resp.begin(), resp.end(), [](unsigned char b) { return b != 0; })) {
            return std::nullopt;
        }
        size_t base = 0;
        if (entry.reportId != 0 && !resp.empty() && resp[0] == entry.reportId) {
            base = 1;
        }
        if (base + 10 > resp.size()) {
            return std::nullopt;
        }
        const int battery = resp[base + 8];
        const int status = resp[base + 9];
        if (battery > 100) {
            return std::nullopt;
        }
        const bool charging = ((status >> 4) & 0x0F) != 0;
        const bool full = (status & 0x0F) != 0;
        const bool wired = !entry.receiver && (charging || full);
        LOG_VERBOSE_W(L"[VGN] ByKeyboard " + name + L" battery=" + std::to_wstring(battery) +
              L"% status=0x" + toHex4(static_cast<unsigned short>(status)) +
              L" charging=" + (charging ? L"1" : L"0") +
              L" full=" + (full ? L"1" : L"0"));
        return makeDevice(id, name, battery, charging || full, wired);
    };

    const std::vector<unsigned char> command = {135, 0, 0, 1, 0, 2, 0};

    std::vector<unsigned char> featureReq(520, 0);
    std::copy(command.begin(), command.end(), featureReq.begin());
    if (auto featureResp = featureReportXfer(dev, entry.reportId, featureReq, 520)) {
        if (auto parsed = parseBattery(*featureResp)) {
            return parsed;
        }
    }

    std::vector<unsigned char> outputReq(63, 0);
    std::copy(command.begin(), command.end(), outputReq.begin());
    if (writeOutputReport(dev, entry.reportId, outputReq) < 0) {
        LOG_VERBOSE_W(L"[VGN] ByKeyboard hid_write failed for " + name);
        return std::nullopt;
    }
    auto resp = readInputReport(dev, 64);
    if (!resp) {
        LOG_VERBOSE_W(L"[VGN] ByKeyboard no response for " + name);
        return std::nullopt;
    }
    return parseBattery(*resp);
}

// VgnKc2 鼠标族：Output Report(reportId=179, 20B)，[6,0,...]。
// 响应 Input Report：i==6 → level=n[19]&0x7F、charging=!!(n[19]&0x80)。
std::optional<BatteryDevice> queryVgnKc2(hid_device *dev, const VgnDeviceEntry &entry,
                                          const std::wstring &id, const std::wstring &name)
{
    if (entry.receiver) {
        std::vector<unsigned char> statusReq(63, 0);
        statusReq[0] = 3; // getMsConnectionStatus，固定 reportId=181
        if (writeOutputReport(dev, 181, statusReq) >= 0) {
            auto status = readInputReport(dev, 64);
            if (status && status->size() >= 7) {
                const auto &s = *status;
                size_t base = 0;
                if (!s.empty() && s[0] == 181) {
                    base = 1;
                }
                if (base + 7 <= s.size() && s[base] == 3 && s[base + 6] != 1) {
                    LOG_VERBOSE_W(L"[VGN] VgnKc2 " + name + L" receiver reports device sleeping/offline");
                    return std::nullopt;
                }
            }
        }
    }

    std::vector<unsigned char> req(20, 0);
    req[0] = 6; // getDeviceInfo
    if (writeOutputReport(dev, entry.reportId, req) < 0) {
        LOG_VERBOSE_W(L"[VGN] VgnKc2 hid_write failed for " + name);
        return std::nullopt;
    }
    auto resp = readInputReport(dev, 21);
    if (!resp || resp->size() < 20) {
        LOG_VERBOSE_W(L"[VGN] VgnKc2 no/short response for " + name);
        return std::nullopt;
    }
    const auto &n = *resp;
    size_t base = 0;
    if (!n.empty() && n[0] == entry.reportId) {
        base = 1;
    }
    if (base + 20 > n.size() || n[base] != 6) {
        LOG_VERBOSE_W(L"[VGN] VgnKc2 response mismatch for " + name);
        return std::nullopt;
    }
    const int levelByte = n[base + 19];
    const int battery = levelByte & 0x7F;
    const bool charging = (levelByte & 0x80) != 0;
    LOG_VERBOSE_W(L"[VGN] VgnKc2 " + name + L" battery=" + std::to_wstring(battery) +
          L"% charging=" + (charging ? L"1" : L"0"));
    return makeDevice(id, name, battery, charging);
}

// MouseEnc 鼠标族：Output Report(reportId=8/32，16B)，0x55 反码 CRC + 尾字节 239。
// 流程：
//   1) 包格式：[cmd, 0×14 字节中 13 个, ..., 239]，共 16B。
//      其中 [15]=239 固定；CRC = 0x55 - (Σpkg[0..14] & 0xFF)；
//      最终 [15] = CRC - reportId。
//      注意 CRC 对 [0..14] 求和，不含原 [15]=239。
//   2) 发 DeviceOnLine(3) → 读响应，receivedData[5]>0 视为在线。
//   3) 在线则发 BatteryLevel(4) → 响应 level=[5]、charging=([6]==1)、voltage=[7..8]。
// 响应判别：reportId 匹配 && receivedData[1]==0 && receivedData[0]==cmd 回显。
std::optional<BatteryDevice> queryMouseEnc(hid_device *dev, const VgnDeviceEntry &entry,
                                            const std::wstring &id, const std::wstring &name)
{
    constexpr uint8_t kCmdDeviceOnLine = 3;
    constexpr uint8_t kCmdBatteryLevel = 4;
    constexpr size_t kPktSize = 16;

    // 构造并发送一条命令，同步读一帧响应；返回去 reportId 头后的 payload。
    // 校验：响应 reportId 匹配、payload[1]==0、payload[0]==cmd 回显。
    auto sendCmd = [&](uint8_t cmd) -> std::optional<std::vector<unsigned char>> {
        std::vector<unsigned char> req(kPktSize, 0);
        req[0] = cmd;
        req[kPktSize - 1] = 239; // 固定尾字节
        // CRC = 0x55 - (Σreq[0..14] & 0xFF)；写入 [15] = CRC - reportId。
        unsigned sum = 0;
        for (size_t i = 0; i < kPktSize - 1; ++i) {
            sum += req[i];
        }
        const unsigned char crc = static_cast<unsigned char>(0x55 - (sum & 0xFF));
        req[kPktSize - 1] = static_cast<unsigned char>((crc - entry.reportId) & 0xFF);

        if (writeOutputReport(dev, entry.reportId, req) < 0) {
            LOG_VERBOSE_W(L"[VGN] MouseEnc hid_write failed for " + name);
            return std::nullopt;
        }
        // 读响应帧：reportId(1) + 16B payload。校验响应 reportId===本族 reportId。
        auto resp = readInputReport(dev, kPktSize + 1);
        if (!resp || resp->size() < kPktSize) {
            return std::nullopt;
        }
        const auto &r = *resp;
        size_t base = 0;
        if (!r.empty() && r[0] == entry.reportId) {
            base = 1; // 跳过 reportId
        }
        if (base + kPktSize > r.size()) {
            return std::nullopt;
        }
        // 响应判别：receivedData[1]==0 且 receivedData[0]==cmd 回显。
        if (r[base + 1] != 0 || r[base] != cmd) {
            return std::nullopt;
        }
        return std::vector<unsigned char>(r.begin() + base, r.begin() + base + kPktSize);
    };

    // 1) 探测在线。DeviceOnLine 响应：online = receivedData[5]>0。
    auto online = sendCmd(kCmdDeviceOnLine);
    if (!online || online->size() < 6) {
        LOG_VERBOSE_W(L"[VGN] MouseEnc " + name + L" DeviceOnLine no valid response (asleep?)");
        return std::nullopt;
    }
    const bool isOnline = (*online)[5] > 0;
    if (!isOnline) {
        LOG_VERBOSE_W(L"[VGN] MouseEnc " + name + L" reports offline (receivedData[5]=0)");
        return std::nullopt;
    }

    // 2) 查电量。BatteryLevel 响应：level=[5]、charging=([6]==1)、voltage=[7..8]。
    auto bat = sendCmd(kCmdBatteryLevel);
    if (!bat || bat->size() < 7) {
        LOG_VERBOSE_W(L"[VGN] MouseEnc " + name + L" BatteryLevel no valid response");
        return std::nullopt;
    }
    const int rawBattery = (*bat)[5];
    const bool charging = (*bat)[6] == 1;
    int voltage = 0;
    int battery = rawBattery;
    if (bat->size() >= 9) {
        voltage = (static_cast<int>((*bat)[7]) << 8) | static_cast<int>((*bat)[8]);
        const int voltageBattery = percentageFromVoltage(voltage);
        if (voltageBattery >= 0) {
            battery = voltageBattery;
        }
    }
    LOG_VERBOSE_W(L"[VGN] MouseEnc " + name + L" battery=" + std::to_wstring(battery) +
          L"% raw=" + std::to_wstring(rawBattery) +
          L" voltage=" + std::to_wstring(voltage) +
          L" charging=" + (charging ? L"1" : L"0"));
    return makeDevice(id, name, battery, charging);
}

// 按族分派。返回 std::nullopt 表示本次查询失败 / 设备休眠。
std::optional<BatteryDevice> queryByFamily(hid_device *dev, const VgnDeviceEntry &entry,
                                            const std::wstring &id, const std::wstring &name)
{
    switch (entry.family) {
    case Family::ThreeMode:  return queryThreeMode(dev, entry, id, name);
    case Family::Weisheng:   return queryWeisheng(dev, entry, id, name);
    case Family::Beiying:    return queryBeiying(dev, entry, id, name);
    case Family::VgnRyMouse: return queryVgnRyMouse(dev, entry, id, name);
    case Family::Yongjiaxin: return queryYongjiaxin(dev, entry, id, name);
    case Family::VgnLdMs:    return queryVgnLdMs(dev, entry, id, name);
    case Family::Arbit:      return queryArbit(dev, entry, id, name);
    case Family::ByKeyboard: return queryByKeyboard(dev, entry, id, name);
    case Family::VgnKc2:     return queryVgnKc2(dev, entry, id, name);
    case Family::MouseEnc:   return queryMouseEnc(dev, entry, id, name);
    }
    return std::nullopt;
}
} // namespace

std::wstring VgnHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> VgnHidProvider::readDevices()
{
    std::vector<BatteryDevice> devices;

    if (hid_init() != 0) {
        LOG_ERR("[VGN] hid_init failed");
        return devices;
    }

    // 同一物理设备常暴露多个接口（标准键盘/鼠标输入 + vendor-defined 配置接口）。
    // 其中只有部分接口能成功 hid_write / 响应电量协议，且 hidapi 枚举顺序不稳定：
    // 某轮排在第一的接口可能恰好是被系统驱动独占 / 会 hid_write 失败的接口。
    //
    // 因此采用「收集候选 → 缓存入口 → 只查缓存接口」策略：
    //   1) 第一遍扫描：按 (VID,PID) 分组，把每个设备的所有候选接口（已通过协议族
    //      匹配 + usage 过滤 + 报告描述符预筛）收集起来，避免遗漏 vendor-defined 接口。
    //   2) 第二遍查询：若该设备已有缓存的「成功接口 path」且仍在本轮枚举里，则只查它
    //      一个（设备休眠导致失败时也保留缓存、不回退全量遍历，避免反复扫全部接口）；
    //      否则全量发现：逐个试候选接口，任一成功即停止并写回缓存；全部失败记一次失败。
    struct VidPid { uint16_t vid; uint16_t pid; };
    auto vidPidKey = [](uint16_t v, uint16_t p) { return static_cast<uint64_t>(v) << 16 | p; };

    // 候选接口记录：设备信息指针 + 解析出的协议族 entry（可能是表项或兜底临时项）。
    struct Candidate {
        const hid_device_info *info;
        VgnDeviceEntry entry; // 拷贝（兜底临时项的生命周期仅在该次循环内）
        bool usedFallback;
    };
    // 按 devKey 分组的候选列表，插入顺序保持枚举顺序。
    std::vector<std::pair<uint64_t, std::vector<Candidate>>> grouped;
    std::unordered_set<uint64_t> seenDevKeys;
    grouped.reserve(32);

    // 收集每条 hid_enumerate 链的头指针：候选接口记录里的 info 指向链表节点，
    // 第二遍查询还要解引用，故所有链表统一在函数末尾释放。
    std::vector<hid_device_info *> enumHeads;
    enumHeads.reserve(sizeof(kVgnVendorIds) / sizeof(kVgnVendorIds[0]));

    for (uint16_t vid : kVgnVendorIds) {
        hid_device_info *enumHead = hid_enumerate(vid, 0);
        if (!enumHead) {
            continue;
        }
        enumHeads.push_back(enumHead);
        for (hid_device_info *cur = enumHead; cur; cur = cur->next) {
            // 解析协议族：先精确查表；表外 PID 对 MouseEnc 相关 VID 按兜底归入。
            const VgnDeviceEntry *entry = findVgnEntry(cur->vendor_id, cur->product_id);
            Candidate cand;
            cand.info = cur;
            cand.usedFallback = false;
            if (entry) {
                cand.entry = *entry;
            } else if (tryMouseEncFallback(cur->vendor_id, cand.entry)) {
                // 兜底仅对 vendor-defined 接口尝试（排除标准鼠标 / 键盘输入接口），
                // 避免对 usage_page=1 的常规输入接口无谓发包。
                if (cur->usage_page <= 0x0B) {
                    continue;
                }
                cand.entry.pid = cur->product_id;
                cand.usedFallback = true;
            } else {
                continue;
            }
            if (!entryMatchesInterface(cand.entry, cur)) {
                continue;
            }

            const uint64_t devKey = vidPidKey(cur->vendor_id, cur->product_id);
            if (seenDevKeys.insert(devKey).second) {
                grouped.push_back({devKey, {}});
            }
            for (auto &g : grouped) {
                if (g.first == devKey) {
                    g.second.push_back(std::move(cand));
                    break;
                }
            }
        }
    }

    // 第二遍：对每个设备依次尝试候选接口直到成功。
    //
    // 接口缓存：hidapi 枚举顺序不稳定，且同一设备常只有一个接口能成功 hid_write。
    // 记住上次成功的接口 path，下轮优先（且只先）查它；命中即跳过该设备其余接口的
    // 无谓发包。缓存接口本轮失败（设备休眠/被独占）时回退该设备其余接口的全量遍历，
    // 成功后更新缓存；缓存 path 已不在枚举结果里（拔插/换口）时自然失效。
    auto tryInterface = [&](const Candidate &cand) -> std::optional<BatteryDevice> {
        const hid_device_info *cur = cand.info;
        const std::wstring path = pathToWString(cur->path);

        LOG_VERBOSE_W(L"[VGN] interface: vid=0x" + toHex4(cur->vendor_id) +
              L" pid=0x" + toHex4(cur->product_id) +
              L" usage_page=0x" + toHex4(cur->usage_page) +
              L" usage=0x" + toHex4(cur->usage) +
              L" family=" + std::to_wstring(static_cast<int>(cand.entry.family)) +
              (cand.usedFallback ? L" (VID fallback)" : L"") +
              L" path=" + path);

        const std::wstring id = L"vgn:" + path;
        std::wstring name = cur->product_string ? cur->product_string : L"";
        if (name.empty()) {
            name = cand.entry.name;
        }

        // 先用报告描述符预筛：既无输出报告也无 Feature 报告的接口（如纯输入接口）无法
        // 承载任何 VGN 写入式查询（hid_write / hid_send_feature_report 必然被拒，
        // ERROR_INVALID_FUNCTION），这里提前跳过，避免每轮对每个此类接口都打开 / 写入 /
        // 报错刷屏。多接口接收器（如 F2 Pro Max Dongle 常暴露 ~9 个接口）只有其中少数
        // 接口可写，预筛后候选集显著收敛。
        size_t outDataLen = kDefaultReportDataSize;
        size_t inDataLen = kDefaultReportDataSize;
        size_t featureDataLen = kDefaultReportDataSize;
        if (!queryReportByteLengths(cur->path, outDataLen, inDataLen, featureDataLen)) {
            LOG_VERBOSE_W(L"[VGN]   -> skipped (interface has no writable report) path=" + path);
            return std::nullopt; // 试同设备下一个候选接口
        }

        hid_device *dev = hid_open_path(cur->path);
        if (!dev) {
            LOG_VERBOSE_W(L"[VGN]   hid_open_path failed for " + name + L": " +
                       hidErrorString(nullptr));
            return std::nullopt; // 试同设备下一个候选接口
        }

        std::optional<BatteryDevice> result;
        try {
            result = queryByFamily(dev, cand.entry, id, name);
            if (result) {
                LOG_VERBOSE_W(L"[VGN]   " + name + L" = " +
                      std::to_wstring(result->percentage) +
                      L"% charging=" + (result->charging ? L"1" : L"0"));
            } else {
                LOG_VERBOSE_W(L"[VGN]   " + name + L" query returned no data on this interface");
            }
        } catch (const std::exception &e) {
            LOG_ERR_W(L"[VGN]   query threw for " + name + L": " +
                      std::wstring(e.what(), e.what() + std::strlen(e.what())));
        } catch (...) {
            LOG_ERR_W(L"[VGN]   query threw unknown exception for " + name);
        }

        hid_close(dev);
        return result;
    };

    for (auto &g : grouped) {
        bool got = false;
        const char *successPath = nullptr; // 本轮真正成功的接口 path（用于刷新缓存）

        // —— 缓存即权威 ——
        // hidapi 枚举顺序不稳定，且同一设备常只有一个接口能成功响应电量协议。一旦在
        // 某轮发现可用的接口 path，就把它当作该设备的「唯一查询入口」长期记住：此后每轮
        // 只查这一个接口。这样即便设备休眠（该接口本轮无响应）也只浪费一次查询，而不是
        // 回去把全部接口又轮询一遍——后者正是历史日志噪音的来源。
        //
        // 只有两种情况需要重新全量发现可用接口：
        //   1) 首次见到该设备（无缓存）；
        //   2) 缓存的 path 已不在本轮枚举结果里（设备拔插 / 换口 / 重新枚举后 path 变了）。
        // 缓存 path 仍在但本轮无响应时，视为设备暂时休眠，保留缓存，下一轮继续只查它。
        const auto cacheIt = m_lastGoodPath.find(g.first);
        const bool hasCache = cacheIt != m_lastGoodPath.end();

        // 缓存 path 是否仍在本轮枚举结果里？
        const Candidate *cachedCand = nullptr;
        if (hasCache) {
            for (const Candidate &cand : g.second) {
                if (std::strcmp(cand.info->path, cacheIt->second.c_str()) == 0) {
                    cachedCand = &cand;
                    break;
                }
            }
        }
        const bool cacheAlive = cachedCand != nullptr;

        if (hasCache && !cacheAlive) {
            // 缓存的接口已消失（拔插 / 换口）：清掉死路径，本轮走全量发现找新接口。
            m_lastGoodPath.erase(g.first);
        }

        if (cacheAlive) {
            // 只查缓存接口一个，无论成败都不再试该设备其余接口。
            LOG_VERBOSE_W(L"[VGN]   querying cached interface only");
            if (auto result = tryInterface(*cachedCand)) {
                devices.push_back(*result);
                got = true;
                successPath = cachedCand->info->path;
            }
            // 失败时保留缓存（设备休眠），不回退到全量遍历。
        } else {
            // 无缓存 / 缓存已失效：全量发现。逐个试候选接口直到成功。
            for (const Candidate &cand : g.second) {
                if (auto result = tryInterface(cand)) {
                    devices.push_back(*result);
                    got = true;
                    successPath = cand.info->path;
                    break; // 同设备已成功，不再试其它接口
                }
            }
        }

        if (got && successPath) {
            m_lastGoodPath[g.first] = successPath;
        }

        if (!got && !g.second.empty()) {
            const auto &cand0 = g.second.front();
            std::wstring name = cand0.info->product_string ? cand0.info->product_string : L"";
            if (name.empty()) {
                name = cand0.entry.name;
            }
            // 汇总为 WARN：这是「该周期设备确实读不到」的真实信号（与 BatteryManager 标
            // (stale) 一致）。设备休眠时只查缓存接口一个，故此处每轮只触发一次 WARN，
            // 不再伴随多条单接口 hid_write 失败。消息末尾注明本轮实际查询方式：
            //   - cached:只查了缓存接口（设备休眠 / 被独占，缓存仍有效所以没回退全量）；
            //   - scanned:N 个候选接口全试过都没成功（首次发现 / 缓存失效后的全量遍历）。
            if (cacheAlive) {
                LOG_WARN_W(L"[VGN]   " + name + L" query returned no data this cycle (cached)");
            } else {
                LOG_WARN_W(L"[VGN]   " + name + L" query returned no data this cycle "
                           L"(scanned " + std::to_wstring(g.second.size()) + L" interface(s))");
            }
        }
    }

    for (hid_device_info *head : enumHeads) {
        hid_free_enumeration(head);
    }

    LOG_VERBOSE_W(L"[VGN] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
