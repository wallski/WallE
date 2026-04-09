// ============================================================
//  Wall-E  -  Android Manager
//  Clean, minimal ImGui UI - no emoji, performance-optimized
// ============================================================
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "DeviceManager.hpp"
#include "AdbManager.hpp"

#include <d3d11.h>
#include <tchar.h>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// --- D3D globals -------------------------------------------------------------
static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain*          g_pSwapChain           = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// --- UI state ----------------------------------------------------------------
static int   g_activeTab      = 0;
static int   g_selectedDevice = -1;
static float g_tabAnim[4]     = {};

// Cached ADB state - only re-checked every 5 seconds, not every frame
static bool  g_adbReady          = false;
static float g_adbCheckTimer     = -99.0f;  // force first check immediately
static const float kAdbCheckInterval = 5.0f;

// --- Fonts -------------------------------------------------------------------
static ImFont* g_fontRegular = nullptr;
static ImFont* g_fontBold    = nullptr;

// --- Forward declarations ----------------------------------------------------
bool   CreateDeviceD3D(HWND hWnd);
void   CleanupDeviceD3D();
void   CreateRenderTarget();
void   CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// -----------------------------------------------------------------------------
//  THEME
// -----------------------------------------------------------------------------
static void ApplyWallETheme()
{
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 7.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 6.0f;
    s.TabRounding       = 8.0f;
    s.ItemSpacing       = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    s.FramePadding      = ImVec2(12.0f, 7.0f);
    s.WindowPadding     = ImVec2(0.0f, 0.0f);
    s.ScrollbarSize     = 8.0f;
    s.GrabMinSize       = 10.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.IndentSpacing     = 16.0f;

    ImVec4* c = s.Colors;

    c[ImGuiCol_WindowBg]          = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.09f,  0.09f,  0.12f,  1.0f);
    c[ImGuiCol_Border]            = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
    c[ImGuiCol_BorderShadow]      = ImVec4(0.0f,   0.0f,   0.0f,   0.0f);
    c[ImGuiCol_Text]              = ImVec4(0.91f,  0.91f,  0.94f,  1.0f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.42f,  0.42f,  0.50f,  1.0f);
    c[ImGuiCol_FrameBg]           = ImVec4(0.10f,  0.10f,  0.14f,  1.0f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.14f,  0.14f,  0.20f,  1.0f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.18f,  0.18f,  0.26f,  1.0f);
    c[ImGuiCol_TitleBg]           = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]  = ImVec4(0.051f, 0.051f, 0.063f, 1.0f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.07f, 0.07f, 0.09f, 1.0f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.25f, 0.25f, 0.35f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.8f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
    c[ImGuiCol_CheckMark]         = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
    c[ImGuiCol_SliderGrab]        = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.482f, 0.557f, 1.0f,   1.0f);
    c[ImGuiCol_Button]            = ImVec4(0.357f, 0.431f, 0.961f, 0.85f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.482f, 0.557f, 1.0f,   1.0f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.270f, 0.340f, 0.840f, 1.0f);
    c[ImGuiCol_Header]            = ImVec4(0.357f, 0.431f, 0.961f, 0.25f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.357f, 0.431f, 0.961f, 0.40f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.357f, 0.431f, 0.961f, 0.60f);
    c[ImGuiCol_Separator]         = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
    c[ImGuiCol_SeparatorHovered]  = ImVec4(0.357f, 0.431f, 0.961f, 0.7f);
    c[ImGuiCol_SeparatorActive]   = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.357f, 0.431f, 0.961f, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.357f, 0.431f, 0.961f, 0.60f);
    c[ImGuiCol_ResizeGripActive]  = ImVec4(0.357f, 0.431f, 0.961f, 0.90f);
    c[ImGuiCol_Tab]                = ImVec4(0.10f,  0.10f,  0.14f,  1.0f);
    c[ImGuiCol_TabHovered]         = ImVec4(0.357f, 0.431f, 0.961f, 0.50f);
    c[ImGuiCol_TabActive]          = ImVec4(0.357f, 0.431f, 0.961f, 0.85f);
    c[ImGuiCol_TabUnfocused]       = ImVec4(0.10f,  0.10f,  0.14f,  1.0f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.20f,  0.22f,  0.40f,  1.0f);
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.10f,  0.10f,  0.14f,  1.0f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.165f, 0.165f, 0.220f, 1.0f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.12f,  0.12f,  0.16f,  1.0f);
    c[ImGuiCol_TableRowBg]        = ImVec4(0.0f,   0.0f,   0.0f,   0.0f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(0.10f,  0.10f,  0.14f,  0.5f);
    c[ImGuiCol_TextSelectedBg]           = ImVec4(0.357f, 0.431f, 0.961f, 0.35f);
    c[ImGuiCol_DragDropTarget]           = ImVec4(0.357f, 0.431f, 0.961f, 0.90f);
    c[ImGuiCol_NavHighlight]             = ImVec4(0.357f, 0.431f, 0.961f, 1.0f);
    c[ImGuiCol_NavWindowingHighlight]    = ImVec4(1.0f,   1.0f,   1.0f,   0.70f);
    c[ImGuiCol_NavWindowingDimBg]        = ImVec4(0.0f,   0.0f,   0.0f,   0.50f);
    c[ImGuiCol_ModalWindowDimBg]         = ImVec4(0.0f,   0.0f,   0.0f,   0.60f);
}

// -----------------------------------------------------------------------------
//  HELPERS - draw a small colored dot without any font glyph
// -----------------------------------------------------------------------------
static void DrawStatusDot(ImDrawList* dl, ImVec2 pos, ImU32 color, float radius = 4.5f)
{
    dl->AddCircleFilled(pos, radius, color, 12);
}

// -----------------------------------------------------------------------------
//  SIDEBAR
// -----------------------------------------------------------------------------
static void RenderSidebar(int& activeTab)
{
    const float sidebarW = 210.0f;
    const float winH     = ImGui::GetIO().DisplaySize.y;
    const float statusH  = 28.0f;
    const float contentH = winH - statusH;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(sidebarW, contentH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.039f, 0.039f, 0.055f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##sidebar", ImVec2(sidebarW, contentH), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      spos = ImGui::GetWindowPos();

    // Right-edge border
    dl->AddLine(
        ImVec2(spos.x + sidebarW - 1.0f, spos.y),
        ImVec2(spos.x + sidebarW - 1.0f, spos.y + contentH),
        IM_COL32(42, 42, 56, 255));

    // --- Logo ----------------------------------------------------
    ImGui::SetCursorPos(ImVec2(20.0f, 24.0f));
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextColored(ImVec4(0.357f, 0.431f, 0.961f, 1.0f), "[ Wall-E ]");
    if (g_fontBold) ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(20.0f, 46.0f));
    ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.50f, 1.0f), "Android Manager");

    dl->AddLine(
        ImVec2(spos.x + 16.0f, spos.y + 78.0f),
        ImVec2(spos.x + sidebarW - 16.0f, spos.y + 78.0f),
        IM_COL32(42, 42, 56, 255));

    // --- Nav items ------------------------------------------------
    static const char* navLabels[] = { "Devices", "Files", "Backup", "Settings" };

    float curY = 92.0f;
    for (int i = 0; i < 4; i++)
    {
        bool selected = (activeTab == i);

        // Smooth lerp
        float target = selected ? 1.0f : 0.0f;
        g_tabAnim[i] += (target - g_tabAnim[i]) * 0.18f;

        if (g_tabAnim[i] > 0.01f)
        {
            // Selection highlight
            ImVec2 bMin(spos.x + 10.0f, spos.y + curY);
            ImVec2 bMax(spos.x + sidebarW - 10.0f, spos.y + curY + 38.0f);
            ImU32 bgCol = IM_COL32(
                (int)(91  * g_tabAnim[i]),
                (int)(110 * g_tabAnim[i]),
                (int)(245 * g_tabAnim[i]),
                (int)(45  * g_tabAnim[i]));
            dl->AddRectFilled(bMin, bMax, bgCol, 8.0f);

            // Left accent bar
            dl->AddRectFilled(
                ImVec2(spos.x + 10.0f, spos.y + curY + 6.0f),
                ImVec2(spos.x + 13.0f, spos.y + curY + 32.0f),
                IM_COL32(91, 110, 245, (int)(255 * g_tabAnim[i])),
                4.0f);
        }

        // Clickable button
        ImGui::SetCursorPos(ImVec2(10.0f, curY));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.357f, 0.431f, 0.961f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.357f, 0.431f, 0.961f, 0.18f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        char btnId[32];
        snprintf(btnId, sizeof(btnId), "##nav%d", i);
        if (ImGui::Button(btnId, ImVec2(sidebarW - 20.0f, 38.0f)))
            activeTab = i;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);

        // Label (no icon - pure text)
        ImVec4 textCol = selected
            ? ImVec4(0.91f, 0.91f, 0.94f, 1.0f)
            : ImVec4(0.50f, 0.50f, 0.60f, 1.0f);

        ImGui::SetCursorPos(ImVec2(30.0f, curY + 10.0f));
        if (selected && g_fontBold) ImGui::PushFont(g_fontBold);
        ImGui::TextColored(textCol, "%s", navLabels[i]);
        if (selected && g_fontBold) ImGui::PopFont();

        curY += 46.0f;
    }

    // Version footer
    ImGui::SetCursorPos(ImVec2(20.0f, contentH - 36.0f));
    ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.33f, 1.0f), "v0.1.0-alpha");

    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
//  DEVICE CARD
// -----------------------------------------------------------------------------
static bool RenderDeviceCard(const DeviceInfo& dev, int idx, bool isSelected)
{
    const ImVec2 cardSize(230.0f, 138.0f);
    bool clicked = false;

    ImDrawList* dl      = ImGui::GetWindowDrawList();
    ImVec2      cardPos = ImGui::GetCursorScreenPos();

    bool hovered = ImGui::IsMouseHoveringRect(
        cardPos, ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y));

    // Card background
    ImU32 bgCol = isSelected
        ? IM_COL32(26, 30, 72, 255)
        : hovered ? IM_COL32(22, 22, 30, 255) : IM_COL32(17, 17, 23, 255);
    ImU32 borderCol = isSelected
        ? IM_COL32(91, 110, 245, 210)
        : hovered ? IM_COL32(91, 110, 245, 80) : IM_COL32(42, 42, 56, 255);

    dl->AddRectFilled(cardPos,
        ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y), bgCol, 12.0f);
    dl->AddRect(cardPos,
        ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y), borderCol, 12.0f, 0,
        isSelected ? 1.5f : 1.0f);

    // Small "phone" outline - drawn as a rounded rect via draw list (no glyph needed)
    float px = cardPos.x + 14.0f;
    float py = cardPos.y + 14.0f;
    dl->AddRect(ImVec2(px, py), ImVec2(px + 14.0f, py + 22.0f),
        IM_COL32(91, 110, 245, 160), 3.0f, 0, 1.4f);
    // Small screen area inside phone shape
    dl->AddRectFilled(ImVec2(px + 2.0f, py + 3.0f), ImVec2(px + 12.0f, py + 16.0f),
        IM_COL32(91, 110, 245, 30), 1.0f);

    // Invisible button for interaction
    ImGui::PushID(idx);
    ImGui::InvisibleButton("##card", cardSize);
    if (ImGui::IsItemClicked()) clicked = true;
    ImGui::PopID();

    float cx = cardPos.x;
    float cy = cardPos.y;

    // Model name (bold) - truncated
    ImGui::SetCursorScreenPos(ImVec2(cx + 38.0f, cy + 14.0f));
    if (g_fontBold) ImGui::PushFont(g_fontBold);
    char modelBuf[28];
    snprintf(modelBuf, sizeof(modelBuf), "%.24s%s",
        dev.model.c_str(), dev.model.size() > 24 ? "..." : "");
    ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "%s", modelBuf);
    if (g_fontBold) ImGui::PopFont();

    // Serial (muted, truncated)
    ImGui::SetCursorScreenPos(ImVec2(cx + 38.0f, cy + 34.0f));
    char serialBuf[24];
    snprintf(serialBuf, sizeof(serialBuf), "%.20s%s",
        dev.serial.c_str(), dev.serial.size() > 20 ? "..." : "");
    ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.50f, 1.0f), "%s", serialBuf);

    // Status pill - drawn entirely via draw list + text (no unicode)
    bool   online = (dev.status == "device");
    bool   unauth = (dev.status == "unauthorized");
    ImU32  pillBg     = online ? IM_COL32(46, 204, 113,  30) : unauth ? IM_COL32(231, 76, 60, 30)  : IM_COL32(243, 156, 18,  30);
    ImU32  pillBorder = online ? IM_COL32(46, 204, 113, 160) : unauth ? IM_COL32(231, 76, 60, 160) : IM_COL32(243, 156, 18, 160);
    ImVec4 pillText   = online ? ImVec4(0.18f, 0.80f, 0.44f, 1.0f)
                                : unauth ? ImVec4(0.91f, 0.30f, 0.24f, 1.0f)
                                         : ImVec4(0.95f, 0.61f, 0.07f, 1.0f);
    ImU32  dotColor   = online ? IM_COL32(46, 204, 113, 220) : unauth ? IM_COL32(231, 76, 60, 220) : IM_COL32(243, 156, 18, 220);
    const char* statusLabel = online ? "Online" : unauth ? "Unauthorized" : "Offline";

    ImVec2 pillMin(cx + 14.0f, cy + 58.0f);
    ImVec2 pillMax(cx + 14.0f + 100.0f, cy + 58.0f + 22.0f);
    dl->AddRectFilled(pillMin, pillMax, pillBg, 11.0f);
    dl->AddRect(pillMin, pillMax, pillBorder, 11.0f);

    // Status dot drawn via draw list (no glyph)
    DrawStatusDot(dl, ImVec2(cx + 25.0f, cy + 69.0f), dotColor, 3.5f);

    ImGui::SetCursorScreenPos(ImVec2(cx + 32.0f, cy + 60.0f));
    ImGui::TextColored(pillText, "%s", statusLabel);

    // Select button
    ImGui::SetCursorScreenPos(ImVec2(cx + 14.0f, cy + 106.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushID(idx + 5000);
    if (isSelected) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.270f, 0.340f, 0.840f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.357f, 0.431f, 0.961f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.200f, 0.260f, 0.720f, 1.0f));
        if (ImGui::Button("[X] Selected", ImVec2(cardSize.x - 28.0f, 24.0f))) clicked = true;
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button("Select Device", ImVec2(cardSize.x - 28.0f, 24.0f))) clicked = true;
    }
    ImGui::PopID();
    ImGui::PopStyleVar();

    return clicked;
}

// -----------------------------------------------------------------------------
//  DEVICES PANEL
// -----------------------------------------------------------------------------
static void RenderDevicesPanel(const std::vector<DeviceInfo>& devices, float panelW, float panelH)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
    ImGui::BeginChild("##devices_panel", ImVec2(panelW, panelH), false);
    ImGui::PopStyleVar();

    if (g_fontBold) ImGui::PushFont(g_fontBold);
    ImGui::TextColored(ImVec4(0.91f, 0.91f, 0.94f, 1.0f), "Connected Devices");
    if (g_fontBold) ImGui::PopFont();
    ImGui::TextColored(ImVec4(0.42f, 0.42f, 0.50f, 1.0f), "Select a device to get started");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    if (devices.empty())
    {
        float cX = (panelW * 0.5f) - 110.0f;
        float cY = (panelH * 0.42f);

        // Draw a simple placeholder phone outline (larger)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2      wp = ImGui::GetWindowPos();
        float px = wp.x + cX + 86.0f;
        float py = wp.y + cY;
        dl->AddRect(ImVec2(px, py), ImVec2(px + 38.0f, py + 62.0f),
            IM_COL32(42, 42, 60, 200), 8.0f, 0, 2.0f);
        dl->AddRectFilled(ImVec2(px + 4.0f, py + 8.0f), ImVec2(px + 34.0f, py + 46.0f),
            IM_COL32(42, 42, 60, 80), 4.0f);
        // Home button dot
        dl->AddCircleFilled(ImVec2(px + 19.0f, py + 54.0f), 3.5f, IM_COL32(60, 60, 80, 200));

        ImGui::SetCursorPos(ImVec2(cX, cY + 72.0f));
        ImGui::TextColored(ImVec4(0.32f, 0.32f, 0.42f, 1.0f), "  No devices connected");
        ImGui::SetCursorPos(ImVec2(cX - 30.0f, cY + 92.0f));
        ImGui::TextColored(ImVec4(0.22f, 0.22f, 0.30f, 1.0f), "Enable USB debugging and plug in your device");
    }
    else
    {
        const float cardW   = 230.0f;
        const float cardGap = 16.0f;
        int columns = (int)((panelW - 28.0f) / (cardW + cardGap));
        if (columns < 1) columns = 1;

        for (size_t i = 0; i < devices.size(); i++)
        {
            if (i % (size_t)columns != 0)
                ImGui::SameLine(0.0f, cardGap);

            bool isSelected = ((int)i == g_selectedDevice);
            if (RenderDeviceCard(devices[i], (int)i, isSelected))
                g_selectedDevice = (int)i;
        }
    }

    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
//  COMING SOON PANEL
// -----------------------------------------------------------------------------
static void RenderComingSoonPanel(const char* title, float panelW, float panelH)
{
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

// -----------------------------------------------------------------------------
//  STATUS BAR
// -----------------------------------------------------------------------------
static void RenderStatusBar(size_t deviceCount)
{
    const float statusH = 28.0f;
    const float winW    = ImGui::GetIO().DisplaySize.x;
    const float winH    = ImGui::GetIO().DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0.0f, winH - statusH));
    ImGui::SetNextWindowSize(ImVec2(winW, statusH));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.039f, 0.039f, 0.055f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 0.0f));
    ImGui::BeginChild("##statusbar", ImVec2(winW, statusH), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      spos = ImGui::GetWindowPos();

    // Top border
    dl->AddLine(
        ImVec2(spos.x, spos.y),
        ImVec2(spos.x + winW, spos.y),
        IM_COL32(42, 42, 56, 255));

    float t = (float)ImGui::GetTime();

    // Status dot + text - dot drawn via draw list (no font glyph)
    float dotX = spos.x + 26.0f;
    float dotY = spos.y + 14.0f;

    ImGui::SetCursorPos(ImVec2(36.0f, 6.0f));

    if (!g_adbReady)
    {
        float p = 0.50f + 0.50f * sinf(t * 3.5f);
        DrawStatusDot(dl, ImVec2(dotX, dotY), IM_COL32(231, 76, 60, (int)(220 * p)));
        ImGui::TextColored(ImVec4(0.91f, 0.30f, 0.24f, 1.0f),
            "ADB not found - add platform-tools to PATH");
    }
    else if (deviceCount == 0)
    {
        float p = 0.50f + 0.50f * sinf(t * 2.0f);
        DrawStatusDot(dl, ImVec2(dotX, dotY), IM_COL32(243, 156, 18, (int)(220 * p)));
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f),
            "No devices connected - enable USB debugging");
    }
    else
    {
        float p = 0.70f + 0.30f * sinf(t * 1.5f);
        DrawStatusDot(dl, ImVec2(dotX, dotY), IM_COL32(46, 204, 113, (int)(220 * p)));
        ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.78f, 1.0f),
            "%zu device%s connected", deviceCount, deviceCount == 1 ? "" : "s");
    }

    // Right-aligned version
    const char* ver   = "Wall-E  v0.1";
    ImVec2      verSz = ImGui::CalcTextSize(ver);
    ImGui::SetCursorPos(ImVec2(winW - verSz.x - 20.0f, 6.0f));
    ImGui::TextColored(ImVec4(0.25f, 0.25f, 0.33f, 1.0f), "%s", ver);

    ImGui::EndChild();
}

// -----------------------------------------------------------------------------
//  WINMAIN
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0, 0,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        _T("WallE"), nullptr
    };
    RegisterClassEx(&wc);

    HWND hwnd = CreateWindow(
        wc.lpszClassName, _T("Wall-E - Android Manager"),
        WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // --- ImGui init ----------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Load Segoe UI (always present on Windows 7+)
    const char* pathRegular = "C:/Windows/Fonts/segoeui.ttf";
    const char* pathBold    = "C:/Windows/Fonts/segoeuib.ttf";
    if (GetFileAttributesA(pathRegular) != INVALID_FILE_ATTRIBUTES)
        g_fontRegular = io.Fonts->AddFontFromFileTTF(pathRegular, 15.0f);
    if (GetFileAttributesA(pathBold) != INVALID_FILE_ATTRIBUTES)
        g_fontBold = io.Fonts->AddFontFromFileTTF(pathBold, 15.0f);
    io.Fonts->Build();

    ApplyWallETheme();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // --- Device manager -----------------------------------------
    static DeviceManager deviceManager;
    deviceManager.Start();

    // --- Main loop ----------------------------------------------
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Throttled ADB check - only runs once every 5 seconds, not every frame
        float now = (float)ImGui::GetTime();
        if (now - g_adbCheckTimer >= kAdbCheckInterval) {
            g_adbReady     = AdbManager::Initialize();
            g_adbCheckTimer = now;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_fontRegular) ImGui::PushFont(g_fontRegular);

        std::vector<DeviceInfo> devices = deviceManager.GetDevices();

        const float winW     = io.DisplaySize.x;
        const float winH     = io.DisplaySize.y;
        const float sidebarW = 210.0f;
        const float statusH  = 28.0f;
        const float contentH = winH - statusH;
        const float panelW   = winW - sidebarW;

        // Full-screen host window
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(winW, winH));
        ImGui::Begin("##host", nullptr,
            ImGuiWindowFlags_NoTitleBar            |
            ImGuiWindowFlags_NoResize              |
            ImGuiWindowFlags_NoMove                |
            ImGuiWindowFlags_NoCollapse            |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoScrollbar           |
            ImGuiWindowFlags_NoScrollWithMouse);

        RenderSidebar(g_activeTab);

        // Main content
        ImGui::SetNextWindowPos(ImVec2(sidebarW, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(panelW, contentH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##main_panel_host", ImVec2(panelW, contentH), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        switch (g_activeTab) {
        case 0: RenderDevicesPanel(devices, panelW, contentH);                 break;
        case 1: RenderComingSoonPanel("File Manager",     panelW, contentH);    break;
        case 2: RenderComingSoonPanel("Backup & Restore", panelW, contentH);    break;
        case 3: RenderComingSoonPanel("Settings",         panelW, contentH);    break;
        }

        ImGui::EndChild();
        ImGui::End();

        RenderStatusBar(devices.size());

        if (g_fontRegular) ImGui::PopFont();

        // Render
        ImGui::Render();
        const float clearColor[4] = { 0.051f, 0.051f, 0.063f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    deviceManager.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

// -----------------------------------------------------------------------------
//  D3D HELPERS
// -----------------------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hWnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    UINT              createDeviceFlags   = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// -----------------------------------------------------------------------------
//  WNDPROC
// -----------------------------------------------------------------------------
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}