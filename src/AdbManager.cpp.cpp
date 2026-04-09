#include <windows.h>
#include "AdbManager.hpp"
#include <cstdio>
#include <array>
#include <sstream>
#include <vector>
#include <algorithm>

std::string AdbManager::m_adbPath = "adb";

bool AdbManager::Initialize(const std::string& adbPath) {
    if (!adbPath.empty()) {
        m_adbPath = adbPath;
    }
    std::string result = Execute(m_adbPath + " version");
    return result.find("Android Debug Bridge") != std::string::npos;
}

void AdbManager::KillServer() {
    Execute(m_adbPath + " kill-server");
}

std::string AdbManager::Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::string AdbManager::Execute(const std::string& command) {
    std::string result;
    HANDLE pipeRead, pipeWrite;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 0)) {
        return "ERROR: CreatePipe failed";
    }

    PROCESS_INFORMATION pi;
    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipeWrite;
    si.hStdError  = pipeWrite;

    // Command must be writable for CreateProcessW
    std::vector<wchar_t> cmdLine(command.begin(), command.end());
    cmdLine.push_back(0);

    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pipeRead);
        CloseHandle(pipeWrite);
        return "ERROR: CreateProcess failed";
    }

    CloseHandle(pipeWrite);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(pipeRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    CloseHandle(pipeRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

std::vector<DeviceInfo> AdbManager::GetDevices() {
    std::vector<DeviceInfo> devices;
    std::string output = Execute(m_adbPath + " devices -l");
    std::istringstream stream(output);
    std::string line;

    bool firstLine = true;
    while (std::getline(stream, line)) {
        if (firstLine) {
            firstLine = false;
            continue;
        }

        if (line.empty()) continue;

        DeviceInfo info;
        std::istringstream lineStream(line);
        lineStream >> info.serial >> info.status;

        if (info.serial.empty()) continue;

        // Check connection type
        bool isUSB = false;
        std::string token;
        while (lineStream >> token) { 
            if (token.find("usb:") != std::string::npos ||
                token.find("product:") != std::string::npos) {
                isUSB = true;
            }
            if (token.find("model:") == 0) {
                info.model = token.substr(6);
            }
        }

        // Skip wireless devices (no USB connection detected)
        if (!isUSB) {
            continue;
        }

        if (info.model.empty()) info.model = "Unknown";

        if (info.status == "device") {
            devices.push_back(info);
        }
    }

    return devices;
}

void AdbManager::GetDeviceDetails(DeviceInfo& dev) {
    if (dev.status != "device") return;

    std::string s = dev.serial;
    auto prop = [&](const std::string& key) -> std::string {
        return Trim(Execute(m_adbPath + " -s " + s + " shell getprop " + key));
    };

    dev.brand        = prop("ro.product.brand");
    dev.manufacturer = prop("ro.product.manufacturer");
    dev.androidVersion = prop("ro.build.version.release");
    dev.sdkVersion   = prop("ro.build.version.sdk");
    dev.buildNumber  = prop("ro.build.display.id");
    dev.securityPatch = prop("ro.build.version.security_patch");
    dev.cpuAbi       = prop("ro.product.cpu.abi");
    dev.bootloaderStatus = prop("ro.boot.verifiedbootstate"); // "green"=locked, "orange"=unlocked

    // Screen resolution
    std::string wm = Trim(Execute(m_adbPath + " -s " + s + " shell wm size"));
    // Output: "Physical size: 1080x2400"
    auto colon = wm.find(':');
    dev.screenResolution = (colon != std::string::npos) ? Trim(wm.substr(colon + 1)) : wm;

    // Battery
    std::string bat = Execute(m_adbPath + " -s " + s + " shell dumpsys battery");
    auto parseField = [&](const std::string& txt, const std::string& field) -> std::string {
        auto pos = txt.find(field);
        if (pos == std::string::npos) return "";
        auto eol = txt.find('\n', pos);
        auto colon2 = txt.find(':', pos);
        if (colon2 == std::string::npos || colon2 > eol) return "";
        return Trim(txt.substr(colon2 + 1, eol - colon2 - 1));
    };
    dev.batteryLevel  = parseField(bat, "level");
    std::string bStat = parseField(bat, "status");
    // status codes: 1=unknown, 2=charging, 3=discharging, 4=not charging, 5=full
    if      (bStat == "1") dev.batteryStatus = "Unknown";
    else if (bStat == "2") dev.batteryStatus = "Charging";
    else if (bStat == "3") dev.batteryStatus = "Discharging";
    else if (bStat == "4") dev.batteryStatus = "Not Charging";
    else if (bStat == "5") dev.batteryStatus = "Full";
    else                   dev.batteryStatus = bStat;

    // Storage - parse df /sdcard output
    std::string df = Execute(m_adbPath + " -s " + s + " shell df /data");
    // Parse the second line
    std::istringstream dfStream(df);
    std::string dfLine;
    std::getline(dfStream, dfLine); // header
    if (std::getline(dfStream, dfLine)) {
        std::istringstream dfL(dfLine);
        std::string filesystem, blocks, used, avail;
        dfL >> filesystem >> blocks >> used >> avail;
        // Values in 1K blocks -> convert to GB
        auto toGB = [](const std::string& kb) -> std::string {
            try {
                long long v = std::stoll(kb);
                float gb = v / (1024.0f * 1024.0f);
                char buf[32];
                snprintf(buf, sizeof(buf), "%.1f GB", gb);
                return buf;
            } catch (...) { return kb; }
        };
        dev.storageTotal = toGB(blocks);
        dev.storageUsed  = toGB(used);
    }

    // RAM - from /proc/meminfo
    std::string mem = Execute(m_adbPath + " -s " + s + " shell cat /proc/meminfo");
    auto memField = [&](const std::string& field) -> std::string {
        auto pos = mem.find(field);
        if (pos == std::string::npos) return "";
        auto eol = mem.find('\n', pos);
        auto col = mem.find(':', pos);
        if (col == std::string::npos || col > eol) return "";
        std::string val = Trim(mem.substr(col + 1, eol - col - 1));
        // val is like "3893608 kB"
        auto sp = val.find(' ');
        try {
            long long kb = std::stoll(val.substr(0, sp));
            float gb = kb / (1024.0f * 1024.0f);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f GB", gb);
            return buf;
        } catch (...) { return val; }
    };
    dev.ramTotal = memField("MemTotal");
    dev.ramAvail = memField("MemAvailable");

    dev.detailsLoaded = true;
}

std::vector<FileEntry> AdbManager::ListFiles(const std::string& serial, const std::string& path, bool useRoot) {
    std::vector<FileEntry> files;
    std::string cmd = m_adbPath + " -s " + serial + " shell ";
    if (useRoot) cmd = cmd + "su -c \"ls -la " + path + "\"";
    else cmd += "ls -la " + path;

    std::string output = Execute(cmd);
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line.find("total ") == 0) continue;
        if (line.find("permission denied") != std::string::npos || line.find("Permission denied") != std::string::npos) continue;

        // --- NEW Pivot-Based Parser ---
        std::vector<std::string> tokens;
        std::istringstream ls(line);
        std::string t;
        while (ls >> t) tokens.push_back(t);

        if (tokens.size() < 4) continue;

        // Find the "Time" token (e.g., "17:06" or "12:03") to use as an anchor.
        // Toybox usually has perms, links, owner, group, size, date, time, name.
        // But columns can be missing. The time is almost always the 7th or 8th token.
        int timeIdx = -1;
        for (int i = 0; i < (int)tokens.size(); ++i) {
            if (tokens[i].find(':') != std::string::npos && tokens[i].length() >= 3 && isdigit(tokens[i][0])) {
                timeIdx = i;
                break;
            }
        }

        if (timeIdx == -1) continue;

        // Name is everything after the time token. 
        // We find where the time token ends in the raw line to capture the name with original spacing.
        size_t namePos = line.find(tokens[timeIdx]);
        if (namePos != std::string::npos) {
            namePos += tokens[timeIdx].length();
            while (namePos < line.length() && (line[namePos] == ' ' || line[namePos] == '\t')) namePos++;
        }
        
        std::string name = (namePos != std::string::npos && namePos < line.length()) ? line.substr(namePos) : tokens.back();
        if (name == "." || name == "..") continue;

        FileEntry fe;
        fe.name = name;
        fe.permissions = tokens[0];
        fe.isDirectory = (fe.permissions[0] == 'd' || fe.permissions[0] == 'l');
        
        // Size is typically 2 tokens before time (links owner group size date time)
        // Adjust for toybox: date is at timeIdx-1, size is at timeIdx-2
        std::string sizeStr = (timeIdx >= 2) ? tokens[timeIdx - 2] : "0";
        fe.size = fe.isDirectory ? "--" : sizeStr;

        if (!fe.isDirectory) {
            try {
                long long s = std::stoll(sizeStr);
                char buf[64];
                if      (s > 1024LL * 1024 * 1024) snprintf(buf, sizeof(buf), "%.2f GB", s / (1024.0f * 1024 * 1024));
                else if (s > 1024LL * 1024)        snprintf(buf, sizeof(buf), "%.2f MB", s / (1024.0f * 1024));
                else if (s > 1024LL)               snprintf(buf, sizeof(buf), "%.2f KB", s / 1024.0f);
                else                               snprintf(buf, sizeof(buf), "%lld B", s);
                fe.size = buf;
            } catch (...) { fe.size = sizeStr; }
        }

        fe.date = tokens[timeIdx - 1] + " " + tokens[timeIdx];
        files.push_back(fe);
    }

    return files;
}

bool AdbManager::Exists(const std::string& serial, const std::string& path, bool useRoot) {
    std::string cmd = m_adbPath + " -s " + serial + " shell ";
    if (useRoot) cmd = cmd + "su -c \"[ -e '" + path + "' ] && echo 1 || echo 0\"";
    else cmd += "[ -e \"" + path + "\" ] && echo 1 || echo 0";
    return Execute(cmd).find('1') != std::string::npos;
}

bool AdbManager::PushFile(const std::string& serial, const std::string& localPath, const std::string& remotePath) {
    std::string cmd = m_adbPath + " -s " + serial + " push \"" + localPath + "\" \"" + remotePath + "\"";
    std::string res = Execute(cmd);
    return res.find("pushed") != std::string::npos;
}

bool AdbManager::PullFile(const std::string& serial, const std::string& remotePath, const std::string& localPath) {
    std::string cmd = m_adbPath + " -s " + serial + " pull \"" + remotePath + "\" \"" + localPath + "\"";
    std::string res = Execute(cmd);
    return res.find("pulled") != std::string::npos;
}

bool AdbManager::CreateDirectory(const std::string& serial, const std::string& path, bool useRoot) {
    std::string cmd = m_adbPath + " -s " + serial + " shell ";
    if (useRoot) cmd = cmd + "su -c \"mkdir -p '" + path + "'\"";
    else cmd += "mkdir -p \"" + path + "\"";
    Execute(cmd);
    return true; 
}

bool AdbManager::DeleteEntry(const std::string& serial, const std::string& path, bool useRoot) {
    std::string cmd = m_adbPath + " -s " + serial + " shell ";
    if (useRoot) cmd = cmd + "su -c \"rm -rf '" + path + "'\"";
    else cmd += "rm -rf \"" + path + "\"";
    Execute(cmd);
    return true;
}

bool AdbManager::RenameEntry(const std::string& serial, const std::string& oldPath, const std::string& newPath, bool useRoot) {
    std::string cmd = m_adbPath + " -s " + serial + " shell ";
    if (useRoot) cmd = cmd + "su -c \"mv '" + oldPath + "' '" + newPath + "'\"";
    else cmd += "mv \"" + oldPath + "\" \"" + newPath + "\"";
    Execute(cmd);
    return true;
}