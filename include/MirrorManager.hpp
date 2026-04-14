#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mfreadwrite.h>
#include <d3d11_1.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

class MirrorManager {
public:
    MirrorManager();
    ~MirrorManager();

    bool Start(const std::string& serial, ID3D11Device* device, std::mutex* d3dMutex, int bitrateMbps = 8);
    void Stop();
    bool IsRunning() const { return m_running; }

    // Remote control
    void SendTap(int x, int y);
    void SendSwipe(int x1, int y1, int x2, int y2, int durationMs);

    // Rendering
    ID3D11ShaderResourceView* GetFrameSRV() { return m_frameSRV; }
    float GetAspectRatio() const { return m_aspectRatio; }

private:
    void ReceiverThread();
    void DecoderThread();
    
    // WMF Helpers
    bool InitDecoder(int width, int height);
    void HandleStreamChange();
    void CleanupWMF();

    std::string m_serial;
    ID3D11Device* m_d3dDevice = nullptr;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    ID3D11Texture2D* m_frameTexture = nullptr;
    ID3D11ShaderResourceView* m_frameSRV = nullptr;

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_isInitialized{ false };
    std::thread m_receiveThread;
    std::thread m_decodeThread;
    
    std::mutex m_bufferMutex;
    std::vector<uint8_t> m_streamBuffer;
    
    float m_aspectRatio = 0.5625f;
    int m_width = 1080;
    int m_height = 2400;
    int m_bitrate = 8;

    // WMF & D3D Objects
    std::mutex* m_d3dMutex = nullptr;
    IMFTransform* m_decoder = nullptr;
    IMFTransform* m_videoProcessor = nullptr;
    DWORD m_inputStreamID = 0;
    DWORD m_outputStreamID = 0;
};
