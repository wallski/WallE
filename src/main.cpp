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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <string>
#include <tchar.h>
#include <thread>
#include <vector>
#include <ctime>
#include <algorithm>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam);

// D3D globals 
static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;

// UI state 
static int g_activeTab = 0;
static int g_selectedDevice = -1;
static float g_tabAnim[4] = {};
static float g_sidebarWidth = 260.0f;

// Cached ADB state - only re-checked every 5 seconds, not every frame
static bool  g_adbReady          = false;
static float g_adbCheckTimer     = -99.0f;  // force first check immediately
static const float kAdbCheckInterval = 5.0f;

// Cached device details - fetched once when a device connects
static std::string g_lastDetailedSerial;
static DeviceInfo  g_cachedDetails;

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

// Forward declarations 
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- NATIVE HELPERS ---
static std::string PickLocalFile() {
    char szFile[MAX_PATH] = { 0 };
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return szFile;
    return "";
}

static std::string SaveLocalFile(const std::string& defaultName) {
    char szFile[MAX_PATH] = { 0 };
    strncpy_s(szFile, defaultName.c_str(), _TRUNCATE);
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return szFile;
    return "";
}

// --- UI COMPONENTS ---
static void DrawFolderIcon(ImDrawList* dl, ImVec2 pos, float size) {
    ImU32 col = IM_COL32(243, 156, 18, 255);
    float w = size * 0.8f;
    float h = size * 0.6f;
    // Tab
    dl->AddRectFilled(ImVec2(pos.x, pos.y - 2.0f), ImVec2(pos.x + w * 0.4f, pos.y), col, 2.0f);
    // Body
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col, 2.0f);
}

static void DrawFileIcon(ImDrawList* dl, ImVec2 pos, float size) {
    ImU32 col = IM_COL32(189, 195, 199, 255);
    float w = size * 0.6f;
    float h = size * 0.8f;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col, 1.0f);
    // Fold
    dl->AddTriangleFilled(ImVec2(pos.x + w - 4.0f, pos.y), ImVec2(pos.x + w, pos.y + 4.0f), ImVec2(pos.x + w, pos.y), IM_COL32(40, 40, 50, 255));
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
  static const char *navLabels[] = {"Home", "Files", "Backup", "Settings"};

  float curY = 92.0f;
  for (int i = 0; i < 4; i++) {
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
  ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.33f, 1.0f), "v0.1.0-alpha");

  ImGui::EndChild();
}

// DEVICE HOME PANEL
static void RenderHomePanel(const std::vector<DeviceInfo>& devices, float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::BeginChild("##home_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();

  if (g_fontBold) ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "Device Home");
  if (g_fontBold) ImGui::PopFont();
  ImGui::Dummy(ImVec2(0.0f, 10.0f));

  if (devices.empty()) {
    // Reset cached details when device is removed
    g_lastDetailedSerial = "";
    g_cachedDetails = {};

    float cY = (panelH * 0.42f) - 30.0f;
    const char* text1 = "No devices connected";
    const char* text2 = "Enable USB debugging and plug in your device";
    ImVec2 sz1 = ImGui::CalcTextSize(text1);
    ImVec2 sz2 = ImGui::CalcTextSize(text2);
    float textX1 = (panelW - sz1.x) * 0.5f;
    float textX2 = (panelW - sz2.x) * 0.5f;
    float phoneW = 38.0f;
    float phoneX = (panelW - phoneW) * 0.5f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float px = wp.x + phoneX, py = wp.y + cY;
    dl->AddRect(ImVec2(px, py), ImVec2(px + phoneW, py + 62.0f), IM_COL32(42, 42, 60, 200), 8.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(px + 4.0f, py + 8.0f), ImVec2(px + 34.0f, py + 46.0f), IM_COL32(42, 42, 60, 80), 4.0f);
    dl->AddCircleFilled(ImVec2(px + 19.0f, py + 54.0f), 3.5f, IM_COL32(60, 60, 80, 200));
    ImGui::SetCursorPos(ImVec2(textX1, cY + 76.0f));
    ImGui::TextColored(ImVec4(0.32f, 0.32f, 0.42f, 1.0f), "%s", text1);
    ImGui::SetCursorPos(ImVec2(textX2, cY + 98.0f));
    ImGui::TextColored(ImVec4(0.22f, 0.22f, 0.30f, 1.0f), "%s", text2);
  } else {
    const DeviceInfo& dev = devices[0];

    if (dev.status == "unauthorized") {
      float cX = (panelW * 0.5f) - 140.0f;
      float cY = (panelH * 0.42f);
      ImGui::SetCursorPos(ImVec2(cX + 66.0f, cY));
      if (g_fontBold) ImGui::PushFont(g_fontBold);
      ImGui::TextColored(ImVec4(0.91f, 0.30f, 0.24f, 1.0f), "Device Unauthorized!");
      if (g_fontBold) ImGui::PopFont();
      ImGui::SetCursorPos(ImVec2(cX, cY + 30.0f));
      ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Please check your phone and allow USB debugging.");
    } else {
      // Fetch details once when a new device connects (not every frame)
      if (dev.serial != g_lastDetailedSerial) {
        g_lastDetailedSerial = dev.serial;
        g_cachedDetails = dev;
        AdbManager::GetDeviceDetails(g_cachedDetails);
      }
      const DeviceInfo& d = g_cachedDetails;

      ImDrawList* dl = ImGui::GetWindowDrawList();
      ImVec2 origin = ImGui::GetWindowPos();

      // == Phone Illustration == (Resized to fit above the bar)
      const float phoneX  = 28.0f;
      const float phoneY  = 40.0f;
      const float phoneW  = 46.0f;
      const float phoneH  = 82.0f;
      float px = origin.x + phoneX;
      float py = origin.y + phoneY;
      // outer frame
      dl->AddRectFilled(ImVec2(px, py), ImVec2(px + phoneW, py + phoneH), IM_COL32(18, 18, 28, 255), 6.0f);
      dl->AddRect(ImVec2(px, py), ImVec2(px + phoneW, py + phoneH), IM_COL32(91, 110, 245, 160), 6.0f, 0, 1.5f);
      // screen area
      dl->AddRectFilled(ImVec2(px + 4.0f, py + 8.0f), ImVec2(px + phoneW - 4.0f, py + phoneH - 10.0f), IM_COL32(91, 110, 245, 18), 3.0f);
      // front camera dot
      dl->AddCircleFilled(ImVec2(px + phoneW * 0.5f, py + 4.0f), 1.5f, IM_COL32(91, 110, 245, 90));
      // home indicator bar
      dl->AddRectFilled(ImVec2(px + phoneW * 0.3f, py + phoneH - 6.0f), ImVec2(px + phoneW * 0.7f, py + phoneH - 4.0f), IM_COL32(91, 110, 245, 120), 1.0f);

      // == Model Name + subtitle ==
      float infoX = phoneX + phoneW + 24.0f;
      ImGui::SetCursorPos(ImVec2(infoX, 40.0f));
      if (g_fontBold) ImGui::PushFont(g_fontBold);
      ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", d.model.c_str());
      if (g_fontBold) ImGui::PopFont();

      ImGui::SetCursorPos(ImVec2(infoX, 65.0f));
      ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.52f, 1.0f), "%s", d.manufacturer.c_str());

      // Online status badge
      ImGui::SetCursorPos(ImVec2(infoX, 88.0f));
      ImVec2 badgePos = ImGui::GetCursorScreenPos();
      dl->AddRectFilled(badgePos, ImVec2(badgePos.x + 80.0f, badgePos.y + 20.0f), IM_COL32(46, 204, 113, 28), 10.0f);
      dl->AddRect(badgePos, ImVec2(badgePos.x + 80.0f, badgePos.y + 20.0f), IM_COL32(46, 204, 113, 130), 10.0f);
      dl->AddCircleFilled(ImVec2(badgePos.x + 12.0f, badgePos.y + 10.0f), 3.5f, IM_COL32(46, 204, 113, 255));
      ImGui::SetCursorPos(ImVec2(infoX + 22.0f, 91.0f));
      ImGui::TextColored(ImVec4(0.18f, 0.80f, 0.44f, 1.0f), "Online");

      // Separator
      float sepY = origin.y + 130.0f;
      dl->AddLine(ImVec2(origin.x + 20.0f, sepY), ImVec2(origin.x + panelW - 20.0f, sepY), IM_COL32(42, 42, 56, 255));

      // == Info Grid (2 columns of label/value pairs) ==
      const float gridTop  = 144.0f;
      const float col1X    = 28.0f;
      const float col2X    = panelW * 0.5f;
      const float rowH     = 26.0f;

      // Helper macros for label+value rows
      auto InfoRow = [&](float x, float y, const char* label, const std::string& value, ImVec4 valueColor = ImVec4(0.9f, 0.9f, 0.92f, 1.0f)) {
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.55f, 1.0f), "%s", label);
        ImGui::SetCursorPos(ImVec2(x + 130.0f, y));
        ImGui::TextColored(valueColor, "%s", value.empty() ? "--" : value.c_str());
      };

      if (!d.detailsLoaded) {
        ImGui::SetCursorPos(ImVec2(col1X, gridTop));
        ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.52f, 1.0f), "Loading device info...");
      } else {
        // Column 1 - Software
        InfoRow(col1X, gridTop + rowH * 0, "Android",     "Android " + d.androidVersion);
        InfoRow(col1X, gridTop + rowH * 1, "SDK",          d.sdkVersion);
        InfoRow(col1X, gridTop + rowH * 2, "Build",        d.buildNumber);
        InfoRow(col1X, gridTop + rowH * 3, "Security",     d.securityPatch);
        InfoRow(col1X, gridTop + rowH * 4, "Bootloader",   d.bootloaderStatus,
          d.bootloaderStatus == "orange" ? ImVec4(0.95f, 0.61f, 0.07f, 1.0f) : ImVec4(0.18f, 0.80f, 0.44f, 1.0f));
        InfoRow(col1X, gridTop + rowH * 5, "CPU",          d.cpuAbi);

        // Column 2 - Hardware
        std::string battStr = d.batteryLevel.empty() ? "--" : d.batteryLevel + "% " + d.batteryStatus;
        ImVec4 battColor = ImVec4(0.9f, 0.9f, 0.92f, 1.0f);
        if (!d.batteryLevel.empty()) {
          int lvl = std::stoi(d.batteryLevel);
          if      (lvl >= 60) battColor = ImVec4(0.18f, 0.80f, 0.44f, 1.0f);
          else if (lvl >= 25) battColor = ImVec4(0.95f, 0.61f, 0.07f, 1.0f);
          else                battColor = ImVec4(0.91f, 0.30f, 0.24f, 1.0f);
        }
        InfoRow(col2X, gridTop + rowH * 0, "Battery",  battStr, battColor);
        InfoRow(col2X, gridTop + rowH * 1, "Screen",   d.screenResolution);
        InfoRow(col2X, gridTop + rowH * 2, "Storage",  d.storageUsed + " / " + d.storageTotal);
        InfoRow(col2X, gridTop + rowH * 3, "RAM",      (d.ramAvail.empty() ? "--" : d.ramAvail + " free / " + d.ramTotal));
        InfoRow(col2X, gridTop + rowH * 4, "Serial",   d.serial);
        InfoRow(col2X, gridTop + rowH * 5, "Brand",    d.brand);
      }
    }
  }

  ImGui::EndChild();
}


// FILE MANAGER PANEL
static void RenderFilePanel(const std::vector<DeviceInfo> &devices, float panelW, float panelH) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
    ImGui::BeginChild("##file_panel", ImVec2(panelW, panelH), false);
    ImGui::PopStyleVar();

    if (devices.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Connect a device to browse files.");
        ImGui::EndChild();
        return;
    }

    const DeviceInfo& dev = devices[0];

    // --- Header / Toolbar ---
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "[ Files ]");
    if (g_fontBold) ImGui::PopFont();
    
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
            if (lastSlash == 0) g_currentPath = "/";
            else g_currentPath = g_currentPath.substr(0, lastSlash);
            g_fileListLoading = false;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(panelW - 320.0f);
    char pathBuf[512];
    strncpy_s(pathBuf, g_currentPath.c_str(), _TRUNCATE);
    if (ImGui::InputText("##curpath", pathBuf, sizeof(pathBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
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
    if (ImGui::Button("Refresh")) g_fileListLoading = false;

    // --- Fetch Logic ---
    if (!g_fileListLoading) {
        g_fileList = AdbManager::ListFiles(dev.serial, g_currentPath, g_rootMode);
        g_fileListLoading = true;
    }

    ImGui::Dummy(ImVec2(0, 10));

    // --- Table ---
    if (ImGui::BeginTable("##file_table", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable, ImVec2(0, panelH - 180.0f))) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableHeadersRow();

        int rowIdx = 0;
        for (const auto& file : g_fileList) {
            // Search Filter
            if (g_fileSearchQuery[0] != '\0') {
                std::string nameLower = file.name;
                std::string searchLower = g_fileSearchQuery;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
                if (nameLower.find(searchLower) == std::string::npos) continue;
            }

            ImGui::PushID(rowIdx++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            
            // Icon + Label
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 cp = ImGui::GetCursorScreenPos();
            if (file.isDirectory) DrawFolderIcon(dl, ImVec2(cp.x, cp.y + 4.0f), 16.0f);
            else DrawFileIcon(dl, ImVec2(cp.x, cp.y + 4.0f), 16.0f);
            
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
            bool selected = (g_selectedFile == file.name);
            const char* label = file.name.empty() ? "[Unnamed]" : file.name.c_str();
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selectedFile = file.name;
                if (file.isDirectory && ImGui::IsMouseDoubleClicked(0)) {
                    g_pathHistory.push(g_currentPath);
                    if (g_currentPath.back() != '/') g_currentPath += "/";
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
                    if (!local.empty()) AdbManager::PullFile(dev.serial, g_currentPath + "/" + file.name, local);
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
            std::string filename = (slash != std::string::npos) ? local.substr(slash + 1) : local;
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
    ImGui::InputTextWithHint("##search", "Search...", g_fileSearchQuery, sizeof(g_fileSearchQuery));

    // --- Popups ---
    if (g_showRootWarning) ImGui::OpenPopup("Root Safety Warning");
    if (ImGui::BeginPopupModal("Root Safety Warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("WARNING: Root mode allows absolute power over the device.");
        ImGui::Text("(Note: Device must be rooted for this to work)");
        ImGui::Text("Incorrect actions can brick the OS or delete critical data.");
        ImGui::Separator();
        if (ImGui::Button("Accept Risk", ImVec2(120, 0))) { g_rootMode = true; g_showRootWarning = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_showRootWarning = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (g_showConflictPopup) ImGui::OpenPopup("File Conflict");
    if (ImGui::BeginPopupModal("File Conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("The file already exists on the device.");
        ImGui::Separator();
        if (ImGui::Button("Overwrite")) {
            AdbManager::PushFile(dev.serial, g_conflictLocalPath, g_conflictRemotePath);
            g_showConflictPopup = false; g_fileListLoading = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename")) {
            // Simple rename: append timestamp
            std::string r = g_conflictRemotePath + "_" + std::to_string(time(NULL));
            AdbManager::PushFile(dev.serial, g_conflictLocalPath, r);
            g_showConflictPopup = false; g_fileListLoading = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip")) { g_showConflictPopup = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (g_showRenamePopup) ImGui::OpenPopup("Rename Item");
    if (ImGui::BeginPopupModal("Rename Item", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter new name for: %s", g_actionTarget.c_str());
        ImGui::InputText("##newname", g_nameBuffer, sizeof(g_nameBuffer));
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            AdbManager::RenameEntry(dev.serial, g_currentPath + "/" + g_actionTarget, g_currentPath + "/" + g_nameBuffer, g_rootMode);
            g_showRenamePopup = false; g_fileListLoading = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_showRenamePopup = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (g_showDeletePopup) ImGui::OpenPopup("Confirm Delete");
    if (ImGui::BeginPopupModal("Confirm Delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure you want to delete '%s'?", g_actionTarget.c_str());
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "This action cannot be undone.");
        ImGui::Separator();
        if (ImGui::Button("Delete", ImVec2(120, 0))) {
            AdbManager::DeleteEntry(dev.serial, g_currentPath + "/" + g_actionTarget, g_rootMode);
            g_showDeletePopup = false; g_fileListLoading = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_showDeletePopup = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (g_showNewFolderPopup) ImGui::OpenPopup("New Folder");
    if (ImGui::BeginPopupModal("New Folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter folder name:");
        ImGui::InputText("##foldername", g_nameBuffer, sizeof(g_nameBuffer));
        ImGui::Separator();
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            AdbManager::CreateDirectory(dev.serial, g_currentPath + "/" + g_nameBuffer, g_rootMode);
            g_showNewFolderPopup = false; g_fileListLoading = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) { g_showNewFolderPopup = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
}

static void RenderComingSoonPanel(const char *title, float panelW, float panelH) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::BeginChild("##cs_panel", ImVec2(panelW, panelH), false);
  ImGui::PopStyleVar();
  if (g_fontBold) ImGui::PushFont(g_fontBold);
  ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "%s", title);
  if (g_fontBold) ImGui::PopFont();
  float cX = (panelW * 0.5f) - 80.0f;
  float cY = (panelH * 0.44f);
  ImGui::SetCursorPos(ImVec2(cX, cY));
  ImGui::TextColored(ImVec4(0.28f, 0.28f, 0.38f, 1.0f), "Coming soon");
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

  if (!CreateDeviceD3D(hwnd)) {
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
  if (GetFileAttributesA(pathRegular) != INVALID_FILE_ATTRIBUTES)
    g_fontRegular = io.Fonts->AddFontFromFileTTF(pathRegular, 18.0f);
  if (GetFileAttributesA(pathBold) != INVALID_FILE_ATTRIBUTES)
    g_fontBold = io.Fonts->AddFontFromFileTTF(pathBold, 18.0f);
  io.Fonts->Build();

  ApplyWallETheme();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  // Device manager
  static DeviceManager deviceManager;
  deviceManager.Start();

  // Main loop
  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        done = true;
    }
    if (done)
      break;

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
      if (g_sidebarWidth < 180.0f) g_sidebarWidth = 180.0f;
      if (g_sidebarWidth > 500.0f) g_sidebarWidth = 500.0f;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    
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

    if (g_activeTab == 0) RenderHomePanel(devices, panelW, contentH);
    else if (g_activeTab == 1) RenderFilePanel(devices, panelW, contentH);
    else if (g_activeTab == 2) RenderComingSoonPanel("Backup & Restore", panelW, contentH);
    else if (g_activeTab == 3) RenderComingSoonPanel("Wall-E Settings", panelW, contentH);

    ImGui::EndChild();
    
    RenderStatusBar(devices.size());

    ImGui::End();

    if (g_fontRegular)
      ImGui::PopFont();

    // Render
    ImGui::Render();
    const float clearColor[4] = {0.051f, 0.051f, 0.063f, 1.00f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                            nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                               clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
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
bool CreateDeviceD3D(HWND hWnd) {
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

  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
      featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
      &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
  if (res == DXGI_ERROR_UNSUPPORTED)
    res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
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