#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000002
#endif

#include "contest_room_views.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>

#include <array>
#include <mutex>
#include <thread>

namespace {
using namespace nstu::client;
using namespace nstu::client::student_ui;
using Clock = std::chrono::steady_clock;
constexpr UINT kDiscoveryComplete = WM_APP + 41;
constexpr UINT_PTR kMaintenanceTimer = 1;
constexpr wchar_t kWindowClass[] = L"NSTU.ContestStudent";
constexpr wchar_t kStandingsClass[] = L"NSTU.ContestStudentStandings";
enum Control : int { name_label = 100, name_edit, manual_label, manual_edit, probe_button,
    refresh_button, language_button, room_list, status_text, sas_text, join_button,
    confirm_button, reject_button, reconnect_button, editor_button, standings_button,
    standings_list, standings_status };

std::wstring wide(std::string_view input) {
    if (input.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!count) return L"[invalid text]";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), count)) return L"[invalid text]";
    return result;
}
std::string utf8(std::wstring_view input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), count, nullptr, nullptr)) return {};
    return result;
}
std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    const int read = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(std::max(0, read)));
    return result;
}
std::wstring environment(const wchar_t* key) {
    const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
    if (!count || count > 32768) return {};
    std::wstring result(count, L'\0');
    const DWORD read = GetEnvironmentVariableW(key, result.data(), count);
    if (!read || read >= count) return {};
    result.resize(read); return result;
}
void set_text(HWND control, const std::wstring& value) {
    if (window_text(control) != value) SetWindowTextW(control, value.c_str());
}
void set_cell(HWND list, int row, int column, const std::wstring& value) {
    LVITEMW item{}; item.iItem = row; item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(value.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
}
void add_row(HWND list, int row, const std::wstring& first) {
    LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = row;
    item.pszText = const_cast<wchar_t*>(first.c_str());
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
}
void add_column(HWND list, int index, const std::wstring& label, int width) {
    LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.iSubItem = index; column.cx = width; column.pszText = const_cast<wchar_t*>(label.c_str());
    SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&column));
}

struct App {
    HWND window = nullptr, scores_window = nullptr;
    HINSTANCE instance = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    bool vi = true, scanning = false, session_started = false, editor_attempted = false, closing = false;
    LobbyPhase lobby = LobbyPhase::empty;
    std::string notice, discovery_error, deployment_error;
    ContestStudentConfig config;
    ContestStudentSession session;
    ExamHost host; // Destroyed before session; normal close has already passed capture.
    std::vector<nstu::contest_discovery::RoomCandidate> rooms;
    std::vector<bool> remembered;
    std::optional<nstu::contest_discovery::RoomCandidate> selected;
    bool selected_remembered = false;
    std::thread discovery_worker;
    std::mutex discovery_mutex;
    std::vector<nstu::contest_discovery::RoomCandidate> discovery_result;
    std::vector<bool> discovery_remembered;
    std::string discovery_result_error;
    std::optional<StudentStandings> displayed_scores;
    std::optional<nstu::exam::StateRequest> scores_context;
    Clock::time_point last_score_request{};

    ~App() {
        if (discovery_worker.joinable()) discovery_worker.join();
        session.stop();
        if (font) DeleteObject(font);
    }
    [[nodiscard]] int px(int value) const noexcept { return MulDiv(value, static_cast<int>(dpi), 96); }
    [[nodiscard]] HWND control(int id) const noexcept { return GetDlgItem(window, id); }
    [[nodiscard]] std::wstring tr(const char* en, const char* vietnamese) const { return wide(text(vi, en, vietnamese)); }

    bool configure(int argc, wchar_t** argv) {
        const auto local = environment(L"LOCALAPPDATA");
        if (!local.empty()) config.user_root = std::filesystem::path(local) / L"NSTU" / L"student";
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg(argv[i]);
            if (arg == L"--profile-root" && i + 1 < argc) config.user_root = std::filesystem::path(argv[++i]);
            else if (arg.starts_with(L"--profile-root=")) config.user_root = std::filesystem::path(arg.substr(15));
            else if (arg == L"--language=en") vi = false;
            else if (arg == L"--language=vi") vi = true;
            else return false;
        }
        if (config.user_root.empty() || !config.user_root.is_absolute() || config.user_root.lexically_normal() != config.user_root || config.user_root == config.user_root.root_path()) return false;
        std::array<wchar_t, 32768> executable{};
        const DWORD count = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!count || count >= executable.size()) return false;
        config.extractor_script = std::filesystem::path(std::wstring(executable.data(), count)).parent_path() / L"stage-contest-package.ps1";
        config.extractor_digest = *nstu::contest_package::digest_from_hex("23dfce986a4d0f212def8d6c630b8488515e4a3747b9a664b921762a805403a6");
        config.powershell_executable = std::filesystem::path(environment(L"NSTU_CONTEST_POWERSHELL_PATH"));
        const auto pin = nstu::contest_package::digest_from_hex(utf8(environment(L"NSTU_CONTEST_POWERSHELL_SHA256")));
        if (config.powershell_executable.empty() || !config.powershell_executable.is_absolute() || !pin)
            deployment_error = native_failure_label(NativeFailure::deployment, vi);
        else config.powershell_digest = *pin;
        return true;
    }
    HWND create(const wchar_t* type, int id, DWORD style, DWORD extended = 0, HWND parent = nullptr) {
        return CreateWindowExW(extended, type, L"", WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
            parent ? parent : window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    }
    void create_controls() {
        create(L"STATIC", name_label, 0);
        create(L"EDIT", name_edit, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        SendMessageW(control(name_edit), EM_SETLIMITTEXT, 64, 0);
        create(L"STATIC", manual_label, 0);
        create(L"EDIT", manual_edit, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        SendMessageW(control(manual_edit), EM_SETLIMITTEXT, 32, 0);
        create(L"BUTTON", probe_button, WS_TABSTOP | BS_PUSHBUTTON);
        create(L"BUTTON", refresh_button, WS_TABSTOP | BS_PUSHBUTTON);
        create(L"BUTTON", language_button, WS_TABSTOP | BS_PUSHBUTTON);
        const auto list = create(WC_LISTVIEWW, room_list, WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE);
        SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        create(L"EDIT", status_text, ES_MULTILINE | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE);
        create(L"STATIC", sas_text, SS_CENTER);
        for (const int id : {join_button, confirm_button, reject_button, reconnect_button, editor_button, standings_button})
            create(L"BUTTON", id, WS_TABSTOP | BS_PUSHBUTTON);
        update_font(); localize(); layout(); scan({});
    }
    void update_font() {
        NONCLIENTMETRICSW metrics{}; metrics.cbSize = sizeof(metrics);
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) return;
        const HFONT replacement = CreateFontIndirectW(&metrics.lfMessageFont);
        if (!replacement) return;
        EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(replacement));
        if (scores_window) EnumChildWindows(scores_window, [](HWND child, LPARAM value) -> BOOL { SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE); return TRUE; }, reinterpret_cast<LPARAM>(replacement));
        if (font) DeleteObject(font);
        font = replacement;
    }
    void localize() {
        if (!deployment_error.empty()) deployment_error = native_failure_label(NativeFailure::deployment, vi);
        if (discovery_error == native_failure_label(NativeFailure::discovery, false) || discovery_error == native_failure_label(NativeFailure::discovery, true))
            discovery_error = native_failure_label(NativeFailure::discovery, vi);
        SetWindowTextW(window, vi ? L"NSTU • Phòng thi lập trình" : L"NSTU • Programming contest rooms");
        set_text(control(name_label), tr("Your display &name (teacher approves identity)", "&Tên hiển thị (giáo viên xác minh danh tính)"));
        set_text(control(manual_label), tr("Teacher &IPv4:UDPport", "&IPv4:cổng UDP của giáo viên"));
        set_text(control(probe_button), tr("&Probe", "&Tìm theo IP"));
        set_text(control(refresh_button), tr("&Refresh rooms", "&Làm mới phòng"));
        set_text(control(language_button), vi ? L"English" : L"Tiếng Việt");
        set_text(control(join_button), tr("&Join selected room", "&Tham gia phòng đã chọn"));
        set_text(control(confirm_button), tr("&Codes match — confirm", "&Mã khớp — xác nhận"));
        set_text(control(reject_button), closing ? tr("&Cancel close", "&Hủy đóng") : tr("&Reject / disconnect", "&Từ chối / ngắt kết nối"));
        set_text(control(reconnect_button), tr("&Reconnect", "&Kết nối lại"));
        set_text(control(editor_button), tr("&Open editor", "&Mở trình soạn thảo"));
        set_text(control(standings_button), tr("&Published standings", "&Bảng xếp hạng công khai"));
        const auto list = control(room_list);
        while (SendMessageW(list, LVM_DELETECOLUMN, 0, 0)) {}
        add_column(list, 0, tr("Teacher (unverified)", "Giáo viên (chưa xác minh)"), px(180));
        add_column(list, 1, tr("Room", "Phòng"), px(210));
        add_column(list, 2, tr("Endpoint", "Địa chỉ"), px(155));
        add_column(list, 3, tr("Joining", "Tham gia"), px(130));
        const auto previous = selected;
        populate_rooms();
        if (previous) for (std::size_t i = 0; i < rooms.size(); ++i) {
            if (rooms[i].address != previous->address || rooms[i].room != previous->room) continue;
            LVITEMW item{}; item.stateMask = LVIS_SELECTED | LVIS_FOCUSED; item.state = item.stateMask;
            SendMessageW(list, LVM_SETITEMSTATE, i, reinterpret_cast<LPARAM>(&item));
            break;
        }
        if (scores_window) {
            SetWindowTextW(scores_window, vi ? L"NSTU • Bảng xếp hạng công khai" : L"NSTU • Published standings");
            const auto scores = GetDlgItem(scores_window, standings_list);
            while (SendMessageW(scores, LVM_DELETECOLUMN, 0, 0)) {}
            add_column(scores, 0, tr("Rank", "Hạng"), px(65));
            add_column(scores, 1, tr("Display name", "Tên hiển thị"), px(260));
            add_column(scores, 2, tr("Solved", "Bài giải"), px(90));
            add_column(scores, 3, tr("Penalty (minutes)", "Phạt (phút)"), px(160));
            displayed_scores.reset();
        }
    }
    void layout() {
        RECT bounds{}; GetClientRect(window, &bounds);
        const int width = bounds.right, height = bounds.bottom, margin = px(16), row = px(30), gap = px(8);
        const auto move = [&](int id, int x, int y, int w, int h) { MoveWindow(control(id), x, y, std::max(1, w), std::max(1, h), TRUE); };
        move(name_label, margin, margin, width - margin * 2 - px(110), px(24));
        move(language_button, width - margin - px(110), margin, px(110), row);
        move(name_edit, margin, margin + px(28), width - margin * 2, row);
        move(manual_label, margin, margin + px(66), width - margin * 2, px(24));
        move(manual_edit, margin, margin + px(94), width - margin * 2 - px(292), row);
        move(probe_button, width - margin - px(284), margin + px(94), px(120), row);
        move(refresh_button, width - margin - px(156), margin + px(94), px(156), row);
        const int bottom = height - margin - px(238), top = margin + px(134);
        move(room_list, margin, top, width - margin * 2, bottom - top);
        const int half = (width - margin * 2 - gap) / 2;
        move(join_button, margin, bottom + gap, half, row);
        move(reconnect_button, margin + half + gap, bottom + gap, half, row);
        move(sas_text, margin, bottom + px(46), width - margin * 2, px(30));
        move(confirm_button, margin, bottom + px(80), half, row);
        move(reject_button, margin + half + gap, bottom + px(80), half, row);
        move(editor_button, margin, bottom + px(118), half, row);
        move(standings_button, margin + half + gap, bottom + px(118), half, row);
        move(status_text, margin, bottom + px(156), width - margin * 2, px(82));
    }
    void populate_rooms() {
        const auto list = control(room_list);
        SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
        for (std::size_t i = 0; i < rooms.size(); ++i) {
            const auto& room = rooms[i]; const int row = static_cast<int>(i);
            add_row(list, row, wide(room.room.teacher_label));
            set_cell(list, row, 1, wide(room.room.title));
            set_cell(list, row, 2, wide(room.address + ":" + std::to_string(room.room.control_port)));
            set_cell(list, row, 3, remembered[i] ? tr("Saved membership", "Thành viên đã lưu") :
                room.room.join_state == nstu::contest_discovery::JoinState::open ? tr("Open", "Đang mở") : tr("Joining closed", "Đã khóa tham gia"));
        }
        selected.reset();
    }
    void scan(std::optional<nstu::contest_discovery::Endpoint> manual) {
        if (scanning || session_started) return;
        if (discovery_worker.joinable()) discovery_worker.join();
        scanning = true; selected.reset(); rooms.clear(); remembered.clear(); populate_rooms(); notice.clear(); discovery_error.clear(); lobby = LobbyPhase::scanning;
        nstu::contest_discovery::DiscoveryOptions options;
        if (manual) options.manual_targets.push_back(*manual);
        const HWND target = window;
        discovery_worker = std::thread([this, options = std::move(options), target] {
            std::string error;
            std::vector<nstu::contest_discovery::RoomCandidate> found;
            std::vector<bool> saved_rows;
            try {
                found = nstu::contest_discovery::discover_rooms(options, &error);
                saved_rows.resize(found.size(), false);
                std::string recovery_error;
                const auto saved = load_student_remembered_rooms(config.user_root, &recovery_error);
                if (!saved) { if (error.empty()) error = recovery_error; }
                else for (const auto& item : *saved) {
                    if (found.size() >= nstu::contest_discovery::kMaximumCandidates) break;
                    const bool duplicate = std::any_of(found.begin(), found.end(), [&](const auto& row) {
                        return row.room.teacher_id == item.realm.teacher_id && row.room.room_id == item.realm.room_id && row.room.round_id == item.realm.round_id && row.address == item.endpoint.address && row.room.control_port == item.endpoint.port;
                    });
                    if (duplicate) continue;
                    nstu::contest_discovery::RoomCandidate row;
                    row.address = item.endpoint.address; row.observed_at = Clock::now();
                    row.room = {item.realm.room_id, item.realm.round_id, item.display_label, item.realm.teacher_id,
                        nstu::contest_discovery::JoinState::join_closed, item.realm.version, item.endpoint.port, item.realm.teacher_id};
                    found.push_back(std::move(row)); saved_rows.push_back(true);
                }
            }
            catch (...) { error = native_failure_label(NativeFailure::discovery, false); }
            { std::scoped_lock lock(discovery_mutex); discovery_result = std::move(found); discovery_remembered = std::move(saved_rows); discovery_result_error = std::move(error); }
            PostMessageW(target, kDiscoveryComplete, 0, 0);
        });
    }
    void discovery_complete() {
        { std::scoped_lock lock(discovery_mutex); rooms = std::move(discovery_result); remembered = std::move(discovery_remembered); discovery_error = std::move(discovery_result_error); }
        if (discovery_error == native_failure_label(NativeFailure::discovery, false)) discovery_error = native_failure_label(NativeFailure::discovery, vi);
        remembered.resize(rooms.size(), false);
        scanning = false;
        lobby = !rooms.empty() ? LobbyPhase::available : discovery_error.empty() ? LobbyPhase::empty : LobbyPhase::failed;
        populate_rooms(); maintain();
    }
    void open_editor() {
        if (host.window()) { ShowWindow(host.window(), SW_SHOWNORMAL); SetForegroundWindow(host.window()); return; }
        const auto options = session.host_options();
        if (!options) return;
        editor_attempted = true;
        auto callbacks = session.host_callbacks();
        callbacks.before_close = [this](const std::map<std::string, nstu::exam::programming::Request>& pending) {
            const auto report = host.capture_report();
            // The host permits a missing report only if no editable document existed.
            return report ? session.preserve_capture(*report, pending) : pending.empty();
        };
        callbacks.status = [this](std::string value) { notice = std::move(value); };
        if (!host.start(window, *options, std::move(callbacks), &notice)) return;
    }
    void show_scores() {
        if (!can_show_standings(session.snapshot())) return;
        if (!scores_window) {
            scores_window = CreateWindowExW(0, kStandingsClass, L"NSTU", WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, px(700), px(480), window, nullptr, instance, this);
            if (!scores_window) return;
            const HWND table = create(WC_LISTVIEWW, standings_list, WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, WS_EX_CLIENTEDGE, scores_window);
            SendMessageW(table, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            create(L"EDIT", standings_status, ES_MULTILINE | ES_READONLY, WS_EX_CLIENTEDGE, scores_window);
            localize(); update_font(); scores_layout();
        }
        ShowWindow(scores_window, SW_SHOWNORMAL); SetForegroundWindow(scores_window);
        last_score_request = Clock::time_point{};
        maintain();
    }
    void scores_layout() {
        if (!scores_window) return;
        RECT area{}; GetClientRect(scores_window, &area);
        const int margin = px(16);
        MoveWindow(GetDlgItem(scores_window, standings_list), margin, margin,
            std::max(1, static_cast<int>(area.right) - margin * 2), std::max(1, static_cast<int>(area.bottom) - margin * 2 - px(70)), TRUE);
        MoveWindow(GetDlgItem(scores_window, standings_status), margin, area.bottom - margin - px(62),
            std::max(1, static_cast<int>(area.right) - margin * 2), px(62), TRUE);
    }
    void update_scores(const ContestStudentSnapshot& state) {
        if (!scores_window || !IsWindowVisible(scores_window)) return;
        const auto table = GetDlgItem(scores_window, standings_list);
        if (!can_show_standings(state) || (scores_context && !same_context(*state.context, *scores_context))) {
            SendMessageW(table, LVM_DELETEALLITEMS, 0, 0); displayed_scores.reset(); scores_context.reset();
            ShowWindow(scores_window, SW_HIDE); return;
        }
        scores_context = state.context;
        if (can_refresh_standings(state) && Clock::now() - last_score_request >= std::chrono::seconds(5)) {
            last_score_request = Clock::now(); (void)session.request_standings();
        }
        if (state.standings && accept_standings(displayed_scores, *state.standings) &&
            (!displayed_scores || displayed_scores->revision != state.standings->revision || displayed_scores->generated_time != state.standings->generated_time)) {
            displayed_scores = state.standings;
            SendMessageW(table, LVM_DELETEALLITEMS, 0, 0);
            for (std::size_t i = 0; i < displayed_scores->rows.size(); ++i) {
                const auto& row = displayed_scores->rows[i]; const int index = static_cast<int>(i);
                add_row(table, index, std::to_wstring(row.rank));
                set_cell(table, index, 1, wide(row.display_name));
                set_cell(table, index, 2, std::to_wstring(row.solved));
                set_cell(table, index, 3, std::to_wstring(row.penalty_minutes));
            }
        }
        std::wstring label = !state.connected ? tr("Disconnected. Last-known standings are provisional.", "Mất kết nối. Bảng xếp hạng gần nhất là tạm thời.") :
            displayed_scores ? wide(standing_label(standing_freshness(state), vi)) :
                tr("Waiting for published standings", "Đang chờ bảng xếp hạng công khai");
        if (displayed_scores) label += L"\r\n" + tr("Revision: ", "Phiên bản: ") + std::to_wstring(displayed_scores->revision);
        set_text(GetDlgItem(scores_window, standings_status), label);
    }
    void maintain() {
        const auto state = session.snapshot();
        if (selected && !selected_remembered && Clock::now() - selected->observed_at >= nstu::contest_discovery::kHintLifetime && !session_started) lobby = LobbyPhase::stale;
        EnableWindow(control(join_button), !scanning && deployment_error.empty() && can_connect(selected, session_started, Clock::now(), selected_remembered));
        set_text(control(join_button), selected && (selected_remembered || selected->room.join_state == nstu::contest_discovery::JoinState::join_closed) ?
            tr("&Resume existing membership", "&Khôi phục thành viên hiện có") : tr("&Join selected room", "&Tham gia phòng đã chọn"));
        EnableWindow(control(refresh_button), !scanning && !session_started);
        EnableWindow(control(probe_button), !scanning && !session_started);
        EnableWindow(control(name_edit), !session_started);
        EnableWindow(control(confirm_button), can_confirm(state));
        EnableWindow(control(reject_button), session_started || closing);
        EnableWindow(control(reconnect_button), session_started && !state.connected && state.phase != StudentPhase::connecting && state.phase != StudentPhase::confirm_teacher && state.phase != StudentPhase::awaiting_admission);
        EnableWindow(control(editor_button), state.package_verified && state.context.has_value());
        ShowWindow(control(standings_button), can_show_standings(state) ? SW_SHOW : SW_HIDE);
        set_text(control(sas_text), can_confirm(state) ? tr("Compare code: ", "So sánh mã: ") + wide(state.sas) : L"");
        std::wstring status = session_started ? wide(phase_label(state.phase, vi)) : wide(lobby_label(lobby, vi));
        if (state.archive_bytes) status += L"\r\n" + tr("Public package bytes received: ", "Số byte gói đề đã nhận: ") + std::to_wstring(state.received_bytes) + L" / " + std::to_wstring(state.archive_bytes);
        if (state.lifecycle.deadline && state.server_time) {
            const auto remaining = state.lifecycle.deadline > state.server_time ? (state.lifecycle.deadline - state.server_time) / 1000 : 0;
            status += L"\r\n" + tr("Server clock remaining: ", "Thời gian còn lại theo máy chủ: ") + std::to_wstring(remaining / 60) + L":" + (remaining % 60 < 10 ? L"0" : L"") + std::to_wstring(remaining % 60);
        }
        const auto& detail = !state.error.empty() ? state.error : !notice.empty() ? notice : !deployment_error.empty() ? deployment_error : discovery_error;
        if (!detail.empty()) status += L"\r\n" + wide(detail);
        if (closing) status += L"\r\n" + tr("Preserving current drafts before close. Keep the editor open; cancel close to continue editing.", "Đang lưu bản nháp trước khi đóng. Giữ trình soạn thảo mở; hủy đóng để tiếp tục sửa.");
        set_text(control(status_text), status);
        if (state.package_verified && !editor_attempted && !closing) open_editor();
        if (host_needs_preservation(host.state())) {
            for (const auto& reply : session.take_replies()) {
                const auto bytes = std::as_bytes(std::span(reply.json));
                (void)exam_bridge().publish({AgentMessageType::exam_programming_response, {bytes.begin(), bytes.end()}});
                host.drain_bridge();
            }
            if (const auto life = host_lifecycle(state)) (void)host.apply_contest_lifecycle(*life);
            host.drain_bridge();
        }
        update_scores(state);
        if (closing && (!host_needs_preservation(host.state()) || host.request_close())) DestroyWindow(window);
    }
    void command(int id) {
        switch (id) {
        case language_button: vi = !vi; localize(); break;
        case refresh_button: scan({}); break;
        case probe_button: {
            const auto endpoint = manual_endpoint(utf8(window_text(control(manual_edit))));
            if (!endpoint) notice = text(vi, "Enter a literal IPv4:UDPport, for example 192.168.1.10:47002.", "Nhập IPv4:cổng UDP, ví dụ 192.168.1.10:47002.");
            else scan(endpoint);
            break;
        }
        case join_button:
            if (!can_connect(selected, session_started, Clock::now(), selected_remembered) || !deployment_error.empty()) break;
            config.display_name = utf8(window_text(control(name_edit)));
            config.room_label = selected->room.title;
            config.endpoint = {selected->address, selected->room.control_port};
            config.realm = {nstu::contest_room::kVersion, selected->room.teacher_id, selected->room.room_id, selected->room.round_id};
            session_started = session.start(config, &notice);
            if (!session_started) notice = text(vi, "Enter a valid display name before joining.", "Nhập tên hiển thị hợp lệ trước khi tham gia.");
            break;
        case confirm_button: { const auto state = session.snapshot(); if (can_confirm(state)) (void)session.confirm_sas(state.sas); break; }
        case reject_button:
            if (closing) { closing = false; (void)host.cancel_capture(); localize(); }
            else if (session.snapshot().phase == StudentPhase::confirm_teacher || session.snapshot().phase == StudentPhase::awaiting_admission) {
                session.cancel_pairing(); session.stop(); session_started = false; editor_attempted = false; scan({});
            }
            else session.disconnect();
            break;
        case reconnect_button: (void)session.reconnect(); break;
        case editor_button: open_editor(); break;
        case standings_button: show_scores(); break;
        default: break;
        }
        maintain();
    }
    void close() {
        if (!host_needs_preservation(host.state()) || host.request_close()) DestroyWindow(window);
        else { closing = true; localize(); maintain(); }
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->window = window;
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_CREATE:
            app->dpi = GetDpiForWindow(window);
            app->create_controls(); SetTimer(window, kMaintenanceTimer, 100, nullptr); return 0;
        case WM_SIZE: app->layout(); return 0;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wparam); app->update_font();
            const auto rect = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            app->localize(); app->layout(); return 0;
        }
        case WM_GETMINMAXINFO: {
            const auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {app->px(680), app->px(540)}; return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDCANCEL) {
                if (app->closing || app->session.snapshot().phase == StudentPhase::confirm_teacher || app->session.snapshot().phase == StudentPhase::awaiting_admission) app->command(reject_button);
                else app->close();
                return 0;
            }
            if (HIWORD(wparam) == BN_CLICKED) { app->command(LOWORD(wparam)); return 0; }
            break;
        case WM_NOTIFY: {
            const auto notification = reinterpret_cast<NMHDR*>(lparam);
            if (notification->idFrom == room_list && notification->code == LVN_ITEMCHANGED) {
                const auto row = static_cast<int>(SendMessageW(app->control(room_list), LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
                app->selected = row >= 0 && static_cast<std::size_t>(row) < app->rooms.size() ? std::optional{app->rooms[static_cast<std::size_t>(row)]} : std::nullopt;
                app->selected_remembered = row >= 0 && static_cast<std::size_t>(row) < app->remembered.size() && app->remembered[static_cast<std::size_t>(row)];
                app->maintain(); return 0;
            }
            break;
        }
        case kDiscoveryComplete: app->discovery_complete(); return 0;
        case WM_TIMER: if (wparam == kMaintenanceTimer) { app->maintain(); return 0; } break;
        case WM_CLOSE: app->close(); return 0;
        case WM_DESTROY: KillTimer(window, kMaintenanceTimer); PostQuitMessage(0); return 0;
        default: break;
        }
    } catch (...) {
        app->notice = text(app->vi, "Student operation unavailable; keep the editor open to preserve source.", "Thao tác không khả dụng; giữ trình soạn thảo mở để bảo toàn mã nguồn.");
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK scores_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app) {
        if (message == WM_SIZE) { app->scores_layout(); return 0; }
        if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
        if (message == WM_DESTROY) { app->scores_window = nullptr; return 0; }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    int result = 1;
    bool vi = true;
    try {
        App app; app.instance = instance;
        int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            if (std::wstring_view(argv[i]) == L"--language=en") vi = false;
            else if (std::wstring_view(argv[i]) == L"--language=vi") vi = true;
        }
        app.vi = vi;
        const bool configured = argv && app.configure(argc, argv);
        if (argv) LocalFree(argv);
        if (!configured) {
            MessageBoxW(nullptr, wide(native_failure_label(NativeFailure::configuration, vi)).c_str(),
                wide(native_failure_title(NativeFailure::configuration, vi)).c_str(), MB_OK | MB_ICONERROR);
        } else {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
            if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Native controls unavailable");
            WNDCLASSEXW klass{}; klass.cbSize = sizeof(klass); klass.hInstance = instance;
            klass.lpfnWndProc = window_proc; klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1); klass.lpszClassName = kWindowClass;
            if (!RegisterClassExW(&klass)) throw std::runtime_error("Native lobby unavailable");
            klass.lpfnWndProc = scores_proc; klass.lpszClassName = kStandingsClass;
            if (!RegisterClassExW(&klass)) throw std::runtime_error("Native standings unavailable");
            const HWND window = CreateWindowExW(0, kWindowClass, L"NSTU", WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, 940, 720, nullptr, nullptr, instance, &app);
            if (!window) throw std::runtime_error("Native student window unavailable");
            ShowWindow(window, show); UpdateWindow(window);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(window, &message) && (!app.scores_window || !IsDialogMessageW(app.scores_window, &message))) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
            }
            result = 0;
        }
    } catch (...) {
        MessageBoxW(nullptr, wide(native_failure_label(NativeFailure::startup, vi)).c_str(),
            wide(native_failure_title(NativeFailure::startup, vi)).c_str(), MB_OK | MB_ICONERROR);
    }
    CoUninitialize(); return result;
}
