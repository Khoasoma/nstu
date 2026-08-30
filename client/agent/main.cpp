#include "nstu/named_pipe.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <iterator>

namespace {

constexpr wchar_t kWindowClass[] = L"NstuAgentOverlay";
constexpr wchar_t kChatWindowClass[] = L"NstuAgentChat";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr int kChatMessages = 1001;
constexpr int kChatInput = 1002;
constexpr int kChatSend = 1003;

HWND g_chat_window = nullptr;
HWND g_chat_messages = nullptr;
HWND g_chat_input = nullptr;

void append_chat_line(const wchar_t* message) {
    if (g_chat_messages == nullptr) {
        return;
    }
    SendMessageW(g_chat_messages, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(message));
    const auto count = SendMessageW(g_chat_messages, LB_GETCOUNT, 0, 0);
    if (count > 0) {
        SendMessageW(g_chat_messages, LB_SETCURSEL, count - 1, 0);
    }
}

LRESULT CALLBACK chat_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_chat_messages = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
            8, 8, 460, 220, window, reinterpret_cast<HMENU>(kChatMessages),
            GetModuleHandleW(nullptr), nullptr);
        g_chat_input = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            8, 240, 360, 26, window, reinterpret_cast<HMENU>(kChatInput),
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(
            0, L"BUTTON", L"Send",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            380, 240, 80, 26, window, reinterpret_cast<HMENU>(kChatSend),
            GetModuleHandleW(nullptr), nullptr);
        append_chat_line(L"NSTU client chat ready.");
        return 0;
    case WM_SIZE: {
        const int width = LOWORD(lparam);
        const int height = HIWORD(lparam);
        const int input_y = std::max(32, height - 38);
        if (g_chat_messages != nullptr) {
            MoveWindow(g_chat_messages, 8, 8, std::max(80, width - 16),
                       std::max(40, input_y - 16), TRUE);
        }
        if (g_chat_input != nullptr) {
            MoveWindow(g_chat_input, 8, input_y, std::max(40, width - 96), 26,
                       TRUE);
        }
        const auto send_button = GetDlgItem(window, kChatSend);
        if (send_button != nullptr) {
            MoveWindow(send_button, std::max(8, width - 80), input_y, 72, 26,
                       TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == kChatSend && HIWORD(wparam) == BN_CLICKED &&
            g_chat_input != nullptr) {
            wchar_t message_text[512]{};
            GetWindowTextW(g_chat_input, message_text,
                           static_cast<int>(std::size(message_text)));
            if (message_text[0] != L'\0') {
                wchar_t line[540]{};
                swprintf_s(line, L"You: %s", message_text);
                append_chat_line(line);
                SetWindowTextW(g_chat_input, L"");
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_chat_window = nullptr;
        g_chat_messages = nullptr;
        g_chat_input = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

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
            if (g_chat_window != nullptr) {
                ShowWindow(g_chat_window,
                           IsWindowVisible(g_chat_window) ? SW_HIDE : SW_SHOW);
                SetForegroundWindow(g_chat_window);
            }
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
    WNDCLASSW chat_class{};
    chat_class.hInstance = instance;
    chat_class.lpfnWndProc = chat_window_proc;
    chat_class.lpszClassName = kChatWindowClass;
    chat_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    chat_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    RegisterClassW(&chat_class);
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
    g_chat_window = CreateWindowExW(
        0, kChatWindowClass, L"NSTU Client Chat", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 340, nullptr, nullptr, instance,
        nullptr);
    if (g_chat_window == nullptr) {
        DestroyWindow(window);
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
    ShowWindow(g_chat_window, SW_SHOWDEFAULT);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &tray);
    return static_cast<int>(message.wParam);
}
