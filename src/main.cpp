#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stack>
#include "AdbManager.hpp"
#include "DeviceManager.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <d3d11_1.h>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <tchar.h>
#include <thread>
#include <vector>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

// D3D globals
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
static std::mutex g_d3dMutex;

// UI state
static int g_activeTab = 0;
static int g_selectedDevice = -1;
static float g_tabAnim[6] = {};
static float g_sidebarWidth = 260.0f;

// GPU Selection state
struct GpuInfo {
    std::string name;
    std::string memory;
};
static std::vector<GpuInfo> g_gpuList;
static int g_selectedGpu = 0;
static int g_requestedGpu = -1;
static bool g_gpuListLoaded = false;

#include "MirrorManager.hpp"
static MirrorManager g_mirror;
static bool g_mirrorActive = false;
static float g_mirrorWatchdog = 0.0f;

// Cached ADB state - only re-checked every 5 seconds, not every frame
static bool g_adbReady = false;
static float g_adbCheckTimer = -99.0f; // force first check immediately
static const float kAdbCheckInterval = 5.0f;

// Device State
static std::string g_lastDetailedSerial = "";
static DeviceInfo g_cachedDetails;

// File Manager State
static std::string g_currentPath = "/storage/emulated/0";
static std::vector<FileEntry> g_fileList;
static std::stack<std::string> g_pathHistory;
static bool g_fileListLoading = false;
static bool g_rootMode = false;
static bool g_showRootWarning = false;
static std::string g_selectedFile;
static bool g_showConflictPopup = false;
static std::string g_conflictLocalPath;
static std::string g_conflictRemotePath;

// Apps Manager State
static std::vector<AdbManager::AppInfo> g_appList;
static bool g_appListLoading = false;
static bool g_includeSystemApps = false;
static char g_appSearchQuery[128] = "";
static std::string g_selectedAppPackage;
static bool g_showUninstallPopup = false;
static std::atomic<bool> g_appResolverRunning{false};
static std::mutex g_appMutex;
static std::string g_appStatusMessage;
static float g_appStatusTimer = 0.0f;

// Terminal Settings
struct TerminalConfig {
  bool showAscii = true;
  ImU32 colPrompt = IM_COL32(155, 89, 182, 255); // Purple Prompt
  ImU32 colLogo = IM_COL32(155, 89, 182, 255);   // Purple Logo
  ImU32 colLabel = IM_COL32(142, 68, 173, 255);  // Deep Purple Label
  ImU32 colValue = IM_COL32(236, 240, 241, 255); // White Value
};
static TerminalConfig g_termConfig = {
    true, IM_COL32(46, 204, 113, 255), IM_COL32(155, 89, 182, 255),
    IM_COL32(155, 89, 182, 255), IM_COL32(236, 240, 241, 255)};

// App Persistence
struct AppCacheEntry {
  std::string name;
  std::string version;
};
static std::map<std::string, AppCacheEntry> g_appLabelCache;
static std::mutex g_appCacheMutex;

// Terminal Pro State
struct TerminalSpan {
  std::string text;
  ImU32 color;
};
struct TerminalLine {
  std::vector<TerminalSpan> spans;
};

static std::vector<TerminalLine> g_terminalBuffer;
static char g_terminalInput[512] = "";
static std::string g_terminalCwd = "/";
static std::vector<std::string> g_terminalHistory;
static int g_terminalHistoryIdx = -1;
static bool g_terminalScrollToBottom = true;
static bool g_terminalRequestFocus = true;

// Modal States
static bool g_showRenamePopup = false;
static bool g_showDeletePopup = false;
static bool g_showNewFolderPopup = false;
static char g_nameBuffer[256] = "";
static char g_fileSearchQuery[128] = "";
static std::string g_actionTarget; // The file/folder being acted upon

// Fonts
static ImFont *g_fontRegular = nullptr;
static ImFont *g_fontBold = nullptr;
static ImFont *g_fontMono = nullptr;

// Forward declarations
struct IDXGIAdapter1;
bool CreateDeviceD3D(HWND hWnd, IDXGIAdapter1* pAdapter);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- NATIVE HELPERS ---
static std::string PickLocalFile() {
  char szFile[MAX_PATH] = {0};
  OPENFILENAMEA ofn = {sizeof(ofn)};
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = sizeof(szFile);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (GetOpenFileNameA(&ofn))
    return szFile;
  return "";
}

static std::string SaveLocalFile(const std::string &defaultName) {
  char szFile[MAX_PATH] = {0};
  strncpy_s(szFile, defaultName.c_str(), _TRUNCATE);
  OPENFILENAMEA ofn = {sizeof(ofn)};
  ofn.lpstrFile = szFile;
  ofn.nMaxFile = sizeof(szFile);
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  if (GetSaveFileNameA(&ofn))
    return szFile;
  return "";
}

// --- UI COMPONENTS ---
static void DrawFolderIcon(ImDrawList *dl, ImVec2 pos, float size) {
  ImU32 col = IM_COL32(243, 156, 18, 255);
  float w = size * 0.8f;
  float h = size * 0.6f;
  // Tab
  dl->AddRectFilled(ImVec2(pos.x, pos.y - 2.0f),
                    ImVec2(pos.x + w * 0.4f, pos.y), col, 2.0f);
  // Body
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col, 2.0f);
}

static void DrawFileIcon(ImDrawList *dl, ImVec2 pos, float size) {
  ImU32 col = IM_COL32(189, 195, 199, 255);
  float w = size * 0.6f;
  float h = size * 0.8f;
  dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col, 1.0f);
  // Fold
  dl->AddTriangleFilled(ImVec2(pos.x + w - 4.0f, pos.y),
                        ImVec2(pos.x + w, pos.y + 4.0f),
                        ImVec2(pos.x + w, pos.y), IM_COL32(40, 40, 50, 255));
}

static void DrawAppIcon(ImDrawList *dl, ImVec2 pos,
                        const std::string &packageId, bool isSystem) {
  float size = 20.0f;
  // Generate a stable color from the packageId hash
  size_t hash = std::hash<std::string>{}(packageId);
  ImU32 col;
  if (isSystem) {
    col = IM_COL32(80, 80, 100, 255);
  } else {
    // Pick from a nice palette based on hash
    ImU32 palette[] = {
        IM_COL32(91, 110, 245, 255), // Blue
        IM_COL32(46, 204, 113, 255), // Green
        IM_COL32(155, 89, 182, 255), // Purple
        IM_COL32(231, 76, 60, 255),  // Red
        IM_COL32(241, 196, 15, 255)  // Yellow
    };
    col = palette[hash % 5];
  }

  dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), col, 6.0f);

  // Draw initial
  char initial =
      packageId.empty()
          ? '?'
          : (char)toupper(packageId[packageId.find_last_of('.') + 1]);
  if (!packageId.empty() && packageId.find('.') == std::string::npos)
    initial = (char)toupper(packageId[0]);

  char buf[2] = {initial, '\0'};
  ImVec2 txtSize = ImGui::CalcTextSize(buf);
  dl->AddText(ImVec2(pos.x + (size - txtSize.x) * 0.5f,
                     pos.y + (size - txtSize.y) * 0.5f),
              IM_COL32(255, 255, 255, 200), buf);
}

// THEME
static void ApplyWallETheme() {
  ImGuiStyle &s = ImGui::GetStyle();

  s.WindowRounding = 0.0f;
  s.ChildRounding = 10.0f;
  s.FrameRounding = 7.0f;
  s.PopupRounding = 8.0f;
  s.ScrollbarRounding = 6.0f;
  s.GrabRounding = 6.0f;
  s.TabRounding = 8.0f;
  s.ItemSpacing = ImVec2(10.0f, 8.0f);
  s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
  s.FramePadding = ImVec2(12.0f, 7.0f);
  s.WindowPadding = ImVec2(0.0f, 0.0f);
  s.ScrollbarSize = 8.0f;
  s.GrabMinSize = 10.0f;
  s.WindowBorderSize = 0.0f;
  s.ChildBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.IndentSpacing = 16.0f;

  ImVec4 *c = s.Colors;

  c[ImGuiCol_WindowBg] = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
  c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.12f, 1.0f);
  c[ImGuiCol_Border] = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_Text] = ImVec4(0.91f, 0.91f, 0.94f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.42f, 0.50f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.14f, 0.14f, 0.20f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.18f, 0.26f, 1.0f);
  c[ImGuiCol_TitleBg] = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0.07f, 0.07f, 0.09f, 1.0f);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.25f, 0.35f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.8f);
  c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
  c[ImGuiCol_CheckMark] = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
  c[ImGuiCol_SliderGrab] = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
  c[ImGuiCol_SliderGrabActive] = ImVec4(0.482f, 0.557f, 1.0f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.357f, 0.431f, 0.961f, 0.85f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.482f, 0.557f, 1.0f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.270f, 0.340f, 0.840f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.357f, 0.431f, 0.961f, 0.25f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.40f);
  c[ImGuiCol_HeaderActive] = ImVec4(0.357f, 0.431f, 0.961f, 0.60f);
  c[ImGuiCol_Separator] = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
  c[ImGuiCol_SeparatorHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.7f);
  c[ImGuiCol_SeparatorActive] = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
  c[ImGuiCol_ResizeGrip] = ImVec4(0.357f, 0.431f, 0.961f, 0.20f);
  c[ImGuiCol_ResizeGripHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.60f);
  c[ImGuiCol_ResizeGripActive] = ImVec4(0.357f, 0.431f, 0.961f, 0.90f);
  c[ImGuiCol_Tab] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
  c[ImGuiCol_TabHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.50f);
  c[ImGuiCol_TabActive] = ImVec4(0.357f, 0.431f, 0.961f, 0.85f);
  c[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
  c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f, 0.22f, 0.40f, 1.0f);
  c[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.10f, 0.14f, 1.0f);
  c[ImGuiCol_TableBorderStrong] = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
  c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(0.10f, 0.10f, 0.14f, 0.5f);
  c[ImGuiCol_TextSelectedBg] = ImVec4(0.357f, 0.431f, 0.961f, 0.35f);
  c[ImGuiCol_DragDropTarget] = ImVec4(0.357f, 0.431f, 0.961f, 0.90f);
  c[ImGuiCol_NavHighlight] = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
  c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
  c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);
}

// DXGI Helpers
static void EnumerateGpus() {
    if (g_gpuListLoaded) return;
    g_gpuList.clear();

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            continue;
        }

        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 128, NULL, NULL);
        g_gpuList.push_back({ name, std::to_string(desc.DedicatedVideoMemory / (1024 * 1024)) + " MB" });
        adapter->Release();
    }
    factory->Release();
    g_gpuListLoaded = true;
}

static IDXGIAdapter1* GetAdapter(int index) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return nullptr;
    IDXGIAdapter1* adapter = nullptr;
    int hardwareIdx = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            if (hardwareIdx == index) {
                factory->Release();
                return adapter;
            }
            hardwareIdx++;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

//  HELPERS - draw a small colored dot without any font glyph
static void DrawStatusDot(ImDrawList *dl, ImVec2 pos, ImU32 color,
                          float radius = 4.5f) {
  dl->AddCircleFilled(pos, radius, color, 12);
}

//  SIDEBAR
static void RenderSidebar(int &activeTab, float sidebarW) {
  const float winH = ImGui::GetIO().DisplaySize.y;
  const float statusH = 28.0f;
  const float contentH = winH - statusH;

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(sidebarW, contentH));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.039f, 0.039f, 0.055f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##sidebar", ImVec2(sidebarW, contentH), false,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 spos = ImGui::GetWindowPos();

  // Right-edge border
  dl->AddLine(ImVec2(spos.x + sidebarW - 1.0f, spos.y),
              ImVec2(spos.x + sidebarW - 1.0f, spos.y + contentH),
              IM_COL32(42, 42, 56, 255));

  // Logo
  ImGui::SetCursorPos(ImVec2(20.0f, 24.0f));
  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.357f, 0.431f, 0.961f, 1.0f), "[ Wall-E ]");
  if (g_fontBold)
    ImGui::PopFont();

  ImGui::SetCursorPos(ImVec2(20.0f, 46.0f));
  ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.50f, 1.0f), "Android Manager");

  dl->AddLine(ImVec2(spos.x + 16.0f, spos.y + 78.0f),
              ImVec2(spos.x + sidebarW - 16.0f, spos.y + 78.0f),
              IM_COL32(42, 42, 56, 255));

  // Nav items
  static const char *navLabels[] = {"Home",     "Files", "Apps",
                                    "Terminal", "Live",  "Settings"};

  float curY = 92.0f;
  // Handle tab switching focus signals
  static int lastTab = -1;
  if (g_activeTab != lastTab) {
    if (g_activeTab == 3)
      g_terminalRequestFocus = true;
    lastTab = g_activeTab;
  }

  for (int i = 0; i < 6; i++) {
    bool selected = (activeTab == i);

    // Smooth lerp
    float target = selected ? 1.0f : 0.0f;
    g_tabAnim[i] += (target - g_tabAnim[i]) * 0.18f;

    if (g_tabAnim[i] > 0.01f) {
      // Selection highlight
      ImVec2 bMin(spos.x + 10.0f, spos.y + curY);
      ImVec2 bMax(spos.x + sidebarW - 10.0f, spos.y + curY + 38.0f);
      ImU32 bgCol =
          IM_COL32((int)(91 * g_tabAnim[i]), (int)(110 * g_tabAnim[i]),
                   (int)(245 * g_tabAnim[i]), (int)(45 * g_tabAnim[i]));
      dl->AddRectFilled(bMin, bMax, bgCol, 8.0f);

      // Left accent bar
      dl->AddRectFilled(ImVec2(spos.x + 10.0f, spos.y + curY + 6.0f),
                        ImVec2(spos.x + 13.0f, spos.y + curY + 32.0f),
                        IM_COL32(91, 110, 245, (int)(255 * g_tabAnim[i])),
                        4.0f);
    }

    // Clickable button
    ImGui::SetCursorPos(ImVec2(10.0f, curY));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.357f, 0.431f, 0.961f, 0.10f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(0.357f, 0.431f, 0.961f, 0.18f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    char btnId[32];
    snprintf(btnId, sizeof(btnId), "##nav%d", i);
    if (ImGui::Button(btnId, ImVec2(sidebarW - 20.0f, 38.0f)))
      activeTab = i;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    // Label (no icon - pure text)
    ImVec4 textCol = selected ? ImVec4(0.91f, 0.91f, 0.94f, 1.0f)
                              : ImVec4(0.50f, 0.50f, 0.60f, 1.0f);

    ImGui::SetCursorPos(ImVec2(30.0f, curY + 10.0f));
    if (selected && g_fontBold)
      ImGui::PushFont(g_fontBold);
    ImGui::TextColored(textCol, "%s", navLabels[i]);
    if (selected && g_fontBold)
      ImGui::PopFont();

    curY += 46.0f;
  }

  // Version footer
  ImGui::SetCursorPos(ImVec2(20.0f, contentH - 36.0f));
  ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.33f, 1.0f), "v1.0.0");

  ImGui::EndChild();
}

// DEVICE HOME PANEL
static void RenderHomePanel(const std::vector<DeviceInfo> &devices,
                            float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::BeginChild("##home_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();

  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "Device Home");
  if (g_fontBold)
    ImGui::PopFont();
  ImGui::Dummy(ImVec2(0.0f, 10.0f));

  if (devices.empty()) {
    // Reset cached details when device is removed
    g_lastDetailedSerial = "";
    g_cachedDetails = {};

    float cY = (panelH * 0.42f) - 30.0f;
    const char *text1 = "No devices connected";
    const char *text2 = "Enable USB debugging and plug in your device";
    ImVec2 sz1 = ImGui::CalcTextSize(text1);
    ImVec2 sz2 = ImGui::CalcTextSize(text2);
    float textX1 = (panelW - sz1.x) * 0.5f;
    float textX2 = (panelW - sz2.x) * 0.5f;
    float phoneW = 38.0f;
    float phoneX = (panelW - phoneW) * 0.5f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float px = wp.x + phoneX, py = wp.y + cY;
    dl->AddRect(ImVec2(px, py), ImVec2(px + phoneW, py + 62.0f),
                IM_COL32(42, 42, 60, 200), 8.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(px + 4.0f, py + 8.0f),
                      ImVec2(px + 34.0f, py + 46.0f), IM_COL32(42, 42, 60, 80),
                      4.0f);
    dl->AddCircleFilled(ImVec2(px + 19.0f, py + 54.0f), 3.5f,
                        IM_COL32(60, 60, 80, 200));
    ImGui::SetCursorPos(ImVec2(textX1, cY + 76.0f));
    ImGui::TextColored(ImVec4(0.32f, 0.32f, 0.42f, 1.0f), "%s", text1);
    ImGui::SetCursorPos(ImVec2(textX2, cY + 98.0f));
    ImGui::TextColored(ImVec4(0.22f, 0.22f, 0.30f, 1.0f), "%s", text2);
  } else {
    const DeviceInfo &dev = devices[0];

    if (dev.status == "unauthorized") {
      float cX = (panelW * 0.5f) - 140.0f;
      float cY = (panelH * 0.42f);
      ImGui::SetCursorPos(ImVec2(cX + 66.0f, cY));
      if (g_fontBold)
        ImGui::PushFont(g_fontBold);
      ImGui::TextColored(ImVec4(0.91f, 0.30f, 0.24f, 1.0f),
                         "Device Unauthorized!");
      if (g_fontBold)
        ImGui::PopFont();
      ImGui::SetCursorPos(ImVec2(cX, cY + 30.0f));
      ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                         "Please check your phone and allow USB debugging.");
    } else {
      // Fetch details once when a new device connects (not every frame)
      if (dev.serial != g_lastDetailedSerial) {
        g_lastDetailedSerial = dev.serial;
        g_cachedDetails = dev;
        AdbManager::GetDeviceDetails(g_cachedDetails);
      }
      const DeviceInfo &d = g_cachedDetails;

      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 origin = ImGui::GetWindowPos();

      // == Phone Illustration == (Resized to fit above the bar)
      const float phoneX = 28.0f;
      const float phoneY = 40.0f;
      const float phoneW = 46.0f;
      const float phoneH = 82.0f;
      float px = origin.x + phoneX;
      float py = origin.y + phoneY;
      // outer frame
      dl->AddRectFilled(ImVec2(px, py), ImVec2(px + phoneW, py + phoneH),
                        IM_COL32(18, 18, 28, 255), 6.0f);
      dl->AddRect(ImVec2(px, py), ImVec2(px + phoneW, py + phoneH),
                  IM_COL32(91, 110, 245, 160), 6.0f, 0, 1.5f);
      // screen area
      dl->AddRectFilled(ImVec2(px + 4.0f, py + 8.0f),
                        ImVec2(px + phoneW - 4.0f, py + phoneH - 10.0f),
                        IM_COL32(91, 110, 245, 18), 3.0f);
      // front camera dot
      dl->AddCircleFilled(ImVec2(px + phoneW * 0.5f, py + 4.0f), 1.5f,
                          IM_COL32(91, 110, 245, 90));
      // home indicator bar
      dl->AddRectFilled(ImVec2(px + phoneW * 0.3f, py + phoneH - 6.0f),
                        ImVec2(px + phoneW * 0.7f, py + phoneH - 4.0f),
                        IM_COL32(91, 110, 245, 120), 1.0f);

      // == Model Name + subtitle ==
      float infoX = phoneX + phoneW + 24.0f;
      ImGui::SetCursorPos(ImVec2(infoX, 40.0f));
      if (g_fontBold)
        ImGui::PushFont(g_fontBold);
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", d.model.c_str());
      if (g_fontBold)
        ImGui::PopFont();

      ImGui::SetCursorPos(ImVec2(infoX, 65.0f));
      ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.52f, 1.0f), "%s",
                         d.manufacturer.c_str());

      // Online status badge
      ImGui::SetCursorPos(ImVec2(infoX, 88.0f));
      ImVec2 badgePos = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(badgePos,
                        ImVec2(badgePos.x + 80.0f, badgePos.y + 20.0f),
                        IM_COL32(46, 204, 113, 28), 10.0f);
      dl->AddRect(badgePos, ImVec2(badgePos.x + 80.0f, badgePos.y + 20.0f),
                  IM_COL32(46, 204, 113, 130), 10.0f);
      dl->AddCircleFilled(ImVec2(badgePos.x + 12.0f, badgePos.y + 10.0f), 3.5f,
                          IM_COL32(46, 204, 113, 255));
      ImGui::SetCursorPos(ImVec2(infoX + 22.0f, 91.0f));
      ImGui::TextColored(ImVec4(0.18f, 0.80f, 0.44f, 1.0f), "Online");

      // Separator
      float sepY = origin.y + 130.0f;
      dl->AddLine(ImVec2(origin.x + 20.0f, sepY),
                  ImVec2(origin.x + panelW - 20.0f, sepY),
                  IM_COL32(42, 42, 56, 255));

      // == Info Grid (2 columns of label/value pairs) ==
      const float gridTop = 144.0f;
      const float col1X = 28.0f;
      const float col2X = panelW * 0.5f;
      const float rowH = 26.0f;

      // Helper macros for label+value rows
      auto InfoRow = [&](float x, float y, const char *label,
                         const std::string &value,
                         ImVec4 valueColor = ImVec4(0.9f, 0.9f, 0.92f, 1.0f)) {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.55f, 1.0f), "%s", label);
        ImGui::SetCursorPos(ImVec2(x + 130.0f, y));
        ImGui::TextColored(valueColor, "%s",
                           value.empty() ? "--" : value.c_str());
      };

      if (!d.detailsLoaded) {
        ImGui::SetCursorPos(ImVec2(col1X, gridTop));
        ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.52f, 1.0f),
                           "Loading device info...");
      } else {
        // Column 1 - Software
        InfoRow(col1X, gridTop + rowH * 0, "Android",
                "Android " + d.androidVersion);
        InfoRow(col1X, gridTop + rowH * 1, "SDK", d.sdkVersion);
        InfoRow(col1X, gridTop + rowH * 2, "Build", d.buildNumber);
        InfoRow(col1X, gridTop + rowH * 3, "Security", d.securityPatch);
        InfoRow(col1X, gridTop + rowH * 4, "Bootloader", d.bootloaderStatus,
                d.bootloaderStatus == "orange"
                    ? ImVec4(0.95f, 0.61f, 0.07f, 1.0f)
                    : ImVec4(0.18f, 0.80f, 0.44f, 1.0f));
        InfoRow(col1X, gridTop + rowH * 5, "CPU", d.cpuAbi);

        // Column 2 - Hardware
        std::string battStr = d.batteryLevel.empty()
                                  ? "--"
                                  : d.batteryLevel + "% " + d.batteryStatus;
        ImVec4 battColor = ImVec4(0.9f, 0.9f, 0.92f, 1.0f);
        if (!d.batteryLevel.empty()) {
          int lvl = std::stoi(d.batteryLevel);
          if (lvl >= 60)
            battColor = ImVec4(0.18f, 0.80f, 0.44f, 1.0f);
          else if (lvl >= 25)
            battColor = ImVec4(0.95f, 0.61f, 0.07f, 1.0f);
          else
            battColor = ImVec4(0.91f, 0.30f, 0.24f, 1.0f);
        }
        InfoRow(col2X, gridTop + rowH * 0, "Battery", battStr, battColor);
        InfoRow(col2X, gridTop + rowH * 1, "Screen", d.screenResolution);
        InfoRow(col2X, gridTop + rowH * 2, "Storage",
                d.storageUsed + " / " + d.storageTotal);
        InfoRow(
            col2X, gridTop + rowH * 3, "RAM",
            (d.ramAvail.empty() ? "--" : d.ramAvail + " free / " + d.ramTotal));
        InfoRow(col2X, gridTop + rowH * 4, "Serial", d.serial);
        InfoRow(col2X, gridTop + rowH * 5, "Brand", d.brand);
      }
    }
  }

  ImGui::EndChild();
}

// FILE MANAGER PANEL
static void RenderFilePanel(const std::vector<DeviceInfo> &devices,
                            float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  ImGui::BeginChild("##file_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();

  if (devices.empty()) {
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f),
                       "Connect a device to browse files.");
    ImGui::EndChild();
    return;
  }

  const DeviceInfo &dev = devices[0];

  // --- Header / Toolbar ---
  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "[ Files ]");
  if (g_fontBold)
    ImGui::PopFont();

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 140.0f);
  if (ImGui::Checkbox("Root Access", &g_rootMode)) {
    if (g_rootMode) {
      g_rootMode = false; // reset until warning accepted
      g_showRootWarning = true;
    }
  }

  ImGui::SetCursorPos(ImVec2(24.0f, 55.0f));
  if (ImGui::Button("Back") && g_currentPath != "/") {
    size_t lastSlash = g_currentPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
      if (lastSlash == 0)
        g_currentPath = "/";
      else
        g_currentPath = g_currentPath.substr(0, lastSlash);
      g_fileListLoading = false;
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(panelW - 320.0f);
  char pathBuf[512];
  strncpy_s(pathBuf, g_currentPath.c_str(), _TRUNCATE);
  if (ImGui::InputText("##curpath", pathBuf, sizeof(pathBuf),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    g_currentPath = pathBuf;
    g_fileListLoading = false;
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 200.0f);
  if (ImGui::Button("New Folder")) {
    g_nameBuffer[0] = '\0';
    g_showNewFolderPopup = true;
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 90.0f);
  if (ImGui::Button("Refresh"))
    g_fileListLoading = false;

  // --- Fetch Logic ---
  if (!g_fileListLoading) {
    g_fileList = AdbManager::ListFiles(dev.serial, g_currentPath, g_rootMode);
    g_fileListLoading = true;
  }

  ImGui::Dummy(ImVec2(0, 10));

  // --- Table ---
  if (ImGui::BeginTable("##file_table", 3,
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable,
                        ImVec2(0, panelH - 180.0f))) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed,
                            180.0f);
    ImGui::TableHeadersRow();

    int rowIdx = 0;
    for (const auto &file : g_fileList) {
      // Search Filter
      if (g_fileSearchQuery[0] != '\0') {
        std::string nameLower = file.name;
        std::string searchLower = g_fileSearchQuery;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       ::tolower);
        std::transform(searchLower.begin(), searchLower.end(),
                       searchLower.begin(), ::tolower);
        if (nameLower.find(searchLower) == std::string::npos)
          continue;
      }

      ImGui::PushID(rowIdx++);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();

      // Icon + Label
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 cp = ImGui::GetCursorScreenPos();
      if (file.isDirectory)
        DrawFolderIcon(dl, ImVec2(cp.x, cp.y + 4.0f), 16.0f);
      else
        DrawFileIcon(dl, ImVec2(cp.x, cp.y + 4.0f), 16.0f);

      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
      bool selected = (g_selectedFile == file.name);
      const char *label = file.name.empty() ? "[Unnamed]" : file.name.c_str();
      if (ImGui::Selectable(label, selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowDoubleClick)) {
        g_selectedFile = file.name;
        if (file.isDirectory && ImGui::IsMouseDoubleClicked(0)) {
          g_pathHistory.push(g_currentPath);
          if (g_currentPath.back() != '/')
            g_currentPath += "/";
          g_currentPath += file.name;
          g_fileListLoading = false;
          g_selectedFile = "";
        }
      }

      // --- Context Menu ---
      if (ImGui::BeginPopupContextItem()) {
        g_selectedFile = file.name;
        if (ImGui::MenuItem("Download")) {
          std::string local = SaveLocalFile(file.name);
          if (!local.empty())
            AdbManager::PullFile(dev.serial, g_currentPath + "/" + file.name,
                                 local);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rename")) {
          g_actionTarget = file.name;
          strncpy_s(g_nameBuffer, file.name.c_str(), _TRUNCATE);
          g_showRenamePopup = true;
        }
        if (ImGui::MenuItem("Delete", nullptr, false, true)) {
          g_actionTarget = file.name;
          g_showDeletePopup = true;
        }
        ImGui::EndPopup();
      }

      ImGui::TableNextColumn();
      ImGui::Text("%s", file.size.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%s", file.date.c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // --- Action Bar ---
  ImGui::SetCursorPos(ImVec2(24.0f, panelH - 50.0f));
  if (ImGui::Button("Download") && !g_selectedFile.empty()) {
    std::string local = SaveLocalFile(g_selectedFile);
    if (!local.empty()) {
      std::string remote = g_currentPath + "/" + g_selectedFile;
      AdbManager::PullFile(dev.serial, remote, local);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Upload")) {
    std::string local = PickLocalFile();
    if (!local.empty()) {
      size_t slash = local.find_last_of("\\/");
      std::string filename =
          (slash != std::string::npos) ? local.substr(slash + 1) : local;
      std::string remote = g_currentPath + "/" + filename;

      if (AdbManager::Exists(dev.serial, remote, g_rootMode)) {
        g_conflictLocalPath = local;
        g_conflictRemotePath = remote;
        g_showConflictPopup = true;
      } else {
        AdbManager::PushFile(dev.serial, local, remote);
        g_fileListLoading = false;
      }
    }
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 220.0f);
  ImGui::SetNextItemWidth(180.0f);
  ImGui::InputTextWithHint("##search", "Search...", g_fileSearchQuery,
                           sizeof(g_fileSearchQuery));

  // --- Popups ---
  if (g_showRootWarning)
    ImGui::OpenPopup("Root Safety Warning");
  if (ImGui::BeginPopupModal("Root Safety Warning", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("WARNING: Root mode allows absolute power over the device.");
    ImGui::Text("(Note: Device must be rooted for this to work)");
    ImGui::Text("Incorrect actions can brick the OS or delete critical data.");
    ImGui::Separator();
    if (ImGui::Button("Accept Risk", ImVec2(120, 0))) {
      g_rootMode = true;
      g_showRootWarning = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      g_showRootWarning = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (g_showConflictPopup)
    ImGui::OpenPopup("File Conflict");
  if (ImGui::BeginPopupModal("File Conflict", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("The file already exists on the device.");
    ImGui::Separator();
    if (ImGui::Button("Overwrite")) {
      AdbManager::PushFile(dev.serial, g_conflictLocalPath,
                           g_conflictRemotePath);
      g_showConflictPopup = false;
      g_fileListLoading = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename")) {
      // Simple rename: append timestamp
      std::string r = g_conflictRemotePath + "_" + std::to_string(time(NULL));
      AdbManager::PushFile(dev.serial, g_conflictLocalPath, r);
      g_showConflictPopup = false;
      g_fileListLoading = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Skip")) {
      g_showConflictPopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (g_showRenamePopup)
    ImGui::OpenPopup("Rename Item");
  if (ImGui::BeginPopupModal("Rename Item", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Enter new name for: %s", g_actionTarget.c_str());
    ImGui::InputText("##newname", g_nameBuffer, sizeof(g_nameBuffer));
    ImGui::Separator();
    if (ImGui::Button("OK", ImVec2(120, 0))) {
      AdbManager::RenameEntry(dev.serial, g_currentPath + "/" + g_actionTarget,
                              g_currentPath + "/" + g_nameBuffer, g_rootMode);
      g_showRenamePopup = false;
      g_fileListLoading = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      g_showRenamePopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (g_showDeletePopup)
    ImGui::OpenPopup("Confirm Delete");
  if (ImGui::BeginPopupModal("Confirm Delete", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Are you sure you want to delete '%s'?",
                g_actionTarget.c_str());
    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f),
                       "This action cannot be undone.");
    ImGui::Separator();
    if (ImGui::Button("Delete", ImVec2(120, 0))) {
      AdbManager::DeleteEntry(dev.serial, g_currentPath + "/" + g_actionTarget,
                              g_rootMode);
      g_showDeletePopup = false;
      g_fileListLoading = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      g_showDeletePopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (g_showNewFolderPopup)
    ImGui::OpenPopup("New Folder");
  if (ImGui::BeginPopupModal("New Folder", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Enter folder name:");
    ImGui::InputText("##foldername", g_nameBuffer, sizeof(g_nameBuffer));
    ImGui::Separator();
    if (ImGui::Button("Create", ImVec2(120, 0))) {
      AdbManager::CreateRemoteDir(
          dev.serial, g_currentPath + "/" + g_nameBuffer, g_rootMode);
      g_showNewFolderPopup = false;
      g_fileListLoading = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      g_showNewFolderPopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::EndChild();
}

// --- Terminal Logic ---
static void AppendToTerminal(const std::string &text,
                             ImU32 defaultColor = IM_COL32(232, 233, 237,
                                                           255)) {
  // Very simple ANSI parser for colors
  TerminalLine currentLine;
  std::string currentText = "";
  ImU32 currentColor = defaultColor;

  auto flushSpan = [&]() {
    if (!currentText.empty()) {
      currentLine.spans.push_back({currentText, currentColor});
      currentText = "";
    }
  };

  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] == '\n') {
      flushSpan();
      g_terminalBuffer.push_back(currentLine);
      currentLine = {};
    } else if (text[i] == '\x1b' && i + 2 < text.length() &&
               text[i + 1] == '[') {
      flushSpan();
      size_t mPos = text.find('m', i);
      if (mPos != std::string::npos) {
        std::string code = text.substr(i + 2, mPos - (i + 2));
        // Basic SGR color mapping
        if (code == "0" || code == "")
          currentColor = defaultColor;
        else if (code == "31")
          currentColor = IM_COL32(232, 76, 61, 255); // Red
        else if (code == "32")
          currentColor = IM_COL32(46, 204, 113, 255); // Green
        else if (code == "33")
          currentColor = IM_COL32(241, 196, 15, 255); // Yellow
        else if (code == "34")
          currentColor = IM_COL32(52, 152, 219, 255); // Blue
        else if (code == "35")
          currentColor = IM_COL32(155, 89, 182, 255); // Magenta
        else if (code == "36")
          currentColor = IM_COL32(26, 188, 156, 255); // Cyan
        i = mPos;
      }
    } else {
      currentText += text[i];
    }
  }
  flushSpan();
  if (!currentLine.spans.empty())
    g_terminalBuffer.push_back(currentLine);
}

static int TerminalInputCallback(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    // We stored the serial in the UserData field (see RenderTerminalPanel)
    const char *serial = (const char *)data->UserData;
    if (!serial)
      return 0;

    // Tab Completion
    std::string input = data->Buf;
    size_t lastSpace = input.find_last_of(' ');
    std::string prefix =
        (lastSpace == std::string::npos) ? input : input.substr(lastSpace + 1);

    if (!prefix.empty()) {
      std::vector<std::string> matches =
          AdbManager::GetCompletions(serial, prefix, g_terminalCwd);
      if (matches.size() == 1) {
        // Single match: complete it
        std::string completion = matches[0];
        data->DeleteChars(
            (int)(lastSpace == std::string::npos ? 0 : lastSpace + 1),
            (int)prefix.length());
        data->InsertChars(data->CursorPos, completion.c_str());
      } else if (matches.size() > 1) {
        // Multiple matches: list them
        AppendToTerminal("\n");
        std::string list = "";
        for (const auto &m : matches)
          list += m + "  ";
        AppendToTerminal(list + "\n", IM_COL32(100, 100, 120, 255));
      }
    }
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    // History Up/Down
    if (g_terminalHistory.empty())
      return 0;

    int prev_history_idx = g_terminalHistoryIdx;
    if (data->EventKey == ImGuiKey_UpArrow) {
      if (g_terminalHistoryIdx == -1)
        g_terminalHistoryIdx = (int)g_terminalHistory.size() - 1;
      else if (g_terminalHistoryIdx > 0)
        g_terminalHistoryIdx--;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
      if (g_terminalHistoryIdx != -1) {
        if (++g_terminalHistoryIdx >= (int)g_terminalHistory.size())
          g_terminalHistoryIdx = -1;
      }
    }

    if (prev_history_idx != g_terminalHistoryIdx) {
      const char *history_str =
          (g_terminalHistoryIdx >= 0)
              ? g_terminalHistory[g_terminalHistoryIdx].c_str()
              : "";
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, history_str);
    }
  }
  return 0;
}

static void RunWallFetch(const std::string &serial) {
  AdbManager::WallFetchData data = AdbManager::GetWallFetchData(serial);

  const char *ascii[] = {
      "      /\\      ", 
      "     /  \\     ",
      "    / /\\ \\    ", 
      "   / /  \\ \\   ",
      "   \\ \\  / /   ", 
      "    \\ \\/ /    ",
      "     \\  /     ", 
      "      \\/      "
  };

  AppendToTerminal("\n");
  for (int i = 0; i < 8; i++) {
    std::string line = std::string(ascii[i]) + "   ";
    TerminalLine tLine;
    tLine.spans.push_back({line, g_termConfig.colLogo});

    if (i == 2)
      tLine.spans.push_back({"DEVICE: ", g_termConfig.colLabel}),
          tLine.spans.push_back({data.model, g_termConfig.colValue});
    if (i == 3)
      tLine.spans.push_back({"OS:     ", g_termConfig.colLabel}),
          tLine.spans.push_back({"Android " + data.androidVer, g_termConfig.colValue});
    if (i == 4)
      tLine.spans.push_back({"KERNEL: ", g_termConfig.colLabel}),
          tLine.spans.push_back({data.kernel, g_termConfig.colValue});
    if (i == 5)
      tLine.spans.push_back({"STORAGE:", g_termConfig.colLabel}),
          tLine.spans.push_back({data.storage, g_termConfig.colValue});

    g_terminalBuffer.push_back(tLine);
  }
  AppendToTerminal("\n");
}

static void RenderLivePanel(const DeviceInfo &dev, float panelW, float panelH) {
    if (!g_mirrorActive) {
        g_mirror.Start(dev.serial, g_pd3dDevice, &g_d3dMutex);
        g_mirrorActive = true;
        g_mirrorWatchdog = (float)ImGui::GetTime();
    }

    ImGui::SetCursorPos(ImVec2(24.0f, 20.0f));
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "[ LIVE MIRROR: %s ]", dev.model.c_str());
    if (g_fontBold) ImGui::PopFont();

    ID3D11ShaderResourceView* srv = g_mirror.GetFrameSRV();
    if (srv) {
        g_mirrorWatchdog = -1.0f; // Live!
        float aspect = g_mirror.GetAspectRatio();
        float viewH = panelH - 80.0f;
        float viewW = viewH * aspect;
        
        ImGui::SetCursorPosX((panelW - viewW) * 0.5f);
        ImGui::SetCursorPosY(60.0f);
        
        ImVec2 startPos = ImGui::GetCursorScreenPos();
        ImGui::Image((void*)srv, ImVec2(viewW, viewH));
        
        // --- INPUT MAPPING ---
        if (ImGui::IsItemClicked()) {
            ImVec2 mousePos = ImGui::GetMousePos();
            float relX = (mousePos.x - startPos.x) / viewW;
            float relY = (mousePos.y - startPos.y) / viewH;
            
            // Pro mapping: use dynamic resolution from MirrorManager
            g_mirror.SendTap((int)(relX * 1080), (int)(relY * 2340));
        }

        // Drag to Swipe logic
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
             static float dragTimer = 0;
             dragTimer += ImGui::GetIO().DeltaTime;
             if (dragTimer > 0.5f) { // Detect long swipe
                ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                // Send swipe logic here...
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                dragTimer = 0;
             }
        }
    } else {
        // Watchdog: If no frames for 4 seconds, restart
        float now = (float)ImGui::GetTime();
        if (g_mirrorWatchdog > 0.0f && now - g_mirrorWatchdog > 4.0f) {
             g_mirror.Stop();
             g_mirrorActive = false; // Will restart on next frame
        }

        ImGui::SetCursorPosY(panelH * 0.4f);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Syncing with device...");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(panelW * 0.4f, 2.0f), "");
    }
}

static void RenderTerminalPanel(const std::vector<DeviceInfo> &devices,
                                float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  ImGui::BeginChild("##terminal_panel", ImVec2(panelW, panelH), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();

  if (devices.empty()) {
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f),
                       "Connect a device to use terminal.");
    ImGui::EndChild();
    return;
  }

  const DeviceInfo &dev = devices[0];

  // --- Header ---
  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f),
                     "[ Interactive Shell ]");
  if (g_fontBold)
    ImGui::PopFont();

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 120.0f);
  if (ImGui::Button("Clear Buffer"))
    g_terminalBuffer.clear();

  ImGui::Dummy(ImVec2(0, 5));

  // --- Unified Terminal Region (Kitty Style) ---
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.02f, 0.02f, 0.03f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
  if (g_fontMono)
    ImGui::PushFont(g_fontMono);

  if (ImGui::BeginChild("##terminal_unified", ImVec2(0, panelH - 80.0f),
                        true)) {
    // 1. Render Log History
    for (const auto &line : g_terminalBuffer) {
      if (line.spans.empty()) {
        ImGui::NewLine();
        continue;
      }
      for (const auto &span : line.spans) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(span.color), "%s",
                           span.text.c_str());
        ImGui::SameLine(0, 0);
      }
      ImGui::NewLine();
    }

    // 2. Render Current Prompt & Unified Input
    std::string prompt = (g_rootMode ? "# " : "$ ");
    std::string fullPrompt =
        "[" + dev.model + ":" + g_terminalCwd + "]" + prompt;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_termConfig.colPrompt),
                       "%s", fullPrompt.c_str());
    ImGui::SameLine(0, 0);

    // Transparent InputText that sits right after the prompt
    // Using -8.0f to perfectly align with Mono font baseline
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushItemWidth(panelW - ImGui::GetCursorPosX() - 40.0f);

    if (g_terminalRequestFocus) {
      ImGui::SetKeyboardFocusHere();
      g_terminalRequestFocus = false;
    }

    ImGuiInputTextFlags input_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_CallbackCompletion |
                                      ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputText("##shell_input", g_terminalInput,
                         sizeof(g_terminalInput), input_flags,
                         &TerminalInputCallback, (void *)dev.serial.c_str())) {
      std::string cmd = g_terminalInput;
      if (!cmd.empty()) {
        AppendToTerminal("> " + cmd + "\n", g_termConfig.colPrompt);
        g_terminalHistory.push_back(cmd);
        g_terminalHistoryIdx = -1;

        if (cmd == "cls" || cmd == "clear") {
          g_terminalBuffer.clear();
        } else if (cmd == "wallfetch") {
          RunWallFetch(dev.serial);
        } else {
          std::string output =
              AdbManager::ExecuteShell(dev.serial, cmd, g_terminalCwd);
          AppendToTerminal(output);
        }

        g_terminalInput[0] = '\0';
        g_terminalScrollToBottom = true;
        g_terminalRequestFocus = true;
      }
    }
    ImGui::PopStyleColor(2);

    if (g_terminalScrollToBottom) {
      ImGui::SetScrollHereY(1.0f);
      g_terminalScrollToBottom = false;
    }
    ImGui::EndChild();
  }
  if (g_fontMono)
    ImGui::PopFont();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImGui::EndChild();
}
static void RenderComingSoonPanel(const char *title, float panelW,
                                  float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::BeginChild("##cs_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();

  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "%s", title);
  if (g_fontBold)
    ImGui::PopFont();

  float cX = (panelW * 0.5f) - 60.0f;
  float cY = (panelH * 0.45f);
  ImGui::SetCursorPos(ImVec2(cX, cY));
  ImGui::TextColored(ImVec4(0.28f, 0.28f, 0.38f, 1.0f), "Coming soon");
  ImGui::EndChild();
}

static void AppResolverThread(std::string serial) {
  g_appResolverRunning = true;

  // We work on a copy of the pointers/indices to minimize lock time
  std::vector<std::string> targetPackages;
  {
    std::lock_guard<std::mutex> lock(g_appMutex);
    for (const auto &app : g_appList)
      targetPackages.push_back(app.packageId);
  }

  for (const auto &pkg : targetPackages) {
    if (!g_appResolverRunning)
      break; // Check for cancellation signal

    std::string version, label;
    if (AdbManager::GetAppDetails(serial, pkg, version, label)) {
      {
        std::lock_guard<std::mutex> cacheLock(g_appCacheMutex);
        g_appLabelCache[pkg] = {label, version};
      }
      std::lock_guard<std::mutex> lock(g_appMutex);
      for (auto &app : g_appList) {
        if (app.packageId == pkg) {
          app.version = version;
          app.name = label;
          break;
        }
      }
    }
    // Small sleep to prevent ADB saturation
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  g_appResolverRunning = false;
}

static void RenderAppsPanel(const std::vector<DeviceInfo> &devices,
                            float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
  ImGui::BeginChild("##apps_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();

  if (devices.empty()) {
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f),
                       "Connect a device to manage apps.");
    ImGui::EndChild();
    return;
  }

  const DeviceInfo &dev = devices[0];

  // --- Header ---
  if (g_fontBold)
    ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "[ App Manager ]");
  if (g_fontBold)
    ImGui::PopFont();

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 200.0f);
  if (ImGui::Checkbox("System Apps", &g_includeSystemApps)) {
    g_appListLoading = false;
  }

  ImGui::SetCursorPos(ImVec2(24.0f, 55.0f));
  ImGui::SetNextItemWidth(panelW - 140.0f);
  ImGui::InputTextWithHint("##appsearch", "Search packages...",
                           g_appSearchQuery, sizeof(g_appSearchQuery));

  ImGui::SameLine();
  ImGui::BeginDisabled(g_appResolverRunning);
  if (ImGui::Button("Refresh")) {
    g_appResolverRunning = false; // Kill signal for current thread
    g_appListLoading = false;     // Trigger re-fetch
    g_selectedAppPackage = "";    // Clear selection
  }
  ImGui::EndDisabled();

  // --- Fetch Logic ---
  if (!g_appListLoading) {
    {
      std::lock_guard<std::mutex> lock(g_appMutex);
      g_appList.clear();
      g_appList = AdbManager::ListApps(dev.serial, g_includeSystemApps);

      // Immediately apply cached labels
      std::lock_guard<std::mutex> cacheLock(g_appCacheMutex);
      for (auto &app : g_appList) {
        if (g_appLabelCache.count(app.packageId)) {
          app.name = g_appLabelCache[app.packageId].name;
          app.version = g_appLabelCache[app.packageId].version;
        }
      }
    }
    if (!g_appResolverRunning) {
      std::thread(AppResolverThread, dev.serial).detach();
    }
    g_appListLoading = true;
  }

  ImGui::Dummy(ImVec2(0, 10));

  // --- Table ---
  if (ImGui::BeginTable("##app_table", 3,
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable,
                        ImVec2(0, panelH - 180.0f))) {
    ImGui::TableSetupColumn("App / Package ID",
                            ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed,
                            100.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 100.0f);
    ImGui::TableHeadersRow();

    int rowIdx = 0;

    // Lock and copy or iterate carefully
    std::vector<AdbManager::AppInfo> displayList;
    {
      std::lock_guard<std::mutex> lock(g_appMutex);
      displayList = g_appList;
    }

    for (const auto &app : displayList) {
      // Search Filter
      if (g_appSearchQuery[0] != '\0') {
        std::string idLower = app.packageId;
        std::string nameLower = app.name;
        std::string searchLower = g_appSearchQuery;
        std::transform(idLower.begin(), idLower.end(), idLower.begin(),
                       ::tolower);
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                       ::tolower);
        std::transform(searchLower.begin(), searchLower.end(),
                       searchLower.begin(), ::tolower);
        if (idLower.find(searchLower) == std::string::npos &&
            nameLower.find(searchLower) == std::string::npos)
          continue;
      }

      ImGui::PushID(app.packageId.c_str());
      ImGui::TableNextRow();
      ImGui::TableNextColumn();

      // Icon + Label
      ImDrawList *dl = ImGui::GetWindowDrawList();
      ImVec2 cp = ImGui::GetCursorScreenPos();

      DrawAppIcon(dl, ImVec2(cp.x, cp.y + 4.0f), app.packageId, app.isSystem);

      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28.0f);
      bool selected = (g_selectedAppPackage == app.packageId);

      // Use Selectable as the ID anchor for the whole row
      if (ImGui::Selectable("##select", selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0, 36.0f))) {
        g_selectedAppPackage = app.packageId;
      }

      // --- Context Menu ---
      if (ImGui::BeginPopupContextItem("AppCtx")) {
        g_selectedAppPackage = app.packageId;
        if (ImGui::MenuItem("Open on Phone")) {
          AdbManager::LaunchApp(dev.serial, app.packageId);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Backup APK to PC")) {
          std::string local = SaveLocalFile(app.packageId + ".apk");
          if (!local.empty()) {
            AdbManager::PullFile(dev.serial, app.apkPath, local);
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Uninstall App", nullptr, false, !app.isSystem)) {
          g_selectedAppPackage = app.packageId;
          ImGui::OpenPopup("Confirm Uninstall");
        }
        ImGui::EndPopup();
      }

      // Overlay the app info on top of the selectable
      ImGui::SetCursorScreenPos(ImVec2(cp.x + 28.0f, cp.y));
      ImGui::BeginGroup();
      if (g_fontBold)
        ImGui::PushFont(g_fontBold);
      ImGui::Text("%s", app.name.c_str());
      if (g_fontBold)
        ImGui::PopFont();

      ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                           5.0f); // Nudge up to prevent overlap
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.6f, 1.0f));
      ImGui::Text("%s", app.packageId.c_str());
      ImGui::PopStyleColor();
      ImGui::EndGroup();

      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
      ImGui::Text("%s", app.version.c_str());

      ImGui::TableNextColumn();
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
      ImGui::TextColored(app.isSystem ? ImVec4(0.5f, 0.5f, 0.6f, 1.0f)
                                      : ImVec4(0.35f, 0.8f, 0.44f, 1.0f),
                         "%s", app.isSystem ? "System" : "User");

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  // --- Action Bar ---
  ImGui::SetCursorPos(ImVec2(24.0f, panelH - 50.0f));
  if (ImGui::Button("Backup APK") && !g_selectedAppPackage.empty()) {
    std::string local = SaveLocalFile(g_selectedAppPackage + ".apk");
    if (!local.empty()) {
      std::string apkPath;
      {
        std::lock_guard<std::mutex> lock(g_appMutex);
        for (auto &a : g_appList)
          if (a.packageId == g_selectedAppPackage)
            apkPath = a.apkPath;
      }
      if (!apkPath.empty())
        AdbManager::PullFile(dev.serial, apkPath, local);
    }
  }

  ImGui::SameLine();
  if (ImGui::Button("Install APK...")) {
    std::string apk =
        PickLocalFile(); // We can add ".apk" filter logic if needed
    if (!apk.empty()) {
      g_appStatusMessage = "Installing APK...";
      if (AdbManager::InstallApp(dev.serial, apk)) {
        g_appStatusMessage = "Installation Successful!";
        g_appListLoading = false; // Refresh
      } else {
        g_appStatusMessage = "Installation Failed!";
      }
      g_appStatusTimer = 5.0f; // Show for 5 seconds
    }
  }

  if (g_appStatusTimer > 0) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.35f, 0.8f, 0.44f, 1.0f), "%s",
                       g_appStatusMessage.c_str());
    g_appStatusTimer -= ImGui::GetIO().DeltaTime;
  }

  if (g_appResolverRunning) {
    ImGui::SameLine();
    ImGui::SetCursorPosX(panelW - 320.0f);
    ImGui::TextColored(ImVec4(0.35f, 0.43f, 0.96f, 1.0f),
                       "Syncing app details...");
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX(panelW - 100.0f);
  if (ImGui::Button("Uninstall") && !g_selectedAppPackage.empty()) {
    ImGui::OpenPopup("Confirm Uninstall");
  }

  // --- Modals ---
  if (ImGui::BeginPopupModal("Confirm Uninstall", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Are you sure you want to uninstall this application?");
    ImGui::TextColored(ImVec4(0.91f, 0.3f, 0.24f, 1.0f), "%s",
                       g_selectedAppPackage.c_str());
    ImGui::Separator();
    if (ImGui::Button("Uninstall", ImVec2(120, 0))) {
      if (AdbManager::UninstallApp(dev.serial, g_selectedAppPackage)) {
        g_appListLoading = false;
        g_selectedAppPackage = "";
      }
      g_showUninstallPopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
      g_showUninstallPopup = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::EndChild();
}

//  STATUS BAR

static void RenderStatusBar(size_t deviceCount) {
  const float statusH = 28.0f;
  const float winW = ImGui::GetIO().DisplaySize.x;
  const float winH = ImGui::GetIO().DisplaySize.y;

  ImGui::SetNextWindowPos(ImVec2(0.0f, winH - statusH));
  ImGui::SetNextWindowSize(ImVec2(winW, statusH));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.039f, 0.039f, 0.055f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 0.0f));
  ImGui::BeginChild("##statusbar", ImVec2(winW, statusH), false,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 spos = ImGui::GetWindowPos();

  // Top border
  dl->AddLine(ImVec2(spos.x, spos.y), ImVec2(spos.x + winW, spos.y),
              IM_COL32(42, 42, 56, 255));

  float t = (float)ImGui::GetTime();

  // Status dot + text - dot drawn via draw list (no font glyph)
  float dotX = spos.x + 26.0f;
  float dotY = spos.y + 14.0f;

  ImGui::SetCursorPos(ImVec2(36.0f, 6.0f));

  if (!g_adbReady) {
    float p = 0.50f + 0.50f * sinf(t * 3.5f);
    DrawStatusDot(dl, ImVec2(dotX, dotY),
                  IM_COL32(231, 76, 60, (int)(220 * p)));
    ImGui::TextColored(ImVec4(0.91f, 0.30f, 0.24f, 1.0f),
                       "ADB not found - add platform-tools to PATH");
  } else if (deviceCount == 0) {
    float p = 0.50f + 0.50f * sinf(t * 2.0f);
    DrawStatusDot(dl, ImVec2(dotX, dotY),
                  IM_COL32(243, 156, 18, (int)(220 * p)));
    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f),
                       "No devices connected - enable USB debugging");
  } else {
    float p = 0.70f + 0.30f * sinf(t * 1.5f);
    DrawStatusDot(dl, ImVec2(dotX, dotY),
                  IM_COL32(46, 204, 113, (int)(220 * p)));
    ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f),
                       "%zu device%s connected", deviceCount,
                       deviceCount == 1 ? "" : "s");
  }

  // Right-aligned version
  const char *ver = "Wall-E  v0.1";
  ImVec2 verSz = ImGui::CalcTextSize(ver);
  ImGui::SetCursorPos(ImVec2(winW - verSz.x - 20.0f, 6.0f));
  ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.33f, 1.0f), "%s", ver);

  ImGui::EndChild();
}

// WINMAIN
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:WinMainCRTStartup")
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
  // Fix blurry text on high DPI monitors
  SetProcessDPIAware();

  // Make the debug console light purple so it contrasts with the WallE UI
  system("color DF");

  WNDCLASSEX wc = {sizeof(WNDCLASSEX),
                   CS_CLASSDC,
                   WndProc,
                   0,
                   0,
                   GetModuleHandle(nullptr),
                   nullptr,
                   nullptr,
                   nullptr,
                   nullptr,
                   _T("WallE"),
                   nullptr};
  RegisterClassEx(&wc);

  HWND hwnd = CreateWindow(wc.lpszClassName, _T("Wall-E - Android Manager"),
                           WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr,
                           nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(hwnd, nullptr)) {
    CleanupDeviceD3D();
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  // ImGui init
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  (void)io;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  // Load Segoe UI (always present on Windows 7+)
  const char *pathRegular = "C:/Windows/Fonts/segoeui.ttf";
  const char *pathBold = "C:/Windows/Fonts/segoeuib.ttf";
  const char *pathMono = "C:/Windows/Fonts/consola.ttf";

  if (GetFileAttributesA(pathRegular) != INVALID_FILE_ATTRIBUTES)
    g_fontRegular = io.Fonts->AddFontFromFileTTF(pathRegular, 18.0f);
  if (GetFileAttributesA(pathBold) != INVALID_FILE_ATTRIBUTES)
    g_fontBold = io.Fonts->AddFontFromFileTTF(pathBold, 18.0f);
  if (GetFileAttributesA(pathMono) != INVALID_FILE_ATTRIBUTES)
    g_fontMono = io.Fonts->AddFontFromFileTTF(pathMono, 15.0f);

  io.Fonts->Build();

  ApplyWallETheme();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  // Device manager
  static DeviceManager deviceManager;
  deviceManager.Start();

  // Main loop
  bool done = false;
  static int requestedGpu = -1; // Global signal
  
  while (!done) {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

    // Handle Live GPU Hot-Swap Request
    if (g_requestedGpu != -1 && g_requestedGpu != g_selectedGpu) {
        // Reset device
        ImGui_ImplDX11_Shutdown();
        CleanupDeviceD3D();

        IDXGIAdapter1* adapter = GetAdapter(g_requestedGpu);
        if (CreateDeviceD3D(hwnd, adapter)) {
             g_selectedGpu = g_requestedGpu;
             ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
             if (adapter) adapter->Release();
        } else {
             // Fallback to default if selected failed
             CreateDeviceD3D(hwnd, nullptr);
             ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
             g_selectedGpu = 0;
        }
        g_requestedGpu = -1;
    }

    // Throttled ADB check - only runs once every 5 seconds, not every frame
    float now = (float)ImGui::GetTime();
    if (now - g_adbCheckTimer >= kAdbCheckInterval) {
      g_adbReady = AdbManager::Initialize();
      g_adbCheckTimer = now;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_fontRegular)
      ImGui::PushFont(g_fontRegular);

    std::vector<DeviceInfo> devices = deviceManager.GetDevices();

    const float winW = io.DisplaySize.x;
    const float winH = io.DisplaySize.y;
    const float statusH = 28.0f;
    const float contentH = winH - statusH;
    const float panelW = winW - g_sidebarWidth;

    // Full-screen host window
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(winW, winH));
    ImGui::Begin("##host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    RenderSidebar(g_activeTab, g_sidebarWidth);

    // Splitter Logic
    ImGui::SetCursorPos(ImVec2(g_sidebarWidth, 0.0f));
    ImGui::InvisibleButton("vsplitter", ImVec2(4.0f, contentH));
    if (ImGui::IsItemActive()) {
      g_sidebarWidth += io.MouseDelta.x;
      if (g_sidebarWidth < 180.0f)
        g_sidebarWidth = 180.0f;
      if (g_sidebarWidth > 500.0f)
        g_sidebarWidth = 500.0f;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    // Splitter highlight hover
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(g_sidebarWidth, 0.0f), ImVec2(g_sidebarWidth + 2.0f, contentH),
          IM_COL32(91, 110, 245, 180));
    }

    ImGui::SetNextWindowPos(ImVec2(g_sidebarWidth + 2.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(panelW - 2.0f, contentH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##main_panel_host", ImVec2(panelW, contentH), false,
                      ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (g_activeTab == 0)
      RenderHomePanel(devices, panelW, contentH);
    else if (g_activeTab == 1)
      RenderFilePanel(devices, panelW, contentH);
    else if (g_activeTab == 2) RenderAppsPanel(devices, panelW, contentH);
    else if (g_activeTab == 3) RenderTerminalPanel(devices, panelW, contentH);
    else if (g_activeTab == 5) {
        // Settings Tab
        ImGui::SetCursorPos(ImVec2(24.0f, 20.0f));
        if (g_fontBold) ImGui::PushFont(g_fontBold);
        ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "[ Hardware Settings ]");
        if (g_fontBold) ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10));

        EnumerateGpus();

        ImGui::Text("Preferred Graphics Adapter");
        ImGui::SetNextItemWidth(panelW * 0.8f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
        if (ImGui::BeginCombo("##gpu_select", g_gpuList.empty() ? "No GPUs found" : g_gpuList[g_selectedGpu].name.c_str())) {
            for (int i = 0; i < (int)g_gpuList.size(); i++) {
                bool is_selected = (g_selectedGpu == i);
                if (ImGui::Selectable(g_gpuList[i].name.c_str(), is_selected)) {
                     if (g_selectedGpu != i) {
                         g_requestedGpu = i;
                     }
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        
        ImGui::Dummy(ImVec2(0, 20));
        ImGui::Text("Wall-E Mirroring Quality");
        static int quality = 1; // 0=Performance, 1=High
        if (ImGui::RadioButton("Performance (4 Mbps)", quality == 0)) quality = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("High Quality (8 Mbps)", quality == 1)) quality = 1;
        
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Higher bitrate requires a better USB cable.");
    }
    else if (g_activeTab == 4) {
        if (!devices.empty()) {
            RenderLivePanel(devices[0], panelW, contentH);
        } else {
            RenderComingSoonPanel("Wall-E Live Mirror", panelW, contentH);
        }
    }

    ImGui::EndChild();

    RenderStatusBar(devices.size());

    ImGui::End();

    if (g_fontRegular)
      ImGui::PopFont();

    // Render
    {
        std::lock_guard<std::mutex> lock(g_d3dMutex);
        ImGui::Render();
        const float clearColor[4] = {0.051f, 0.051f, 0.063f, 1.00f};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                                nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                   clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }
  }

  deviceManager.Stop();
  AdbManager::KillServer();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDeviceD3D();
  DestroyWindow(hwnd);
  UnregisterClass(wc.lpszClassName, wc.hInstance);
  return 0;
}

//  D3D HELPERS
bool CreateDeviceD3D(HWND hWnd, IDXGIAdapter1* pAdapter) {
  DXGI_SWAP_CHAIN_DESC sd;
  ZeroMemory(&sd, sizeof(sd));
  sd.BufferCount = 2;
  sd.BufferDesc.Width = 0;
  sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT createDeviceFlags = 0;
  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL featureLevelArray[2] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_0,
  };

  // If pAdapter is null, we use hardware default. If not null, driver type must be UNKNOWN.
  D3D_DRIVER_TYPE driverType = pAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;

  HRESULT res = D3D11CreateDeviceAndSwapChain(
      pAdapter, driverType, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);

  if (res != S_OK)
    return false;

  CreateRenderTarget();
  return true;
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

void CreateRenderTarget() {
  ID3D11Texture2D *pBackBuffer;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
  g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr,
                                       &g_mainRenderTargetView);
  pBackBuffer->Release();
}

void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

//  WNDPROC

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {
  case WM_SIZE:
    if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
      CleanupRenderTarget();
      g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                  DXGI_FORMAT_UNKNOWN, 0);
      CreateRenderTarget();
    }
    return 0;
  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU)
      return 0;
    break;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}