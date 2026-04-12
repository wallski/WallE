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
    struct AppInfo {
        std::string name;
        std::string packageId;
        std::string version;
        std::string apkPath;
        bool isSystem;
    };

    static bool Initialize(const std::string& adbPath = "");
    static void KillServer();
    static std::string Execute(const std::string& command);

    static std::vector<DeviceInfo> GetDevices();
    static void GetDeviceDetails(DeviceInfo& dev);
    
    // File Operations
    static std::vector<FileEntry> ListFiles(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool PullFile(const std::string& serial, const std::string& remote, const std::string& local);
    static bool PushFile(const std::string& serial, const std::string& local, const std::string& remote);
    static bool DeleteEntry(const std::string& serial, const std::string& remote, bool isDirectory, bool useRoot = false);
    static bool RenameEntry(const std::string& serial, const std::string& oldPath, const std::string& newPath, bool useRoot = false);
    static bool CreateDirectory(const std::string& serial, const std::string& path, bool useRoot = false);
    static bool Exists(const std::string& serial, const std::string& path, bool useRoot = false);

    // App Management
    static std::vector<AppInfo> ListApps(const std::string& serial, bool includeSystem = false);
    static bool UninstallApp(const std::string& serial, const std::string& packageId);
    static bool GetAppDetails(const std::string& serial, const std::string& packageId, std::string& outVersion, std::string& outLabel);
    static bool LaunchApp(const std::string& serial, const std::string& packageId);
    static bool InstallApp(const std::string& serial, const std::string& localPath);

private:
    static std::string m_adbPath;
    static std::string Trim(const std::string& s);
};