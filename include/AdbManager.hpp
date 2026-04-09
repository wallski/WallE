#pragma once
#include <string>
#include <vector>

struct DeviceInfo {
    std::string serial;
    std::string status;     // "device", "offline", "unauthorized"
    std::string model;
};

class AdbManager {
public:
    static bool Initialize(const std::string& adbPath = "");
    static std::vector<DeviceInfo> GetDevices();

private:
    static std::string m_adbPath;
    static std::string Execute(const std::string& command);
};