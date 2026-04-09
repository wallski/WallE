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

struct FileEntry {
    std::string name;
    std::string size;
    std::string date;
    std::string permissions;
    bool isDirectory;
};

class AdbManager {
public:
    static bool Initialize(const std::string& adbPath = "");
    static void KillServer();
    static std::vector<DeviceInfo> GetDevices();
    static void GetDeviceDetails(DeviceInfo& dev);

    // File Management
    static std::vector<FileEntry> ListFiles(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool Exists(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool PushFile(const std::string& serial, const std::string& localPath, const std::string& remotePath);
    static bool PullFile(const std::string& serial, const std::string& remotePath, const std::string& localPath);
    static bool CreateDirectory(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool DeleteEntry(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool RenameEntry(const std::string& serial, const std::string& oldPath, const std::string& newPath, bool useRoot = false);

private:
    static std::string m_adbPath;
    static std::string Execute(const std::string& command);
    static std::string Trim(const std::string& s);
};