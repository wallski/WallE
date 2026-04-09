#pragma once
#include "AdbManager.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    void Start();
    void Stop();
    std::vector<DeviceInfo> GetDevices(); // Thread-safe copy

private:
    void UpdateThread();

    std::thread m_thread;
    std::atomic<bool> m_running;
    std::mutex m_mutex;
    std::vector<DeviceInfo> m_devices;
    float m_lastUpdateTime;
};