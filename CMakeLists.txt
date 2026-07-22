cmake_minimum_required(VERSION 3.19)
project(BatteryMonitor VERSION 1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(APP_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(APP_VERSION_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(APP_VERSION_HEADER "${APP_VERSION_GENERATED_DIR}/GeneratedAppVersion.h")
set(APP_VERSION_RC "${APP_VERSION_GENERATED_DIR}/app_version.rc")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets WebSockets Sql LinguistTools)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
qt_standard_project_setup(I18N_SOURCE_LANGUAGE en)

# ---------------------------------------------------------------------------
# Windows SDK cppwinrt 头文件探测
#
# WinRT 头文件位置随 SDK 安装目录变化（默认 C:/Program Files (x86)/Windows Kits/10，
# 本机实际在 D:/Windows Kits/10），因此动态探测，取最高版本下的 cppwinrt 目录。
# ---------------------------------------------------------------------------
if(WIN32)
    file(GLOB _winrt_sdk_dirs
        "$ENV{WindowsSdkDir}Include/*/cppwinrt"
        "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt"
        "C:/Program Files/Windows Kits/10/Include/*/cppwinrt"
        "D:/Windows Kits/10/Include/*/cppwinrt")
    if(_winrt_sdk_dirs)
        list(SORT _winrt_sdk_dirs ORDER DESCENDING)
        list(GET _winrt_sdk_dirs 0 _winrt_include_dir)
        message(STATUS "Using cppwinrt headers: ${_winrt_include_dir}")
    else()
        message(FATAL_ERROR
            "Windows SDK cppwinrt headers not found. "
            "Please install the Windows SDK (e.g. 10.0.26100.0) with the "
            "'Windows SDK for Desktop C++ Apps' component (cppwinrt).")
    endif()
endif()

if(MSVC)
    add_compile_options(/Zc:__cplusplus /utf-8 /permissive-)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

set(APP_RESOURCE_FILES
    res/resources.qrc
)

if(WIN32)
    set_source_files_properties("${APP_VERSION_RC}" PROPERTIES GENERATED TRUE)
    list(APPEND APP_RESOURCE_FILES
        "${APP_VERSION_RC}"
    )
endif()

qt_add_executable(BatteryMonitor
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    # 核心抽象层（BatteryDevice / IBatteryProvider / BatteryManager）。
    src/core/BatteryDevice.h
    src/core/IBatteryProvider.h
    src/core/BatteryManager.cpp
    src/core/BatteryManager.h
    # 电量历史持久化与自绘图表。
    src/history/BatteryHistoryStore.cpp
    src/history/BatteryHistoryStore.h
    src/history/BatteryHistoryChart.cpp
    src/history/BatteryHistoryChart.h
    # WebSocket JSON-RPC Server（src/rpc/）。
    src/rpc/DeviceJson.cpp
    src/rpc/DeviceJson.h
    src/rpc/JsonRpc.cpp
    src/rpc/JsonRpc.h
    src/rpc/RpcServer.cpp
    src/rpc/RpcServer.h
    # 具体 Provider 实现：按传输方式分目录，新增传输方式在此追加。
    src/providers/bluetooth/BluetoothProvider.cpp
    src/providers/bluetooth/BluetoothProvider.h
    src/providers/bluetooth/ClassicBluetoothProvider.cpp
    src/providers/bluetooth/ClassicBluetoothProvider.h
    src/providers/bluetooth/AirPodsProvider.cpp
    src/providers/bluetooth/AirPodsProvider.h
    src/providers/xbox/XboxProvider.cpp
    src/providers/xbox/XboxProvider.h
    src/providers/hid/AulaHidProvider.cpp
    src/providers/hid/AulaHidProvider.h
    src/providers/hid/AsusRogHidProvider.cpp
    src/providers/hid/AsusRogHidProvider.h
    src/providers/hid/AsusMouseHidProvider.cpp
    src/providers/hid/AsusMouseHidProvider.h
    src/providers/hid/LogitechHidProvider.cpp
    src/providers/hid/LogitechHidProvider.h
    src/providers/hid/RazerHidProvider.cpp
    src/providers/hid/RazerHidProvider.h
    src/providers/hid/VgnHidProvider.cpp
    src/providers/hid/VgnHidProvider.h
    util/AppSettings.cpp
    util/AppSettings.h
    util/DeviceSettings.cpp
    util/DeviceSettings.h
    util/Logger.cpp
    util/Logger.h
    ${APP_RESOURCE_FILES}
)

add_custom_target(generate_app_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DAPP_VERSION_MAJOR=${APP_VERSION_MAJOR}
        -DAPP_VERSION_MINOR=${APP_VERSION_MINOR}
        -DAPP_VERSION_HEADER=${APP_VERSION_HEADER}
        -DAPP_VERSION_HEADER_IN=${CMAKE_CURRENT_SOURCE_DIR}/util/GeneratedAppVersion.h.in
        -DAPP_VERSION_RC=${APP_VERSION_RC}
        -DAPP_VERSION_RC_IN=${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in
        -DAPP_ICON_PATH=${CMAKE_CURRENT_SOURCE_DIR}/res/icon.ico
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVersion.cmake
    BYPRODUCTS
        "${APP_VERSION_HEADER}"
        "${APP_VERSION_RC}"
    DEPENDS
        util/GeneratedAppVersion.h.in
        res/app.rc.in
        cmake/GenerateVersion.cmake
        res/icon.ico
    VERBATIM
)
add_dependencies(BatteryMonitor generate_app_version)

target_include_directories(BatteryMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(BatteryMonitor PRIVATE ${APP_VERSION_GENERATED_DIR})

qt_add_translations(
    TARGETS BatteryMonitor
    TS_FILES lang/BatteryMonitor_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(BatteryMonitor
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt6::WebSockets
        Qt6::Sql
)

if(WIN32)
    target_include_directories(BatteryMonitor PRIVATE ${_winrt_include_dir})
    # windowsapp: WinRT (BluetoothProvider); XInput: Xbox 手柄;
    # setupapi: ClassicBluetoothProvider 的 SetupDi 调用;
    # shell32: SHGetKnownFolderPath (Logger 解析 %APPDATA%)。
    # hid: HidD_GetPreparsedData / HidP_GetCaps（AulaHidProvider 查询设备报告长度）。
    target_link_libraries(BatteryMonitor PRIVATE windowsapp XInput setupapi shell32 hid)

    # —— hidapi（AulaHidProvider 读取 AULA 2.4G 键盘/鼠标电量）——
    # external/hidapi 内为预编译的 Windows 二进制（hidapi.dll/.lib/.h，v0.15.0）。
    set(HIDAPI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/external/hidapi)
    target_include_directories(BatteryMonitor PRIVATE ${HIDAPI_ROOT})
    target_link_libraries(BatteryMonitor PRIVATE ${HIDAPI_ROOT}/hidapi.lib)
    # 运行时需要 hidapi.dll 与 exe 同目录（bin/），编译后自动拷贝。
    add_custom_command(TARGET BatteryMonitor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${HIDAPI_ROOT}/hidapi.dll
                $<TARGET_FILE_DIR:BatteryMonitor>
        VERBATIM)
endif()

install(TARGETS BatteryMonitor
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET BatteryMonitor
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
