#include "AulaHidProvider.h"
#include "util/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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
// —— 协议常量 ——
// AULA 品牌专属 VendorId（vendorId = 3141 = 0x0C45）。
constexpr uint16_t kAulaVendorId = 0x0C45;

// —— GET_DEVICE_INFO 协议族 ——
//
// AULA / 关联品牌同时存在两代固件协议，命令号、contentSize、尾包标记都不同：
//
//   新协议（如 F87ProV2D）：
//     - cmd = 0x10
//     - contentSize = 56
//     - 请求头 byte[6] = 「是否尾包」标志（最后一包置 1，其余 0）。
//     - 响应同头格式，battery@17 / chargeStatus@18。
//
//   旧协议（早期型号）：
//     - cmd = 0xA0
//     - contentSize = 64
//     - 请求头 byte[6] 恒 0（无尾包标记）。
//     - 响应同头格式，battery@17 / chargeStatus@18。
//
// 由于无法从枚举信息判断设备走哪一代协议，readDevices() 对每个候选接口先尝试
// 新协议（0x10），若响应全 0 / 无效再回退旧协议（0xA0），以新设备优先。
struct DeviceInfoProtocol
{
    uint8_t cmd;            // 命令号
    size_t contentSize;     // 总 payload 长度
    bool useLastPacketFlag; // 是否在请求头 byte[6] 设置尾包标记
};

constexpr DeviceInfoProtocol kProtoNew = {0x10, 56, true};  // 新协议（F87ProV2D 等）
constexpr DeviceInfoProtocol kProtoLegacy = {0xA0, 64, false}; // 旧协议（早期型号）

// 请求包首字节标记（0xAA）；响应包首字节为 0x55。
constexpr uint8_t kHeaderRequest = 0xAA;
constexpr uint8_t kHeaderResponse = 0x55;

// 单包数据字节数（不含 Report ID）的默认回退值。实际值优先从设备报告描述符动态
// 查询（见 queryReportByteLengths）；无法查询时退回此值。AULA 不同型号存在 32 / 64
// 两种报告大小，错误的大小会让 hid_write 因写入长度与设备输出报告不符被 Windows
// 拒绝（ERROR_INVALID_FUNCTION）。
constexpr size_t kDefaultReportDataSize = 32;
// 协议头字节数（[0]=header,[1]=cmd,[2]=len,[3..4]=addr,[5..7]=otherHeader/尾包标记）。
constexpr size_t kHeaderSize = 8;

// battery / chargeStatus 在 payload 中的偏移（两代协议一致）。
constexpr size_t kBatteryOffset = 17;
constexpr size_t kChargeStatusOffset = 18;
// 响应里 vid/pid 偏移（payload[4..7]，小端），用于日志核对设备自报型号。
constexpr size_t kDevInfoVidOffset = 4;
constexpr size_t kDevInfoPidOffset = 6;
// 解析电量所需的最小 payload 字节数（chargeStatus 在偏移 18，至少要 19 字节）。
constexpr size_t kMinPayloadForBattery = kChargeStatusOffset + 1;

// 输入报告读取超时（毫秒）。桌面 hidapi 单线程轮询，放宽到 500ms
// 以兼容响应较慢的接收器。
constexpr int kReadTimeoutMs = 500;
// 单个分包无响应时的重发次数。初次发送失败或所有重试均超时后，再由上层决定是否回退。
constexpr int kReadMaxRetries = 3;

// —— 工具函数前向声明（定义见文件末尾）——
std::wstring pathToWString(const char *path);
std::wstring toHex4(unsigned short v);

// 查询某个设备路径下输入 / 输出报告的数据字节长度（不含 Report ID 字节）。
// outLen 留给 hid_write 的缓冲区大小（输出报告长度 + 1 个 Report ID 字节）；
// inLen 用于分配读取缓冲区。查询失败时两者均回退到 kDefaultReportDataSize。
// 通过 SetupAPI 打开设备路径对应的 HID 句柄，再用 HidD_GetPreparsedData +
// HidP_GetCaps 读取报告描述符里的 caps.OutputReportByteLength /
// InputReportByteLength，这是取得真实报告大小的唯一可靠途径——hidapi 的
// hid_device_info 不暴露报告长度字段。
// 返回 false 表示该接口没有定义输出报告（或读取失败）——这种接口无法承载
// 写入式查询协议，调用方应直接跳过，避免 hid_write 被拒（ERROR_INVALID_FUNCTION）。
bool queryReportByteLengths(const char *path, size_t &outDataLen, size_t &inDataLen);

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

// —— AULA 设备型号表 ——
//
// 每条记录该 PID 期望匹配的顶层 usage。仅保留 VID=0x0C45 的 AULA 条目。
//
// usagePage/usage 为 0 表示「通配簇」——对该 PID 不带 usage 过滤，即该 PID 的
// 任一接口都可承载配置协议。非 0 表示「按 usage 精确匹配」：
//   - 标准簇 0xFF68 / 0x0061：绝大多数键盘 / 鼠标（80 个 PID）；
//   - 特殊簇 0xFF80 / 0x0001：0xFEFC（1 个 PID）。
//
// 新增型号：在此追加对应条目即可，无需改动其它逻辑。
struct AulaDeviceEntry
{
    uint16_t pid;
    uint16_t usagePage; // 0 = 通配
    uint16_t usage;     // 0 = 通配
};

constexpr AulaDeviceEntry kAulaDevices[] = {
    // —— 特殊簇 usage_page=0xFF80 / usage=0x0001 ——
    { 0xFEFC, 0xFF80, 0x0001 },
    // —— 标准簇 usage_page=0xFF68 / usage=0x0061 ——
    { 0x8030, 0xFF68, 0x0061 },
    { 0x8031, 0xFF68, 0x0061 },
    { 0x8032, 0xFF68, 0x0061 },
    { 0x8033, 0xFF68, 0x0061 },
    { 0x8034, 0xFF68, 0x0061 },
    { 0x8035, 0xFF68, 0x0061 },
    { 0x8036, 0xFF68, 0x0061 },
    { 0x8037, 0xFF68, 0x0061 },
    { 0x8040, 0xFF68, 0x0061 },
    { 0x8041, 0xFF68, 0x0061 },
    { 0x8043, 0xFF68, 0x0061 },
    { 0x8044, 0xFF68, 0x0061 },
    { 0x8045, 0xFF68, 0x0061 },
    { 0x8046, 0xFF68, 0x0061 },
    { 0x8048, 0xFF68, 0x0061 },
    { 0x8049, 0xFF68, 0x0061 },
    { 0x8051, 0xFF68, 0x0061 },
    { 0x8052, 0xFF68, 0x0061 },
    { 0x8053, 0xFF68, 0x0061 },
    { 0x8055, 0xFF68, 0x0061 },
    { 0x8056, 0xFF68, 0x0061 },
    { 0x8057, 0xFF68, 0x0061 },
    { 0x8061, 0xFF68, 0x0061 },
    { 0x8063, 0xFF68, 0x0061 },
    { 0x8065, 0xFF68, 0x0061 },
    { 0x806A, 0xFF68, 0x0061 },
    { 0x806B, 0xFF68, 0x0061 },
    { 0x806F, 0xFF68, 0x0061 },
    { 0x8070, 0xFF68, 0x0061 },
    { 0x8071, 0xFF68, 0x0061 },
    { 0x8075, 0xFF68, 0x0061 },
    { 0x8077, 0xFF68, 0x0061 },
    { 0x8078, 0xFF68, 0x0061 },
    { 0x8079, 0xFF68, 0x0061 },
    { 0x807D, 0xFF68, 0x0061 },
    { 0x807E, 0xFF68, 0x0061 },
    { 0x807F, 0xFF68, 0x0061 },
    { 0x8081, 0xFF68, 0x0061 },
    { 0x8085, 0xFF68, 0x0061 },
    { 0x8089, 0xFF68, 0x0061 },
    { 0x808C, 0xFF68, 0x0061 },
    { 0x808E, 0xFF68, 0x0061 },
    { 0x8090, 0xFF68, 0x0061 },
    { 0x8092, 0xFF68, 0x0061 },
    { 0x8093, 0xFF68, 0x0061 },
    { 0x8094, 0xFF68, 0x0061 },
    { 0x8095, 0xFF68, 0x0061 },
    { 0x8096, 0xFF68, 0x0061 },
    { 0x8097, 0xFF68, 0x0061 },
    { 0x8098, 0xFF68, 0x0061 },
    { 0x809A, 0xFF68, 0x0061 },
    { 0x809B, 0xFF68, 0x0061 },
    { 0x809C, 0xFF68, 0x0061 },
    { 0x809D, 0xFF68, 0x0061 },
    { 0x809E, 0xFF68, 0x0061 },
    { 0x809F, 0xFF68, 0x0061 },
    { 0x80A0, 0xFF68, 0x0061 },
    { 0x80A2, 0xFF68, 0x0061 },
    { 0x80A3, 0xFF68, 0x0061 },
    { 0x80A4, 0xFF68, 0x0061 },
    { 0x80A5, 0xFF68, 0x0061 },
    { 0x80A6, 0xFF68, 0x0061 },
    { 0x80A7, 0xFF68, 0x0061 },
    { 0x80A8, 0xFF68, 0x0061 },
    { 0x80A9, 0xFF68, 0x0061 },
    { 0x80AA, 0xFF68, 0x0061 },
    { 0x80AB, 0xFF68, 0x0061 },
    { 0x80AC, 0xFF68, 0x0061 },
    { 0x80AD, 0xFF68, 0x0061 },
    { 0x80AE, 0xFF68, 0x0061 },
    { 0x80AF, 0xFF68, 0x0061 },
    { 0x80B0, 0xFF68, 0x0061 },
    { 0x80B2, 0xFF68, 0x0061 },
    { 0x80B3, 0xFF68, 0x0061 },
    { 0x80B4, 0xFF68, 0x0061 },
    { 0x80B5, 0xFF68, 0x0061 },
    { 0x80B6, 0xFF68, 0x0061 },
    { 0x80B7, 0xFF68, 0x0061 },
    { 0x80B8, 0xFF68, 0x0061 },
    { 0x8800, 0xFF68, 0x0061 },
    // —— 通配簇（PID 命中即算，不限定 usage）——
    { 0x0256, 0x0000, 0x0000 },
    { 0x2710, 0x0000, 0x0000 },
    { 0x2711, 0x0000, 0x0000 },
    { 0x2712, 0x0000, 0x0000 },
    { 0x2713, 0x0000, 0x0000 },
    { 0x8006, 0x0000, 0x0000 },
    { 0x8007, 0x0000, 0x0000 },
    { 0x8008, 0x0000, 0x0000 },
    { 0x8009, 0x0000, 0x0000 },
    { 0x800A, 0x0000, 0x0000 },
    { 0x800B, 0x0000, 0x0000 },
    { 0x800C, 0x0000, 0x0000 },
    { 0x8010, 0x0000, 0x0000 },
    { 0x8011, 0x0000, 0x0000 },
    { 0x8013, 0x0000, 0x0000 },
    { 0x8016, 0x0000, 0x0000 },
    { 0x8018, 0x0000, 0x0000 },
    { 0x8019, 0x0000, 0x0000 },
    { 0x8023, 0x0000, 0x0000 },
    { 0x8029, 0x0000, 0x0000 },
    { 0x8039, 0x0000, 0x0000 },
    { 0x8047, 0x0000, 0x0000 },
    { 0x8050, 0x0000, 0x0000 },
    { 0x8054, 0x0000, 0x0000 },
    { 0x8080, 0x0000, 0x0000 },
    { 0x8083, 0x0000, 0x0000 },
    { 0x8087, 0x0000, 0x0000 },
    { 0x808A, 0x0000, 0x0000 },
    { 0x8099, 0x0000, 0x0000 },
    { 0x80A1, 0x0000, 0x0000 },
    { 0x80B1, 0x0000, 0x0000 },
    { 0x80B9, 0x0000, 0x0000 },
    { 0x80BA, 0x0000, 0x0000 },
    { 0x80BB, 0x0000, 0x0000 },
    { 0x80BC, 0x0000, 0x0000 },
    { 0x80BD, 0x0000, 0x0000 },
    { 0x80BE, 0x0000, 0x0000 },
    { 0x80BF, 0x0000, 0x0000 },
    { 0x80C0, 0x0000, 0x0000 },
    { 0x80C1, 0x0000, 0x0000 },
    { 0x80C2, 0x0000, 0x0000 },
    { 0x80C3, 0x0000, 0x0000 },
    { 0x80C4, 0x0000, 0x0000 },
    { 0x80C6, 0x0000, 0x0000 },
    { 0x80C7, 0x0000, 0x0000 },
    { 0x80C8, 0x0000, 0x0000 },
    { 0x80C9, 0x0000, 0x0000 },
    { 0x80CA, 0x0000, 0x0000 },
    { 0x80CB, 0x0000, 0x0000 },
    { 0x80CC, 0x0000, 0x0000 },
    { 0x80CD, 0x0000, 0x0000 },
    { 0x80CE, 0x0000, 0x0000 },
    { 0x80D1, 0x0000, 0x0000 },
    { 0x80D4, 0x0000, 0x0000 },
    { 0x80D5, 0x0000, 0x0000 },
    { 0x80D6, 0x0000, 0x0000 },
    { 0x80D7, 0x0000, 0x0000 },
    { 0x80D8, 0x0000, 0x0000 },
    { 0x80D9, 0x0000, 0x0000 },
    { 0x80DA, 0x0000, 0x0000 },
    { 0x80DB, 0x0000, 0x0000 },
    { 0x80DF, 0x0000, 0x0000 },
    { 0x80E1, 0x0000, 0x0000 },
    { 0x8109, 0x0000, 0x0000 },
    { 0x8801, 0x0000, 0x0000 },
    { 0x8802, 0x0000, 0x0000 },
    { 0x8803, 0x0000, 0x0000 },
    { 0x8804, 0x0000, 0x0000 },
    { 0x8806, 0x0000, 0x0000 },
    { 0x880B, 0x0000, 0x0000 },
    { 0x880C, 0x0000, 0x0000 },
    { 0x8A01, 0x0000, 0x0000 },
    { 0xFB00, 0x0000, 0x0000 },
    { 0xFEF8, 0x0000, 0x0000 },
    { 0xFEF9, 0x0000, 0x0000 },
    { 0xFEFA, 0x0000, 0x0000 },
    { 0xFEFD, 0x0000, 0x0000 },
    { 0xFEFE, 0x0000, 0x0000 }, // AULA F87ProV2D Dongle（2.4G 接收器）已验证
};

// 在设备表中查找 PID 命中的条目；返回 nullptr 表示该 PID 不被支持。
const AulaDeviceEntry *findAulaEntry(std::uint16_t pid)
{
    for (const AulaDeviceEntry &e : kAulaDevices) {
        if (e.pid == pid) {
            return &e;
        }
    }
    return nullptr;
}

// 判断枚举到的接口是否承载 AULA 配置协议（即值得查询电量）。
//   entry->usagePage==0（通配簇）：取该 PID 下任一 vendor-defined 接口
//     （usage_page>0x0B，排除标准键盘 / 鼠标输入接口 0x01）；
//   否则（按 usage 精确匹配）：要求 hid_device_info 的 usage_page / usage 与表中一致。
bool interfaceMatches(const AulaDeviceEntry &entry, const hid_device_info &info)
{
    if (entry.usagePage == 0) {
        // 通配簇：仅排除标准 HID 顶层集合（Generic Desktop / Consumer 等输入接口）。
        return info.usage_page > 0x0B;
    }
    return info.usage_page == entry.usagePage && info.usage == entry.usage;
}

// 读一帧响应包并校验帧头 / 命令号；返回去掉 8 字节头后的 payload 段。
// bufCapacity 为读取缓冲区容量（含可能的 Report ID 字节，通常 = 输入报告长度 + 1）。
// chunkSize 为单包 payload 字节数（= 输出报告数据长度 - 协议头）。
// 返回 std::nullopt 表示读失败或不是目标响应帧。
std::optional<std::vector<unsigned char>> readResponsePacket(hid_device *dev,
                                                             uint8_t expectedCmd,
                                                             size_t bufCapacity,
                                                             size_t chunkSize)
{
    std::vector<unsigned char> buf(bufCapacity, 0);
    int n = hid_read_timeout(dev, buf.data(), buf.size(), kReadTimeoutMs);
    if (n <= 0) {
        if (n < 0) {
            LOG_ERR_W(L"[HID] hid_read_timeout failed: " + hidErrorString(dev));
        }
        return std::nullopt;
    }

    // hid_read 返回数据：编号报告设备首字节为 Report ID。本设备只有单一报告，
    // 保守起见，当首字节非 0x55 时尝试从第 1 字节起匹配帧头。
    size_t start = 0;
    if (static_cast<unsigned char>(buf[0]) != kHeaderResponse) {
        if (n >= 2 && static_cast<unsigned char>(buf[1]) == kHeaderResponse) {
            start = 1;
        } else {
            // 不是协议响应帧（可能是键盘常规输入），丢弃。
            return std::nullopt;
        }
    }

    const size_t available = static_cast<size_t>(n) - start;
    if (available < kHeaderSize) {
        return std::nullopt;
    }
    // 校验命令号（buf[start+1]）。
    if (buf[start + 1] != expectedCmd) {
        return std::nullopt;
    }

    const size_t chunk = (std::min)(chunkSize, available - kHeaderSize);
    return std::vector<unsigned char>(buf.begin() + start + kHeaderSize,
                                      buf.begin() + start + kHeaderSize + chunk);
}

// 向已打开的设备发送 GET_DEVICE_INFO，按指定协议（cmd/contentSize/尾包标记）分多包请求，
// 拼接完整 payload。outDataLen / inDataLen 为该设备的输出 / 输入报告数据字节长度
//（不含 Report ID），由调用方用 queryReportByteLengths 查得。
//
// 协议：
//   - 按设备的单包 payload 大小（outDataLen - 协议头）分 ceil(contentSize/chunk) 包；
//     每包 addr = packetIdx*chunk，本包 size 取剩余长度；
//   - 新协议在请求头 byte[6] 置「是否尾包」标志（最后一包 1，其余 0）；
//   - 每包请求后读一包响应，校验帧头 / cmd，取 [8..] 拼成 contentSize 字节 payload；
//   - 容错：只要已拼到 ≥ kMinPayloadForBattery 字节即认为可解析电量。
//     某些型号收到首包查询后只回一包即停（实测），此时电量字段仍在首包内，
//     可正常解析。返回 std::nullopt 表示连电量字段都没拿到。
std::optional<std::vector<unsigned char>> getDeviceInfo(hid_device *dev,
                                                        const DeviceInfoProtocol &proto,
                                                        size_t outDataLen,
                                                        size_t inDataLen)
{
    // 单包 payload = 输出报告数据长度 - 协议头。兜底：长度不足时退回默认值。
    const size_t chunkSize =
        (outDataLen > kHeaderSize) ? (outDataLen - kHeaderSize)
                                   : (kDefaultReportDataSize - kHeaderSize);
    // hid_write 缓冲区 = 1 个 Report ID 字节 + 输出报告数据长度。
    const size_t writeLen = outDataLen + 1;
    // hid_read 缓冲区容量 = 1 个 Report ID 字节 + 输入报告数据长度。
    const size_t readLen = inDataLen + 1;

    const size_t totalPackets = (proto.contentSize + chunkSize - 1) / chunkSize;

    std::vector<unsigned char> payload;
    payload.reserve(proto.contentSize);

    for (size_t packet = 0; packet < totalPackets; ++packet) {
        const size_t addr = packet * chunkSize;
        const size_t remaining = proto.contentSize - addr;
        const size_t thisSize = (std::min)(chunkSize, remaining);
        const bool isLast = (packet == totalPackets - 1);

        // 构造协议包：[0]=0xAA,[1]=cmd,[2]=contentSize,[3]=addrLo,[4]=addrHi,
        //              [5]=0,[6]=isLastFlag(新协议)或0,[7]=0,[8..]=0。
        // hid_write 首字节为 Report ID（设备只有单一报告，用 0x00），
        // 实际写入长度 = 1(Report ID) + 设备输出报告数据长度，必须与设备报告描述符
        // 声明的 OutputReportByteLength 精确一致，否则 WriteFile 被拒。
        std::vector<unsigned char> request(writeLen, 0);
        request[1] = kHeaderRequest;                                 // 0xAA
        request[2] = proto.cmd;                                      // 命令号
        request[3] = static_cast<unsigned char>(thisSize);           // contentSize
        request[4] = static_cast<unsigned char>(addr & 0xFF);        // addrLo
        request[5] = static_cast<unsigned char>((addr >> 8) & 0xFF); // addrHi
        if (proto.useLastPacketFlag) {
            request[7] = isLast ? 1 : 0; // 协议包 byte[6]，前面有 Report ID 偏移
        }

        std::optional<std::vector<unsigned char>> chunk;
        for (int attempt = 0; attempt <= kReadMaxRetries; ++attempt) {
            if (hid_write(dev, request.data(), request.size()) < 0) {
                // 写入失败常见于设备休眠 / 被独占 / 瞬时总线复位，属探测期间的常态，
                // 降为 VERBOSE。无输出报告的接口已在 tryInterface 中提前跳过，
                // 不会走到这里。
                LOG_VERBOSE_W(L"[HID] hid_write failed: " + hidErrorString(dev));
                break;
            }

            chunk = readResponsePacket(dev, proto.cmd, readLen, chunkSize);
            if (chunk) {
                break;
            }
            if (attempt < kReadMaxRetries) {
                LOG_VERBOSE_W(L"[HID] GET_DEVICE_INFO(0x" +
                              std::wstring{toHex4(proto.cmd)} +
                              L") packet " + std::to_wstring(packet) +
                              L" no response, retry " + std::to_wstring(attempt + 1));
            }
        }
        if (!chunk) {
            // 该包无响应：跳出，由长度检查决定是否可用（已拼到的部分可能已含电量字段）。
            break;
        }
        payload.insert(payload.end(), chunk->begin(), chunk->end());

        // 已凑齐全量 payload，无需再请求后续包。
        if (payload.size() >= proto.contentSize) {
            break;
        }
    }

    if (payload.size() < kMinPayloadForBattery) {
        // 单包无响应在多接口探测中是常态（只有其中一个接口能响应电量协议），
        // 降为 VERBOSE，避免每个接口每次轮询都刷屏。真正的"所有接口都失败"
        // 由 tryInterface 中的汇总日志反映。
        LOG_VERBOSE_W(L"[HID] GET_DEVICE_INFO(0x" +
                   std::wstring{toHex4(proto.cmd)} + L") payload too short: got " +
                   std::to_wstring(payload.size()) + L" / need " +
                   std::to_wstring(kMinPayloadForBattery) + L" bytes");
        return std::nullopt;
    }
    return payload;
}

// 判定一次 GET_DEVICE_INFO 响应是否「有效」。
//
// 用作协议选择依据：当用错误的协议查询时（如对走新协议 0x10 的设备发 0xA0），
// 设备可能仍回帧但 payload 全 0（vid/pid/battery 都是 0），这种情况应判为无效，
// 让上层回退到另一种协议。判定标准：自报 vid 或 pid 非 0，或 battery/chargeStatus 非 0，
// 任一成立即视为有效。
bool deviceInfoLooksValid(const std::vector<unsigned char> &payload)
{
    const uint16_t reportedVid =
        static_cast<uint16_t>(payload[kDevInfoVidOffset]) |
        (static_cast<uint16_t>(payload[kDevInfoVidOffset + 1]) << 8);
    const uint16_t reportedPid =
        static_cast<uint16_t>(payload[kDevInfoPidOffset]) |
        (static_cast<uint16_t>(payload[kDevInfoPidOffset + 1]) << 8);
    const int battery = payload[kBatteryOffset];
    const int chargeStatus = payload[kChargeStatusOffset];
    return reportedVid != 0 || reportedPid != 0 || battery != 0 || chargeStatus != 0;
}

// 解析 payload 为 BatteryDevice。
BatteryDevice parseDeviceInfo(const std::vector<unsigned char> &payload,
                              const DeviceInfoProtocol &proto,
                              const std::wstring &name,
                              const std::wstring &id)
{
    const auto &a = payload;
    const int battery = a[kBatteryOffset];
    const int chargeStatus = a[kChargeStatusOffset];

    // 设备自报的 vid/pid（payload[4..7]，小端），仅用于日志核对，不参与匹配。
    const uint16_t reportedVid =
        static_cast<uint16_t>(a[kDevInfoVidOffset]) |
        (static_cast<uint16_t>(a[kDevInfoVidOffset + 1]) << 8);
    const uint16_t reportedPid =
        static_cast<uint16_t>(a[kDevInfoPidOffset]) |
        (static_cast<uint16_t>(a[kDevInfoPidOffset + 1]) << 8);

    LOG_VERBOSE_W(L"[HID]     proto=0x" + std::wstring{toHex4(proto.cmd)} +
          L" payload vid=0x" +
          std::wstring{toHex4(reportedVid)} + L" pid=0x" +
          std::wstring{toHex4(reportedPid)} +
          L" battery=" + std::to_wstring(battery) +
          L"% chargeStatus=" + std::to_wstring(chargeStatus) +
          L" payloadLen=" + std::to_wstring(payload.size()));

    BatteryDevice device;
    device.id = id;
    device.name = name;
    device.type = BatteryDevice::Type::Hid;
    device.percentage = battery;
    device.level = levelFromPercentage(battery);
    device.charging = (chargeStatus != 0);
    device.wired = false; // HID 键盘/鼠标不区分有线供电
    device.connected = true;
    return device;
}

// 把 char*（hidapi 路径，ASCII）转 std::wstring。
std::wstring pathToWString(const char *path)
{
    if (!path) {
        return {};
    }
    return std::wstring(path, path + std::strlen(path));
}

// 把 unsigned short 格式化为 4 位十六进制 wchar（如 0x0C45），用于日志。
std::wstring toHex4(unsigned short v)
{
    wchar_t buf[8];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%04X", v);
    return std::wstring(buf);
}

// hidapi 返回的设备路径形如 "\\\\?\\hid#vid_0c45&pid_80a0 ..."，可直接用于
// CreateFileW 打开（hidapi 内部也是这么做的）。打开后用 HidD_GetPreparsedData 取
// 报告描述符预处理数据，再 HidP_GetCaps 读出 caps.OutputReportByteLength /
// InputReportByteLength。这两个值已包含 Report ID 字节，故减 1 得到纯数据长度。
bool queryReportByteLengths(const char *path, size_t &outDataLen, size_t &inDataLen)
{
    outDataLen = kDefaultReportDataSize;
    inDataLen = kDefaultReportDataSize;
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
        // caps 的报告长度已含 Report ID 字节（设备使用编号报告时），故实际数据长度减 1；
        // 对不使用 Report ID 的设备，caps 报告长度等于数据长度，减 1 会偏小，但协议头仅
        // 8 字节、请求包远大于 8 字节，0 偏小会在 getDeviceInfo 内部回退到默认值，安全。
        if (caps.OutputReportByteLength > 1) {
            outDataLen = caps.OutputReportByteLength - 1;
        }
        if (caps.InputReportByteLength > 1) {
            inDataLen = caps.InputReportByteLength - 1;
        }
        // 没有定义输出报告的接口（OutputReportByteLength == 0）无法承载写入式查询协议，
        // hid_write 必然被拒（ERROR_INVALID_FUNCTION）。标记为不可用，调用方直接跳过。
        usable = caps.OutputReportByteLength > 0;
    }

    HidD_FreePreparsedData(preparsed);
    CloseHandle(h);
    return usable;
}
} // namespace

std::wstring AulaHidProvider::displayName() const
{
    return L"HID";
}

std::vector<BatteryDevice> AulaHidProvider::readDevices()
{
    std::vector<BatteryDevice> devices;

    // hid_init 幂等且线程安全；显式调用以确保跨线程首次使用前完成初始化。
    if (hid_init() != 0) {
        LOG_ERR("[HID] hid_init failed");
        return devices;
    }

    // 仅枚举 AULA VID，再按设备表 + usage 二次过滤。
    hid_device_info *enumHead = hid_enumerate(kAulaVendorId, 0);
    if (!enumHead) {
        // 没有 VID=0x0C45 的设备：常态，仅 debug 级提示（INFO 输出便于排障）。
        LOG_VERBOSE_W(L"[HID] hid_enumerate(0x0C45) returned 0 devices "
              L"(键盘是否处于 2.4G 模式？接收器是否已插入？)");
        return devices;
    }

    // 先统计枚举到的接口数，便于从日志判断接收器是否就绪。
    int totalInterfaces = 0;
    for (hid_device_info *c = enumHead; c; c = c->next) {
        ++totalInterfaces;
    }
    LOG_VERBOSE_W(L"[HID] hid_enumerate(0x0C45) returned " + std::to_wstring(totalInterfaces) +
          L" interface(s)");

    // 同一物理设备常暴露多个接口；按 PID 去重，避免对一台设备查询多次。
    //
    // 接口缓存：Dongle 常暴露多个同 PID 的 vendor-defined 接口，但只有一个能成功
    // 响应电量协议。原「边枚举边查、失败不去重」会让每个匹配接口每轮都重发
    // 0x10/0xA0。记住上次成功的接口 path，下轮优先（且只先）查它；命中即跳过同 PID
    // 其余接口的无谓发包。缓存接口本轮失败（设备休眠/被独占）时回退该设备其余接口
    // 的全量遍历，成功后更新缓存；缓存 path 已不在枚举结果里（拔插/换口）时自然失效。
    std::unordered_set<std::uint16_t> processedPids;

    // 单接口查询：打开设备 → 双协议尝试（0x10 → 0xA0）→ 解析。
    // 返回成功解析的 BatteryDevice；失败返回 nullopt。
    auto tryInterface = [&](hid_device_info *cur,
                            const AulaDeviceEntry *entry) -> std::optional<BatteryDevice> {
        LOG_VERBOSE_W(L"[HID] interface: pid=0x" +
              std::wstring{toHex4(cur->product_id)} +
              L" usage_page=0x" + std::wstring{toHex4(cur->usage_page)} +
              L" usage=0x" + std::wstring{toHex4(cur->usage)} +
              L" path=" + pathToWString(cur->path));

        const std::wstring id = L"hid:" + pathToWString(cur->path);
        std::wstring name = cur->product_string ? cur->product_string : L"";
        if (name.empty()) {
            name = L"AULA Device 0x" + std::wstring{toHex4(cur->product_id)};
        }

        // 先查询该接口的报告长度：既决定请求包大小，也能预先判断接口是否可写。
        // 没有输出报告的接口（如纯输入接口）无法承载写入式查询协议，hid_write 必然
        // 被拒（ERROR_INVALID_FUNCTION），这里提前跳过，避免无谓的打开 / 写入 / 报错。
        size_t outDataLen = kDefaultReportDataSize;
        size_t inDataLen = kDefaultReportDataSize;
        if (!queryReportByteLengths(cur->path, outDataLen, inDataLen)) {
            LOG_VERBOSE_W(L"[HID]   -> skipped (interface has no output report)");
            return std::nullopt;
        }

        hid_device *dev = hid_open_path(cur->path);
        if (!dev) {
            LOG_WARN_W(L"[HID]   hid_open_path failed for " + name + L": " +
                       hidErrorString(nullptr));
            return std::nullopt;
        }

        std::optional<BatteryDevice> out;
        try {
            // 先尝试新协议（cmd=0x10），新设备优先；
            // 若响应全 0 / 无效或无响应，回退旧协议（cmd=0xA0）。
            //
            // 用有效性校验而非「拿到包就接受」：实测对走新协议的设备发旧协议 cmd，
            // 设备会回帧但 payload 全 0，仅靠长度判断会误判为电量 0%。
            std::optional<std::vector<unsigned char>> payload;
            const DeviceInfoProtocol *usedProto = nullptr;

            payload = getDeviceInfo(dev, kProtoNew, outDataLen, inDataLen);
            if (payload && deviceInfoLooksValid(*payload)) {
                usedProto = &kProtoNew;
            } else {
                if (payload) {
                    LOG_VERBOSE_W(L"[HID]   new proto(0x10) response looks invalid "
                               L"(all-zero), falling back to legacy proto(0xA0)");
                }
                payload = getDeviceInfo(dev, kProtoLegacy, outDataLen, inDataLen);
                if (payload && deviceInfoLooksValid(*payload)) {
                    usedProto = &kProtoLegacy;
                } else {
                    payload.reset();
                }
            }

            if (payload && usedProto) {
                out = parseDeviceInfo(*payload, *usedProto, name, id);
                LOG_VERBOSE_W(L"[HID]   " + name + L" = " +
                      std::to_wstring(out->percentage) +
                      L"% charging=" + (out->charging ? L"1" : L"0"));
            } else {
                // 单接口查询无果在多接口探测中是常态（同 PID 只有部分接口承载电量协议）。
                // 降为 VERBOSE，避免每次轮询都刷屏。真正的"所有接口都失败"由第二遍
                // 遍历结束后的汇总日志体现。
                LOG_VERBOSE_W(L"[HID]   " + name +
                           L" getDeviceInfo returned no valid data "
                           L"(tried proto 0x10 then 0xA0)");
            }
        } catch (const std::exception &e) {
            LOG_ERR_W(L"[HID]   getDeviceInfo threw for " + name + L": " +
                      std::wstring(e.what(), e.what() + std::strlen(e.what())));
        } catch (...) {
            LOG_ERR_W(L"[HID]   getDeviceInfo threw unknown exception for " + name);
        }

        hid_close(dev);
        return out;
    };

    // 第一遍：只对缓存命中的 PID + 匹配 path 的接口查询。
    for (hid_device_info *cur = enumHead; cur; cur = cur->next) {
        if (processedPids.count(cur->product_id) != 0) {
            continue;
        }
        const AulaDeviceEntry *entry = findAulaEntry(cur->product_id);
        if (!entry || !interfaceMatches(*entry, *cur)) {
            continue;
        }
        const auto cacheIt = m_lastGoodPath.find(cur->product_id);
        if (cacheIt == m_lastGoodPath.end()) {
            continue; // 该 PID 无缓存，留给第二遍
        }
        if (std::strcmp(cur->path, cacheIt->second.c_str()) != 0) {
            continue; // 该接口非缓存接口，留给第二遍
        }
        LOG_VERBOSE_W(L"[HID]   trying cached interface path first");
        if (auto result = tryInterface(cur, entry)) {
            devices.push_back(*result);
            processedPids.insert(cur->product_id);
            m_lastGoodPath[cur->product_id] = cur->path; // 刷新（path 一般不变，等价写回）
        }
    }

    // 第二遍：对未命中的 PID（或第一遍缓存接口失败的 PID）遍历其余接口，成功即停。
    for (hid_device_info *cur = enumHead; cur; cur = cur->next) {
        if (processedPids.count(cur->product_id) != 0) {
            continue; // 该 PID 本轮已成功，跳过同设备其它接口
        }
        const AulaDeviceEntry *entry = findAulaEntry(cur->product_id);
        if (!entry) {
            LOG_VERBOSE_W(L"[HID]   -> skipped (pid not in AULA device table)");
            continue;
        }
        if (!interfaceMatches(*entry, *cur)) {
            LOG_VERBOSE_W(L"[HID]   -> skipped (interface usage does not match entry)");
            continue;
        }
        const auto cacheIt = m_lastGoodPath.find(cur->product_id);
        if (cacheIt != m_lastGoodPath.end() &&
            std::strcmp(cur->path, cacheIt->second.c_str()) == 0) {
            continue; // 缓存接口刚才已在第一遍试过，跳过
        }

        if (auto result = tryInterface(cur, entry)) {
            devices.push_back(*result);
            processedPids.insert(cur->product_id);
            m_lastGoodPath[cur->product_id] = cur->path; // 成功后更新缓存
        }
    }

    // 清死路径：有缓存但本轮该 PID 所有接口都失败的，若缓存 path 已不在枚举结果里
    //（拔插/换口），清掉缓存；缓存接口仍在但无响应时保留，下一轮继续优先试。
    for (auto it = m_lastGoodPath.begin(); it != m_lastGoodPath.end();) {
        if (processedPids.count(it->first) != 0) {
            ++it; // 本轮已成功，缓存有效
            continue;
        }
        bool stillPresent = false;
        for (hid_device_info *cur = enumHead; cur; cur = cur->next) {
            if (cur->product_id == it->first &&
                std::strcmp(cur->path, it->second.c_str()) == 0) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            it = m_lastGoodPath.erase(it);
        } else {
            ++it;
        }
    }

    hid_free_enumeration(enumHead);
    LOG_VERBOSE_W(L"[HID] readDevices total = " + std::to_wstring(devices.size()));
    return devices;
}
