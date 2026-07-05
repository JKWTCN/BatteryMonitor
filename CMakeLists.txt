cmake_minimum_required(VERSION 3.19)
project(BatteryMonitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets LinguistTools)

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

qt_add_executable(BatteryMonitor
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    battery/BatteryDevice.h
    battery/IBatteryProvider.h
    battery/BatteryManager.cpp
    battery/BatteryManager.h
    battery/AirPodsProvider.cpp
    battery/AirPodsProvider.h
    battery/BluetoothProvider.cpp
    battery/BluetoothProvider.h
    battery/ClassicBluetoothProvider.cpp
    battery/ClassicBluetoothProvider.h
    battery/XboxProvider.cpp
    battery/XboxProvider.h
    util/Logger.cpp
    util/Logger.h
)

target_include_directories(BatteryMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

qt_add_translations(
    TARGETS BatteryMonitor
    TS_FILES lang/BatteryMonitor_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
)

target_link_libraries(BatteryMonitor
    PRIVATE
        Qt::Core
        Qt::Widgets
)

if(WIN32)
    target_include_directories(BatteryMonitor PRIVATE ${_winrt_include_dir})
    # windowsapp: WinRT (BluetoothProvider); XInput: Xbox 手柄;
    # setupapi: ClassicBluetoothProvider 的 SetupDi 调用;
    # shell32: SHGetKnownFolderPath (Logger 解析 %APPDATA%)。
    target_link_libraries(BatteryMonitor PRIVATE windowsapp XInput setupapi shell32)
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
