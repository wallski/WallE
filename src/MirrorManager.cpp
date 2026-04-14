#include "MirrorManager.hpp"
#include <iostream>
#include <fcntl.h>
#include <io.h>
#include <wmcodecdsp.h>
#include <mferror.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

static void RunSilentAdb(const std::string& cmd);

MirrorManager::MirrorManager() {
    MFStartup(MF_VERSION);
}

MirrorManager::~MirrorManager() {
    Stop();
    MFShutdown();
}

bool MirrorManager::Start(const std::string& serial, ID3D11Device* device, std::mutex* d3dMutex, int bitrateMbps) {
    if (m_running) return true;

    m_serial = serial;
    m_d3dDevice = device;
    m_d3dDevice->GetImmediateContext(&m_d3dContext);
    m_d3dMutex = d3dMutex;
    m_bitrate = bitrateMbps;

    m_running = true;
    m_isInitialized = false;
    m_receiveThread = std::thread(&MirrorManager::ReceiverThread, this);
    m_decodeThread = std::thread(&MirrorManager::DecoderThread, this);

    return true;
}

void MirrorManager::Stop() {
    m_running = false;

    if (m_receiveThread.joinable()) m_receiveThread.join();
    if (m_decodeThread.joinable()) m_decodeThread.join();

    CleanupWMF();

    if (m_frameSRV) { m_frameSRV->Release(); m_frameSRV = nullptr; }
    if (m_frameTexture) { m_frameTexture->Release(); m_frameTexture = nullptr; }
    if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
    m_d3dMutex = nullptr;
}

void MirrorManager::ReceiverThread() {
    m_width = 1080; m_height = 2340;

    RunSilentAdb("adb -s \"" + m_serial + "\" shell input keyevent 26");

    std::string resCmd = "adb -s \"" + m_serial + "\" shell wm size";
    FILE* p = _popen(resCmd.c_str(), "r");
    if (p) {
        char buf[128];
        if (fgets(buf, sizeof(buf), p)) {
            std::string s(buf);
            size_t xPos = s.find('x');
            if (xPos != std::string::npos) {
                try {
                    m_width = std::stoi(s.substr(s.find(':') + 1, xPos));
                    m_height = std::stoi(s.substr(xPos + 1));
                }
                catch (...) {}
            }
        }
        _pclose(p);
    }
    m_aspectRatio = (float)m_width / (float)m_height;
    m_isInitialized = true;

    std::string cmd = "adb -s \"" + m_serial + "\" exec-out screenrecord --display-id 0 --output-format=h264 --bit-rate " + std::to_string(m_bitrate * 1000000) + " --time-limit 180 -";

    SECURITY_ATTRIBUTES saAttr = { sizeof(saAttr), NULL, TRUE };
    HANDLE hRd, hWr;
    if (!CreatePipe(&hRd, &hWr, &saAttr, 0)) { m_running = false; return; }
    SetHandleInformation(hRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.hStdError = hWr; si.hStdOutput = hWr; si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = { 0 };

    std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(0);

    if (!CreateProcessA(NULL, cmdBuffer.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        m_running = false; CloseHandle(hRd); CloseHandle(hWr); return;
    }
    CloseHandle(hWr);

    uint8_t readBuf[64 * 1024];
    DWORD bytesRead;
    while (m_running) {
        if (ReadFile(hRd, readBuf, sizeof(readBuf), &bytesRead, NULL) && bytesRead > 0) {
            if (bytesRead > 10 && bytesRead < 300 && strstr((char*)readBuf, "INVALID_LAYER_STACK")) {
                OutputDebugStringA("[Mirror Error] Phone is LOCKED or on a SECURE screen. Please unlock your phone!\n");
            }

            std::lock_guard<std::mutex> lock(m_bufferMutex);
            m_streamBuffer.insert(m_streamBuffer.end(), readBuf, readBuf + bytesRead);
            if (m_streamBuffer.size() > 10 * 1024 * 1024) m_streamBuffer.clear();
        }
        else {
            break;
        }
    }

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(hRd);
    m_running = false;
}

void MirrorManager::DecoderThread() {
    while (m_running && !m_isInitialized) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (FAILED(InitDecoder(m_width, m_height))) {
        m_running = false;
        return;
    }

    std::vector<uint8_t> workBuf;
    while (m_running) {
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

        size_t pos = 0;
        while (pos + 3 < workBuf.size()) {
            int scLen = 0;
            if (workBuf[pos] == 0 && workBuf[pos + 1] == 0 && workBuf[pos + 2] == 1) scLen = 3;
            else if (pos + 4 <= workBuf.size() && workBuf[pos] == 0 && workBuf[pos + 1] == 0 && workBuf[pos + 2] == 0 && workBuf[pos + 3] == 1) scLen = 4;

            if (scLen > 0) {
                size_t nextPos = pos + scLen;
                bool foundNext = false;
                while (nextPos + 3 < workBuf.size()) {
                    if (workBuf[nextPos] == 0 && workBuf[nextPos + 1] == 0 && (workBuf[nextPos + 2] == 1 || (nextPos + 4 <= workBuf.size() && workBuf[nextPos + 2] == 0 && workBuf[nextPos + 3] == 1))) {
                        foundNext = true;
                        break;
                    }
                    nextPos++;
                }

                if (foundNext) {
                    size_t nalSize = nextPos - pos;
                    IMFSample* pSample = nullptr;
                    IMFMediaBuffer* pBuffer = nullptr;
                    BYTE* pData = nullptr;

                    if (SUCCEEDED(MFCreateMemoryBuffer((DWORD)nalSize, &pBuffer))) {
                        pBuffer->Lock(&pData, NULL, NULL);
                        memcpy(pData, &workBuf[pos], nalSize);
                        pBuffer->Unlock();
                        pBuffer->SetCurrentLength((DWORD)nalSize);
                        MFCreateSample(&pSample);
                        pSample->AddBuffer(pBuffer);

                        m_decoder->ProcessInput(m_inputStreamID, pSample, 0);

                        MFT_OUTPUT_DATA_BUFFER out = { 0 };
                        out.dwStreamID = m_outputStreamID;
                        DWORD status = 0;
                        HRESULT hr = m_decoder->ProcessOutput(0, 1, &out, &status);

                        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                            HandleStreamChange();
                        }
                        else if (SUCCEEDED(hr)) {
                            if (out.pSample) {
                                IMFMediaBuffer* b = nullptr;
                                out.pSample->GetBufferByIndex(0, &b);
                                if (b) {
                                    BYTE* bits = nullptr;
                                    DWORD curLen = 0;
                                    if (SUCCEEDED(b->Lock(&bits, NULL, &curLen))) {
                                        if (m_frameTexture) {
                                            if (m_d3dMutex) {
                                                std::lock_guard<std::mutex> lock(*m_d3dMutex);
                                                m_d3dContext->UpdateSubresource(m_frameTexture, 0, NULL, bits, m_width * 4, 0);
                                            }
                                        }
                                        b->Unlock();
                                    }
                                    b->Release();
                                }
                                out.pSample->Release();
                            }
                            if (out.pEvents) out.pEvents->Release();
                        }

                        pSample->Release();
                        pBuffer->Release();
                    }

                    workBuf.erase(workBuf.begin(), workBuf.begin() + nextPos);
                    pos = 0;
                    continue;
                }
                else break;
            }
            pos++;
        }
        std::this_thread::yield();
    }
}

void MirrorManager::HandleStreamChange() {
    IMFMediaType* pType = nullptr;
    if (SUCCEEDED(m_decoder->GetOutputAvailableType(m_outputStreamID, 0, &pType))) {
        m_decoder->SetOutputType(m_outputStreamID, pType, 0);

        if (m_frameSRV) m_frameSRV->Release();
        if (m_frameTexture) m_frameTexture->Release();

        D3D11_TEXTURE2D_DESC d = { 0 };
        d.Width = m_width; d.Height = m_height;
        d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        if (m_d3dMutex) {
            std::lock_guard<std::mutex> lock(*m_d3dMutex);
            m_d3dDevice->CreateTexture2D(&d, NULL, &m_frameTexture);
            m_d3dDevice->CreateShaderResourceView(m_frameTexture, NULL, &m_frameSRV);
        }

        pType->Release();
    }
}

bool MirrorManager::InitDecoder(int width, int height) {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_decoder));
    if (FAILED(hr)) return false;

    IMFMediaType* pInType = nullptr;
    MFCreateMediaType(&pInType);
    pInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    m_decoder->SetInputType(m_inputStreamID, pInType, 0);
    pInType->Release();

    IMFMediaType* pOutType = nullptr;
    MFCreateMediaType(&pOutType);
    pOutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    m_decoder->SetOutputType(m_outputStreamID, pOutType, 0);
    pOutType->Release();

    m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}

void MirrorManager::CleanupWMF() {
    if (m_decoder) { m_decoder->Release(); m_decoder = nullptr; }
    if (m_videoProcessor) { m_videoProcessor->Release(); m_videoProcessor = nullptr; }
}

// CreateProcessA needs mutable buffer
static void RunSilentAdb(const std::string& cmd) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> cmdBuffer(cmd.begin(), cmd.end());
    cmdBuffer.push_back(0);

    if (CreateProcessA(NULL, cmdBuffer.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 1000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void MirrorManager::SendTap(int x, int y) {
    std::string cmd = "adb -s " + m_serial + " shell input tap " + std::to_string(x) + " " + std::to_string(y);
    RunSilentAdb(cmd);
}

void MirrorManager::SendSwipe(int x1, int y1, int x2, int y2, int duration) {
    std::string cmd = "adb -s " + m_serial + " shell input swipe " + std::to_string(x1) + " " + std::to_string(y1) + " " + std::to_string(x2) + " " + std::to_string(y2) + " " + std::to_string(duration);
    RunSilentAdb(cmd);
}