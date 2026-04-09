#include <windows.h>
#include "AdbManager.hpp"
#include <cstdio>
#include <array>
#include <sstream>
#include <vector>

std::string AdbManager::m_adbPath = "adb";

bool AdbManager::Initialize(const std::string& adbPath) {
    if (!adbPath.empty()) {
        m_adbPath = adbPath;
    }

    std::string result = Execute(m_adbPath + " version");
    return result.find("Android Debug Bridge") != std::string::npos;
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
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = pipeWrite;
    si.hStdError = pipeWrite;

    // Command must be writable for CreateProcessW
    std::vector<wchar_t> cmdLine(command.begin(), command.end());
    cmdLine.push_back(0);

    if (!CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pipeRead);
        CloseHandle(pipeWrite);
        return "ERROR: CreateProcess failed";
    }

    CloseHandle(pipeWrite);

    // Read output
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

        // Parse model from "model:XXX"
        std::string token;
        while (lineStream >> token) {
            if (token.find("model:") == 0) {
                info.model = token.substr(6);
                break;
            }
        }

        if (info.model.empty()) info.model = "Unknown";

        devices.push_back(info);
    }

    return devices;
}