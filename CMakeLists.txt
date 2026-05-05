cmake_minimum_required(VERSION 3.21)
project(BluetoothBatteryOverlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(BluetoothBatteryOverlay WIN32
    src/main.cpp
)

target_compile_definitions(BluetoothBatteryOverlay PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

target_include_directories(BluetoothBatteryOverlay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(BluetoothBatteryOverlay PRIVATE
    setupapi
    bluetoothapis
    bthprops
    user32
    gdi32
    shell32
    ole32
)
