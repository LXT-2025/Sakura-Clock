#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

// ===== 适配旧TDM-GCC：手动声明DPI感知API =====
typedef void* DPI_AWARENESS_CONTEXT;
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
typedef BOOL(WINAPI *pfnSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
pfnSetProcessDpiAwarenessContext g_SetProcessDpiAwarenessContext = NULL;

// ==========================================
// 1. 定义 SetWindowCompositionAttribute 所需结构
// ==========================================
enum WINDOWCOMPOSITIONATTRIB {
    WCA_UNDEFINED = 0,
    WCA_ACCENT_POLICY = 19
};

enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

struct ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* pfnSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
typedef BOOL(WINAPI* pfnGradientFill)(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);
pfnGradientFill MyGradientFill = NULL;
HMODULE g_hMsimg = NULL;

// ==========================================
// 2. 全局状态与数据结构
// ==========================================
struct Particle {
    float x, y;
    float vx;
    float speedY;
    float swayAmp;
    float swayFreq;
    float phase;
    float size;
    float angle;
    float angleSpeed;
    bool isPetal;
    int colorIndex;
};

struct DesktopWinInfo {
    HWND hwnd;
    std::wstring title;
    int alphaVal;
    bool isHidden;
    bool isTopmost;
};

typedef std::vector<Particle> ParticleList;
typedef std::vector<DesktopWinInfo> DesktopWinList;
typedef std::vector<HWND> HwndList;

HWND g_hMainWnd = NULL;
HWND g_hSettingsWnd = NULL;
HWND g_hExtraWnd = NULL;

int g_winWidth = 560;
int g_winHeight = 340;
const int PARTICLE_COUNT = 55;
const size_t MAX_TRAIL_PETALS = 70;

// 可保存的设置项
int g_lang = 0;             // 0: 简体中文, 1: English
int g_alpha = 225;          // 当前主窗口透明度 (80 ~ 255)
int g_defaultAlpha = 225;   // 默认启动透明度
int g_colorTheme = 0;       // 0:极光流转, 1:樱花粉, 2:赛博紫, 3:薄荷海, 4:落日金
bool g_isTopmost = false;
bool g_cleanMode = false;

bool g_windowMinimized = false;
bool g_windowFocus = true;

ParticleList g_particles;
ParticleList g_trailPetals;
POINT g_lastMousePt = { -9999, -9999 };
float g_hue = 310.0f;

// 主窗口双缓冲与 GDI 缓存
HDC g_memDC = NULL;
HBITMAP g_memBmp = NULL;
HBITMAP g_oldBmp = NULL;
HBRUSH g_fallbackBgBrush = NULL;

HFONT g_fontTime = NULL;
HFONT g_fontDate = NULL;
HFONT g_fontTip  = NULL;
int g_lastFontBaseSize = 0;

HPEN g_petalPen = NULL;
HPEN g_nullPen = NULL;
HBRUSH g_petalBrush0 = NULL;
HBRUSH g_petalBrush1 = NULL;
HBRUSH g_petalBrush2 = NULL;
HBRUSH g_dotBrush0 = NULL;
HBRUSH g_dotBrush1 = NULL;
HBRUSH g_dotBrush2 = NULL;
HBRUSH g_glowBrush = NULL;
HBRUSH g_glowBrushOuter = NULL;  // 光点最外层柔光
HBRUSH g_secBgBrush = NULL;      // 秒进度条底色
HBRUSH g_secFgBrush = NULL;      // 秒进度条前景

// 设置窗口交互状态
int g_hoverItem = -1;
bool g_savedToast = false;
DWORD g_savedToastTick = 0;

// 设置/额外窗口缓存字体（避免每次重绘都创建销毁 GDI 字体）
HFONT g_uiFontTitle = NULL;  // 26px semibold 标题
HFONT g_uiFontSub   = NULL;  // 18px semibold 小标题
HFONT g_uiFontDesc  = NULL;  // 15px normal 描述文字
HFONT g_fontInfo    = NULL;  // 13px 信息行（第N天/干支年）

// ===== v1.1 系统集成状态 =====
int g_winX = -1;             // 上次窗口 X（-1 表示居中）
int g_winY = -1;             // 上次窗口 Y
bool g_autoStart = false;    // 开机自启
bool g_chimeOn = false;      // 整点报时
bool g_clickThrough = false; // 鼠标点击穿透
bool g_autoTheme = false;    // 按时段自动变色
int  g_lastChimeHour = -1;
bool g_trayAdded = false;

#define WM_TRAYICON          (WM_USER + 20)
#define ID_TRAY_ICON         1
#define IDM_TRAY_SHOW        1001
#define IDM_TRAY_SETTINGS    1002
#define IDM_TRAY_EXTRA       1003
#define IDM_TRAY_TOPMOST     1004
#define IDM_TRAY_THROUGH     1005
#define IDM_TRAY_AUTOSTART   1006
#define IDM_TRAY_CLEAN       1007
#define IDM_TRAY_EXIT        1008
#define IDM_TRAY_24H         1009
#define IDM_TRAY_SECONDS     1010

// ===== 显示与专注计时器 =====
bool g_use24Hour = true;     // 24 小时制
bool g_showSeconds = true;   // 显示秒
int  g_alphaTarget = -1;     // 透明度动画目标值（-1 表示无动画）

// ===== 自绘拖拽 / 边缘缩放 =====
// 说明：Win11 下分层窗口 + 自绘非客户区会让系统移动循环失效（窗口拖不动），
// 因此这里完全自己处理命中、拖拽与缩放，不再依赖 HTCAPTION/HTLEFT 等系统行为。
const int ZONE_MOVE   = 0;
const int ZONE_LEFT   = 1;
const int ZONE_RIGHT  = 2;
const int ZONE_TOP    = 4;
const int ZONE_BOTTOM = 8;
bool  g_dragging = false;
int   g_dragZone = 0;
POINT g_dragStartCursor = { 0, 0 };
RECT  g_dragStartRect   = { 0, 0, 0, 0 };

int HitTestResizeZone(int cx, int cy) {
    const int B = 10;
    bool left   = cx < B;
    bool right  = cx >= g_winWidth - B;
    bool top    = cy < B;
    bool bottom = cy >= g_winHeight - B;
    if (left && top)     return ZONE_LEFT | ZONE_TOP;
    if (right && top)    return ZONE_RIGHT | ZONE_TOP;
    if (left && bottom)  return ZONE_LEFT | ZONE_BOTTOM;
    if (right && bottom) return ZONE_RIGHT | ZONE_BOTTOM;
    if (left)   return ZONE_LEFT;
    if (right)  return ZONE_RIGHT;
    if (top)    return ZONE_TOP;
    if (bottom) return ZONE_BOTTOM;
    return 0;
}

LPCSTR ResizeCursorId(int zone) {
    switch (zone) {
        case ZONE_LEFT | ZONE_TOP:     return IDC_SIZENWSE;
        case ZONE_RIGHT | ZONE_BOTTOM: return IDC_SIZENWSE;
        case ZONE_RIGHT | ZONE_TOP:    return IDC_SIZENESW;
        case ZONE_LEFT | ZONE_BOTTOM:  return IDC_SIZENESW;
        case ZONE_LEFT:                return IDC_SIZEWE;
        case ZONE_RIGHT:               return IDC_SIZEWE;
        case ZONE_TOP:                 return IDC_SIZENS;
        case ZONE_BOTTOM:              return IDC_SIZENS;
        default:                       return IDC_ARROW;
    }
}
int  g_settingsTick = 0;     // 设置窗重绘节流计数
UINT g_taskbarCreatedMsg = 0; // 资源管理器重启广播消息
bool g_pomoOn = false;       // 专注计时器面板
bool g_pomoRunning = false;  // 计时中
int  g_pomoRemain = 25 * 60; // 剩余秒数
DWORD g_pomoLastTick = 0;

#define HK_SETTINGS          1
#define HK_EXTRA             2
#define HK_TOPMOST           3
#define HK_THROUGH           4

// 旧版 MinGW 头文件缺失的常量
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

// 额外功能窗口（外部桌面窗口、桌面图标与任务栏控制）状态
DesktopWinList g_desktopWins;
HwndList g_hiddenHwnds;
int g_selectedWinIdx = 0;
int g_scrollOffset = 0;
int g_extraHover = -1;
bool g_desktopIconsHidden = false;
bool g_taskbarHidden = false;

// ==========================================
// 3. 配置文件读写 (sakura_clock.ini)
// ==========================================
std::string GetConfigPath() {
    std::string path(MAX_PATH, '\0');
    DWORD len = GetModuleFileNameA(NULL, &path.at(0), MAX_PATH);
    path.resize(len);
    size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) {
        path = path.substr(0, pos + 1);
    }
    return path + "sakura_clock.ini";
}

void LoadSettings() {
    std::string ini = GetConfigPath();
    g_lang = GetPrivateProfileIntA("Settings", "Language", 0, ini.c_str());
    if (g_lang != 0 && g_lang != 1) g_lang = 0;

    g_defaultAlpha = GetPrivateProfileIntA("Settings", "DefaultAlpha", 225, ini.c_str());
    if (g_defaultAlpha < 80) g_defaultAlpha = 80;
    if (g_defaultAlpha > 255) g_defaultAlpha = 255;
    g_alpha = g_defaultAlpha;

    g_colorTheme = GetPrivateProfileIntA("Settings", "ColorTheme", 0, ini.c_str());
    if (g_colorTheme < 0 || g_colorTheme > 5) g_colorTheme = 0;

    g_isTopmost = GetPrivateProfileIntA("Settings", "Topmost", 0, ini.c_str()) != 0;
    g_cleanMode = GetPrivateProfileIntA("Settings", "CleanMode", 0, ini.c_str()) != 0;

    // 窗口几何（容错：越界/异常值回退到居中默认）
    int w = GetPrivateProfileIntA("Window", "Width", 560, ini.c_str());
    int h = GetPrivateProfileIntA("Window", "Height", 340, ini.c_str());
    if (w < 240 || w > 4000) w = 560;
    if (h < 140 || h > 3000) h = 340;
    g_winWidth = w;
    g_winHeight = h;

    g_winX = GetPrivateProfileIntA("Window", "X", -1, ini.c_str());
    g_winY = GetPrivateProfileIntA("Window", "Y", -1, ini.c_str());

    g_chimeOn = GetPrivateProfileIntA("Settings", "Chime", 0, ini.c_str()) != 0;
    g_autoTheme = GetPrivateProfileIntA("Settings", "AutoTheme", 0, ini.c_str()) != 0;
    g_clickThrough = GetPrivateProfileIntA("Settings", "ClickThrough", 0, ini.c_str()) != 0;
    g_autoStart = GetPrivateProfileIntA("Settings", "AutoStart", 0, ini.c_str()) != 0;
    g_use24Hour = GetPrivateProfileIntA("Settings", "Use24Hour", 1, ini.c_str()) != 0;
    g_showSeconds = GetPrivateProfileIntA("Settings", "ShowSeconds", 1, ini.c_str()) != 0;
}

void SaveSettings() {
    std::string ini = GetConfigPath();
    std::string buf(32, '\0');

    snprintf(&buf.at(0), buf.size(), "%d", g_lang);
    WritePrivateProfileStringA("Settings", "Language", buf.c_str(), ini.c_str());

    snprintf(&buf.at(0), buf.size(), "%d", g_defaultAlpha);
    WritePrivateProfileStringA("Settings", "DefaultAlpha", buf.c_str(), ini.c_str());

    snprintf(&buf.at(0), buf.size(), "%d", g_colorTheme);
    WritePrivateProfileStringA("Settings", "ColorTheme", buf.c_str(), ini.c_str());

    WritePrivateProfileStringA("Settings", "Topmost", g_isTopmost ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "CleanMode", g_cleanMode ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "Chime", g_chimeOn ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "AutoTheme", g_autoTheme ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "ClickThrough", "0", ini.c_str());  // 穿透不跨会话记住，避免启动即无法点击
    WritePrivateProfileStringA("Settings", "AutoStart", g_autoStart ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "Use24Hour", g_use24Hour ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Settings", "ShowSeconds", g_showSeconds ? "1" : "0", ini.c_str());

    // 记录窗口位置与大小
    RECT wr;
    if (g_hMainWnd && IsWindow(g_hMainWnd) && GetWindowRect(g_hMainWnd, &wr)) {
        snprintf(&buf.at(0), buf.size(), "%d", (int)wr.left);
        WritePrivateProfileStringA("Window", "X", buf.c_str(), ini.c_str());
        snprintf(&buf.at(0), buf.size(), "%d", (int)wr.top);
        WritePrivateProfileStringA("Window", "Y", buf.c_str(), ini.c_str());
        snprintf(&buf.at(0), buf.size(), "%d", (int)(wr.right - wr.left));
        WritePrivateProfileStringA("Window", "Width", buf.c_str(), ini.c_str());
        snprintf(&buf.at(0), buf.size(), "%d", (int)(wr.bottom - wr.top));
        WritePrivateProfileStringA("Window", "Height", buf.c_str(), ini.c_str());
    }
}

// ==========================================
// 3.1 开机自启（HKCU Run 键）
// ==========================================
bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD size = (DWORD)(sizeof(buf) - sizeof(wchar_t));
    DWORD type = 0;
    bool exists = (RegQueryValueExW(hKey, L"SakuraClock", NULL, &type,
                                    (LPBYTE)buf, &size) == ERROR_SUCCESS && type == REG_SZ);
    RegCloseKey(hKey);
    return exists;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        wchar_t path[MAX_PATH] = { 0 };
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring val = L"\"";
        val += path;
        val += L"\"";
        RegSetValueExW(hKey, L"SakuraClock", 0, REG_SZ,
                       (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, L"SakuraClock");
    }
    RegCloseKey(hKey);
    g_autoStart = enable;
}

// ==========================================
// 4. HSV 转 RGB 与窗口属性辅助函数
// ==========================================
void HsvToRgb(float h, float s, float v, BYTE& outR, BYTE& outG, BYTE& outB) {
    while (h >= 360.0f) h -= 360.0f;
    while (h < 0.0f) h += 360.0f;

    float c = v * s;
    float x = c * (1.0f - fabs(fmod(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;

    if (h < 60) { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else { r = c; g = 0; b = x; }

    outR = (BYTE)((r + m) * 255);
    outG = (BYTE)((g + m) * 255);
    outB = (BYTE)((b + m) * 255);
}

void ApplyWindowAlpha(HWND hwnd, int alphaVal) {
    if (alphaVal < 80) alphaVal = 80;
    if (alphaVal > 255) alphaVal = 255;
    g_alpha = alphaVal;
    g_alphaTarget = -1;
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
}

void ApplyWindowTopmost(HWND hwnd, bool topmost) {
    g_isTopmost = topmost;
    SetWindowPos(
        hwnd,
        g_isTopmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE
    );
}

// ==========================================
// 4.1 鼠标穿透 / 托盘 / 全局热键 / 边缘吸附
// ==========================================
void SetClickThrough(HWND hwnd, bool enable) {
    g_clickThrough = enable;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (enable) {
        ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    } else {
        ex &= ~WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
}

void AddTrayIcon(HWND hwnd) {
    if (g_trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), L"A", IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    lstrcpynW(nid.szTip, L"Sakura Clock  \u6a31\u82b1\u65f6\u949f", 128);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_trayAdded = true;
    }
}

void RemoveTrayIcon(HWND hwnd) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

void ShowTrayMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW,
                g_lang == 0 ? L"\u663e\u793a / \u9690\u85cf\u65f6\u949f" : L"Show / Hide Clock");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SETTINGS,
                g_lang == 0 ? L"\u4e2a\u6027\u5316\u4e0e\u8bbe\u7f6e  (Ctrl+Alt+A)" : L"Settings  (Ctrl+Alt+A)");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXTRA,
                g_lang == 0 ? L"\u989d\u5916\u529f\u80fd  (Ctrl+Alt+W)" : L"Extra Tools  (Ctrl+Alt+W)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (g_isTopmost ? MF_CHECKED : 0), IDM_TRAY_TOPMOST,
                g_lang == 0 ? L"\u59cb\u7ec8\u7f6e\u9876  (Ctrl+Alt+T)" : L"Always on Top  (Ctrl+Alt+T)");
    AppendMenuW(hMenu, MF_STRING | (g_clickThrough ? MF_CHECKED : 0), IDM_TRAY_THROUGH,
                g_lang == 0 ? L"\u9f20\u6807\u7a7f\u900f  (Ctrl+Alt+P)" : L"Click-Through  (Ctrl+Alt+P)");
    AppendMenuW(hMenu, MF_STRING | (g_autoStart ? MF_CHECKED : 0), IDM_TRAY_AUTOSTART,
                g_lang == 0 ? L"\u5f00\u673a\u81ea\u542f" : L"Run at Startup");
    AppendMenuW(hMenu, MF_STRING | (g_cleanMode ? MF_CHECKED : 0), IDM_TRAY_CLEAN,
                g_lang == 0 ? L"\u7eaf\u51c0\u6a21\u5f0f" : L"Clean Mode");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING | (g_use24Hour ? MF_CHECKED : 0), IDM_TRAY_24H,
                g_lang == 0 ? L"24 \u5c0f\u65f6\u5236" : L"24-Hour Clock");
    AppendMenuW(hMenu, MF_STRING | (g_showSeconds ? MF_CHECKED : 0), IDM_TRAY_SECONDS,
                g_lang == 0 ? L"\u663e\u793a\u79d2" : L"Show Seconds");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, g_lang == 0 ? L"\u9000\u51fa" : L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

void RegisterGlobalHotkeys(HWND hwnd) {
    RegisterHotKey(hwnd, HK_SETTINGS, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'A');
    RegisterHotKey(hwnd, HK_EXTRA,    MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'W');
    RegisterHotKey(hwnd, HK_TOPMOST,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T');
    RegisterHotKey(hwnd, HK_THROUGH,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
}

void UnregisterGlobalHotkeys(HWND hwnd) {
    UnregisterHotKey(hwnd, HK_SETTINGS);
    UnregisterHotKey(hwnd, HK_EXTRA);
    UnregisterHotKey(hwnd, HK_TOPMOST);
    UnregisterHotKey(hwnd, HK_THROUGH);
}

void GetMonitorWorkArea(HWND hwnd, RECT& rc) {
    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (hMon && GetMonitorInfoW(hMon, &mi)) {
        rc = mi.rcWork;
        return;
    }
    rc.left = 0;
    rc.top = 0;
    rc.right = GetSystemMetrics(SM_CXSCREEN);
    rc.bottom = GetSystemMetrics(SM_CYSCREEN);
}

void SnapToScreenEdges(HWND hwnd) {
    RECT wr, wa;
    if (!GetWindowRect(hwnd, &wr)) return;
    GetMonitorWorkArea(hwnd, wa);
    const int SNAP = 18;
    int nx = wr.left;
    int ny = wr.top;
    if (abs((int)wr.left - (int)wa.left) <= SNAP) nx = wa.left;
    if (abs((int)wa.right - (int)wr.right) <= SNAP) nx = wa.right - (wr.right - wr.left);
    if (abs((int)wr.top - (int)wa.top) <= SNAP) ny = wa.top;
    if (abs((int)wa.bottom - (int)wr.bottom) <= SNAP) ny = wa.bottom - (wr.bottom - wr.top);
    if (nx != wr.left || ny != wr.top) {
        SetWindowPos(hwnd, NULL, nx, ny, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// ==========================================
// 5. 初始化系统毛玻璃背景与双缓冲
// ==========================================
void InitAcrylicOnce(HWND hwnd) {
    HMODULE hUser = GetModuleHandleA("user32.dll");
    if (!hUser) return;

    pfnSetWindowCompositionAttribute SetWCA =
        (pfnSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");

    if (SetWCA) {
        ACCENT_POLICY policy;
        policy.AccentState = ACCENT_ENABLE_BLURBEHIND;
        policy.AccentFlags = 2;
        policy.GradientColor = 0x66201030;
        policy.AnimationId = 0;

        WINDOWCOMPOSITIONATTRIBDATA data;
        data.Attrib = WCA_ACCENT_POLICY;
        data.pvData = &policy;
        data.cbData = sizeof(policy);

        SetWCA(hwnd, &data);
    }
}

void RebuildBufferAndFonts(HWND hwnd, int width, int height) {
    if (width <= 0 || height <= 0) return;
    g_winWidth = width;
    g_winHeight = height;

    HDC hdc = GetDC(hwnd);
    if (!g_memDC) {
        g_memDC = CreateCompatibleDC(hdc);
    } else if (g_memBmp) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_memBmp);
    }
    g_memBmp = CreateCompatibleBitmap(hdc, g_winWidth, g_winHeight);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_memBmp);
    ReleaseDC(hwnd, hdc);

    int minDim = (g_winWidth * 0.65f < g_winHeight) ? (int)(g_winWidth * 0.65f) : g_winHeight;
    int timeFontSize = minDim / 3;
    if (timeFontSize < 28) timeFontSize = 28;

    if (timeFontSize != g_lastFontBaseSize) {
        g_lastFontBaseSize = timeFontSize;
        if (g_fontTime) DeleteObject(g_fontTime);
        if (g_fontDate) DeleteObject(g_fontDate);
        if (g_fontTip)  DeleteObject(g_fontTip);

        g_fontTime = CreateFontW(timeFontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        int dateFontSize = timeFontSize / 4;
        if (dateFontSize < 14) dateFontSize = 14;
        g_fontDate = CreateFontW(dateFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        g_fontTip = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        if (g_fontInfo) DeleteObject(g_fontInfo);
        int infoFontSize = dateFontSize - 1;
        if (infoFontSize < 12) infoFontSize = 12;
        g_fontInfo = CreateFontW(infoFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
    }
}

// ==========================================
// 6. 粒子初始化、鼠标生花与更新
// ==========================================
void ResetParticle(Particle& p, bool randomY) {
    int safeW = (g_winWidth > 20) ? g_winWidth : 560;
    int safeH = (g_winHeight > 20) ? g_winHeight : 340;

    p.x = (float)(rand() % safeW);
    p.y = randomY ? (float)(rand() % safeH) : -15.0f;
    p.vx = 0.0f;
    p.isPetal = (rand() % 100) < 55;
    p.phase = (float)(rand() % 628) / 100.0f;
    p.swayFreq = 0.025f + (rand() % 25) / 1000.0f;
    p.colorIndex = rand() % 3;

    if (p.isPetal) {
        p.speedY = 0.9f + (rand() % 110) / 100.0f;
        p.swayAmp = 14.0f + (rand() % 22);
        p.size = 5.0f + (rand() % 4);
        p.angle = (float)(rand() % 360) * 3.14159f / 180.0f;
        p.angleSpeed = ((rand() % 100) - 50) / 1000.0f;
    } else {
        p.speedY = 0.45f + (rand() % 75) / 100.0f;

p.swayAmp = 8.0f + (rand() % 14);
p.size = 2.0f + (rand() % 3);
p.angle = 0;
p.angleSpeed = 0;
}
}
void SpawnMousePetal(float mx, float my) {
if (g_trailPetals.size() >= MAX_TRAIL_PETALS) {
g_trailPetals.erase(g_trailPetals.begin());
}
Particle p;
p.x = mx + ((rand() % 12) - 6);
p.y = my + ((rand() % 12) - 6);
p.vx = ((rand() % 140) - 70) / 100.0f;
p.speedY = 1.0f + (rand() % 120) / 100.0f;
p.swayAmp = 10.0f + (rand() % 18);
p.swayFreq = 0.03f + (rand() % 25) / 1000.0f;
p.phase = (float)(rand() % 628) / 100.0f;
p.size = 5.0f + (rand() % 4);
p.angle = (float)(rand() % 360) * 3.14159f / 180.0f;
p.angleSpeed = ((rand() % 120) - 60) / 1000.0f;
p.isPetal = true;
p.colorIndex = rand() % 3;
g_trailPetals.push_back(p);
}
void CheckMouseTrail(HWND hwnd) {
POINT pt;
if (!GetCursorPos(&pt)) return;
ScreenToClient(hwnd, &pt);
if (pt.x >= 0 && pt.x < g_winWidth && pt.y >= 0 && pt.y < g_winHeight) {
if (g_lastMousePt.x != -9999) {
int dx = pt.x - g_lastMousePt.x;
int dy = pt.y - g_lastMousePt.y;
if (dx * dx + dy * dy >= 16) {
SpawnMousePetal((float)pt.x, (float)pt.y);
if (dx * dx + dy * dy >= 400) {
SpawnMousePetal((float)(pt.x - dx * 0.5f), (float)(pt.y - dy * 0.5f));
}
}
}
g_lastMousePt = pt;
} else {
g_lastMousePt.x = -9999;
g_lastMousePt.y = -9999;
}
}
void InitResources(HWND hwnd) {
g_hMsimg = LoadLibraryA("msimg32.dll");
if (g_hMsimg) {
MyGradientFill = (pfnGradientFill)GetProcAddress(g_hMsimg, "GradientFill");
}
RebuildBufferAndFonts(hwnd, g_winWidth, g_winHeight);
g_petalPen = CreatePen(PS_SOLID, 1, RGB(255, 235, 245));
g_nullPen = (HPEN)GetStockObject(NULL_PEN);
g_petalBrush0 = CreateSolidBrush(RGB(255, 195, 218));
g_petalBrush1 = CreateSolidBrush(RGB(255, 222, 236));
g_petalBrush2 = CreateSolidBrush(RGB(250, 175, 208));
g_dotBrush0 = CreateSolidBrush(RGB(255, 255, 255));
g_dotBrush1 = CreateSolidBrush(RGB(255, 245, 195));
g_dotBrush2 = CreateSolidBrush(RGB(205, 245, 255));
g_glowBrush = CreateSolidBrush(RGB(170, 160, 195));
g_glowBrushOuter = CreateSolidBrush(RGB(120, 110, 150));
g_secBgBrush = CreateSolidBrush(RGB(70, 55, 85));
g_secFgBrush = CreateSolidBrush(RGB(255, 225, 240));
g_fallbackBgBrush = CreateSolidBrush(RGB(200, 160, 190));
g_uiFontTitle = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
g_uiFontSub = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
g_uiFontDesc = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
srand((unsigned int)time(NULL));
g_particles.resize(PARTICLE_COUNT);
for (int i = 0; i < PARTICLE_COUNT; ++i) {
ResetParticle(g_particles.at(i), true);
}
}
void CleanupResources() {
if (g_hMsimg) {
FreeLibrary(g_hMsimg);
g_hMsimg = NULL;
MyGradientFill = NULL;
}
if (g_memDC) {
SelectObject(g_memDC, g_oldBmp);
DeleteObject(g_memBmp);
DeleteDC(g_memDC);
}
if (g_fontTime) DeleteObject(g_fontTime);
if (g_fontDate) DeleteObject(g_fontDate);
if (g_fontTip)  DeleteObject(g_fontTip);
if (g_fontInfo) DeleteObject(g_fontInfo);
if (g_petalPen) DeleteObject(g_petalPen);
if (g_glowBrush) DeleteObject(g_glowBrush);
if (g_glowBrushOuter) DeleteObject(g_glowBrushOuter);
if (g_secBgBrush) DeleteObject(g_secBgBrush);
if (g_secFgBrush) DeleteObject(g_secFgBrush);
if (g_fallbackBgBrush) DeleteObject(g_fallbackBgBrush);
if (g_petalBrush0) DeleteObject(g_petalBrush0);
if (g_petalBrush1) DeleteObject(g_petalBrush1);
if (g_petalBrush2) DeleteObject(g_petalBrush2);
if (g_dotBrush0) DeleteObject(g_dotBrush0);
if (g_dotBrush1) DeleteObject(g_dotBrush1);
if (g_dotBrush2) DeleteObject(g_dotBrush2);
if (g_uiFontTitle) DeleteObject(g_uiFontTitle);
if (g_uiFontSub)   DeleteObject(g_uiFontSub);
if (g_uiFontDesc)  DeleteObject(g_uiFontDesc);
}
void UpdateParticles(float speedScale) {
for (int i = 0; i < PARTICLE_COUNT; ++i) {
Particle& p = g_particles.at(i);
p.y += p.speedY * speedScale;
p.phase += p.swayFreq * speedScale;
p.angle += p.angleSpeed * speedScale;
if (p.y > g_winHeight + 15 || p.x > g_winWidth + 30 || p.x < -30) {
ResetParticle(p, false);
}
}
for (size_t i = 0; i < g_trailPetals.size(); ) {
Particle& p = g_trailPetals.at(i);
p.x += p.vx * speedScale;
p.vx *= (1.0f - 0.04f * speedScale);  // 空气阻力近似，避免每帧 pow
p.y += p.speedY * speedScale;
p.phase += p.swayFreq * speedScale;
p.angle += p.angleSpeed * speedScale;
if (p.y > g_winHeight + 15) {
g_trailPetals.erase(g_trailPetals.begin() + i);
} else {
++i;
}
}
}
// ==========================================
// 7. 绘制主时钟背景、粒子与双语文字
// ==========================================
void DrawDreamyBackground(HDC hdc, float hue) {
BYTE r1, g1, b1, r2, g2, b2;
// 自动时段变色：早晨薄荷、白天樱粉、傍晚落日、夜晚幻紫
int theme = g_colorTheme;
if (g_autoTheme) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int h = st.wHour;
    if (h >= 5 && h < 11) theme = 3;
    else if (h >= 11 && h < 17) theme = 1;
    else if (h >= 17 && h < 21) theme = 4;
    else theme = 2;
}
if (theme == 1) {
float wave = sin(hue * 0.05f) * 14.0f;
HsvToRgb(330.0f + wave, 0.42f, 0.85f, r1, g1, b1);
HsvToRgb(350.0f + wave, 0.48f, 0.74f, r2, g2, b2);
} else if (theme == 2) {
float wave = sin(hue * 0.05f) * 18.0f;
HsvToRgb(265.0f + wave, 0.55f, 0.76f, r1, g1, b1);
HsvToRgb(305.0f + wave, 0.58f, 0.65f, r2, g2, b2);
} else if (theme == 3) {
float wave = sin(hue * 0.05f) * 16.0f;
HsvToRgb(175.0f + wave, 0.46f, 0.78f, r1, g1, b1);
HsvToRgb(210.0f + wave, 0.52f, 0.68f, r2, g2, b2);
} else if (theme == 4) {
float wave = sin(hue * 0.05f) * 14.0f;
HsvToRgb(22.0f + wave, 0.48f, 0.84f, r1, g1, b1);
HsvToRgb(345.0f + wave, 0.50f, 0.72f, r2, g2, b2);
} else if (theme == 5) {
float wave = sin(hue * 0.05f) * 12.0f;
HsvToRgb(228.0f + wave, 0.62f, 0.52f, r1, g1, b1);
HsvToRgb(272.0f + wave, 0.66f, 0.38f, r2, g2, b2);
} else {
HsvToRgb(hue, 0.45f, 0.78f, r1, g1, b1);
HsvToRgb(hue + 55.0f, 0.50f, 0.68f, r2, g2, b2);
}
if (MyGradientFill) {
struct TwoVerts { TRIVERTEX v0; TRIVERTEX v1; } vert;
vert.v0.x = 0;
vert.v0.y = 0;
vert.v0.Red   = (COLOR16)(r1 << 8);
vert.v0.Green = (COLOR16)(g1 << 8);
vert.v0.Blue  = (COLOR16)(b1 << 8);
vert.v0.Alpha = 0x0000;
vert.v1.x = g_winWidth;
vert.v1.y = g_winHeight;
vert.v1.Red   = (COLOR16)(r2 << 8);
vert.v1.Green = (COLOR16)(g2 << 8);
vert.v1.Blue  = (COLOR16)(b2 << 8);
vert.v1.Alpha = 0x0000;
GRADIENT_RECT gRect;
gRect.UpperLeft = 0;
gRect.LowerRight = 1;
MyGradientFill(hdc, &vert.v0, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
} else {
RECT rc = { 0, 0, g_winWidth, g_winHeight };
FillRect(hdc, &rc, g_fallbackBgBrush);
}
}
void DrawSakuraPetal(HDC hdc, float cx, float cy, float size, float angle, int colorIdx) {
    // 真实樱花瓣：顶部带小缺口的泪滴形（7 点多边形）
    struct SevenPts { POINT p[7]; } pts;
    float cosA = cos(angle);
    float sinA = sin(angle);
    // 局部坐标：顶部有缺口的花瓣轮廓
    const float lx[7] = {  0.00f,  0.18f, 0.62f,  0.55f, 0.00f, -0.55f, -0.62f };
    const float ly[7] = { -1.00f, -0.58f, -0.68f, 0.10f, 1.18f,  0.10f, -0.68f };
    for (int i = 0; i < 7; ++i) {
        pts.p[i].x = (LONG)(cx + lx[i] * size * cosA - ly[i] * size * sinA);
        pts.p[i].y = (LONG)(cy + lx[i] * size * sinA + ly[i] * size * cosA);
    }
    HBRUSH brush = g_petalBrush0;
    if (colorIdx == 1) brush = g_petalBrush1;
    else if (colorIdx == 2) brush = g_petalBrush2;
    SelectObject(hdc, brush);
    SelectObject(hdc, g_petalPen);
    Polygon(hdc, pts.p, 7);
}
void DrawGlowingDot(HDC hdc, float cx, float cy, float size, float phase, int colorIdx) {
    // 三层辉光：呼吸脉冲 -> 外圈柔光 -> 中圈过渡 -> 内核亮点
    float pulse = 0.7f + 0.5f * sin(phase * 2.5f);
    int r = (int)(size * pulse);
    if (r < 1) r = 1;
    SelectObject(hdc, g_nullPen);
    // 外层大柔光（缓存画刷）
    int glowR2 = r + 5;
    SelectObject(hdc, g_glowBrushOuter);
    Ellipse(hdc, (int)cx - glowR2, (int)cy - glowR2, (int)cx + glowR2, (int)cy + glowR2);
    // 中层辉光
    int glowR = r + 2;
    SelectObject(hdc, g_glowBrush);
    Ellipse(hdc, (int)cx - glowR, (int)cy - glowR, (int)cx + glowR, (int)cy + glowR);
    HBRUSH brush = g_dotBrush0;
    if (colorIdx == 1) brush = g_dotBrush1;
    else if (colorIdx == 2) brush = g_dotBrush2;
    SelectObject(hdc, brush);
    Ellipse(hdc, (int)cx - r, (int)cy - r, (int)cx + r, (int)cy + r);
}
// ===== 专注计时器（番茄钟） =====
void SpawnPetalBurst(int count) {
    int w = (g_winWidth > 10) ? g_winWidth : 200;
    int h = (g_winHeight > 10) ? g_winHeight : 160;
    for (int i = 0; i < count; ++i) {
        SpawnMousePetal((float)(rand() % w), (float)(rand() % (h / 2 + 1)));
    }
}

void UpdatePomodoro() {
    if (!g_pomoOn || !g_pomoRunning) return;
    DWORD now = GetTickCount();
    if (g_pomoLastTick == 0) {
        g_pomoLastTick = now;
        return;
    }
    DWORD dt = now - g_pomoLastTick;
    if (dt < 1000) return;
    int secs = (int)(dt / 1000);
    g_pomoLastTick += (DWORD)secs * 1000;
    g_pomoRemain -= secs;
    if (g_pomoRemain <= 0) {
        g_pomoRemain = 0;
        g_pomoRunning = false;
        g_pomoLastTick = 0;
        MessageBeep(MB_ICONEXCLAMATION);
        SpawnPetalBurst(48);
    }
}

// ===== 日期信息辅助：年内天数 / 干支年 / 节日 =====
int DayOfYearOf(const SYSTEMTIME& st) {
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int d = cum[st.wMonth - 1] + st.wDay;
    bool leap = ((st.wYear % 4 == 0 && st.wYear % 100 != 0) || st.wYear % 400 == 0);
    if (leap && st.wMonth > 2) d++;
    return d;
}

int DaysInYearOf(int year) {
    bool leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
    return leap ? 366 : 365;
}

const wchar_t* GanZhiOfYear(int year) {
    static const wchar_t* stems[10] = {
        L"\u7532", L"\u4e59", L"\u4e19", L"\u4e01", L"\u620a",
        L"\u5df1", L"\u5e9a", L"\u8f9b", L"\u58ec", L"\u7678"
    };
    static const wchar_t* branches[12] = {
        L"\u5b50", L"\u4e11", L"\u5bc5", L"\u536f", L"\u8fb0", L"\u5df3",
        L"\u5348", L"\u672a", L"\u7533", L"\u9149", L"\u620c", L"\u4ea5"
    };
    static const wchar_t* animals[12] = {
        L"\u9f20", L"\u725b", L"\u864e", L"\u5154", L"\u9f99", L"\u86c7",
        L"\u9a6c", L"\u7f8a", L"\u7334", L"\u9e21", L"\u72d7", L"\u732a"
    };
    static wchar_t buf[32];
    int si = (year - 4) % 10; if (si < 0) si += 10;
    int bi = (year - 4) % 12; if (bi < 0) bi += 12;
    _snwprintf(buf, 32, L"%ls%ls%ls\u5e74", stems[si], branches[bi], animals[bi]);
    return buf;
}

const wchar_t* FestivalOfDay(const SYSTEMTIME& st) {
    int m = st.wMonth, d = st.wDay;
    if (g_lang == 0) {
        if (m == 1 && d == 1)   return L"\u5143\u65e6\u5feb\u4e50";
        if (m == 2 && d == 14)  return L"\u60c5\u4eba\u8282\u5feb\u4e50";
        if (m == 3 && d == 8)   return L"\u5987\u5973\u8282\u5feb\u4e50";
        if (m == 4 && d == 1)   return L"\u611a\u4eba\u8282\u5feb\u4e50";
        if (m == 5 && d == 1)   return L"\u52b3\u52a8\u8282\u5feb\u4e50";
        if (m == 6 && d == 1)   return L"\u513f\u7ae5\u8282\u5feb\u4e50";
        if (m == 9 && d == 10)  return L"\u6559\u5e08\u8282\u5feb\u4e50";
        if (m == 10 && d == 1)  return L"\u56fd\u5e86\u8282\u5feb\u4e50";
        if (m == 12 && d == 24) return L"\u5e73\u5b89\u591c\u5feb\u4e50";
        if (m == 12 && d == 25) return L"\u5723\u8bde\u5feb\u4e50";
    } else {
        if (m == 1 && d == 1)   return L"Happy New Year";
        if (m == 2 && d == 14)  return L"Happy Valentine's Day";
        if (m == 3 && d == 8)   return L"Happy Women's Day";
        if (m == 4 && d == 1)   return L"April Fools' Day";
        if (m == 5 && d == 1)   return L"Happy Labour Day";
        if (m == 6 && d == 1)   return L"Happy Children's Day";
        if (m == 9 && d == 10)  return L"Happy Teachers' Day";
        if (m == 10 && d == 1)  return L"Happy National Day";
        if (m == 12 && d == 24) return L"Christmas Eve";
        if (m == 12 && d == 25) return L"Merry Christmas";
    }
    return NULL;
}

void DrawClockOverlay(HDC hdc) {
SYSTEMTIME st;
GetLocalTime(&st);
std::wstring timeBuf(64, L'\0');
if (g_use24Hour) {
    if (g_showSeconds)
        snwprintf(&timeBuf.at(0), timeBuf.size(), L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    else
        snwprintf(&timeBuf.at(0), timeBuf.size(), L"%02d:%02d", st.wHour, st.wMinute);
} else {
    int h12 = st.wHour % 12;
    if (h12 == 0) h12 = 12;
    const wchar_t* ampm = (g_lang == 0)
        ? (st.wHour < 12 ? L"\u4e0a\u5348" : L"\u4e0b\u5348")
        : (st.wHour < 12 ? L"AM" : L"PM");
    if (g_showSeconds)
        snwprintf(&timeBuf.at(0), timeBuf.size(), L"%d:%02d:%02d %ls", h12, st.wMinute, st.wSecond, ampm);
    else
        snwprintf(&timeBuf.at(0), timeBuf.size(), L"%d:%02d %ls", h12, st.wMinute, ampm);
}
static const wchar_t* kWeekZh[7] = {
    L"\u661f\u671f\u65e5", L"\u661f\u671f\u4e00", L"\u661f\u671f\u4e8c", L"\u661f\u671f\u4e09",
    L"\u661f\u671f\u56db", L"\u661f\u671f\u4e94", L"\u661f\u671f\u516d"
};
static const wchar_t* kWeekEn[7] = {
    L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
    L"Thursday", L"Friday", L"Saturday"
};
const wchar_t* weekName = (g_lang == 0) ? kWeekZh[st.wDayOfWeek] : kWeekEn[st.wDayOfWeek];
const wchar_t* ganzhi = GanZhiOfYear(st.wYear);
std::wstring dateBuf(96, L'\0');
if (g_lang == 0) {
snwprintf(&dateBuf.at(0), dateBuf.size(), L"%04d\u5e74%02d\u6708%02d\u65e5   %ls  \u00b7  %ls",
st.wYear, st.wMonth, st.wDay, weekName, ganzhi);
} else {
snwprintf(&dateBuf.at(0), dateBuf.size(), L"%04d-%02d-%02d   %ls  \u00b7  %ls",
st.wYear, st.wMonth, st.wDay, weekName, ganzhi);
}
SetBkMode(hdc, TRANSPARENT);
HFONT oldFont = (HFONT)SelectObject(hdc, g_fontTime);
int centerY = g_winHeight / 2;

// 时间文字：外发光 -> 投影 -> 主体白字
RECT timeGlowRect = { 0, centerY - g_lastFontBaseSize, g_winWidth, centerY + 20 };
SetTextColor(hdc, RGB(255, 205, 230));
RECT glowR = timeGlowRect; OffsetRect(&glowR, -1, 0);
DrawTextW(hdc, timeBuf.c_str(), -1, &glowR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
glowR = timeGlowRect; OffsetRect(&glowR, 1, 0);
DrawTextW(hdc, timeBuf.c_str(), -1, &glowR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
glowR = timeGlowRect; OffsetRect(&glowR, 0, -1);
DrawTextW(hdc, timeBuf.c_str(), -1, &glowR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
glowR = timeGlowRect; OffsetRect(&glowR, 0, 1);
DrawTextW(hdc, timeBuf.c_str(), -1, &glowR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

RECT timeShadowRect = { 3, centerY - g_lastFontBaseSize + 3, g_winWidth + 3, centerY + 23 };
SetTextColor(hdc, RGB(45, 25, 60));
DrawTextW(hdc, timeBuf.c_str(), -1, &timeShadowRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
RECT timeRect = { 0, centerY - g_lastFontBaseSize, g_winWidth, centerY + 20 };
SetTextColor(hdc, RGB(255, 255, 255));
DrawTextW(hdc, timeBuf.c_str(), -1, &timeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
SelectObject(hdc, g_fontDate);
RECT dateShadowRect = { 2, centerY + 22, g_winWidth + 2, centerY + 72 };
SetTextColor(hdc, RGB(45, 25, 60));
DrawTextW(hdc, dateBuf.c_str(), -1, &dateShadowRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
RECT dateRect = { 0, centerY + 20, g_winWidth, centerY + 70 };
SetTextColor(hdc, RGB(245, 235, 255));
DrawTextW(hdc, dateBuf.c_str(), -1, &dateRect, DT_CENTER | DT_TOP | DT_SINGLELINE);

// 年内进度信息行
int dateFontSize = g_lastFontBaseSize / 4;
if (dateFontSize < 14) dateFontSize = 14;
if (g_winHeight >= 200 && g_fontInfo) {
SelectObject(hdc, g_fontInfo);
int doy = DayOfYearOf(st);
int weekNo = (doy + 6) / 7;
int daysLeft = DaysInYearOf(st.wYear) - doy + 1;
std::wstring infoBuf(160, L'\0');
if (g_lang == 0) {
snwprintf(&infoBuf.at(0), infoBuf.size(),
L"\u7b2c %d \u5929  \u00b7  \u7b2c %d \u5468  \u00b7  \u8ddd\u5143\u65e6 %d \u5929",
doy, weekNo, daysLeft);
} else {
snwprintf(&infoBuf.at(0), infoBuf.size(),
L"Day %d  \u00b7  Week %d  \u00b7  %d days to New Year",
doy, weekNo, daysLeft);
}
SetTextColor(hdc, RGB(235, 220, 245));
RECT infoRect = { 0, centerY + 20 + dateFontSize + 8, g_winWidth, centerY + 20 + dateFontSize + 34 };
DrawTextW(hdc, infoBuf.c_str(), -1, &infoRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// 专注计时器显示
if (g_pomoOn) {
SelectObject(hdc, g_fontInfo ? g_fontInfo : g_fontDate);
int pmm = g_pomoRemain / 60;
int pss = g_pomoRemain % 60;
std::wstring pbuf(96, L'\0');
if (g_lang == 0) {
snwprintf(&pbuf.at(0), pbuf.size(), L"\u4e13\u6ce8 %02d:%02d   %ls",
pmm, pss, g_pomoRunning ? L"\u8fd0\u884c\u4e2d" : L"\u5df2\u6682\u505c");
} else {
snwprintf(&pbuf.at(0), pbuf.size(), L"Focus %02d:%02d   %ls",
pmm, pss, g_pomoRunning ? L"running" : L"paused");
}
SetTextColor(hdc, RGB(255, 215, 238));
int pomoY = centerY + 20 + dateFontSize + 8;
if (g_winHeight >= 200 && g_fontInfo) pomoY += 26;
RECT pomoRect = { 0, pomoY, g_winWidth, pomoY + 26 };
DrawTextW(hdc, pbuf.c_str(), -1, &pomoRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// 节日问候（窗口足够高时显示在顶部）
const wchar_t* festival = FestivalOfDay(st);
if (festival && g_winHeight >= 230) {
SelectObject(hdc, g_fontInfo ? g_fontInfo : g_fontDate);
SetTextColor(hdc, RGB(60, 35, 70));
RECT fs = { 2, 12, g_winWidth + 2, 40 };
DrawTextW(hdc, festival, -1, &fs, DT_CENTER | DT_TOP | DT_SINGLELINE);
SetTextColor(hdc, RGB(255, 240, 250));
RECT fr = { 0, 10, g_winWidth, 38 };
DrawTextW(hdc, festival, -1, &fr, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

// 底部进度线：默认显示秒进度，开启专注时显示倒计时进度
{
float ratio = (float)st.wSecond / 60.0f;
if (g_pomoOn && 25 * 60 > 0) {
ratio = 1.0f - (float)g_pomoRemain / (float)(25 * 60);
if (ratio < 0) ratio = 0;
if (ratio > 1) ratio = 1;
} else if (!g_showSeconds) {
ratio = (float)st.wMinute / 60.0f;
}
int lineW = (int)((float)g_winWidth * ratio);
RECT bgRc = { 0, g_winHeight - 3, g_winWidth, g_winHeight };
FillRect(hdc, &bgRc, g_secBgBrush);
if (lineW > 0) {
RECT fgRc = { 0, g_winHeight - 3, lineW, g_winHeight };
FillRect(hdc, &fgRc, g_secFgBrush);
}
}

if (!g_cleanMode && g_winHeight >= 170) {
SelectObject(hdc, g_fontTip);
RECT tipRect = { 0, g_winHeight - 28, g_winWidth, g_winHeight - 6 };
SetTextColor(hdc, RGB(235, 225, 245));
int pct = (g_alpha * 100) / 255;
std::wstring tipBuf(128, L'\0');
if (g_lang == 0) {
snwprintf(&tipBuf.at(0), tipBuf.size(),
L"\u8bbe\u7f6e(A) | \u989d\u5916(W) | \u8ba1\u65f6(C) | \u900f\u660e\u5ea6 %d%% | \u7f6e\u9876:%ls(T) | \u53f3\u952e\u83dc\u5355",
pct, g_isTopmost ? L"\u5f00" : L"\u5173");
} else {
snwprintf(&tipBuf.at(0), tipBuf.size(),
L"A:Settings | W:Extra | C:Timer | Opacity %d%% | T:Top %ls | Right-Click Menu",
pct, g_isTopmost ? L"ON" : L"OFF");
}
DrawTextW(hdc, tipBuf.c_str(), -1, &tipRect, DT_CENTER | DT_SINGLELINE);
}
SelectObject(hdc, oldFont);
}
void RenderFrame() {
if (!g_memDC) return;
DrawDreamyBackground(g_memDC, g_hue);
for (int i = 0; i < PARTICLE_COUNT; ++i) {
const Particle& p = g_particles.at(i);
float drawX = p.x + sin(p.phase) * p.swayAmp;
if (p.isPetal) {
DrawSakuraPetal(g_memDC, drawX, p.y, p.size, p.angle, p.colorIndex);
} else {
DrawGlowingDot(g_memDC, drawX, p.y, p.size, p.phase, p.colorIndex);
}
}
DrawClockOverlay(g_memDC);
for (size_t i = 0; i < g_trailPetals.size(); ++i) {
const Particle& p = g_trailPetals.at(i);
float drawX = p.x + sin(p.phase) * p.swayAmp;
DrawSakuraPetal(g_memDC, drawX, p.y, p.size, p.angle, p.colorIndex);
}
}
// ==========================================
// 8. 通用 Win11 圆角卡片与开关绘制辅助
// ==========================================
void DrawWin11Card(HDC hdc, int x, int y, int w, int h, bool hovered) {
COLORREF bg = hovered ? RGB(48, 48, 54) : RGB(40, 40, 46);
COLORREF border = hovered ? RGB(82, 82, 92) : RGB(58, 58, 66);
HBRUSH hBrush = CreateSolidBrush(bg);
HPEN hPen = CreatePen(PS_SOLID, 1, border);
HBRUSH oldB = (HBRUSH)SelectObject(hdc, hBrush);
HPEN oldP = (HPEN)SelectObject(hdc, hPen);
RoundRect(hdc, x, y, x + w, y + h, 10, 10);
SelectObject(hdc, oldB);
SelectObject(hdc, oldP);
DeleteObject(hBrush);
DeleteObject(hPen);
}
void DrawWin11Toggle(HDC hdc, int x, int y, bool isOn) {
COLORREF trackBg = isOn ? RGB(235, 130, 175) : RGB(40, 40, 46);
COLORREF trackBorder = isOn ? RGB(235, 130, 175) : RGB(150, 150, 160);
COLORREF thumbColor = isOn ? RGB(25, 25, 30) : RGB(210, 210, 220);
HBRUSH hTrack = CreateSolidBrush(trackBg);
HPEN hBorder = CreatePen(PS_SOLID, 1, trackBorder);
HBRUSH oldB = (HBRUSH)SelectObject(hdc, hTrack);
HPEN oldP = (HPEN)SelectObject(hdc, hBorder);
RoundRect(hdc, x, y, x + 46, y + 22, 22, 22);
SelectObject(hdc, oldB);
SelectObject(hdc, oldP);
DeleteObject(hTrack);
DeleteObject(hBorder);
HBRUSH hThumb = CreateSolidBrush(thumbColor);
HPEN hNull = (HPEN)GetStockObject(NULL_PEN);
oldB = (HBRUSH)SelectObject(hdc, hThumb);
oldP = (HPEN)SelectObject(hdc, hNull);
int cx = isOn ? (x + 34) : (x + 12);
int cy = y + 11;
Ellipse(hdc, cx - 7, cy - 7, cx + 7, cy + 7);
SelectObject(hdc, oldB);
SelectObject(hdc, oldP);
DeleteObject(hThumb);
}
// ==========================================
// 9. 按 W 打开的"额外功能"窗口（含外部窗口置顶、图标与任务栏控制）
// ==========================================
const int EXT_W = 580;
const int EXT_H = 710;
const int VISIBLE_WIN_ROWS = 5;
bool IsHwndInHiddenList(HWND h) {
for (size_t i = 0; i < g_hiddenHwnds.size(); ++i) {
if (g_hiddenHwnds.at(i) == h) return true;
}
return false;
}
void AddHiddenHwnd(HWND h) {
if (!IsHwndInHiddenList(h)) {
g_hiddenHwnds.push_back(h);
}
}
void RemoveHiddenHwnd(HWND h) {
for (size_t i = 0; i < g_hiddenHwnds.size(); ) {
if (g_hiddenHwnds.at(i) == h) {
g_hiddenHwnds.erase(g_hiddenHwnds.begin() + i);
} else {
++i;
}
}
}
void RestoreAllHiddenWindows() {
for (size_t i = 0; i < g_hiddenHwnds.size(); ++i) {
HWND h = g_hiddenHwnds.at(i);
if (IsWindow(h)) {
ShowWindow(h, SW_SHOW);
}
}
g_hiddenHwnds.clear();
}
void ToggleDesktopIcons() {
HWND hProgman = FindWindowW(L"Progman", NULL);
HWND hDefView = FindWindowExW(hProgman, NULL, L"SHELLDLL_DefView", NULL);
if (!hDefView) {
HWND hWorker = NULL;
while ((hWorker = FindWindowExW(NULL, hWorker, L"WorkerW", NULL)) != NULL) {
hDefView = FindWindowExW(hWorker, NULL, L"SHELLDLL_DefView", NULL);
if (hDefView) break;
}
}
if (hDefView) {
g_desktopIconsHidden = !g_desktopIconsHidden;
ShowWindow(hDefView, g_desktopIconsHidden ? SW_HIDE : SW_SHOW);
}
}
void ToggleTaskbar() {
HWND hTaskbar = FindWindowW(L"Shell_TrayWnd", NULL);
if (hTaskbar) {
g_taskbarHidden = !g_taskbarHidden;
ShowWindow(hTaskbar, g_taskbarHidden ? SW_HIDE : SW_SHOW);
}
}
bool IsExternalWindowTopmost(HWND h) {
if (!IsWindow(h)) return false;
LONG_PTR exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
return (exStyle & WS_EX_TOPMOST) != 0;
}
void ToggleExternalWindowTopmost(HWND h) {
if (!IsWindow(h)) return;
bool isTop = IsExternalWindowTopmost(h);
SetWindowPos(h, isTop ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
int GetExternalWindowAlpha(HWND h) {
if (!IsWindow(h)) return 255;
LONG_PTR exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
if (!(exStyle & WS_EX_LAYERED)) return 255;
BYTE curA = 255;
DWORD flags = 0;
if (GetLayeredWindowAttributes(h, NULL, &curA, &flags)) {
if (flags & LWA_ALPHA) return (int)curA;
}
return 255;
}
void SetExternalWindowAlpha(HWND h, int alphaVal) {
if (!IsWindow(h)) return;
if (alphaVal < 50) alphaVal = 50;
if (alphaVal > 255) alphaVal = 255;
LONG_PTR exStyle = GetWindowLongPtrW(h, GWL_EXSTYLE);
if (alphaVal == 255) {
SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
SetWindowLongPtrW(h, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
RedrawWindow(h, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
} else {
if (!(exStyle & WS_EX_LAYERED)) {
SetWindowLongPtrW(h, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
}
SetLayeredWindowAttributes(h, 0, (BYTE)alphaVal, LWA_ALPHA);
}
}
BOOL CALLBACK EnumDesktopWindowsProc(HWND hwnd, LPARAM lParam) {
if (hwnd == g_hMainWnd || hwnd == g_hSettingsWnd || hwnd == g_hExtraWnd) return TRUE;
if (!IsWindowVisible(hwnd)) return TRUE;
int len = GetWindowTextLengthW(hwnd);
if (len <= 0) return TRUE;
LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
std::wstring titleBuf(len + 1, L'\0');
GetWindowTextW(hwnd, &titleBuf.at(0), len + 1);
titleBuf.resize(wcslen(titleBuf.c_str()));
if (titleBuf == L"Program Manager" || titleBuf == L"Windows Input Experience") return TRUE;
DesktopWinInfo info;
info.hwnd = hwnd;
info.title = titleBuf;
info.alphaVal = GetExternalWindowAlpha(hwnd);
info.isHidden = false;
info.isTopmost = IsExternalWindowTopmost(hwnd);
g_desktopWins.push_back(info);
return TRUE;
}
void RefreshDesktopWindowsList() {
g_desktopWins.clear();
for (size_t i = 0; i < g_hiddenHwnds.size(); ) {
HWND h = g_hiddenHwnds.at(i);
if (!IsWindow(h)) {
g_hiddenHwnds.erase(g_hiddenHwnds.begin() + i);
continue;
}
int len = GetWindowTextLengthW(h);
std::wstring titleBuf(len + 1, L'\0');
if (len > 0) {
GetWindowTextW(h, &titleBuf.at(0), len + 1);
titleBuf.resize(wcslen(titleBuf.c_str()));
} else {
titleBuf = L"(Untitled Window)";
}
DesktopWinInfo info;
info.hwnd = h;
info.title = titleBuf;
info.alphaVal = GetExternalWindowAlpha(h);
info.isHidden = true;
info.isTopmost = IsExternalWindowTopmost(h);
g_desktopWins.push_back(info);
++i;
}
EnumWindows(EnumDesktopWindowsProc, 0);
if (g_selectedWinIdx >= (int)g_desktopWins.size()) {
g_selectedWinIdx = g_desktopWins.empty() ? 0 : (int)g_desktopWins.size() - 1;
}
if (g_scrollOffset > g_selectedWinIdx) g_scrollOffset = g_selectedWinIdx;
if (g_scrollOffset < 0) g_scrollOffset = 0;
}
int HitTestExtra(int mx, int my) {
if (mx >= 526 && mx <= 564 && my >= 16 && my <= 48) return 1;
if (mx >= 434 && mx <= 534 && my >= 78 && my <= 110) return 2;
if (mx >= 40 && mx <= 502 && my >= 120 && my < 120 + VISIBLE_WIN_ROWS * 38) {
int row = (my - 120) / 38;
int idx = g_scrollOffset + row;
if (idx >= 0 && idx < (int)g_desktopWins.size()) {
return 100 + idx;
}
}
if (mx >= 508 && mx <= 538 && my >= 120 && my <= 210) return 3;
if (mx >= 508 && mx <= 538 && my >= 216 && my <= 306) return 4;
if (mx >= 26 && mx <= 554 && my >= 332 && my <= 416) return 5;
// 选中窗口 3 个操作按钮 (Y: 436..478)
if (my >= 436 && my <= 478) {
if (mx >= 40 && mx <= 200) return 6;  // 隐藏/显示
if (mx >= 210 && mx <= 370) return 11; // 置顶/取消置顶该窗口
if (mx >= 380 && mx <= 540) return 7;  // 恢复100%不透明
}
// 全局老板键按钮 (Y: 490..532)
if (my >= 490 && my <= 532) {
if (mx >= 40 && mx <= 284) return 8;
if (mx >= 296 && mx <= 540) return 9;
}
// 桌面图标与任务栏控制按钮 (Y: 544..586)
if (my >= 544 && my <= 586) {
if (mx >= 40 && mx <= 284) return 12; // 隐藏/显示桌面图标
if (mx >= 296 && mx <= 540) return 13; // 隐藏/显示任务栏
}
// 底部关闭按钮
if (mx >= 390 && mx <= 554 && my >= 654 && my <= 694) return 10;
return 0;
}
void UpdateExtraSliderFromMouse(int mx) {
if (g_desktopWins.empty() || g_selectedWinIdx < 0 || g_selectedWinIdx >= (int)g_desktopWins.size()) return;
int relX = mx - 42;
if (relX < 0) relX = 0;
if (relX > 496) relX = 496;
int val = 50 + (relX * (255 - 50)) / 496;
DesktopWinInfo& info = g_desktopWins.at(g_selectedWinIdx);
info.alphaVal = val;
SetExternalWindowAlpha(info.hwnd, val);
}
void DrawExtraUI(HDC hdc) {
HDC memDC = CreateCompatibleDC(hdc);
HBITMAP memBmp = CreateCompatibleBitmap(hdc, EXT_W, EXT_H);
HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);
HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 36));
RECT fullRc = { 0, 0, EXT_W, EXT_H };
FillRect(memDC, &fullRc, hBg);
DeleteObject(hBg);
HPEN hOuterPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 82));
HPEN oldPen = (HPEN)SelectObject(memDC, hOuterPen);
HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
Rectangle(memDC, 0, 0, EXT_W, EXT_H);
SelectObject(memDC, oldPen);
SelectObject(memDC, oldBrush);
DeleteObject(hOuterPen);
HFONT fTitle = g_uiFontTitle;
HFONT fSub = g_uiFontSub;
HFONT fDesc = g_uiFontDesc;
SetBkMode(memDC, TRANSPARENT);
HPEN hNull = (HPEN)GetStockObject(NULL_PEN);
HFONT oldF = (HFONT)SelectObject(memDC, fTitle);
SetTextColor(memDC, RGB(245, 245, 250));
RECT rcTitle = { 26, 16, 480, 52 };
DrawTextW(memDC,
g_lang == 0 ? L"\u989d\u5916\u529f\u80fd - \u684c\u9762\u4e0e\u7a97\u53e3\u63a7\u5236" : L"Extra Tools - Desktop & Windows",
-1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
if (g_extraHover == 1) {
HBRUSH hCloseHover = CreateSolidBrush(RGB(196, 43, 28));
RECT rcCloseBg = { 526, 16, 564, 48 };
FillRect(memDC, &rcCloseBg, hCloseHover);
DeleteObject(hCloseHover);
}
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(235, 235, 240));
RECT rcClose = { 526, 16, 564, 48 };
DrawTextW(memDC, L"X", -1, &rcClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
// 卡片 1：桌面窗口选择列表 (5行)
DrawWin11Card(memDC, 26, 68, 528, 252, false);
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcListT = { 42, 80, 410, 108 };
DrawTextW(memDC,
g_lang == 0 ? L"\u9009\u62e9\u684c\u9762\u7a97\u53e3 (\u70b9\u51fb\u9009\u4e2d)" : L"Select Desktop Window",
-1, &rcListT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
HBRUSH hRefBtn = CreateSolidBrush(g_extraHover == 2 ? RGB(72, 72, 84) : RGB(54, 54, 64));
SelectObject(memDC, hRefBtn);
SelectObject(memDC, hNull);
RoundRect(memDC, 434, 78, 534, 110, 8, 8);
DeleteObject(hRefBtn);
SelectObject(memDC, fDesc);
SetTextColor(memDC, RGB(235, 235, 245));
RECT rcRefTxt = { 434, 78, 534, 110 };
DrawTextW(memDC, g_lang == 0 ? L"\u5237\u65b0\u5217\u8868" : L"Refresh",
-1, &rcRefTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
for (int r = 0; r < VISIBLE_WIN_ROWS; ++r) {
int idx = g_scrollOffset + r;
int ry = 120 + r * 38;
if (idx >= (int)g_desktopWins.size()) break;
const DesktopWinInfo& info = g_desktopWins.at(idx);
bool isSel = (idx == g_selectedWinIdx);
bool isHov = (g_extraHover == 100 + idx);
COLORREF rowBg = isSel ? RGB(78, 62, 92) : (isHov ? RGB(54, 54, 62) : RGB(34, 34, 40));
COLORREF rowBd = isSel ? RGB(235, 130, 175) : RGB(50, 50, 58);
HBRUSH hRow = CreateSolidBrush(rowBg);
HPEN hRowPen = CreatePen(PS_SOLID, 1, rowBd);
SelectObject(memDC, hRow);
SelectObject(memDC, hRowPen);
RoundRect(memDC, 40, ry, 502, ry + 34, 6, 6);
DeleteObject(hRow);
DeleteObject(hRowPen);
std::wstring label;
if (info.isHidden) {
label += (g_lang == 0) ? L"[\u9690\u85cf] " : L"[HIDDEN] ";
}
if (info.isTopmost) {
label += (g_lang == 0) ? L"[\u7f6e\u9876] " : L"[TOP] ";
}
label += info.title;
SetTextColor(memDC, info.isHidden ? RGB(255, 165, 185) : (isSel ? RGB(255, 255, 255) : RGB(220, 220, 230)));
RECT rcItemTxt = { 50, ry, 435, ry + 34 };
DrawTextW(memDC, label.c_str(), -1, &rcItemTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
std::wstring alphaStr(16, L'\0');
snwprintf(&alphaStr.at(0), alphaStr.size(), L"%d%%", (info.alphaVal * 100) / 255);
SetTextColor(memDC, RGB(165, 210, 255));
RECT rcAlphaTxt = { 438, ry, 494, ry + 34 };
DrawTextW(memDC, alphaStr.c_str(), -1, &rcAlphaTxt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
HBRUSH hUpBtn = CreateSolidBrush(g_extraHover == 3 ? RGB(72, 72, 84) : RGB(52, 52, 60));
SelectObject(memDC, hUpBtn);
SelectObject(memDC, hNull);
RoundRect(memDC, 508, 120, 538, 210, 6, 6);
DeleteObject(hUpBtn);
RECT rcUp = { 508, 120, 538, 210 };
SetTextColor(memDC, RGB(230, 230, 240));
DrawTextW(memDC, L"\u25b2", -1, &rcUp, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
HBRUSH hDnBtn = CreateSolidBrush(g_extraHover == 4 ? RGB(72, 72, 84) : RGB(52, 52, 60));
SelectObject(memDC, hDnBtn);
RoundRect(memDC, 508, 216, 538, 306, 6, 6);
DeleteObject(hDnBtn);
RECT rcDn = { 508, 216, 538, 306 };
DrawTextW(memDC, L"\u25bc", -1, &rcDn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
// 卡片 2：修改选中窗口的透明度滑块
DrawWin11Card(memDC, 26, 332, 528, 84, g_extraHover == 5);
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcOpT = { 42, 344, 430, 370 };
DrawTextW(memDC,
g_lang == 0 ? L"\u4fee\u6539\u9009\u4e2d\u7a97\u53e3\u900f\u660e\u5ea6 (\u62d6\u52a8\u6ed1\u5757)" : L"Target Window Opacity",
-1, &rcOpT, DT_LEFT | DT_SINGLELINE);
int curTargetAlpha = 255;
bool curTargetHidden = false;
bool curTargetTop = false;
if (!g_desktopWins.empty() && g_selectedWinIdx >= 0 && g_selectedWinIdx < (int)g_desktopWins.size()) {
curTargetAlpha = g_desktopWins.at(g_selectedWinIdx).alphaVal;
curTargetHidden = g_desktopWins.at(g_selectedWinIdx).isHidden;
curTargetTop = g_desktopWins.at(g_selectedWinIdx).isTopmost;
}
std::wstring targetPct(16, L'\0');
snwprintf(&targetPct.at(0), targetPct.size(), L"%d%%", (curTargetAlpha * 100) / 255);
SetTextColor(memDC, RGB(240, 145, 185));
RECT rcOpV = { 440, 344, 538, 370 };
DrawTextW(memDC, targetPct.c_str(), -1, &rcOpV, DT_RIGHT | DT_SINGLELINE);
HBRUSH hTrackBg = CreateSolidBrush(RGB(68, 68, 78));
HBRUSH hTrackFill = CreateSolidBrush(RGB(235, 130, 175));
SelectObject(memDC, hNull);
int fillW = ((curTargetAlpha - 50) * 496) / (255 - 50);
if (fillW < 0) fillW = 0;
if (fillW > 496) fillW = 496;
SelectObject(memDC, hTrackBg);
RoundRect(memDC, 42, 388, 538, 395, 7, 7);
SelectObject(memDC, hTrackFill);
RoundRect(memDC, 42, 388, 42 + fillW, 395, 7, 7);
Ellipse(memDC, 42 + fillW - 8, 391 - 8, 42 + fillW + 8, 391 + 8);
DeleteObject(hTrackBg);
DeleteObject(hTrackFill);
// 选中窗口 3 个按钮：隐藏/恢复 | 置顶/取消置顶 | 重置100%透明
SelectObject(memDC, fSub);
COLORREF hideBtnBg = curTargetHidden
? (g_extraHover == 6 ? RGB(55, 155, 100) : RGB(45, 135, 85))
: (g_extraHover == 6 ? RGB(245, 145, 188) : RGB(225, 120, 165));
HBRUSH hHideBtn = CreateSolidBrush(hideBtnBg);
SelectObject(memDC, hHideBtn);
RoundRect(memDC, 40, 436, 200, 478, 8, 8);
DeleteObject(hHideBtn);
SetTextColor(memDC, curTargetHidden ? RGB(255, 255, 255) : RGB(25, 20, 30));
RECT rcHideTxt = { 40, 436, 200, 478 };
const wchar_t* hideLabel = (g_lang == 0)
? (curTargetHidden ? L"\u6062\u590d\u663e\u793a" : L"\u9690\u85cf\u7a97\u53e3")
: (curTargetHidden ? L"Show Win" : L"Hide Win");
DrawTextW(memDC, hideLabel, -1, &rcHideTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
COLORREF topBtnBg = curTargetTop
? (g_extraHover == 11 ? RGB(95, 145, 225) : RGB(75, 125, 205))
: (g_extraHover == 11 ? RGB(72, 72, 84) : RGB(54, 54, 64));
HBRUSH hTopBtn = CreateSolidBrush(topBtnBg);
SelectObject(memDC, hTopBtn);
RoundRect(memDC, 210, 436, 370, 478, 8, 8);
DeleteObject(hTopBtn);
SetTextColor(memDC, RGB(245, 245, 255));
RECT rcTopTxt = { 210, 436, 370, 478 };
const wchar_t* topLabel = (g_lang == 0)
? (curTargetTop ? L"\u53d6\u6d88\u7f6e\u9876" : L"\u7f6e\u9876\u8be5\u7a97\u53e3")
: (curTargetTop ? L"Unpin Top" : L"Pin Topmost");
DrawTextW(memDC, topLabel, -1, &rcTopTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
HBRUSH hResetBtn = CreateSolidBrush(g_extraHover == 7 ? RGB(72, 72, 84) : RGB(54, 54, 64));
SelectObject(memDC, hResetBtn);
RoundRect(memDC, 380, 436, 540, 478, 8, 8);
DeleteObject(hResetBtn);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcResetTxt = { 380, 436, 540, 478 };
DrawTextW(memDC, g_lang == 0 ? L"\u900f\u660e\u5ea6 100%" : L"Opacity 100%",
-1, &rcResetTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
// 全局老板键按钮：一键隐藏所有其他窗口 / 一键恢复全部隐藏窗口
HBRUSH hBossHide = CreateSolidBrush(g_extraHover == 8 ? RGB(72, 72, 84) : RGB(54, 54, 64));
SelectObject(memDC, hBossHide);
RoundRect(memDC, 40, 490, 284, 532, 8, 8);
DeleteObject(hBossHide);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcBossHide = { 40, 490, 284, 532 };
DrawTextW(memDC,
g_lang == 0 ? L"\u4e00\u952e\u9690\u85cf\u5176\u4ed6\u6240\u6709\u7a97\u53e3" : L"Hide All Other Windows",
-1, &rcBossHide, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
HBRUSH hBossShow = CreateSolidBrush(g_extraHover == 9 ? RGB(72, 72, 84) : RGB(54, 54, 64));
SelectObject(memDC, hBossShow);
RoundRect(memDC, 296, 490, 540, 532, 8, 8);
DeleteObject(hBossShow);
SetTextColor(memDC, RGB(165, 220, 255));
RECT rcBossShow = { 296, 490, 540, 532 };
DrawTextW(memDC,
g_lang == 0 ? L"\u4e00\u952e\u6062\u590d\u5168\u90e8\u9690\u85cf\u7a97\u53e3" : L"Restore All Hidden",
-1, &rcBossShow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
// 系统桌面控制按钮：隐藏/显示桌面图标 & 隐藏/显示任务栏
COLORREF iconBtnBg = g_desktopIconsHidden
? (g_extraHover == 12 ? RGB(55, 155, 100) : RGB(45, 135, 85))
: (g_extraHover == 12 ? RGB(72, 72, 84) : RGB(54, 54, 64));
HBRUSH hIconBtn = CreateSolidBrush(iconBtnBg);
SelectObject(memDC, hIconBtn);
RoundRect(memDC, 40, 544, 284, 586, 8, 8);
DeleteObject(hIconBtn);
SetTextColor(memDC, RGB(245, 245, 250));
RECT rcIconTxt = { 40, 544, 284, 586 };
const wchar_t* iconLabel = (g_lang == 0)
? (g_desktopIconsHidden ? L"\u6062\u590d\u684c\u9762\u56fe\u6807" : L"\u9690\u85cf\u684c\u9762\u56fe\u6807")
: (g_desktopIconsHidden ? L"Show Desktop Icons" : L"Hide Desktop Icons");
DrawTextW(memDC, iconLabel, -1, &rcIconTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
COLORREF tbBtnBg = g_taskbarHidden
? (g_extraHover == 13 ? RGB(55, 155, 100) : RGB(45, 135, 85))
: (g_extraHover == 13 ? RGB(72, 72, 84) : RGB(54, 54, 64));
HBRUSH hTbBtn = CreateSolidBrush(tbBtnBg);
SelectObject(memDC, hTbBtn);
RoundRect(memDC, 296, 544, 540, 586, 8, 8);
DeleteObject(hTbBtn);
SetTextColor(memDC, RGB(245, 245, 250));
RECT rcTbTxt = { 296, 544, 540, 586 };
const wchar_t* tbLabel = (g_lang == 0)
? (g_taskbarHidden ? L"\u6062\u590d\u7cfb\u7edf\u4efb\u52a1\u680f" : L"\u9690\u85cf\u7cfb\u7edf\u4efb\u52a1\u680f")
: (g_taskbarHidden ? L"Show Taskbar" : L"Hide Taskbar");
DrawTextW(memDC, tbLabel, -1, &rcTbTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
// 底部提示与关闭按钮
SelectObject(memDC, fDesc);
SetTextColor(memDC, RGB(155, 155, 165));
RECT rcSafeNote = { 26, 654, 380, 694 };
DrawTextW(memDC,
g_lang == 0 ? L"\u9000\u51fa\u7a0b\u5e8f\u65f6\u5c06\u81ea\u52a8\u6062\u590d\u6240\u6709\u9690\u85cf\u9879" : L"All hidden items auto-restore on exit",
-1, &rcSafeNote, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
HBRUSH hCloseBtn = CreateSolidBrush(g_extraHover == 10 ? RGB(66, 66, 76) : RGB(52, 52, 60));
HPEN hClosePen = CreatePen(PS_SOLID, 1, RGB(80, 80, 92));
SelectObject(memDC, hCloseBtn);
SelectObject(memDC, hClosePen);
RoundRect(memDC, 390, 654, 554, 694, 8, 8);
DeleteObject(hCloseBtn);
DeleteObject(hClosePen);
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(235, 235, 240));
RECT rcCloseBtnTxt = { 390, 654, 554, 694 };
DrawTextW(memDC, g_lang == 0 ? L"\u5173\u95ed" : L"Close",
-1, &rcCloseBtnTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
BitBlt(hdc, 0, 0, EXT_W, EXT_H, memDC, 0, 0, SRCCOPY);
SelectObject(memDC, oldF);
SelectObject(memDC, oldBmp);
DeleteObject(memBmp);
DeleteDC(memDC);
}
LRESULT CALLBACK ExtraWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
switch (msg) {
case WM_ERASEBKGND:
return 1;
case WM_PAINT: {
PAINTSTRUCT ps;
HDC hdc = BeginPaint(hwnd, &ps);
DrawExtraUI(hdc);
EndPaint(hwnd, &ps);
return 0;
}
case WM_MOUSEWHEEL: {
int delta = GET_WHEEL_DELTA_WPARAM(wParam);
if (delta > 0 && g_scrollOffset > 0) {
g_scrollOffset--;
InvalidateRect(hwnd, NULL, FALSE);
} else if (delta < 0 && g_scrollOffset + VISIBLE_WIN_ROWS < (int)g_desktopWins.size()) {
g_scrollOffset++;
InvalidateRect(hwnd, NULL, FALSE);
}
return 0;
}
case WM_MOUSEMOVE: {
int mx = (int)(short)LOWORD(lParam);
int my = (int)(short)HIWORD(lParam);
if ((wParam & MK_LBUTTON) && g_extraHover == 5) {
UpdateExtraSliderFromMouse(mx);
InvalidateRect(hwnd, NULL, FALSE);
return 0;
}
int hit = HitTestExtra(mx, my);
if (hit != g_extraHover) {
g_extraHover = hit;
InvalidateRect(hwnd, NULL, FALSE);
}
return 0;
}
case WM_LBUTTONDOWN: {
int mx = (int)(short)LOWORD(lParam);
int my = (int)(short)HIWORD(lParam);
int hit = HitTestExtra(mx, my);
if (hit == 0 && my < 66) {
ReleaseCapture();
SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
return 0;
}
if (hit == 1 || hit == 10) {
DestroyWindow(hwnd);
} else if (hit == 2) {
RefreshDesktopWindowsList();
} else if (hit == 3) {
if (g_scrollOffset > 0) g_scrollOffset--;
} else if (hit == 4) {
if (g_scrollOffset + VISIBLE_WIN_ROWS < (int)g_desktopWins.size()) g_scrollOffset++;
} else if (hit >= 100) {
g_selectedWinIdx = hit - 100;
} else if (hit == 5) {
g_extraHover = 5;
SetCapture(hwnd);
UpdateExtraSliderFromMouse(mx);
} else if (hit == 6) {
if (!g_desktopWins.empty() && g_selectedWinIdx >= 0 && g_selectedWinIdx < (int)g_desktopWins.size()) {
DesktopWinInfo& info = g_desktopWins.at(g_selectedWinIdx);
if (IsWindow(info.hwnd)) {
if (info.isHidden) {
ShowWindow(info.hwnd, SW_SHOW);
RemoveHiddenHwnd(info.hwnd);
info.isHidden = false;
} else {
ShowWindow(info.hwnd, SW_HIDE);
AddHiddenHwnd(info.hwnd);
info.isHidden = true;
}
} else {
RefreshDesktopWindowsList();
}
}
} else if (hit == 11) {
if (!g_desktopWins.empty() && g_selectedWinIdx >= 0 && g_selectedWinIdx < (int)g_desktopWins.size()) {
DesktopWinInfo& info = g_desktopWins.at(g_selectedWinIdx);
if (IsWindow(info.hwnd)) {
ToggleExternalWindowTopmost(info.hwnd);
info.isTopmost = IsExternalWindowTopmost(info.hwnd);
SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
} else {
RefreshDesktopWindowsList();
}
}
} else if (hit == 7) {
if (!g_desktopWins.empty() && g_selectedWinIdx >= 0 && g_selectedWinIdx < (int)g_desktopWins.size()) {
DesktopWinInfo& info = g_desktopWins.at(g_selectedWinIdx);
info.alphaVal = 255;
SetExternalWindowAlpha(info.hwnd, 255);
}
} else if (hit == 8) {
for (size_t i = 0; i < g_desktopWins.size(); ++i) {
DesktopWinInfo& info = g_desktopWins.at(i);
if (!info.isHidden && IsWindow(info.hwnd)) {
ShowWindow(info.hwnd, SW_HIDE);
AddHiddenHwnd(info.hwnd);
info.isHidden = true;
}
}
} else if (hit == 9) {
RestoreAllHiddenWindows();
RefreshDesktopWindowsList();
} else if (hit == 12) {
ToggleDesktopIcons();
} else if (hit == 13) {
ToggleTaskbar();
}
InvalidateRect(hwnd, NULL, FALSE);
return 0;
}
case WM_LBUTTONUP: {
if (GetCapture() == hwnd) {
ReleaseCapture();
}
return 0;
}
case WM_KEYDOWN: {
if (wParam == VK_ESCAPE || wParam == 'W' || wParam == 'w') {
DestroyWindow(hwnd);
}
return 0;
}
case WM_DESTROY: {
g_hExtraWnd = NULL;
g_extraHover = -1;
return 0;
}
}
return DefWindowProcW(hwnd, msg, wParam, lParam);
}
void OpenExtraToolsWindow(HWND parentHwnd) {
if (g_hExtraWnd && IsWindow(g_hExtraWnd)) {
SetForegroundWindow(g_hExtraWnd);
return;
}
RefreshDesktopWindowsList();
static bool registered = false;
const wchar_t EXT_CLASS[] = L"Win11SakuraExtraClass";
if (!registered) {
WNDCLASSEXW wc;
ZeroMemory(&wc, sizeof(WNDCLASSEXW));
wc.cbSize = sizeof(WNDCLASSEXW);
wc.lpfnWndProc = ExtraWndProc;
wc.hInstance = GetModuleHandleW(NULL);
wc.hCursor = LoadCursor(NULL, IDC_ARROW);
wc.lpszClassName = EXT_CLASS;
if (RegisterClassExW(&wc)) {
registered = true;
}
}
// 居中到鼠标所在显示器的工作区
POINT cp;
GetCursorPos(&cp);
HMONITOR hMon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
MONITORINFO mi;
mi.cbSize = sizeof(MONITORINFO);
RECT wa;
if (hMon && GetMonitorInfoW(hMon, &mi)) wa = mi.rcWork;
else { wa.left = 0; wa.top = 0; wa.right = GetSystemMetrics(SM_CXSCREEN); wa.bottom = GetSystemMetrics(SM_CYSCREEN); }
int sx = wa.left + ((wa.right - wa.left) - EXT_W) / 2;
int sy = wa.top + ((wa.bottom - wa.top) - EXT_H) / 2;
if (sy < wa.top) sy = wa.top;
g_hExtraWnd = CreateWindowExW(
WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
EXT_CLASS,
L"Extra Tools",
WS_POPUP | WS_VISIBLE,
sx, sy, EXT_W, EXT_H,
parentHwnd, NULL, GetModuleHandleW(NULL), NULL
);
if (g_hExtraWnd) {
ShowWindow(g_hExtraWnd, SW_SHOW);
UpdateWindow(g_hExtraWnd);
}
}
// ==========================================
// 10. 按 A 打开的"个性化与设置"窗口 (含动态变色高亮作者: LXT)
// ==========================================
const int SET_W = 560;
const int SET_H = 820;

int HitTestSettings(int mx, int my) {
    if (mx >= 506 && mx <= 544 && my >= 16 && my <= 48) return 1;

    if (my >= 84 && my <= 122) {
        if (mx >= 330 && mx <= 420) return 20;
        if (mx >= 430 && mx <= 520) return 21;
    }

    if (mx >= 26 && mx <= 534 && my >= 146 && my <= 224) return 2;
    if (mx >= 26 && mx <= 534 && my >= 234 && my <= 312) return 3;

    // 6 个色系胶囊：x = 32 + i*84，宽 80
    if (my >= 364 && my <= 402) {
        for (int i = 0; i < 6; ++i) {
            int bx = 32 + i * 84;
            if (mx >= bx && mx <= bx + 80) return 10 + i;
        }
    }

    if (mx >= 26 && mx <= 534 && my >= 426 && my <= 486) return 4;
    if (mx >= 26 && mx <= 534 && my >= 496 && my <= 556) return 5;

    // 新增 2x2 开关网格
    if (my >= 566 && my <= 626) {
        if (mx >= 26 && mx <= 268) return 30;
        if (mx >= 292 && mx <= 534) return 31;
    }
    if (my >= 636 && my <= 696) {
        if (mx >= 26 && mx <= 268) return 32;
        if (mx >= 292 && mx <= 534) return 33;
    }

    if (mx >= 26 && mx <= 346 && my >= 716 && my <= 764) return 6;
    if (mx >= 360 && mx <= 534 && my >= 716 && my <= 764) return 7;

    return 0;
}

void UpdateSliderFromMouse(int sliderId, int mx) {
    g_alphaTarget = -1;   // 手动拖动时取消动画目标
    int relX = mx - 42;
    if (relX < 0) relX = 0;
    if (relX > 476) relX = 476;

    int val = 80 + (relX * (255 - 80)) / 476;
    if (sliderId == 2) {
        ApplyWindowAlpha(g_hMainWnd, val);
    } else if (sliderId == 3) {
        g_defaultAlpha = val;
        ApplyWindowAlpha(g_hMainWnd, val);
    }
}

void DrawSettingsUI(HDC hdc) {
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, SET_W, SET_H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 36));
    RECT fullRc = { 0, 0, SET_W, SET_H };
    FillRect(memDC, &fullRc, hBg);
    DeleteObject(hBg);

    HPEN hOuterPen = CreatePen(PS_SOLID, 1, RGB(70, 70, 82));
    HPEN oldPen = (HPEN)SelectObject(memDC, hOuterPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
    Rectangle(memDC, 0, 0, SET_W, SET_H);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBrush);
    DeleteObject(hOuterPen);

    HFONT fTitle = g_uiFontTitle;
    HFONT fSub = g_uiFontSub;
    HFONT fDesc = g_uiFontDesc;

    SetBkMode(memDC, TRANSPARENT);
    HPEN hNull = (HPEN)GetStockObject(NULL_PEN);

    // 1. 顶部标题栏
    HFONT oldF = (HFONT)SelectObject(memDC, fTitle);
    SetTextColor(memDC, RGB(245, 245, 250));
    RECT rcTitle = { 26, 16, 330, 52 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u4e2a\u6027\u5316\u4e0e\u8bbe\u7f6e" : L"Personalization & Settings",
              -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // ===== 动态变色高亮的“作者: LXT”流光徽标 =====
    BYTE authR, authG, authB;
    HsvToRgb(g_hue * 2.2f, 0.72f, 1.0f, authR, authG, authB);
    COLORREF neonColor = RGB(authR, authG, authB);

    HBRUSH hBadgeBg = CreateSolidBrush(RGB(44, 42, 54));
    HPEN hBadgePen = CreatePen(PS_SOLID, 1, neonColor);
    SelectObject(memDC, hBadgeBg);
    SelectObject(memDC, hBadgePen);
    RoundRect(memDC, 345, 18, 492, 48, 14, 14);
    DeleteObject(hBadgeBg);
    DeleteObject(hBadgePen);

    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(20, 15, 28));
    RECT rcAuthShadow = { 346, 19, 493, 49 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u4f5c\u8005: LXT" : L"Author: LXT",
              -1, &rcAuthShadow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(memDC, neonColor);
    RECT rcAuth = { 345, 18, 492, 48 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u4f5c\u8005: LXT" : L"Author: LXT",
              -1, &rcAuth, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    // ===============================================

    if (g_hoverItem == 1) {
        HBRUSH hCloseHover = CreateSolidBrush(RGB(196, 43, 28));
        RECT rcCloseBg = { 506, 16, 544, 48 };
        FillRect(memDC, &rcCloseBg, hCloseHover);
        DeleteObject(hCloseHover);
    }
    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(235, 235, 240));
    RECT rcClose = { 506, 16, 544, 48 };
    DrawTextW(memDC, L"X", -1, &rcClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 卡片 0：界面语言
    DrawWin11Card(memDC, 26, 72, 508, 62, g_hoverItem == 20 || g_hoverItem == 21);
    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(240, 240, 245));
    RECT rcLangT = { 42, 80, 315, 104 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u754c\u9762\u8bed\u8a00 (Language)" : L"Display Language",
              -1, &rcLangT, DT_LEFT | DT_SINGLELINE);

    SelectObject(memDC, fDesc);
    SetTextColor(memDC, RGB(170, 170, 180));
    RECT rcLangD = { 42, 105, 320, 126 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u5207\u6362\u65f6\u949f\u4e0e\u8bbe\u7f6e\u9762\u677f\u663e\u793a\u8bed\u8a00" : L"Switch UI & clock display language",
              -1, &rcLangD, DT_LEFT | DT_SINGLELINE);

    for (int i = 0; i < 2; ++i) {
        int lx = 330 + i * 100;
        int ly = 86;
        bool sel = (g_lang == i);
        bool hov = (g_hoverItem == 20 + i);
        COLORREF lBg = sel ? RGB(72, 62, 86) : (hov ? RGB(56, 56, 64) : RGB(48, 48, 56));
        COLORREF lBd = sel ? RGB(235, 130, 175) : RGB(75, 75, 85);

        HBRUSH hLBtn = CreateSolidBrush(lBg);
        HPEN hLPen = CreatePen(PS_SOLID, sel ? 2 : 1, lBd);
        SelectObject(memDC, hLBtn);
        SelectObject(memDC, hLPen);
        RoundRect(memDC, lx, ly, lx + 90, ly + 34, 8, 8);
        DeleteObject(hLBtn);
        DeleteObject(hLPen);

        SetTextColor(memDC, sel ? RGB(255, 255, 255) : RGB(215, 215, 225));
        RECT rcLTxt = { lx, ly, lx + 90, ly + 34 };
        DrawTextW(memDC, i == 0 ? L"\u7b80\u4f53\u4e2d\u6587" : L"English",
                  -1, &rcLTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 卡片 1：实时透明度
    DrawWin11Card(memDC, 26, 146, 508, 78, g_hoverItem == 2);
    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(240, 240, 245));
    RECT rcC1T = { 42, 156, 410, 182 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u5b9e\u65f6\u7a97\u53e3\u900f\u660e\u5ea6 (\u652f\u6301\u9f20\u6807\u6eda\u8f6e)" : L"Live Window Opacity (Mouse Wheel)",
              -1, &rcC1T, DT_LEFT | DT_SINGLELINE);

    std::wstring pct1(16, L'\0');
    snwprintf(&pct1.at(0), pct1.size(), L"%d%%", (g_alpha * 100) / 255);
    SetTextColor(memDC, RGB(240, 145, 185));
    RECT rcC1V = { 420, 156, 518, 182 };
    DrawTextW(memDC, pct1.c_str(), -1, &rcC1V, DT_RIGHT | DT_SINGLELINE);

    HBRUSH hTrackBg = CreateSolidBrush(RGB(68, 68, 78));
    HBRUSH hTrackFill = CreateSolidBrush(RGB(235, 130, 175));
    SelectObject(memDC, hNull);

    int fillW1 = ((g_alpha - 80) * 476) / (255 - 80);
    SelectObject(memDC, hTrackBg);
    RoundRect(memDC, 42, 198, 518, 205, 7, 7);
    SelectObject(memDC, hTrackFill);
    RoundRect(memDC, 42, 198, 42 + fillW1, 205, 7, 7);
    Ellipse(memDC, 42 + fillW1 - 8, 201 - 8, 42 + fillW1 + 8, 201 + 8);

    // 卡片 2：默认启动透明度
    DrawWin11Card(memDC, 26, 234, 508, 78, g_hoverItem == 3);
    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(240, 240, 245));
    RECT rcC2T = { 42, 244, 410, 270 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u9ed8\u8ba4\u542f\u52a8\u900f\u660e\u5ea6" : L"Default Startup Opacity",
              -1, &rcC2T, DT_LEFT | DT_SINGLELINE);

    std::wstring pct2(16, L'\0');
    snwprintf(&pct2.at(0), pct2.size(), L"%d%%", (g_defaultAlpha * 100) / 255);
    SetTextColor(memDC, RGB(160, 210, 255));
    RECT rcC2V = { 420, 244, 518, 270 };
    DrawTextW(memDC, pct2.c_str(), -1, &rcC2V, DT_RIGHT | DT_SINGLELINE);

    int fillW2 = ((g_defaultAlpha - 80) * 476) / (255 - 80);
    HBRUSH hTrackFill2 = CreateSolidBrush(RGB(130, 190, 250));
    SelectObject(memDC, hTrackBg);
    RoundRect(memDC, 42, 286, 518, 293, 7, 7);
    SelectObject(memDC, hTrackFill2);
    RoundRect(memDC, 42, 286, 42 + fillW2, 293, 7, 7);
    Ellipse(memDC, 42 + fillW2 - 8, 289 - 8, 42 + fillW2 + 8, 289 + 8);

    DeleteObject(hTrackBg);
    DeleteObject(hTrackFill);
    DeleteObject(hTrackFill2);

    // 卡片 3：自定义默认色系
    DrawWin11Card(memDC, 26, 322, 508, 94, g_hoverItem >= 10 && g_hoverItem <= 14);
    SelectObject(memDC, fSub);
    SetTextColor(memDC, RGB(240, 240, 245));
    RECT rcC3T = { 42, 332, 460, 358 };
    DrawTextW(memDC,
              g_lang == 0 ? L"\u9ed8\u8ba4\u68a6\u5e7b\u8272\u7cfb" : L"Default Color Theme",
              -1, &rcC3T, DT_LEFT | DT_SINGLELINE);

    SelectObject(memDC, fDesc);
    {
        struct ThemeDef { COLORREF dot; const wchar_t* zh; const wchar_t* en; };
        static const ThemeDef themes[6] = {
            { RGB(180, 140, 255), L"\u6781\u5149", L"Aurora" },
            { RGB(255, 165, 200), L"\u6a31\u7c89", L"Sakura" },
            { RGB(165, 110, 245), L"\u5e7b\u7d2b", L"Violet" },
            { RGB(110, 220, 215), L"\u8584\u8377", L"Ocean"  },
            { RGB(250, 175, 115), L"\u843d\u65e5", L"Sunset" },
            { RGB(120, 150, 255), L"\u6781\u591c", L"Polar"  }
        };
        for (int i = 0; i < 6; ++i) {
            int bx = 32 + i * 84;
            int by = 366;
            bool selected = (g_colorTheme == i);
            bool hovered = (g_hoverItem == 10 + i);

            COLORREF pillBg = selected ? RGB(72, 62, 86) : (hovered ? RGB(56, 56, 64) : RGB(48, 48, 56));
            COLORREF pillBorder = selected ? RGB(235, 130, 175) : RGB(75, 75, 85);

            HBRUSH hPill = CreateSolidBrush(pillBg);
            HPEN hPillPen = CreatePen(PS_SOLID, selected ? 2 : 1, pillBorder);
            SelectObject(memDC, hPill);
            SelectObject(memDC, hPillPen);
            RoundRect(memDC, bx, by, bx + 80, by + 34, 8, 8);
            DeleteObject(hPill);
            DeleteObject(hPillPen);

            HBRUSH hDot = CreateSolidBrush(themes[i].dot);
            SelectObject(memDC, hDot);
            SelectObject(memDC, hNull);
            Ellipse(memDC, bx + 8, by + 13, bx + 17, by + 22);
            DeleteObject(hDot);

            SelectObject(memDC, fDesc);
            SetTextColor(memDC, selected ? RGB(255, 255, 255) : RGB(215, 215, 225));
            RECT rcPillTxt = { bx + 21, by, bx + 78, by + 34 };
            DrawTextW(memDC, (g_lang == 0) ? themes[i].zh : themes[i].en,
                      -1, &rcPillTxt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
// 卡片 4：始终置顶
DrawWin11Card(memDC, 26, 426, 508, 60, g_hoverItem == 4);
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcC4T = { 42, 434, 440, 458 };
DrawTextW(memDC,
g_lang == 0 ? L"\u59cb\u7ec8\u7f6e\u9876\u663e\u793a (\u5feb\u6377\u952e: T)" : L"Always on Top (Shortcut: T)",
-1, &rcC4T, DT_LEFT | DT_SINGLELINE);
SelectObject(memDC, fDesc);
SetTextColor(memDC, RGB(170, 170, 180));
RECT rcC4D = { 42, 459, 440, 480 };
DrawTextW(memDC,
g_lang == 0 ? L"\u8ba9\u65f6\u949f\u59cb\u7ec8\u60ac\u6d6e\u5728\u5176\u4ed6\u7a97\u53e3\u6700\u4e0a\u5c42" : L"Keep clock floating above other windows",
-1, &rcC4D, DT_LEFT | DT_SINGLELINE);
DrawWin11Toggle(memDC, 472, 445, g_isTopmost);
// 卡片 5：纯净模式
DrawWin11Card(memDC, 26, 496, 508, 60, g_hoverItem == 5);
SelectObject(memDC, fSub);
SetTextColor(memDC, RGB(240, 240, 245));
RECT rcC5T = { 42, 504, 440, 528 };
DrawTextW(memDC,
g_lang == 0 ? L"\u7eaf\u51c0\u65e0\u6253\u6270\u6a21\u5f0f (\u53cc\u51fb\u7a97\u53e3)" : L"Clean Mode (Double-Click)",
-1, &rcC5T, DT_LEFT | DT_SINGLELINE);
SelectObject(memDC, fDesc);
SetTextColor(memDC, RGB(170, 170, 180));
RECT rcC5D = { 42, 529, 440, 550 };
DrawTextW(memDC,
g_lang == 0 ? L"\u9690\u85cf\u5e95\u90e8\u5feb\u6377\u952e\u63d0\u793a\u680f" : L"Hide bottom shortcut hint bar",
-1, &rcC5D, DT_LEFT | DT_SINGLELINE);
DrawWin11Toggle(memDC, 472, 515, g_cleanMode);

// ===== 新增：系统集成开关 2x2 =====
{
    struct TCard { int x, y, id; bool on; const wchar_t* zhT; const wchar_t* enT;
                   const wchar_t* zhD; const wchar_t* enD; };
    TCard tcards[4] = {
        { 26, 566, 30, g_autoStart,
          L"\u5f00\u673a\u81ea\u542f", L"Run at Startup",
          L"\u767b\u5f55\u540e\u81ea\u52a8\u542f\u52a8\u65f6\u949f", L"Launch with Windows" },
        { 292, 566, 31, g_chimeOn,
          L"\u6574\u70b9\u62a5\u65f6", L"Hourly Chime",
          L"\u6574\u70b9\u67d4\u548c\u63d0\u793a\u97f3", L"Soft beep each hour" },
        { 26, 636, 32, g_clickThrough,
          L"\u9f20\u6807\u7a7f\u900f", L"Click-Through",
          L"\u70b9\u51fb\u7a7f\u900f\u65f6\u949f (Ctrl+Alt+P)", L"Clicks pass through (Ctrl+Alt+P)" },
        { 292, 636, 33, g_autoTheme,
          L"\u81ea\u52a8\u53d8\u8272", L"Auto Theme",
          L"\u8272\u7cfb\u968f\u663c\u591c\u53d8\u5316", L"Theme follows time of day" }
    };
    for (int i = 0; i < 4; ++i) {
        TCard& c = tcards[i];
        DrawWin11Card(memDC, c.x, c.y, 242, 60, g_hoverItem == c.id);
        SelectObject(memDC, fSub);
        SetTextColor(memDC, RGB(240, 240, 245));
        RECT rcT = { c.x + 14, c.y + 6, c.x + 176, c.y + 30 };
        DrawTextW(memDC, (g_lang == 0) ? c.zhT : c.enT, -1, &rcT,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(memDC, fDesc);
        SetTextColor(memDC, RGB(170, 170, 180));
        RECT rcD = { c.x + 14, c.y + 32, c.x + 186, c.y + 54 };
        DrawTextW(memDC, (g_lang == 0) ? c.zhD : c.enD, -1, &rcD,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawWin11Toggle(memDC, c.x + 242 - 46 - 12, c.y + 19, c.on);
    }
}
// 底部按钮
bool showSaved = g_savedToast && (GetTickCount() - g_savedToastTick < 2000);
COLORREF saveBg = showSaved ? RGB(45, 135, 85) : (g_hoverItem == 6 ? RGB(245, 145, 188) : RGB(225, 120, 165));
HBRUSH hSaveBtn = CreateSolidBrush(saveBg);
SelectObject(memDC, hSaveBtn);
SelectObject(memDC, hNull);
RoundRect(memDC, 26, 716, 346, 764, 8, 8);
DeleteObject(hSaveBtn);
SelectObject(memDC, fSub);
SetTextColor(memDC, showSaved ? RGB(255, 255, 255) : RGB(25, 20, 30));
RECT rcSaveTxt = { 26, 716, 346, 764 };
const wchar_t* saveLabel = L"Save as Startup Default";
if (g_lang == 0) {
saveLabel = showSaved ? L"\u5df2\u4fdd\u5b58\u81f3 sakura_clock.ini!" : L"\u4fdd\u5b58\u4e3a\u9ed8\u8ba4\u542f\u52a8\u9879";
} else {
saveLabel = showSaved ? L"Saved to sakura_clock.ini!" : L"Save as Startup Default";
}
DrawTextW(memDC, saveLabel, -1, &rcSaveTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
HBRUSH hCloseBtn = CreateSolidBrush(g_hoverItem == 7 ? RGB(66, 66, 76) : RGB(52, 52, 60));
HPEN hClosePen = CreatePen(PS_SOLID, 1, RGB(80, 80, 92));
SelectObject(memDC, hCloseBtn);
SelectObject(memDC, hClosePen);
RoundRect(memDC, 360, 716, 534, 764, 8, 8);
DeleteObject(hCloseBtn);
DeleteObject(hClosePen);
SetTextColor(memDC, RGB(235, 235, 240));
RECT rcCloseBtnTxt = { 360, 716, 534, 764 };
DrawTextW(memDC, g_lang == 0 ? L"\u5173\u95ed" : L"Close",
-1, &rcCloseBtnTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

// 底部全局热键提示
SelectObject(memDC, g_uiFontDesc);
SetTextColor(memDC, RGB(150, 150, 160));
RECT rcHotkeyHint = { 26, 776, 534, 802 };
DrawTextW(memDC,
g_lang == 0
? L"\u5168\u5c40\u70ed\u952e\uff1aCtrl+Alt+A \u8bbe\u7f6e  \u00b7  Ctrl+Alt+W \u989d\u5916  \u00b7  Ctrl+Alt+T \u7f6e\u9876  \u00b7  Ctrl+Alt+P \u7a7f\u900f"
: L"Hotkeys: Ctrl+Alt+A Settings \u00b7 Ctrl+Alt+W Extra \u00b7 Ctrl+Alt+T Top \u00b7 Ctrl+Alt+P Through",
-1, &rcHotkeyHint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
BitBlt(hdc, 0, 0, SET_W, SET_H, memDC, 0, 0, SRCCOPY);
SelectObject(memDC, oldF);
SelectObject(memDC, oldBmp);
DeleteObject(memBmp);
DeleteDC(memDC);
}
LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
switch (msg) {
case WM_ERASEBKGND:
return 1;
case WM_PAINT: {
PAINTSTRUCT ps;
HDC hdc = BeginPaint(hwnd, &ps);
DrawSettingsUI(hdc);
EndPaint(hwnd, &ps);
return 0;
}
case WM_MOUSEMOVE: {
int mx = (int)(short)LOWORD(lParam);
int my = (int)(short)HIWORD(lParam);
if (wParam & MK_LBUTTON) {
if (g_hoverItem == 2 || g_hoverItem == 3) {
UpdateSliderFromMouse(g_hoverItem, mx);
InvalidateRect(hwnd, NULL, FALSE);
if (g_hMainWnd) {
RenderFrame();
InvalidateRect(g_hMainWnd, NULL, FALSE);
}
return 0;
}
}
int hit = HitTestSettings(mx, my);
if (hit != g_hoverItem) {
g_hoverItem = hit;
InvalidateRect(hwnd, NULL, FALSE);
}
return 0;
}
case WM_LBUTTONDOWN: {
int mx = (int)(short)LOWORD(lParam);
int my = (int)(short)HIWORD(lParam);
int hit = HitTestSettings(mx, my);
if (hit == 0 && my < 66) {
ReleaseCapture();
SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
return 0;
}
if (hit == 1 || hit == 7) {
DestroyWindow(hwnd);
} else if (hit == 20 || hit == 21) {
g_lang = hit - 20;
if (g_hExtraWnd && IsWindow(g_hExtraWnd)) {
InvalidateRect(g_hExtraWnd, NULL, FALSE);
}
} else if (hit == 2 || hit == 3) {
g_hoverItem = hit;
SetCapture(hwnd);
UpdateSliderFromMouse(hit, mx);
} else if (hit >= 10 && hit <= 15) {
g_colorTheme = hit - 10;
} else if (hit == 30) {
SetAutoStart(!g_autoStart);
} else if (hit == 31) {
g_chimeOn = !g_chimeOn;
if (g_chimeOn) MessageBeep(MB_ICONASTERISK);
} else if (hit == 32) {
SetClickThrough(g_hMainWnd, !g_clickThrough);
} else if (hit == 33) {
g_autoTheme = !g_autoTheme;
} else if (hit == 4) {
ApplyWindowTopmost(g_hMainWnd, !g_isTopmost);
SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
} else if (hit == 5) {
g_cleanMode = !g_cleanMode;
} else if (hit == 6) {
SaveSettings();
g_savedToast = true;
g_savedToastTick = GetTickCount();
}
InvalidateRect(hwnd, NULL, FALSE);
if (g_hMainWnd) {
RenderFrame();
InvalidateRect(g_hMainWnd, NULL, FALSE);
}
return 0;
}
case WM_LBUTTONUP: {
if (GetCapture() == hwnd) {
ReleaseCapture();
}
return 0;
}
case WM_KEYDOWN: {
if (wParam == VK_ESCAPE || wParam == 'A' || wParam == 'a') {
DestroyWindow(hwnd);
}
return 0;
}
case WM_DESTROY: {
g_hSettingsWnd = NULL;
g_hoverItem = -1;
return 0;
}
}
return DefWindowProcW(hwnd, msg, wParam, lParam);
}
void OpenModernSettingsWindow(HWND parentHwnd) {
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
SetForegroundWindow(g_hSettingsWnd);
return;
}
static bool registered = false;
const wchar_t SET_CLASS[] = L"Win11SakuraSettingsClass";
if (!registered) {
WNDCLASSEXW wc;
ZeroMemory(&wc, sizeof(WNDCLASSEXW));
wc.cbSize = sizeof(WNDCLASSEXW);
wc.lpfnWndProc = SettingsWndProc;
wc.hInstance = GetModuleHandleW(NULL);
wc.hCursor = LoadCursor(NULL, IDC_ARROW);
wc.lpszClassName = SET_CLASS;
if (RegisterClassExW(&wc)) {
registered = true;
}
}
// 居中到鼠标所在显示器的工作区
POINT cp;
GetCursorPos(&cp);
HMONITOR hMon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
MONITORINFO mi;
mi.cbSize = sizeof(MONITORINFO);
RECT wa;
if (hMon && GetMonitorInfoW(hMon, &mi)) wa = mi.rcWork;
else { wa.left = 0; wa.top = 0; wa.right = GetSystemMetrics(SM_CXSCREEN); wa.bottom = GetSystemMetrics(SM_CYSCREEN); }
int sx = wa.left + ((wa.right - wa.left) - SET_W) / 2;
int sy = wa.top + ((wa.bottom - wa.top) - SET_H) / 2;
if (sy < wa.top) sy = wa.top;
g_hSettingsWnd = CreateWindowExW(
WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
SET_CLASS,
L"Sakura Clock Settings",
WS_POPUP | WS_VISIBLE,
sx, sy, SET_W, SET_H,
parentHwnd, NULL, GetModuleHandleW(NULL), NULL
);
if (g_hSettingsWnd) {
ShowWindow(g_hSettingsWnd, SW_SHOW);
UpdateWindow(g_hSettingsWnd);
}
}
// ==========================================
// 11. 主时钟窗口消息处理
// ==========================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
// 资源管理器重启后自动重建托盘图标
if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
    g_trayAdded = false;
    AddTrayIcon(hwnd);
    return 0;
}
switch (msg) {
case WM_CREATE: {
InitAcrylicOnce(hwnd);
InitResources(hwnd);
SetTimer(hwnd, 1, 33, NULL);
AddTrayIcon(hwnd);
RegisterGlobalHotkeys(hwnd);
break;
}
case WM_ERASEBKGND:
return 1;
case WM_SYSCOMMAND: {
if ((wParam & 0xFFF0) == SC_MINIMIZE) {
    // 最小化到托盘而不是任务栏
    ShowWindow(hwnd, SW_HIDE);
    return 0;
}
break;
}
case WM_EXITSIZEMOVE: {
SnapToScreenEdges(hwnd);
break;
}
case WM_HOTKEY: {
if (wParam == HK_SETTINGS) {
    OpenModernSettingsWindow(hwnd);
} else if (wParam == HK_EXTRA) {
    OpenExtraToolsWindow(hwnd);
} else if (wParam == HK_TOPMOST) {
    ApplyWindowTopmost(hwnd, !g_isTopmost);
    RenderFrame();
    InvalidateRect(hwnd, NULL, FALSE);
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) InvalidateRect(g_hSettingsWnd, NULL, FALSE);
} else if (wParam == HK_THROUGH) {
    SetClickThrough(hwnd, !g_clickThrough);
    RenderFrame();
    InvalidateRect(hwnd, NULL, FALSE);
}
return 0;
}
case WM_TRAYICON: {
UINT evt = (UINT)LOWORD(lParam);
if (evt == WM_LBUTTONDBLCLK) {
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
    } else {
        ShowWindow(hwnd, SW_HIDE);
    }
} else if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
    ShowTrayMenu(hwnd);
}
return 0;
}
case WM_COMMAND: {
UINT id = LOWORD(wParam);
switch (id) {
case IDM_TRAY_SHOW:
    if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
    else { ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd); }
    break;
case IDM_TRAY_SETTINGS:
    OpenModernSettingsWindow(hwnd);
    break;
case IDM_TRAY_EXTRA:
    OpenExtraToolsWindow(hwnd);
    break;
case IDM_TRAY_TOPMOST:
    ApplyWindowTopmost(hwnd, !g_isTopmost);
    break;
case IDM_TRAY_THROUGH:
    SetClickThrough(hwnd, !g_clickThrough);
    break;
case IDM_TRAY_AUTOSTART:
    SetAutoStart(!g_autoStart);
    break;
case IDM_TRAY_CLEAN:
    g_cleanMode = !g_cleanMode;
    break;
case IDM_TRAY_24H:
    g_use24Hour = !g_use24Hour;
    break;
case IDM_TRAY_SECONDS:
    g_showSeconds = !g_showSeconds;
    break;
case IDM_TRAY_EXIT:
    DestroyWindow(hwnd);
    return 0;
}
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) InvalidateRect(g_hSettingsWnd, NULL, FALSE);
return 0;
}
case WM_NCCALCSIZE: {
if (wParam == TRUE) {
return 0;
}
break;
}
case WM_GETMINMAXINFO: {
MINMAXINFO* mmi = (MINMAXINFO*)lParam;
mmi->ptMinTrackSize.x = 240;
mmi->ptMinTrackSize.y = 140;
return 0;
}
case WM_SIZE: {
g_windowMinimized = (wParam == SIZE_MINIMIZED);
int newW = LOWORD(lParam);
int newH = HIWORD(lParam);
if (newW > 0 && newH > 0 && !g_windowMinimized) {
RebuildBufferAndFonts(hwnd, newW, newH);
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
}
return 0;
}
case WM_ACTIVATE: {
g_windowFocus = (LOWORD(wParam) != WA_INACTIVE);
SetTimer(hwnd, 1, g_windowFocus ? 33 : 80, NULL);
break;
}
case WM_NCHITTEST: {
// 边框区域交给我们自己在 WM_LBUTTONDOWN 里处理，其余一律当作客户区，
// 避免依赖系统模态移动循环（Win11 + 分层窗口下会被吞掉，导致窗口无法拖动）
return HTCLIENT;
}
case WM_SETCURSOR: {
POINT cpt;
GetCursorPos(&cpt);
ScreenToClient(hwnd, &cpt);
int zone = HitTestResizeZone(cpt.x, cpt.y);
if (zone != 0) {
    SetCursor(LoadCursorA(NULL, ResizeCursorId(zone)));
    return TRUE;
}
break;
}
case WM_LBUTTONDOWN: {
POINT cpt;
GetCursorPos(&cpt);
POINT rel = cpt;
ScreenToClient(hwnd, &rel);
int zone = HitTestResizeZone(rel.x, rel.y);
if (zone != 0) {
    g_dragZone = zone;
} else {
    g_dragZone = ZONE_MOVE;
}
g_dragStartCursor = cpt;
GetWindowRect(hwnd, &g_dragStartRect);
SetCapture(hwnd);
g_dragging = true;
return 0;
}
case WM_MOUSEMOVE: {
if (!g_dragging) break;
POINT cpt;
GetCursorPos(&cpt);
int dx = cpt.x - g_dragStartCursor.x;
int dy = cpt.y - g_dragStartCursor.y;
int nl = g_dragStartRect.left;
int nt = g_dragStartRect.top;
int nr = g_dragStartRect.right;
int nb = g_dragStartRect.bottom;
const int MIN_W = 320, MIN_H = 200;
if (g_dragZone == ZONE_MOVE) {
    nl += dx; nt += dy;
    nr += dx; nb += dy;
} else {
    if (g_dragZone & ZONE_LEFT)   nl += dx;
    if (g_dragZone & ZONE_RIGHT)  nr += dx;
    if (g_dragZone & ZONE_TOP)    nt += dy;
    if (g_dragZone & ZONE_BOTTOM) nb += dy;
    if (nr - nl < MIN_W) { if (g_dragZone & ZONE_LEFT) nl = nr - MIN_W; else nr = nl + MIN_W; }
    if (nb - nt < MIN_H) { if (g_dragZone & ZONE_TOP)  nt = nb - MIN_H; else nb = nt + MIN_H; }
}
SetWindowPos(hwnd, NULL, nl, nt, nr - nl, nb - nt,
             SWP_NOZORDER | SWP_NOACTIVATE);
return 0;
}
case WM_LBUTTONUP: {
if (!g_dragging) break;
g_dragging = false;
ReleaseCapture();
if (g_dragZone == ZONE_MOVE) {
    SnapToScreenEdges(hwnd);
}
g_dragZone = 0;
return 0;
}
case WM_CAPTURECHANGED: {
g_dragging = false;
g_dragZone = 0;
break;
}
case WM_MOUSEWHEEL: {
int delta = GET_WHEEL_DELTA_WPARAM(wParam);
int step = (delta > 0) ? 12 : -12;
int newA = g_alpha + step;
if (newA < 80) newA = 80;
if (newA > 255) newA = 255;
g_alphaTarget = newA;   // 由定时器平滑逼近，避免生硬跳变
return 0;
}
case WM_NCLBUTTONDBLCLK:
case WM_LBUTTONDBLCLK: {
g_cleanMode = !g_cleanMode;
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
InvalidateRect(g_hSettingsWnd, NULL, FALSE);
}
return 0;
}
case WM_TIMER: {
if (wParam == 1) {
if (g_windowMinimized) break;
// 整点报时（可选）
if (g_chimeOn) {
    SYSTEMTIME cst;
    GetLocalTime(&cst);
    if (cst.wMinute == 0 && cst.wSecond == 0 && g_lastChimeHour != (int)cst.wHour) {
        g_lastChimeHour = (int)cst.wHour;
        MessageBeep(MB_ICONASTERISK);
    }
}
float speedScale = g_windowFocus ? 1.0f : (80.0f / 33.0f);
g_hue += 0.45f * speedScale;
if (g_hue >= 360.0f) g_hue -= 360.0f;
CheckMouseTrail(hwnd);
UpdatePomodoro();
// 透明度平滑动画
if (g_alphaTarget >= 0) {
    if (g_alpha < g_alphaTarget) {
        g_alpha += 6;
        if (g_alpha > g_alphaTarget) g_alpha = g_alphaTarget;
    } else if (g_alpha > g_alphaTarget) {
        g_alpha -= 6;
        if (g_alpha < g_alphaTarget) g_alpha = g_alphaTarget;
    }
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
    if (g_alpha == g_alphaTarget) g_alphaTarget = -1;
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) InvalidateRect(g_hSettingsWnd, NULL, FALSE);
}
UpdateParticles(speedScale);
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
// 设置窗仅需 ~10fps 刷新（作者徽标流光），避免每帧重建大尺寸离屏位图
g_settingsTick++;
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd) && (g_settingsTick % 3 == 0)) {
InvalidateRect(g_hSettingsWnd, NULL, FALSE);
}
}
break;
}
case WM_PAINT: {
PAINTSTRUCT ps;
HDC hdc = BeginPaint(hwnd, &ps);
if (g_memDC) {
BitBlt(hdc, 0, 0, g_winWidth, g_winHeight, g_memDC, 0, 0, SRCCOPY);
}
EndPaint(hwnd, &ps);
break;
}
case WM_KEYDOWN: {
if (wParam == VK_ESCAPE) {
DestroyWindow(hwnd);
} else if (wParam == 'T' || wParam == 't') {
ApplyWindowTopmost(hwnd, !g_isTopmost);
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
InvalidateRect(g_hSettingsWnd, NULL, FALSE);
}
} else if (wParam == 'A' || wParam == 'a') {
OpenModernSettingsWindow(hwnd);
} else if (wParam == 'W' || wParam == 'w') {
OpenExtraToolsWindow(hwnd);
} else if (wParam == 'C' || wParam == 'c') {
g_pomoOn = !g_pomoOn;
if (!g_pomoOn) { g_pomoRunning = false; g_pomoLastTick = 0; }
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
} else if (wParam == 'R' || wParam == 'r') {
g_pomoRemain = 25 * 60;
g_pomoRunning = false;
g_pomoLastTick = 0;
g_pomoOn = true;
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
} else if (wParam == VK_SPACE) {
if (g_pomoOn) {
g_pomoRunning = !g_pomoRunning;
g_pomoLastTick = 0;
RenderFrame();
InvalidateRect(hwnd, NULL, FALSE);
}
}
break;
}
case WM_NCRBUTTONUP: {
ShowTrayMenu(hwnd);
return 0;
}
case WM_RBUTTONUP: {
ShowTrayMenu(hwnd);
return 0;
}
case WM_DESTROY: {
KillTimer(hwnd, 1);
SaveSettings();
RemoveTrayIcon(hwnd);
UnregisterGlobalHotkeys(hwnd);
RestoreAllHiddenWindows();
if (g_desktopIconsHidden) ToggleDesktopIcons();
if (g_taskbarHidden) ToggleTaskbar();
if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) {
DestroyWindow(g_hSettingsWnd);
}
if (g_hExtraWnd && IsWindow(g_hExtraWnd)) {
DestroyWindow(g_hExtraWnd);
}
CleanupResources();
PostQuitMessage(0);
break;
}
default:
return DefWindowProcW(hwnd, msg, wParam, lParam);
}
return 0;
}
// ==========================================
// 12. 程序主入口
// ==========================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
// 单实例：已有进程则唤出旧窗口后退出
HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\SakuraClockSingleInstance");
if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND hExist = FindWindowW(L"ResizableSakuraClockClass", NULL);
    if (hExist) {
        ShowWindow(hExist, SW_SHOW);
        SetForegroundWindow(hExist);
    }
    CloseHandle(hMutex);
    return 0;
}

HMODULE hUser32 = GetModuleHandleA("user32.dll");
if (hUser32) {
g_SetProcessDpiAwarenessContext = (pfnSetProcessDpiAwarenessContext)
GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
if (g_SetProcessDpiAwarenessContext) {
g_SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
}
LoadSettings();
g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
const wchar_t CLASS_NAME[] = L"ResizableSakuraClockClass";
WNDCLASSEXW wc;
ZeroMemory(&wc, sizeof(WNDCLASSEXW));
wc.cbSize = sizeof(WNDCLASSEXW);
wc.style = CS_DBLCLKS;
wc.lpfnWndProc = WndProc;
wc.hInstance = hInstance;
wc.hCursor = LoadCursor(NULL, IDC_ARROW);
wc.lpszClassName = CLASS_NAME;
if (!RegisterClassExW(&wc)) return 0;
// 位置恢复：优先使用上次位置，越界则居中
int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
int screenX, screenY;
if (g_winX > vx - g_winWidth / 2 && g_winX < vx + vw - 100 &&
    g_winY > vy - 40 && g_winY < vy + vh - 100) {
    screenX = g_winX;
    screenY = g_winY;
} else {
    screenX = vx + (vw - g_winWidth) / 2;
    screenY = vy + (vh - g_winHeight) / 2;
}
HWND hwnd = CreateWindowExW(
WS_EX_APPWINDOW | WS_EX_LAYERED,
CLASS_NAME,
L"Sakura Dreamy Clock",
WS_POPUP | WS_THICKFRAME | WS_VISIBLE,
screenX, screenY, g_winWidth, g_winHeight,
NULL, NULL, hInstance, NULL
);
if (!hwnd) return 0;
g_hMainWnd = hwnd;
ApplyWindowAlpha(hwnd, g_defaultAlpha);
if (g_isTopmost) {
ApplyWindowTopmost(hwnd, true);
}
SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
ShowWindow(hwnd, nCmdShow);
UpdateWindow(hwnd);
MSG msg;
ZeroMemory(&msg, sizeof(MSG));
while (GetMessage(&msg, NULL, 0, 0)) {
TranslateMessage(&msg);
DispatchMessage(&msg);
}
if (hMutex) CloseHandle(hMutex);
return (int)msg.wParam;
}