#pragma once
// Leave revision at zero for USB setup. To use personal build defaults, copy
// this file to wifi_defaults.h, enter your settings, and set REVISION to 1.
// Keep that revision unchanged for GUI updates so NVS settings remain in use.
namespace wifi_defaults {
constexpr unsigned REVISION = 0;
constexpr char SSID[] = "";
constexpr char PASSWORD[] = "";
constexpr double LATITUDE = 32.7157;
constexpr double LONGITUDE = -117.1611;
}
