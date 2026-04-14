#include "MirrorManager.hpp"
#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <wmcodecdsp.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

// ---------------------------------------------------------------------------
// Internal helper: fire-and-forget adb command
// ---------------------------------------------------------------------------
static void RunSilentAdb(const std::string& cmd) {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    if (CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
MirrorManager::MirrorManager() {
    MFStartup(MF_VERSION);
}

MirrorManager::~MirrorManager() {
    Stop();
    MFShutdown();
}

// ---------------------------------------------------------------------------
// Public: Start
// ---------------------------------------------------------------------------
bool MirrorManager::Start(const std::string& serial, ID3D11Device* device, std::mutex* d3dMutex, int bitrateMbps) {
    if (m_running) return true;

    m_serial = serial;
    m_d3dDevice = device;
    m_d3dMutex = d3dMutex;
    m_bitrate = bitrateMbps;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);

    m_running = true;
    m_isInitialized = false;

    m_receiveThread = std::thread(&MirrorManager::ReceiverThread, this);
    m_decodeThread = std::thread(&MirrorManager::DecoderThread, this);
    return true;
}

// ---------------------------------------------------------------------------
// Public: Stop
// ---------------------------------------------------------------------------
void MirrorManager::Stop() {
    m_running = false;

    if (m_receiveThread.joinable()) m_receiveThread.join();
    if (m_decodeThread.joinable())  m_decodeThread.join();

    CleanupWMF();

    if (m_frameSRV) { m_frameSRV->Release();     m_frameSRV = nullptr; }
    if (m_frameTexture) { m_frameTexture->Release();  m_frameTexture = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release();    m_d3dContext = nullptr; }
    m_d3dMutex = nullptr;
}

// ---------------------------------------------------------------------------
// Private: ReceiverThread
// ---------------------------------------------------------------------------
void MirrorManager::ReceiverThread() {
    // KEYCODE_WAKEUP (224): turns screen on, no-op if already on.
    // keyevent 26 = power toggle (caused the flashlight/on-off bug).
    RunSilentAdb("adb -s \"" + m_serial + "\" shell input keyevent 224");

    // Read actual screen resolution
    m_width = 1080;
    m_height = 2340;
    {
        std::string resCmd = "adb -s \"" + m_serial + "\" shell wm size";
        FILE* p = _popen(resCmd.c_str(), "r");
        if (p) {
            char buf[128] = {};
            if (fgets(buf, sizeof(buf), p)) {
                std::string s(buf);
                size_t xPos = s.find('x');
                size_t colon = s.find(':');
                if (xPos != std::string::npos && colon != std::string::npos) {
                    try {
                        m_width = std::stoi(s.substr(colon + 1, xPos - colon - 1));
                        m_height = std::stoi(s.substr(xPos + 1));
                    }
                    catch (...) {}
                }
            }
            _pclose(p);
        }
    }
    m_aspectRatio = (float)m_width / (float)m_height;
    m_isInitialized = true;

    // Stream loop — restarts automatically when screenrecord times out
    while (m_running) {
        std::string cmd =
            "adb -s \"" + m_serial + "\" exec-out screenrecord "
            "--display-id 0 "
            "--output-format=h264 "
            "--bit-rate " + std::to_string(m_bitrate * 1000000) + " "
            "--time-limit 170 -";

        SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
        HANDLE hRd, hWr;
        if (!CreatePipe(&hRd, &hWr, &sa, 0)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        SetHandleInformation(hRd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = { sizeof(si) };
        si.hStdOutput = hWr;
        si.hStdError = hWr;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        std::vector<char> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(0);

        if (!CreateProcessA(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hRd);
            CloseHandle(hWr);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        CloseHandle(hWr);

        uint8_t readBuf[64 * 1024];
        DWORD bytesRead = 0;
        while (m_running) {
            BOOL ok = ReadFile(hRd, readBuf, sizeof(readBuf), &bytesRead, NULL);
            if (!ok || bytesRead == 0) break;

            std::lock_guard<std::mutex> lock(m_bufferMutex);
            m_streamBuffer.insert(m_streamBuffer.end(), readBuf, readBuf + bytesRead);

            if (m_streamBuffer.size() > 16 * 1024 * 1024)
                m_streamBuffer.clear();
        }

        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRd);

        if (m_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ---------------------------------------------------------------------------
// Private: DecoderThread
//   H264 -> NV12 (decoder) -> ARGB32 (VideoProcessorMFT) -> D3D11 texture
//
//   KEY FIX: VideoProcessorMFT does NOT allocate its own output buffer.
//   You must pre-allocate a sample and pass it in via vpOut.pSample before
//   calling ProcessOutput, otherwise it silently returns E_INVALIDARG and
//   you never get any frames out.
// ---------------------------------------------------------------------------
void MirrorManager::DecoderThread() {
    while (m_running && !m_isInitialized)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!m_running) return;

    if (!InitDecoder(m_width, m_height)) {
        m_running = false;
        return;
    }

    LONGLONG sampleTime = 0;
    const LONGLONG frameDur = 333333; // 30fps in 100ns units

    std::vector<uint8_t> workBuf;

    while (m_running) {
        // Drain shared buffer
        {
            std::lock_guard<std::mutex> lock(m_bufferMutex);
            if (!m_streamBuffer.empty()) {
                workBuf.insert(workBuf.end(), m_streamBuffer.begin(), m_streamBuffer.end());
                m_streamBuffer.clear();
            }
        }

        if (workBuf.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Feed chunk into H264 decoder
        IMFSample* pSample = nullptr;
        IMFMediaBuffer* pBuffer = nullptr;
        BYTE* pData = nullptr;
        DWORD           dataSize = (DWORD)workBuf.size();

        if (SUCCEEDED(MFCreateMemoryBuffer(dataSize, &pBuffer))) {
            pBuffer->Lock(&pData, NULL, NULL);
            memcpy(pData, workBuf.data(), dataSize);
            pBuffer->Unlock();
            pBuffer->SetCurrentLength(dataSize);

            MFCreateSample(&pSample);
            pSample->AddBuffer(pBuffer);
            pSample->SetSampleTime(sampleTime);
            pSample->SetSampleDuration(frameDur);
            sampleTime += frameDur;

            HRESULT hrIn = m_decoder->ProcessInput(m_inputStreamID, pSample, 0);
            pSample->Release();
            pBuffer->Release();

            char inDbg[64];
            sprintf_s(inDbg, "[Mirror] ProcessInput: 0x%08X, buf size: %zu\n", (unsigned)hrIn, workBuf.size());
            OutputDebugStringA(inDbg);

            if (SUCCEEDED(hrIn)) {
                // Pull NV12 frames from decoder
                bool keepPulling = true;
                while (keepPulling && m_running) {
                    MFT_OUTPUT_DATA_BUFFER decOut = {};
                    decOut.dwStreamID = m_outputStreamID;
                    DWORD decStatus = 0;

                    HRESULT hrDec = m_decoder->ProcessOutput(0, 1, &decOut, &decStatus);

                    char decDbg[64];
                    sprintf_s(decDbg, "[Mirror] Decoder out: 0x%08X\n", (unsigned)hrDec);
                    OutputDebugStringA(decDbg);

                    if (hrDec == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                        keepPulling = false;
                    }
                    else if (hrDec == MF_E_TRANSFORM_STREAM_CHANGE) {
                        HandleStreamChange();
                    }
                    else if (SUCCEEDED(hrDec) && decOut.pSample) {
                        // Feed NV12 into VideoProcessorMFT
                        decOut.pSample->SetSampleTime(sampleTime);
                        decOut.pSample->SetSampleDuration(frameDur);

                        HRESULT hrVpIn = m_videoProcessor->ProcessInput(0, decOut.pSample, 0);
                        decOut.pSample->Release();

                        char vpInDbg[64];
                        sprintf_s(vpInDbg, "[Mirror] VP ProcessInput: 0x%08X\n", (unsigned)hrVpIn);
                        OutputDebugStringA(vpInDbg);

                        if (SUCCEEDED(hrVpIn)) {
                            // Pre-allocate output sample for VideoProcessorMFT.
                            // This is required — VideoProcessorMFT does not allocate
                            // its own output buffer. Without this, ProcessOutput
                            // returns E_INVALIDARG silently and no frames come out.
                            IMFSample* pVpSample = nullptr;
                            IMFMediaBuffer* pVpBuffer = nullptr;
                            DWORD vpBufSize = m_width * m_height * 4; // ARGB32 = 4 bytes/pixel

                            MFCreateMemoryBuffer(vpBufSize, &pVpBuffer);
                            MFCreateSample(&pVpSample);
                            pVpSample->AddBuffer(pVpBuffer);
                            pVpBuffer->Release();

                            MFT_OUTPUT_DATA_BUFFER vpOut = {};
                            vpOut.dwStreamID = 0;
                            vpOut.pSample = pVpSample; // hand it our pre-allocated sample
                            DWORD vpStatus = 0;

                            HRESULT hrVpOut = m_videoProcessor->ProcessOutput(0, 1, &vpOut, &vpStatus);

                            if (SUCCEEDED(hrVpOut)) {
                                IMFMediaBuffer* pOutBuf = nullptr;
                                if (SUCCEEDED(pVpSample->GetBufferByIndex(0, &pOutBuf))) {
                                    BYTE* bits = nullptr;
                                    DWORD curLen = 0;
                                    if (SUCCEEDED(pOutBuf->Lock(&bits, NULL, &curLen))) {
                                        if (m_frameTexture && m_d3dContext) {
                                            std::lock_guard<std::mutex> lock(*m_d3dMutex);
                                            m_d3dContext->UpdateSubresource(
                                                m_frameTexture, 0, NULL,
                                                bits,
                                                m_width * 4, // row pitch: 4 bytes per pixel
                                                0
                                            );
                                        }
                                        pOutBuf->Unlock();
                                    }
                                    pOutBuf->Release();
                                }
                            }
                            else {
                                char dbg[128];
                                sprintf_s(dbg, "[Mirror] VP ProcessOutput failed: 0x%08X\n", (unsigned)hrVpOut);
                                OutputDebugStringA(dbg);
                            }

                            pVpSample->Release();
                            if (vpOut.pEvents) vpOut.pEvents->Release();
                        }
                    }
                    else {
                        if (decOut.pSample) decOut.pSample->Release();
                        if (decOut.pEvents) decOut.pEvents->Release();
                        keepPulling = false;
                    }
                }
            }
        }

        std::this_thread::yield();
    }
}

// ---------------------------------------------------------------------------
// Private: InitDecoder
//   Two-step pipeline:
//     1. CLSID_CMSH264DecoderMFT  -> NV12  (universally supported)
//     2. CLSID_VideoProcessorMFT  -> ARGB32 (maps to DXGI_FORMAT_B8G8R8A8_UNORM)
// ---------------------------------------------------------------------------
bool MirrorManager::InitDecoder(int width, int height) {
    // --- Step 1: H264 decoder, output NV12 ---
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT, NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_decoder)
    );
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to create H264 decoder\n");
        return false;
    }

    IMFMediaType* pInType = nullptr;
    MFCreateMediaType(&pInType);
    pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(pInType, MF_MT_FRAME_SIZE, width, height);
    hr = m_decoder->SetInputType(m_inputStreamID, pInType, 0);
    pInType->Release();
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to set H264 input type\n");
        return false;
    }

    IMFMediaType* pDecOut = nullptr;
    MFCreateMediaType(&pDecOut);
    pDecOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pDecOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pDecOut, MF_MT_FRAME_SIZE, width, height);
    hr = m_decoder->SetOutputType(m_outputStreamID, pDecOut, 0);
    pDecOut->Release();
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to set NV12 output on decoder\n");
        return false;
    }

    m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // --- Step 2: VideoProcessorMFT: NV12 -> ARGB32 ---
    hr = CoCreateInstance(
        CLSID_VideoProcessorMFT, NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_videoProcessor)
    );
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to create VideoProcessorMFT\n");
        return false;
    }

    IMFMediaType* pVpIn = nullptr;
    MFCreateMediaType(&pVpIn);
    pVpIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pVpIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pVpIn, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pVpIn, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_videoProcessor->SetInputType(0, pVpIn, 0);
    pVpIn->Release();
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to set VideoProcessor input type\n");
        return false;
    }

    IMFMediaType* pVpOut = nullptr;
    MFCreateMediaType(&pVpOut);
    pVpOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pVpOut->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    MFSetAttributeSize(pVpOut, MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(pVpOut, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    hr = m_videoProcessor->SetOutputType(0, pVpOut, 0);
    pVpOut->Release();
    if (FAILED(hr)) {
        OutputDebugStringA("[Mirror] Failed to set VideoProcessor output type\n");
        return false;
    }

    m_videoProcessor->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_videoProcessor->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_videoProcessor->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    // --- Create D3D11 texture + SRV ---
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    {
        std::lock_guard<std::mutex> lock(*m_d3dMutex);
        m_d3dDevice->CreateTexture2D(&desc, NULL, &m_frameTexture);
        m_d3dDevice->CreateShaderResourceView(m_frameTexture, NULL, &m_frameSRV);
    }

    OutputDebugStringA("[Mirror] Decoder + VideoProcessor init OK\n");
    return true;
}

// ---------------------------------------------------------------------------
// Private: HandleStreamChange
//   Re-negotiates NV12 on decoder when stream format changes.
// ---------------------------------------------------------------------------
void MirrorManager::HandleStreamChange() {
    IMFMediaType* pType = nullptr;
    for (DWORD i = 0; ; i++) {
        if (FAILED(m_decoder->GetOutputAvailableType(m_outputStreamID, i, &pType))) break;

        GUID subtype = GUID_NULL;
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            m_decoder->SetOutputType(m_outputStreamID, pType, 0);

            UINT32 w = 0, h = 0;
            MFGetAttributeSize(pType, MF_MT_FRAME_SIZE, &w, &h);
            if (w > 0 && h > 0 && ((int)w != m_width || (int)h != m_height)) {
                m_width = (int)w;
                m_height = (int)h;
                m_aspectRatio = (float)m_width / (float)m_height;

                if (m_frameSRV) { m_frameSRV->Release();    m_frameSRV = nullptr; }
                if (m_frameTexture) { m_frameTexture->Release(); m_frameTexture = nullptr; }

                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = m_width;
                desc.Height = m_height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                std::lock_guard<std::mutex> lock(*m_d3dMutex);
                m_d3dDevice->CreateTexture2D(&desc, NULL, &m_frameTexture);
                m_d3dDevice->CreateShaderResourceView(m_frameTexture, NULL, &m_frameSRV);
            }

            pType->Release();
            break;
        }
        pType->Release();
    }
}

// ---------------------------------------------------------------------------
// Private: CleanupWMF
// ---------------------------------------------------------------------------
void MirrorManager::CleanupWMF() {
    if (m_decoder) { m_decoder->Release();        m_decoder = nullptr; }
    if (m_videoProcessor) { m_videoProcessor->Release(); m_videoProcessor = nullptr; }
}

// ---------------------------------------------------------------------------
// Public: SendTap / SendSwipe — detached threads so UI doesn't freeze
// ---------------------------------------------------------------------------
void MirrorManager::SendTap(int x, int y) {
    std::string cmd = "adb -s " + m_serial +
        " shell input tap " + std::to_string(x) + " " + std::to_string(y);
    std::thread([cmd]() { RunSilentAdb(cmd); }).detach();
}

void MirrorManager::SendSwipe(int x1, int y1, int x2, int y2, int durationMs) {
    std::string cmd = "adb -s " + m_serial +
        " shell input swipe " +
        std::to_string(x1) + " " + std::to_string(y1) + " " +
        std::to_string(x2) + " " + std::to_string(y2) + " " +
        std::to_string(durationMs);
    std::thread([cmd]() { RunSilentAdb(cmd); }).detach();
}