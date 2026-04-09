#pragma once
#include <string>
#include <vector>

struct DeviceInfo {
    std::string serial;
    std::string status;       // "device", "offline", "unauthorized"
    std::string model;

    // Rich device info (populated by GetDeviceDetails)
    std::string brand;
    std::string manufacturer;
    std::string androidVersion;
    std::string sdkVersion;
    std::string buildNumber;
    std::string securityPatch;
    std::string cpuAbi;
    std::string screenResolution;
    std::string batteryLevel;
    std::string batteryStatus;
    std::string storageTotal;
    std::string storageUsed;
    std::string ramTotal;
    std::string ramAvail;
    std::string bootloaderStatus;
    std::string imei;

    bool detailsLoaded = false;
};

class AdbManager {
public:
    static bool Initialize(const std::string& adbPath = "");
    static void KillServer();
    static std::vector<DeviceInfo> GetDevices();
    static void GetDeviceDetails(DeviceInfo& dev);

private:
    static std::string m_adbPath;
    static std::string Execute(const std::string& command);
    static std::string Trim(const std::string& s);
};