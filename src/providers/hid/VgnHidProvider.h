#pragma once

#include "src/core/IBatteryProvider.h"

// HID 电量提供者：通过 hidapi 读取 VGN / 关联品牌 2.4G 接收器
// 后挂接的键盘 / 鼠标的电量。
//
// —— 协议族概览 ——
// VGN 生态是异构多协议的：不同设备走完全不同的 HID 报文格式、命令号、
// 校验与字段布局。本 Provider 内置一张 (VID,PID) → 协议族 的型号表，并在 readDevices()
// 中按协议族分派到对应的查询函数。已实现的协议族：
//
//   1. ThreeMode 键盘族（reportId=0，64B Output/Input Report，无校验）
//        覆盖：flashextreme / flash75 / rongyana 系列。
//        命令包：[0]=13,[1]=1，其余 0，共 64B。
//        响应：[2]=mode,[3]=charge,[4]=battery；charge≠0 即充电。
//
//   2. Weisheng 键盘族（reportId=4，7B Output Report，无校验）
//        覆盖：VGN S99 / V98pro / VXE75 / VXE V87 / S87 等。
//        命令包：[0,0,26,6,0,0,0]，共 7B。
//        响应（与命令同格式的 Input Report）：[2]==26 时取 o=slice(7)，
//          battery.level=o[0]、battery.charging=o[1]。
//
//   3. Beiying 键盘族（reportId=9/19/6，19B Output Report，加和 CRC）
//        覆盖：VGN V98pro V3 / V87 V2 / N75 V2 / V108 / VXE V87pro 等。
//        命令包：[74,1,0,0,0]，pad 到 19B，末字节 = CRC = (reportId+Σbytes)&0xFF。
//        响应（Input Report）：n[0]==74 且 n.length>4 → level=n[4]、
//          charging=((n[5]>>4)&0x0F)==1。
//
//   4. VgnRyMouse 鼠标族（reportId=0，64B Feature Report，反码 CRC）
//        覆盖：Dragonfly 3 Pro Max / Y2 V2 Pro Max / 3 Ultra / F1 V5 Ultra。
//        查询前需初始化接收器：先发 [247,...]（状态探测），失败回退；
//        再发 [246,5,0,0,0,0,0]（切换到 2.4G 模式）。
//        电量命令：[247,0,0,0,0,0,0]，pad 到 64B，末字节 = CRC = 255-(Σbytes&0xFF)。
//        响应：i[4]==1 或 i[0]==0 视为失败；否则 battery.level=i[2]、charging=false
//        （该族 dongle 路径不回报真实充电状态，统一按未充电处理）。
//
//   5. Yongjiaxin 鼠标族（reportId=0，64B Output Report，0x55 头 + 反码 CRC）
//        覆盖：Dragonfly 3 SE/Pro、Y2 V2 Pro/Plus、F1 V2 Pro/SE/Plus。
//        命令包：[0]=0x55,[1]=48，其余 0，共 64B，末字节 CRC = 85-(Σ&0xFF)。
//        响应（Input Report）：n[1]==48 时取 receivedData=slice(8)，
//          receivedData[1]==1 → charging=true/isWired=true（电量未知但设备保留显示）；
//          否则 battery.level=receivedData[0]、charging=false。
//        查询前需唤醒（[0x55,237,...]）；桌面轮询模型下直接查询，
//        无响应或响应无效即视为设备休眠，跳过本次。
//
//   6. VgnLdMs 鼠标族（reportId=160，32B Output Report，反码 CRC）
//        覆盖：Dragonfly F1 V2 Extreme / Falcon3 Extreme(+)。
//        命令包：[2,37,0,0,0,0,0]（GetDeviceInfo，子参数 37=电源），共 32B，
//          末字节 CRC = 255-(Σ&0xFF)。
//        响应（Input Report）：i==2(GetDeviceInfo) 且 n[1]==37 →
//          battery.level=n[2]；有线 PID 标记为 wired/charging，接收器 PID 标记为无线。
//
//   7. Arbit 键盘族（reportId=0，64B Output Report，0x55 帧 + 加和校验，分块读）
//        覆盖：VGN FLASH68 / FLASH68+。
//        电量随「功能配置块」一并读出：cmd 5(GetFunc) 分两次读 56 字节 chunk（偏移 0/56），
//          拼成 funcBuf 后解析：level=funcBuf[32]&0x7F、
//          charging=(funcBuf[37]!=0)、isWired=(funcBuf[37]==15)。
//
//   8. ByKeyboard 键盘族（reportId=0，Feature/Output Report，结构化帧）
//        覆盖：VGN V98Pro V4 8K / V4 1K。
//        电量命令：[135,0,0,1,0,2,0]，优先 520B Feature，不支持时回退 63B Output/Input。
//        响应解码：level=resp[8]、statusByte=resp[9]、
//          charging=((statusByte&0xF0)>>4)!=0、full=(statusByte&0x0F)!=0。
//
//   9. VgnKc2 鼠标族（reportId=179，20B Output Report，无校验）
//        覆盖：Dragonfly F1 V5 Turbo+。
//        命令包：[6,0,...0]，共 20B（getDeviceInfo 临时把 reportId 置为 179）。
//        响应（Input Report）：i==6 → level=n[19]&0x7F、charging=!!(n[19]&0x80)。
//
//  10. MouseEnc 鼠标族（reportId=8/32，16B Output Report，0x55 反码 CRC + 尾字节 239）
//        覆盖：VGN F2 Pro Max / F2 Master / Y2 系列 / F1 系列 / Dragonfly King 等。
//        设备型号持续扩充，故该族按 VID 兜底匹配（见下）。
//        包格式：[cmd, 0,0,0,0,0,0,0,0,0,0,0,0,0,0, 239]，[15] = (0x55-Σ[0..14]) - reportId。
//        流程：先发 DeviceOnLine(3) 探测，receivedData[5]>0 视为在线；在线后发
//          BatteryLevel(4)，响应 level=receivedData[5]、charging=(receivedData[6]==1)，
//          voltage=(receivedData[7]<<8)+receivedData[8]；显示百分比优先用电压表换算。
//        响应判别：reportId 匹配且 receivedData[1]==0、receivedData[0]==cmd 回显。
//
// —— 设备表与兜底匹配 ——
// 设备型号表随新型号出现持续扩充。为避免「表里没有的 PID」识别不到，本 Provider
// 在精确 (VID,PID) 查表失败时，对 MouseEnc 族相关的 VID（0x3554 / 0x391D / 0x9981 /
// 0x372E 等）按 VID 兜底归入 MouseEnc 族尝试查询；查询无响应即视为不支持，跳过，
// 不会误报。
//
// 各协议族均与 BatteryManager 的 10s 轮询模型契合：readDevices() 对每个候选接口
// 主动发一次电量查询并同步读取响应。
class VgnHidProvider : public IBatteryProvider
{
public:
    std::wstring displayName() const override;
    std::vector<BatteryDevice> readDevices() override;
};
