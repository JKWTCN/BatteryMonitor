# BatteryMonitor

[English](README.en.md) | 简体中文

![BatteryMonitor 主界面](image/README/1783349507421.png "BatteryMonitor 主界面")

![BatteryMonitor 系统托盘电量提示](image/README/1783349577055.png "BatteryMonitor 系统托盘电量提示")

BatteryMonitor 是一个 Windows 桌面电量监控工具，用来在一个窗口和系统托盘里查看常用无线外设的电量。

项目基于 Qt 6 / CMake 开发，通过多个 Provider 分别读取蓝牙、Xbox 和 HID 2.4G 接收器设备的电量，并在界面中统一展示。

## 已验证设备

目前验证过的设备只有：

| 设备类型                    | PID/VID                                          | 是否验证 |
| --------------------------- | ------------------------------------------------ | -------- |
| 普通蓝牙设备                | 无固定 USB VID/PID，走 Windows 蓝牙设备信息      | 已验证   |
| AULA F87ProV2D Dongle       | VID `0x0C45` / PID `0xFEFE`                  | 已验证   |
| VGN DragonFly F2 Pro Max    | VGN MouseEnc 协议族，VID/PID 以设备实际上报为准  | 已验证   |
| AirPods 2                   | Apple Company ID `0x004C` / Model ID `0x200F` | 已验证   |
| Xbox 手柄                   | VID `0x045E`，PID 依具体型号而定               | 已验证   |
| Razer Basilisk X HyperSpeed | VID `0x1532` / PID `0x0083`                  | 已验证   |

## 理论支持设备

除上述已验证设备外，代码中也包含一些同品牌、同协议族或同 VID/PID 范围内设备的适配逻辑。这些设备属于理论支持范围，但尚未在作者环境中实际验证。

| 设备类型                             | PID/VID                                          | 是否验证         |
| ------------------------------------ | ------------------------------------------------ | ---------------- |
| AULA 2.4G 接收器设备                 | VID `0x0C45`，代码内置 PID 表                  | 理论支持，未验证 |
| VGN / 关联品牌 2.4G 接收器键盘、鼠标 | 多个 VID/PID，代码内置协议族和部分 VID 兜底匹配  | 理论支持，未验证 |
| Razer 鼠标 / 键盘                    | VID `0x1532`，代码内置 PID 表                  | 理论支持，未验证 |
| AirPods / Beats 系列                 | Apple Company ID `0x004C`，代码内置 Model ID 表 | 理论支持，未验证 |
| Xbox / XInput / Windows 游戏控制器   | Microsoft VID `0x045E` 或 Windows 控制器接口   | 理论支持，未验证 |
| 标准 BLE 电量服务设备                | BLE GATT Battery Service，无固定 USB VID/PID     | 理论支持，未验证 |
| 经典蓝牙音频设备                     | Windows BTHENUM 设备属性，无固定 USB VID/PID     | 理论支持，未验证 |

如果你的设备能被识别并正常读取电量，欢迎提交设备型号和测试结果，后续可以补充到已验证设备列表。

## 功能

- 查看设备电量百分比或电量档位
- 支持系统托盘显示
- 支持按设备设置是否显示到托盘
- 支持低电量提醒和提醒策略
- 支持设备别名
- 支持设备掉线后的电量缓存显示
- 支持隐藏未配对的 AirPods / Beats 广播
- 支持开机自启并最小化到托盘
- 支持浅色、深色、跟随系统主题
- 支持中文界面

## 当前读取方式

项目内部按设备来源拆分为多个 Provider：

- `BluetoothProvider`：读取 BLE GATT Battery Service / Windows 电池信息
- `ClassicBluetoothProvider`：读取经典蓝牙设备的 Windows 设备属性电量
- `AirPodsProvider`：解析 Apple Continuity BLE 广播，读取 AirPods / Beats 左耳、右耳、充电盒电量
- `XboxProvider`：通过 XInput、RawGameController 和 Windows 设备属性读取 Xbox 手柄电量
- `AulaHidProvider`：通过 hidapi 读取 AULA 2.4G 接收器设备电量
- `VgnHidProvider`：通过 hidapi 读取 VGN / 关联品牌 2.4G 接收器设备电量
- `RazerHidProvider`：通过 hidapi 读取 Razer 鼠标 / 键盘电量

## 环境要求

- Windows
- Qt 6.5 或更高版本
- CMake 3.19 或更高版本
- 支持 C++17 的 MSVC 工具链
- Windows SDK，需包含 Desktop C++ Apps / cppwinrt 头文件

`external/hidapi` 目录中已包含当前项目使用的 Windows 版 hidapi 预编译文件。构建后会自动把 `hidapi.dll` 拷贝到输出目录。

## 构建

在已配置 Qt 和 MSVC 环境的命令行中执行：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

默认可执行文件输出到：

```text
bin/BatteryMonitor.exe
```

也可以直接用 Qt Creator 打开本项目的 `CMakeLists.txt` 构建运行。

## 使用说明

启动后主窗口会显示当前读取到的设备列表。关闭窗口时程序不会退出，而是继续驻留在系统托盘；如需完全退出，请使用托盘菜单。

点击设备可以进入设备详情页，配置：

- 设备别名
- 是否显示到托盘
- 是否启用低电量提醒
- 低电量提醒阈值
- 低电量提醒策略
- 是否永久缓存该设备

设置页中可以配置：

- 刷新间隔
- 语言
- 主题
- 开机自启
- 掉线缓存保留时长
- 是否隐藏未配对 AirPods

## 项目结构

```text
.
├── main.cpp                         # 应用入口、Provider 注册、语言/主题初始化
├── mainwindow.cpp / mainwindow.h     # 主窗口、托盘、设置页、设备详情页
├── src/core                         # 设备模型、Provider 接口、BatteryManager 聚合逻辑
├── src/providers/bluetooth          # 蓝牙 / AirPods Provider
├── src/providers/hid                # AULA / VGN / Razer HID Provider
├── src/providers/xbox               # Xbox Provider
├── util                             # 设置、日志、版本信息
├── lang                             # Qt 翻译文件
├── res                              # 图标和 Qt 资源
└── external/hidapi                  # hidapi Windows 预编译依赖
```

## 适配说明

如果要增加新设备，通常需要新增或扩展对应 Provider：

- 标准蓝牙电量设备优先看 `BluetoothProvider`
- 经典蓝牙音频设备优先看 `ClassicBluetoothProvider`
- Apple 音频设备优先看 `AirPodsProvider`
- XInput / Windows 游戏控制器设备优先看 `XboxProvider`
- 2.4G 接收器或私有协议设备需要按 HID 协议扩展 `src/providers/hid`

新增 Provider 后，在 `main.cpp` 中注册到 `BatteryManager` 即可接入现有 UI、托盘、低电量提醒和缓存逻辑。

## 特别鸣谢

感谢 [hidapi](https://github.com/libusb/hidapi) 项目。本项目的 HID 设备电量读取依赖 hidapi 与 Windows HID 设备通信。

## 许可证

本项目基于 MIT License 开源，详见 [LICENSE](LICENSE)。
