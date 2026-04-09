#include "DeviceManager.hpp"
#include "AdbManager.hpp"
#include <windows.h>
#include <chrono>
#include <thread>

DeviceManager::DeviceManager() : m_running(false), m_lastUpdateTime(0) {}

DeviceManager::~DeviceManager() {
    Stop();
}

void DeviceManager::Start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&DeviceManager::UpdateThread, this);
}

void DeviceManager::Stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DeviceManager::UpdateThread() {
    AdbManager::Initialize(); // Initialize once

    while (m_running) {
        float now = (float)GetTickCount() / 1000.0f;

        // Update every 2 seconds
        if (now - m_lastUpdateTime >= 2.0f) {
            std::vector<DeviceInfo> newDevices = AdbManager::GetDevices();

            std::lock_guard<std::mutex> lock(m_mutex);
            m_devices = newDevices;
            m_lastUpdateTime = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

std::vector<DeviceInfo> DeviceManager::GetDevices() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devices;
}