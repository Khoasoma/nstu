#include "nstu/named_pipe.hpp"

#include <windows.h>
#include <shellapi.h>

namespace {

constexpr wchar_t kWindowClass[] = L"NstuAgentOverlay";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case kTrayMessage:
        if (lparam == WM_LBUTTONDBLCLK) {
            ShowWindow(window, IsWindowVisible(window) ? SW_HIDE : SW_SHOW);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC context = BeginPaint(window, &paint);
        FillRect(context, &paint.rcPaint,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(context, RGB(255, 255, 255));
        SetBkMode(context, TRANSPARENT);
        const wchar_t text[] = L"This computer is locked by the instructor.";
        RECT area{};
        GetClientRect(window, &area);
        DrawTextW(context, text, -1, &area,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&window_class);
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"NSTU Lock",
        WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        return 1;
    }
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = kTrayId;
    tray.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    tray.uCallbackMessage = kTrayMessage;
    tray.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    lstrcpyW(tray.szTip, L"NSTU client");
    Shell_NotifyIconW(NIM_ADD, &tray);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &tray);
    return static_cast<int>(message.wParam);
}
