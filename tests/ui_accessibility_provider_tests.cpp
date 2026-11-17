#include "../server/app/ui_accessibility_provider.hpp"
#ifdef NSTU_UIA_CONTEST_PRODUCER
#include "../server/app/contest_room_panel.hpp"
#ifdef FindText
#undef FindText
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <exception>
#include <cwchar>
#include <iostream>
#include <memory>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace nstu::ui::accessibility;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct ReleaseCom {
    template<class T> void operator()(T* value) const noexcept { if (value) value->Release(); }
};
template<class T> using Com = std::unique_ptr<T, ReleaseCom>;
template<class T> Com<T> query(IUnknown* value) {
    T* result = nullptr;
    require(SUCCEEDED(value->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result))), "required interface missing");
    return Com<T>(result);
}
std::wstring name(IRawElementProviderSimple* provider) {
    VARIANT value{};
    require(provider->GetPropertyValue(UIA_NamePropertyId, &value) == S_OK, "name query failed");
    require(value.vt == VT_BSTR, "name is not a BSTR");
    const std::wstring result(value.bstrVal, SysStringLen(value.bstrVal));
    VariantClear(&value);
    return result;
}
LONG runtime(IRawElementProviderSimple* provider) {
    auto fragment = query<IRawElementProviderFragment>(provider);
    SAFEARRAY* value = nullptr;
    require(fragment->GetRuntimeId(&value) == S_OK && value, "runtime ID missing");
    LONG first = 0, second = 1, prefix = 0, token = 0;
    require(SafeArrayGetElement(value, &first, &prefix) == S_OK && prefix == 3, "runtime prefix is wrong");
    require(SafeArrayGetElement(value, &second, &token) == S_OK && token > 0, "runtime token is wrong");
    SafeArrayDestroy(value);
    return token;
}
Com<IRawElementProviderSimple> child(IRawElementProviderSimple* parent, std::size_t index) {
    auto fragment = query<IRawElementProviderFragment>(parent);
    IRawElementProviderFragment* value = nullptr;
    require(fragment->Navigate(NavigateDirection_FirstChild, &value) == S_OK && value, "first child missing");
    Com<IRawElementProviderFragment> current(value);
    while (index-- > 0) {
        value = nullptr;
        require(current->Navigate(NavigateDirection_NextSibling, &value) == S_OK && value, "sibling missing");
        current.reset(value);
    }
    return query<IRawElementProviderSimple>(current.get());
}
std::vector<Node> settings() {
    return {
        {1, 0, L"Settings", Kind::button, {20, 20, 80, 30}},
        {2, 0, L"Cài đặt", Kind::pane, {20, 60, 260, 300}},
        {3, 2, L"Reading size", Kind::combo, {30, 70, 180, 30}, true, false, false, false, true},
        {4, 3, L"Reading choices", Kind::list, {30, 100, 180, 60}},
        {5, 4, L"Comfortable", Kind::list_item, {30, 100, 180, 30}, true, false, false, true},
        {6, 4, L"Large", Kind::list_item, {30, 130, 180, 30}},
        {7, 2, L"Language", Kind::group, {30, 170, 180, 30}},
        {8, 7, L"English", Kind::radio, {30, 170, 80, 30}, true, false, false, true},
        {9, 7, L"Tiếng Việt", Kind::radio, {120, 170, 90, 30}},
        {10, 2, L"Appearance", Kind::group, {30, 220, 180, 30}},
        {11, 10, L"Light", Kind::radio, {30, 220, 80, 30}, true, false, false, true},
        {12, 10, L"Dark", Kind::radio, {120, 220, 90, 30}},
        {13, 2, L"Chữ mẫu: chuẩn bị phòng.", Kind::text, {30, 270, 210, 50}},
    };
}
constexpr UINT kDisconnectDuringSend = Bridge::command_message + 1;
constexpr UINT kModelEventUpdate = Bridge::command_message + 2;
std::function<void(WPARAM)> model_event_update;
std::atomic<HRESULT> disconnect_result{S_OK};
LRESULT CALLBACK test_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* bridge = reinterpret_cast<Bridge*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (bridge) {
        LRESULT result = 0;
        if (bridge->handle_getobject(window, message, wparam, lparam, result)) return result;
        if (message == kModelEventUpdate && model_event_update) { model_event_update(wparam); return 0; }
        if (message == kDisconnectDuringSend) {
            if (!wparam) bridge->detach();
            disconnect_result.store(bridge->disconnect_retired());
            return 0;
        }
        if (message == WM_DESTROY) bridge->detach();
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct Fixture {
    HWND window = nullptr;
    std::unique_ptr<Bridge> bridge;
    std::vector<Node> nodes = settings();
    Fixture() {
        WNDCLASSW descriptor{};
        descriptor.lpfnWndProc = test_window_proc;
        descriptor.hInstance = GetModuleHandleW(nullptr);
        descriptor.lpszClassName = L"NSTUSettingsProviderTest";
        require(RegisterClassW(&descriptor) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "test class registration failed");
        window = CreateWindowExW(0, descriptor.lpszClassName, L"Settings provider test", WS_OVERLAPPEDWINDOW,
                                 50, 50, 320, 420, nullptr, nullptr, descriptor.hInstance, nullptr);
        require(window != nullptr, "owned test HWND creation failed");
        bridge = std::make_unique<Bridge>(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(bridge.get()));
        publish();
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }
    ~Fixture() {
        if (bridge) bridge->detach();
        if (window) { SetWindowLongPtrW(window, GWLP_USERDATA, 0); DestroyWindow(window); }
        if (bridge) (void)bridge->disconnect_retired();
    }
    void publish(bool visible = true) {
        // Mirror the product's outer-loop cleanup before admitting another rendered snapshot.
        (void)bridge->disconnect_retired();
        require(bridge->publish(nodes, visible) == S_OK, "valid settings snapshot rejected");
    }
    Com<IRawElementProviderSimple> root() { return Com<IRawElementProviderSimple>(bridge->root_provider()); }
};
template<class Request, class Update>
Command complete_request(Fixture& fixture, Request request, Update update, HRESULT expected = S_OK) {
    std::atomic<bool> done{false};
    std::atomic<HRESULT> result{E_PENDING};
    std::thread caller([&] { result.store(request()); done.store(true); });
    std::vector<Command> commands;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (commands.empty() && !done.load() && std::chrono::steady_clock::now() < deadline) {
        (void)fixture.bridge->take_commands(commands);
        std::this_thread::yield();
    }
    if (commands.size() != 1) { caller.join(); throw std::runtime_error("setter failed to enqueue one completion request"); }
    const Command command = commands.front();
    const HRESULT claimed = fixture.bridge->begin_command(command);
    if (FAILED(claimed)) { caller.join(); throw std::runtime_error("UI dispatcher could not claim the live request"); }
    std::exception_ptr error;
    HRESULT completed = E_FAIL;
    try { update(fixture, done); fixture.publish(); completed = fixture.bridge->complete_command(command, S_OK); }
    catch (...) { error = std::current_exception(); (void)fixture.bridge->complete_command(command, E_FAIL); }
    caller.join();
    if (error) std::rethrow_exception(error);
    require(completed == expected && result.load() == expected, "setter reported success before its real published result");
    return command;
}

void interface_identity_and_real_pattern_matrix() {
    Fixture fixture;
    auto root = fixture.root();
    require(root != nullptr, "published root missing");
    auto button = child(root.get(), 0);
    auto pane = child(root.get(), 1);
    auto combo = child(pane.get(), 0);
    auto group = child(pane.get(), 1);
    auto radio = child(group.get(), 0);
    auto invoke = query<IInvokeProvider>(button.get());
    auto a = query<IUnknown>(button.get());
    auto b = query<IUnknown>(invoke.get());
    require(a.get() == b.get(), "patterns changed canonical COM identity");
    IUnknown* unsupported = reinterpret_cast<IUnknown*>(1);
    require(radio->GetPatternProvider(UIA_InvokePatternId, &unsupported) == S_OK && !unsupported, "radio advertised Invoke");
    require(radio->QueryInterface(__uuidof(IToggleProvider), reinterpret_cast<void**>(&unsupported)) == E_NOINTERFACE && !unsupported,
            "radio advertised Toggle");
    require(combo->GetPatternProvider(UIA_ValuePatternId, &unsupported) == S_OK && !unsupported, "fixed combo advertised editable Value");
    require(combo->GetPatternProvider(UIA_ExpandCollapsePatternId, &unsupported) == S_OK && unsupported, "combo lacks ExpandCollapse");
    unsupported->Release();
    require(root->GetPropertyValue(UIA_NamePropertyId, nullptr) == E_POINTER, "null VARIANT output was accepted");
    require(root->GetPatternProvider(UIA_InvokePatternId, nullptr) == E_POINTER, "null pattern output was accepted");
    VARIANT property{};
    property.vt = VT_I4;
    require(button->GetPropertyValue(UIA_ValueValuePropertyId, &property) == S_OK && property.vt == VT_EMPTY,
            "unsupported property output was not initialized");
}

void unicode_outputs_tree_focus_and_physical_bounds() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    require(name(pane.get()) == L"Cài đặt", "Vietnamese name was corrupted");
    VARIANT owned{};
    require(pane->GetPropertyValue(UIA_NamePropertyId, &owned) == S_OK, "owned name query failed");
    fixture.nodes[1].name = L"Settings";
    fixture.publish();
    require(std::wstring(owned.bstrVal, SysStringLen(owned.bstrVal)) == L"Cài đặt", "published update changed caller-owned BSTR");
    VariantClear(&owned);
    auto button = child(root.get(), 0);
    const LONG before = runtime(button.get());
    fixture.nodes[0].name = L"Cài đặt";
    fixture.nodes[0].bounds = {40, 50, 150, 45};
    fixture.nodes[0].focused = true;
    fixture.publish();
    require(runtime(button.get()) == before, "localization/reflow recreated a continuing node");
    auto fragment = query<IRawElementProviderFragment>(button.get());
    UiaRect bounds{};
    require(fragment->get_BoundingRectangle(&bounds) == S_OK && bounds.left == 40 && bounds.width == 150, "physical bounds are wrong");
    auto root_fragment = query<IRawElementProviderFragment>(root.get());
    SAFEARRAY* root_id = reinterpret_cast<SAFEARRAY*>(1);
    require(root_fragment->GetRuntimeId(&root_id) == S_OK && !root_id, "HWND root invented its runtime ID");
    IRawElementProviderFragment* parent = reinterpret_cast<IRawElementProviderFragment*>(1);
    require(root_fragment->Navigate(NavigateDirection_Parent, &parent) == S_OK && !parent, "root navigated outside its fragment");
    auto root_pattern = query<IRawElementProviderFragmentRoot>(root.get());
    IRawElementProviderFragment* focused = nullptr;
    require(root_pattern->GetFocus(&focused) == S_OK && focused, "real snapshot focus missing");
    Com<IRawElementProviderFragment> focus_owner(focused);
    require(query<IUnknown>(focused).get() == query<IUnknown>(fragment.get()).get(), "selected option substituted for focus");
    IRawElementProviderFragment* hit = nullptr;
    require(root_pattern->ElementProviderFromPoint(50, 55, &hit) == S_OK && hit, "hit test missed actual bounds");
    Com<IRawElementProviderFragment> hit_owner(hit);
    require(query<IUnknown>(hit).get() == query<IUnknown>(fragment.get()).get(), "hit test returned a covered/wrong node");
    require(fragment->SetFocus() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "UI thread tried waiting for its own focus queue");
    const Command focus = complete_request(fixture, [&] { return fragment->SetFocus(); }, [](Fixture& value, const std::atomic<bool>&) { value.nodes[0].focused = true; });
    require(focus.key == 1 && focus.action == Action::focus, "focus did not route through the queue");
}

void selections_are_required_exclusive_and_report_committed_state() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto group = child(pane.get(), 1);
    auto english = child(group.get(), 0);
    auto vietnamese = child(group.get(), 1);
    auto selection = query<ISelectionProvider>(group.get());
    BOOL flag = TRUE;
    require(selection->get_CanSelectMultiple(&flag) == S_OK && !flag, "group allowed multiple selection");
    require(selection->get_IsSelectionRequired(&flag) == S_OK && flag, "group allowed no selection");
    SAFEARRAY* selected = nullptr;
    require(selection->GetSelection(&selected) == S_OK && selected, "selected provider array missing");
    LONG index = 0;
    IUnknown* selected_item = nullptr;
    require(SafeArrayGetElement(selected, &index, &selected_item) == S_OK && selected_item, "selected provider absent");
    Com<IUnknown> selected_owner(selected_item);
    require(name(query<IRawElementProviderSimple>(selected_item).get()) == L"English", "wrong committed selection");
    SafeArrayDestroy(selected);
    auto en = query<ISelectionItemProvider>(english.get());
    auto vi = query<ISelectionItemProvider>(vietnamese.get());
    require(en->RemoveFromSelection() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "required choice was removed");
    require(vi->AddToSelection() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "single choice gained a second selection");
    (void)complete_request(fixture, [&] { return vi->Select(); }, [](Fixture&, const std::atomic<bool>&) {}, static_cast<HRESULT>(UIA_E_INVALIDOPERATION));
    const Command selected_command = complete_request(fixture, [&] { return vi->Select(); }, [&](Fixture& value, const std::atomic<bool>&) {
        flag = TRUE;
        require(vi->get_IsSelected(&flag) == S_OK && !flag, "queued request invented committed state");
        value.nodes[7].selected = false; value.nodes[8].selected = true;
    });
    require(selected_command.key == 9 && selected_command.action == Action::select, "selection targeted an unrelated option");
    require(vi->get_IsSelected(&flag) == S_OK && flag, "published selection was not exposed");
    auto invalid = fixture.nodes;
    invalid[7].selected = true;
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "two selected radio options admitted");
    require(vi->get_IsSelected(&flag) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !flag, "invalid publish retained an obsolete selected state");
    fixture.publish();
    require(vi->get_IsSelected(&flag) == S_OK && flag, "valid publication did not restore the selected state");
    auto combo = child(pane.get(), 0);
    auto expand = query<IExpandCollapseProvider>(combo.get());
    const Command collapsed = complete_request(fixture, [&] { return expand->Collapse(); }, [](Fixture& value, const std::atomic<bool>&) {
        value.nodes[2].expanded = false;
        for (std::size_t index = 3; index <= 5; ++index) value.nodes[index].offscreen = true;
        ++value.nodes[3].generation;
    });
    require(collapsed.action == Action::collapse, "combo collapse bypassed dispatch");
}

void fixed_combo_delegates_its_current_localized_selection() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto combo = child(pane.get(), 0);
    auto selection = query<ISelectionProvider>(combo.get());
    const auto selected_name = [&] {
        SAFEARRAY* selected = nullptr;
        require(selection->GetSelection(&selected) == S_OK && selected, "combo selection array missing");
        LONG lower = 0, upper = 0;
        require(SafeArrayGetLBound(selected, 1, &lower) == S_OK && SafeArrayGetUBound(selected, 1, &upper) == S_OK && lower == upper,
                "combo did not delegate exactly one selected option");
        IUnknown* option = nullptr;
        require(SafeArrayGetElement(selected, &lower, &option) == S_OK && option, "combo selected option missing");
        SafeArrayDestroy(selected);
        Com<IUnknown> owner(option);
        return name(query<IRawElementProviderSimple>(option).get());
    };
    require(selected_name() == L"Comfortable", "expanded combo selected the wrong option");
    fixture.nodes[2].expanded = false;
    for (std::size_t index = 3; index <= 5; ++index) fixture.nodes[index].offscreen = true;
    fixture.publish();
    require(selected_name() == L"Comfortable", "collapsed combo lost its selected option");
    fixture.nodes[4].selected = false;
    fixture.nodes[5].selected = true;
    fixture.nodes[5].name = L"Chữ lớn";
    fixture.publish();
    require(selected_name() == L"Chữ lớn" && name(combo.get()) == L"Reading size", "combo selection did not report the current localized choice separately from its name");
}

void complete_512_node_tree_rejects_513_without_truncation() {
    Fixture fixture;
    fixture.nodes.clear();
    for (Key key = 1000; key < 1512; ++key) fixture.nodes.push_back({key, 0, L"Capacity node", Kind::text, {20, 20, 10, 10}});
    fixture.publish();
    auto root = fixture.root();
    auto last = child(root.get(), 511);
    require(name(last.get()) == L"Capacity node", "512-node tree silently omitted its final node");
    fixture.nodes.push_back({1512, 0, L"Excess node", Kind::text, {20, 20, 10, 10}});
    require(fixture.bridge->publish(fixture.nodes, true) == E_INVALIDARG, "513-node tree exceeded the admitted bound");
    VARIANT rejected{};
    require(last->GetPropertyValue(UIA_NamePropertyId, &rejected) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && rejected.vt == VT_EMPTY,
            "excess tree left the obsolete snapshot exposed");
    require(fixture.bridge->root_provider() == nullptr, "excess tree kept its obsolete root exposed");
    fixture.nodes.pop_back(); fixture.publish();
    require(name(last.get()) == L"Capacity node", "valid publication did not restore the complete snapshot");
}

void typed_contests_choices_and_nonselecting_453_node_view() {
    Fixture fixture;
    fixture.nodes = {
        {1000, 0, L"Workspace", Kind::tab, {20, 20, 180, 30}},
        {1001, 1000, L"Setup", Kind::tab_item, {20, 20, 80, 30}, true, false, false, true},
        {1002, 1000, L"Participants", Kind::tab_item, {100, 20, 100, 30}},
        {1003, 0, L"Publish public material", Kind::checkbox, {20, 60, 180, 30}},
        {1004, 0, L"Verification details", Kind::disclosure, {20, 100, 180, 30}, true, false, false, false, true},
    };
    fixture.publish();
    auto root = fixture.root();
    auto tab = child(root.get(), 0);
    auto setup = child(tab.get(), 0), participants = child(tab.get(), 1);
    auto tab_selection = query<ISelectionProvider>(tab.get());
    BOOL required = FALSE;
    require(tab_selection->get_IsSelectionRequired(&required) == S_OK && required, "Contests tabs did not require one selected workspace");
    const auto selected = complete_request(fixture, [&] { return query<ISelectionItemProvider>(participants.get())->Select(); },
        [](Fixture& value, const std::atomic<bool>&) { value.nodes[1].selected = false; value.nodes[2].selected = true; });
    require(selected.key == 1002 && query<ISelectionItemProvider>(setup.get())->RemoveFromSelection() == S_OK, "tab selection targeted a different item");
    IUnknown* invoke = reinterpret_cast<IUnknown*>(1);
    require(participants->GetPatternProvider(UIA_InvokePatternId, &invoke) == S_OK && !invoke, "tab item incorrectly advertised Invoke");
    auto policy = child(root.get(), 1);
    const auto toggled = complete_request(fixture, [&] { return query<IToggleProvider>(policy.get())->Toggle(); },
        [](Fixture& value, const std::atomic<bool>&) { value.nodes[3].checked = true; });
    require(toggled.action == Action::toggle && toggled.target_checked, "publication toggle lost its guarded target state");
    ToggleState state = ToggleState_Off;
    require(query<IToggleProvider>(policy.get())->get_ToggleState(&state) == S_OK && state == ToggleState_On, "toggle success preceded the published checked state");
    auto disclosure = child(root.get(), 2);
    complete_request(fixture, [&] { return query<IExpandCollapseProvider>(disclosure.get())->Collapse(); },
        [](Fixture& value, const std::atomic<bool>&) { value.nodes[4].expanded = false; });
    require(disclosure->GetPatternProvider(UIA_SelectionPatternId, &invoke) == S_OK && !invoke, "disclosure inherited radio-group selection");
    fixture.nodes.clear();
    for (Key key = 1000; key < 1128; ++key) fixture.nodes.push_back({key, 0, L"Static shell", Kind::text, {20, 20, 10, 10}});
    for (Key row = 0; row < 64; ++row) {
        const Key key = 2000 + row * 5;
        fixture.nodes.push_back({key, 0, L"Student row", Kind::row, {20, 40, 180, 30}});
        for (Key value = 1; value < 4; ++value) fixture.nodes.push_back({key + value, key, L"Public row value", Kind::text, {20, 40, 30, 30}});
        fixture.nodes.push_back({key + 4, key, L"Compare", Kind::button, {120, 40, 80, 30}});
    }
    for (Key key = 3000; key < 3005; ++key) fixture.nodes.push_back({key, 0, L"Active comparison", Kind::text, {20, 20, 10, 10}});
    require(fixture.nodes.size() == 453, "populated view fixture does not exercise the admitted 64-row budget");
    fixture.publish();
    auto first_row = child(root.get(), 128);
    require(first_row->GetPatternProvider(UIA_SelectionPatternId, &invoke) == S_OK && !invoke, "ordinary student row advertised selection");
    auto last_row = child(root.get(), 191);
    auto last_action = child(last_row.get(), 3);
    require(name(last_action.get()) == L"Compare", "64-row current view omitted the last student's action");
    VARIANT automation_id{};
    require(last_action->GetPropertyValue(UIA_AutomationIdPropertyId, &automation_id) == S_OK && automation_id.vt == VT_BSTR &&
            std::wstring_view(automation_id.bstrVal) == L"nstu.contests.2319", "Contests IDs reused the Settings namespace");
    VariantClear(&automation_id);
}

TextData unicode_edit_snapshot() {
    TextData text;
    text.value = L"A\u0302\u0301 \U0001F600 Việt";
    text.utf8_capacity = 97; text.selection_start = 4; text.selection_end = text.caret = 6;
    text.clip = {35, 54, 50, 26};
    text.glyphs = {{0,1,{30,60,8,20}}, {1,2,{32,57,4,4}}, {2,3,{33,54,4,4}}, {3,4,{38,60,5,20}},
                   {4,6,{43,60,18,20}}, {6,7,{61,60,5,20}}, {7,8,{66,60,11,20}}, {8,9,{77,60,4,20}},
                   {9,10,{81,60,10,20}}, {10,11,{91,60,6,20}}};
    text.carets = {{0,{30,60,1,20}}, {1,{38,60,1,20}}, {2,{38,60,1,20}}, {3,{38,60,1,20}},
                   {4,{43,60,1,20}}, {6,{61,60,1,20}}, {7,{66,60,1,20}}, {8,{77,60,1,20}},
                   {9,{81,60,1,20}}, {10,{91,60,1,20}}, {11,{97,60,1,20}}};
    return text;
}
std::wstring range_text(ITextRangeProvider* range, int limit = -1) {
    BSTR text = nullptr;
    require(range->GetText(limit, &text) == S_OK && text, "text range content retrieval failed");
    const std::wstring result(text, SysStringLen(text)); SysFreeString(text); return result;
}
Com<ITextRangeProvider> first_text_range(SAFEARRAY* ranges) {
    require(ranges != nullptr, "text range array missing");
    LONG lower = 0, upper = 0;
    require(SafeArrayGetLBound(ranges, 1, &lower) == S_OK && SafeArrayGetUBound(ranges, 1, &upper) == S_OK && lower == upper,
            "single-line text did not return one range");
    IUnknown* first = nullptr;
    require(SafeArrayGetElement(ranges, &lower, &first) == S_OK && first, "text range array entry missing");
    SafeArrayDestroy(ranges);
    Com<IUnknown> owner(first);
    return query<ITextRangeProvider>(first);
}
std::vector<double> range_rectangles(ITextRangeProvider* range) {
    SAFEARRAY* rectangles = nullptr;
    require(range->GetBoundingRectangles(&rectangles) == S_OK && rectangles, "text range bounds array missing");
    LONG lower = 0, upper = -1;
    require(SafeArrayGetLBound(rectangles, 1, &lower) == S_OK && SafeArrayGetUBound(rectangles, 1, &upper) == S_OK, "text bounds array dimensions invalid");
    std::vector<double> values;
    for (LONG index = lower; index <= upper; ++index) {
        double value = 0; require(SafeArrayGetElement(rectangles, &index, &value) == S_OK, "text bound coordinate missing"); values.push_back(value);
    }
    SafeArrayDestroy(rectangles); return values;
}
void single_line_ranges_use_unicode_and_published_clipped_geometry() {
    Fixture fixture;
    fixture.nodes = {{4000,0,L"Existing content scroller",Kind::row,{0,0,400,300}},
                     {4001,4000,L"Contest title",Kind::edit,{20,45,110,40},true,false,true}};
    fixture.nodes[0].scrollable = true;
    fixture.nodes[1].scroll_owner = 4000; fixture.nodes[1].text = unicode_edit_snapshot();
    fixture.publish();
    auto root = fixture.root(), scroller = child(root.get(), 0), edit = child(scroller.get(), 0);
    auto value = query<IValueProvider>(edit.get());
    auto text = query<ITextProvider2>(edit.get());
    require(query<IUnknown>(value.get()).get() == query<IUnknown>(text.get()).get(), "Value/Text broke canonical element identity");
    ITextRangeProvider* raw = nullptr;
    require(text->get_DocumentRange(&raw) == S_OK && raw, "edit DocumentRange missing");
    Com<ITextRangeProvider> document(raw);
    require(range_text(document.get()) == fixture.nodes[1].text->value, "DocumentRange lost combining or supplementary text");
    require(range_rectangles(document.get()) == std::vector<double>({35,54,50,26}), "document bounds fabricated full-field or unclipped geometry");
    SAFEARRAY* ranges = nullptr;
    require(text->GetSelection(&ranges) == S_OK, "actual text selection query failed");
    auto selection = first_text_range(ranges);
    require(range_text(selection.get()) == L"\U0001F600" && range_rectangles(selection.get()) == std::vector<double>({43,60,18,20}),
            "selection split a surrogate pair or invented substring bounds");
    require(text->GetVisibleRanges(&ranges) == S_OK, "visible text range query failed");
    auto visible = first_text_range(ranges);
    require(range_text(visible.get()) == L"A\u0302\u0301 \U0001F600 Việ", "visible range ignored actual glyph clipping");
    BOOL active = FALSE;
    require(text->GetCaretRange(&active, &raw) == S_OK && active && raw, "actual focused caret range missing");
    Com<ITextRangeProvider> caret(raw);
    require(range_text(caret.get()).empty() && range_rectangles(caret.get()).empty(), "degenerate caret fabricated text or a line rectangle");
    require(text->RangeFromPoint({61,70}, &raw) == S_OK && raw, "published caret hit test failed");
    Com<ITextRangeProvider> hit(raw);
    BOOL equal = FALSE;
    require(caret->Compare(hit.get(), &equal) == S_OK && equal, "hit test disagreed with the real caret coordinate");
    require(text->RangeFromPoint({50,70}, &raw) == S_OK && raw, "variable-width glyph hit test failed");
    Com<ITextRangeProvider> emoji(raw);
    require(emoji->ExpandToEnclosingUnit(TextUnit_Character) == S_OK && range_text(emoji.get()) == L"\U0001F600", "Character expansion split the supplementary glyph");
    require(document->Clone(&raw) == S_OK && raw, "range clone failed");
    Com<ITextRangeProvider> moved(raw);
    require(moved->MoveEndpointByRange(TextPatternRangeEndpoint_End, document.get(), TextPatternRangeEndpoint_Start) == S_OK,
            "endpoint move could not form a start caret");
    require(moved->ExpandToEnclosingUnit(TextUnit_Character) == S_OK && range_text(moved.get()) == L"A\u0302\u0301", "Character expansion separated Vietnamese stacked marks");
    int actual = 0;
    require(moved->Move(TextUnit_Character, 2, &actual) == S_OK && actual == 2 && range_text(moved.get()) == L"\U0001F600",
            "Unicode Character movement split marks or surrogate units");
    BSTR needle = SysAllocString(L"việt");
    require(document->FindText(needle, FALSE, TRUE, &raw) == S_OK && raw, "case-insensitive Vietnamese search failed"); SysFreeString(needle);
    Com<ITextRangeProvider> found(raw);
    require(range_text(found.get()) == L"Việt" && range_rectangles(found.get()) == std::vector<double>({66,60,19,20}),
            "search range did not preserve offscreen text and visible clipped bounds");
    VARIANT attribute{};
    require(found->GetAttributeValue(UIA_IsHiddenAttributeId, &attribute) == S_OK && attribute.vt == VT_UNKNOWN, "partially hidden range did not return Mixed attribute sentinel");
    IUnknown* mixed = nullptr; require(UiaGetReservedMixedAttributeValue(&mixed) == S_OK && mixed, "Mixed sentinel missing");
    Com<IUnknown> mixed_owner(mixed);
    require(query<IUnknown>(attribute.punkVal).get() == query<IUnknown>(mixed).get(), "partial visibility returned a fabricated attribute"); VariantClear(&attribute);
    require(found->GetAttributeValue(UIA_FontNameAttributeId, &attribute) == S_OK && attribute.vt == VT_UNKNOWN, "unsupported font attribute was fabricated");
    IUnknown* unsupported = nullptr; require(UiaGetReservedNotSupportedValue(&unsupported) == S_OK && unsupported, "NotSupported sentinel missing");
    Com<IUnknown> unsupported_owner(unsupported);
    require(query<IUnknown>(attribute.punkVal).get() == query<IUnknown>(unsupported).get(), "unsupported attribute did not return its reserved sentinel"); VariantClear(&attribute);
    IRawElementProviderSimple* enclosing = nullptr;
    require(found->GetEnclosingElement(&enclosing) == S_OK && enclosing, "range enclosing element missing");
    Com<IRawElementProviderSimple> enclosing_owner(enclosing);
    require(query<IUnknown>(enclosing).get() == query<IUnknown>(edit.get()).get(), "range enclosing element changed ownership");
    require(text->RangeFromChild(root.get(), &raw) == E_INVALIDARG && !raw, "single-line edit fabricated embedded child text");
    require(found->AddToSelection() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION) && found->RemoveFromSelection() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION),
            "single-selection edit accepted multiple range selection");
    fixture.nodes[1].offscreen = true; fixture.publish();
    require(range_text(found.get()) == L"Việt" && range_rectangles(found.get()).empty(), "offscreen query lost text or invented visible bounds");
    fixture.bridge->detach();
    BSTR stale = reinterpret_cast<BSTR>(1);
    require(found->GetText(-1, &stale) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !stale, "retained range read detached text");
}

void middle_of_clipped_wide_glyph_maps_to_an_actual_caret() {
    Fixture fixture;
    fixture.nodes = {{4001,0,L"Horizontally scrolled edit",Kind::edit,{0,25,40,30}}};
    TextData text; text.value = L"\U0001F600"; text.utf8_capacity = 97; text.clip = {12,30,6,20};
    text.glyphs = {{0,2,{0,30,30,20}}}; text.carets = {{0,{0,30,1,20}},{2,{30,30,1,20}}};
    fixture.nodes[0].text = text; fixture.publish();
    auto root = fixture.root(), edit = child(root.get(),0); auto provider = query<ITextProvider2>(edit.get());
    ITextRangeProvider* raw = nullptr;
    require(provider->RangeFromPoint({14,40},&raw) == S_OK && raw, "visible middle of a clipped wide glyph could not resolve either actual caret");
    Com<ITextRangeProvider> hit(raw);
    require(hit->ExpandToEnclosingUnit(TextUnit_Character) == S_OK && range_text(hit.get()) == text.value &&
            range_rectangles(hit.get()) == std::vector<double>({12,30,6,20}), "clipped glyph hit invented a position or unclipped geometry");
}

struct ForeignTextRange final : ITextRangeProvider {
    std::atomic<ULONG> references{1};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ITextRangeProvider)) return E_NOINTERFACE;
        *output = static_cast<ITextRangeProvider*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return references.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { const auto left = references.fetch_sub(1) - 1; if (!left) { const std::unique_ptr<ForeignTextRange> self(this); } return left; }
    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider*, BOOL*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint, ITextRangeProvider*, TextPatternRangeEndpoint, int*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL, ITextRangeProvider**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindText(BSTR, BOOL, BOOL, ITextRangeProvider**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID, VARIANT*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetText(int, BSTR*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Move(TextUnit, int, int*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint, TextUnit, int, int*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint, ITextRangeProvider*, TextPatternRangeEndpoint) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Select() noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddToSelection() noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY**) noexcept override { return E_NOTIMPL; }
};
void ranges_reject_foreign_owners_invalid_points_and_same_text_revisions() {
    Fixture fixture;
    fixture.nodes = {{4100,0,L"Fields",Kind::row,{0,0,400,300}},
                     {4101,4100,L"Duplicate title",Kind::edit,{20,45,110,40}},
                     {4102,4100,L"Duplicate title",Kind::edit,{20,45,110,40}}};
    fixture.nodes[1].text = fixture.nodes[2].text = unicode_edit_snapshot(); fixture.publish();
    auto root = fixture.root(), fields = child(root.get(), 0), first = child(fields.get(), 0), second = child(fields.get(), 1);
    auto first_text = query<ITextProvider>(first.get()), second_text = query<ITextProvider>(second.get());
    SupportedTextSelection supported = SupportedTextSelection_None;
    require(first_text->get_SupportedTextSelection(&supported) == S_OK && supported == SupportedTextSelection_Single, "edit did not report implemented single selection");
    auto text2 = query<ITextProvider2>(first.get());
    ITextRangeProvider* failed_output = reinterpret_cast<ITextRangeProvider*>(1);
    require(text2->GetCaretRange(nullptr, &failed_output) == E_POINTER && !failed_output, "caret failure left a valid output pointer uninitialized");
    BOOL failed_active = TRUE;
    require(text2->GetCaretRange(&failed_active, nullptr) == E_POINTER && !failed_active, "caret failure left activity uninitialized");
    ITextRangeProvider* raw = nullptr;
    require(first_text->get_DocumentRange(&raw) == S_OK && raw, "first owner range missing"); Com<ITextRangeProvider> range(raw);
    require(second_text->get_DocumentRange(&raw) == S_OK && raw, "second owner range missing"); Com<ITextRangeProvider> other(raw);
    BOOL equal = TRUE; int distance = 99;
    require(range->Compare(other.get(), &equal) == E_INVALIDARG && !equal, "duplicate text/bounds bypassed range owner validation");
    require(range->CompareEndpoints(TextPatternRangeEndpoint_Start, other.get(), TextPatternRangeEndpoint_Start, &distance) == E_INVALIDARG && distance == 0,
            "foreign owner endpoint comparison inferred a text position");
    auto foreign_allocation = std::make_unique<ForeignTextRange>(); Com<ForeignTextRange> foreign(foreign_allocation.release());
    require(range->Compare(foreign.get(), &equal) == E_INVALIDARG && !equal, "foreign COM implementation was cast to a native range");
    require(range->MoveEndpointByRange(TextPatternRangeEndpoint_Start, foreign.get(), TextPatternRangeEndpoint_End) == E_INVALIDARG,
            "foreign range endpoints bypassed typed peer validation");
    raw = reinterpret_cast<ITextRangeProvider*>(1);
    require(first_text->RangeFromPoint({std::numeric_limits<double>::quiet_NaN(),70}, &raw) == E_INVALIDARG && !raw,
            "NaN text hit-test coordinates were accepted");
    require(first_text->RangeFromPoint({0,70}, &raw) == E_INVALIDARG && !raw, "outside viewport hit test fabricated a caret");
    BSTR contents = reinterpret_cast<BSTR>(1);
    require(range->GetText(-2, &contents) == E_INVALIDARG && !contents, "invalid text length returned an uninitialized BSTR");
    require(range_text(range.get(), 1).empty() && range_text(range.get(), 5) == L"A\u0302\u0301 ", "text length truncation split a cluster or surrogate pair");
    const wchar_t embedded[]{L'A',0,L'B'}; BSTR needle = SysAllocStringLen(embedded, 3);
    require(range->FindText(needle, FALSE, FALSE, &raw) == E_INVALIDARG && !raw, "embedded-NUL range search crossed the text boundary"); SysFreeString(needle);
    auto invalid = fixture.nodes; invalid[1].text->selection_start = 5;
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "selection endpoint split a UTF16 surrogate pair");
    invalid = fixture.nodes; invalid[1].text->carets[5].offset = 5;
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "invalid caret geometry entered the snapshot");
    invalid = fixture.nodes; invalid[1].text->clip.width = std::numeric_limits<double>::infinity();
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "nonfinite text clipping geometry entered the snapshot");
    ITextRangeProvider* rejected_range = reinterpret_cast<ITextRangeProvider*>(1);
    require(range->Clone(&rejected_range) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !rejected_range,
            "invalid geometry retained readable obsolete ranges");
    fixture.publish();
    require(range->Clone(&raw) == S_OK && raw, "same-owner clone missing"); Com<ITextRangeProvider> moved(raw);
    require(moved->MoveEndpointByRange(TextPatternRangeEndpoint_End, range.get(), TextPatternRangeEndpoint_Start) == S_OK,
            "same-owner endpoint movement failed");
    require(moved->ExpandToEnclosingUnit(TextUnit_Word) == S_OK && range_text(moved.get()) == L"A\u0302\u0301 ", "word boundaries lost combining marks or trailing spacing");
    require(moved->Move(TextUnit_Word, 1, &distance) == S_OK && distance == 1 && range_text(moved.get()) == L"\U0001F600 ", "word movement split the supplementary token");
    require(moved->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, std::numeric_limits<int>::min(), &distance) == S_OK && distance < 0 && range_text(moved.get()).empty(),
            "negative endpoint count overflowed or failed to collapse crossing endpoints");
    ++fixture.nodes[1].text->revision; fixture.publish();
    require(range->GetText(-1, &contents) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !contents, "same-text replacement revision revived stale range text");
    require(range->Select() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "stale same-text range queued old endpoints");
    require(first_text->get_DocumentRange(&raw) == S_OK && raw, "new revision range missing"); Com<ITextRangeProvider> current(raw);
    require(range_text(current.get()) == fixture.nodes[1].text->value, "same-text revision changed current text contents");
}
TextData bmp_edit_snapshot(std::wstring value, std::uint64_t revision, std::size_t capacity = 97, TextInput input = TextInput::plain) {
    TextData text; text.value = std::move(value); text.revision = revision; text.utf8_capacity = capacity; text.input = input;
    text.clip = {30,50,90,30}; text.selection_start = text.selection_end = text.caret = static_cast<LONG>(text.value.size());
    double x = 30; text.carets.push_back({0,{x,60,1,20}});
    for (LONG offset = 0; static_cast<std::size_t>(offset) < text.value.size(); ++offset) {
        const double width = 5.0 + offset % 4;
        text.glyphs.push_back({offset,offset+1,{x,60,width,20}}); x += width;
        text.carets.push_back({offset+1,{x,60,1,20}});
    }
    return text;
}
void value_and_range_commands_commit_exact_state_and_preserve_guards() {
    Fixture fixture;
    fixture.nodes = {{4200,0,L"Existing scroller",Kind::row,{0,0,400,300}}, {4201,4200,L"Title",Kind::edit,{20,45,110,40}}};
    fixture.nodes[0].scrollable = true; fixture.nodes[1].scroll_owner = 4200; fixture.nodes[1].text = unicode_edit_snapshot(); fixture.publish();
    auto root = fixture.root(), scroller = child(root.get(), 0), edit = child(scroller.get(), 0);
    auto value = query<IValueProvider>(edit.get()); auto text = query<ITextProvider>(edit.get());
    ITextRangeProvider* raw = nullptr; require(text->get_DocumentRange(&raw) == S_OK && raw, "value fixture document range missing");
    Com<ITextRangeProvider> before(raw);
    const auto changed = complete_request(fixture, [&] { return value->SetValue(L"Việt"); },
        [](Fixture& current, const std::atomic<bool>&) { current.nodes[1].text = bmp_edit_snapshot(L"Việt", 2); });
    require(changed.action == Action::set_value && changed.text_revision == 1 && changed.value == L"Việt", "copied Value payload lost revision or Unicode");
    BSTR output = nullptr; require(value->get_Value(&output) == S_OK && output && std::wstring_view(output) == L"Việt", "SetValue success preceded its actual published value"); SysFreeString(output);
    require(before->GetText(-1, &output) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !output, "value edit left old ranges readable");
    complete_request(fixture, [&] { return value->SetValue(L"Việt"); }, [](Fixture& current, const std::atomic<bool>&) { ++current.nodes[1].text->revision; });
    complete_request(fixture, [&] { return value->SetValue(L"Not applied"); }, [](Fixture&, const std::atomic<bool>&) {}, static_cast<HRESULT>(UIA_E_INVALIDOPERATION));
    const wchar_t bad_surrogate[]{static_cast<wchar_t>(0xD800),0};
    require(value->SetValue(bad_surrogate) == E_INVALIDARG, "Value setter admitted malformed UTF16");
    const std::wstring too_many_bytes(49, L'é');
    require(value->SetValue(too_many_bytes.c_str()) == E_INVALIDARG, "UTF8 byte capacity was confused with UTF16 length");
    fixture.nodes[1].read_only = true; fixture.publish();
    require(value->SetValue(L"Forbidden") == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "read-only edit queued a Value setter");
    fixture.nodes[1].read_only = false; fixture.nodes[1].enabled = false; fixture.publish();
    require(value->SetValue(L"Forbidden") == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED), "disabled edit queued a Value setter");
    fixture.nodes[1].enabled = true; fixture.nodes[1].text = unicode_edit_snapshot(); fixture.nodes[1].text->revision = 4; fixture.publish();
    require(text->get_DocumentRange(&raw) == S_OK && raw, "selection fixture document missing"); Com<ITextRangeProvider> document(raw);
    BSTR needle = SysAllocString(L"Việt"); require(document->FindText(needle, FALSE, FALSE, &raw) == S_OK && raw, "selection target range missing"); SysFreeString(needle);
    Com<ITextRangeProvider> selection(raw);
    const auto selected = complete_request(fixture, [&] { return selection->Select(); }, [](Fixture& current, const std::atomic<bool>&) {
        current.nodes[1].text->selection_start = 7; current.nodes[1].text->selection_end = current.nodes[1].text->caret = 11;
    });
    require(selected.action == Action::text_select && selected.range_start == 7 && selected.range_end == 11 && selected.text_revision == 4,
            "text selection queued unbounded or unrelated endpoints");
    const auto scrolled = complete_request(fixture, [&] { return document->ScrollIntoView(FALSE); }, [](Fixture& current, const std::atomic<bool>&) {
        for (auto& glyph : current.nodes[1].text->glyphs) glyph.bounds.left -= 20;
        for (auto& caret : current.nodes[1].text->carets) caret.bounds.left -= 20;
    });
    require(scrolled.action == Action::scroll && !scrolled.align_to_top && scrolled.range_end == 11, "text scroll lost the actual target endpoint");
    fixture.nodes[1].offscreen = true; fixture.publish();
    complete_request(fixture, [&] { return query<IScrollItemProvider>(edit.get())->ScrollIntoView(); },
        [](Fixture& current, const std::atomic<bool>&) { current.nodes[1].offscreen = false; });
    fixture.nodes[1].covered = true; fixture.publish();
    require(query<IScrollItemProvider>(edit.get())->ScrollIntoView() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "modal-covered field bypassed the scroll guard");
    require(selection->Select() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "covered field accepted range mutation");
    fixture.nodes[1].covered = false; fixture.nodes[1].hidden = true; fixture.publish();
    require(query<IScrollItemProvider>(edit.get())->ScrollIntoView() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "hidden disclosure content bypassed the scroll guard");
    fixture.nodes[1].hidden = false; fixture.nodes[1].text = bmp_edit_snapshot(L"-", 5, 33, TextInput::integer); fixture.publish();
    require(value->SetValue(L"2147483648") == E_INVALIDARG && value->SetValue(L"1x") == E_INVALIDARG, "integer setter accepted overflow or partial input");
    const auto canonical_one = complete_request(fixture, [&] { return value->SetValue(L"+01"); }, [](Fixture& current, const std::atomic<bool>&) {
        current.nodes[1].text = bmp_edit_snapshot(L"1", 6, 33, TextInput::integer);
    });
    require(canonical_one.value == L"1", "integer +01 did not queue its explicit canonical request");
    const auto canonical_twelve = complete_request(fixture, [&] { return value->SetValue(L"000012"); }, [](Fixture& current, const std::atomic<bool>&) {
        current.nodes[1].text = bmp_edit_snapshot(L"12", 7, 33, TextInput::integer);
    });
    require(canonical_twelve.value == L"12", "leading-zero integer did not queue its explicit canonical request");
    complete_request(fixture, [&] { return value->SetValue(L"-1"); }, [](Fixture& current, const std::atomic<bool>&) {
        current.nodes[1].text = bmp_edit_snapshot(L"-1", 8, 33, TextInput::integer);
    });
    fixture.nodes[1].text = bmp_edit_snapshot(L"", 9, 7, TextInput::digits); fixture.publish();
    require(value->SetValue(L"1234567") == E_INVALIDARG && value->SetValue(L"12é") == E_INVALIDARG, "comparison setter accepted excess or non-ASCII digits");
    complete_request(fixture, [&] { return value->SetValue(L"123456"); }, [](Fixture& current, const std::atomic<bool>&) {
        current.nodes[1].text = bmp_edit_snapshot(L"123456", 10, 7, TextInput::digits);
    });
    auto secret = fixture.nodes; secret[1].protected_value = true;
    require(fixture.bridge->publish(secret, true) == E_INVALIDARG, "protected value entered a text snapshot");
    fixture.nodes[1].protected_value = true; fixture.nodes[1].text.reset(); fixture.publish();
    auto protected_edit = child(scroller.get(), 0); IUnknown* pattern = reinterpret_cast<IUnknown*>(1);
    require(protected_edit->GetPatternProvider(UIA_ValuePatternId, &pattern) == S_OK && !pattern &&
            protected_edit->GetPatternProvider(UIA_TextPatternId, &pattern) == S_OK && !pattern, "protected edit exposed Value/Text patterns");
    VARIANT suppressed{};
    require(protected_edit->GetPropertyValue(UIA_ValueValuePropertyId, &suppressed) == S_OK && suppressed.vt == VT_UNKNOWN,
            "protected Value fallback was not explicitly suppressed"); VariantClear(&suppressed);
    require(value->get_Value(&output) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !output,
            "retained old Value interface read replacement protected data");
}

void claimed_value_does_not_certify_a_replacement_incarnation() {
    Fixture fixture;
    fixture.nodes = {{4301,0,L"Title",Kind::edit,{20,45,110,40}}};
    fixture.nodes[0].text = bmp_edit_snapshot(L"Old", 1); fixture.publish();
    auto root = fixture.root(), edit = child(root.get(), 0);
    auto value = query<IValueProvider>(edit.get());
    complete_request(fixture, [&] { return value->SetValue(L"Replacement"); }, [](Fixture& current, const std::atomic<bool>&) {
        ++current.nodes[0].generation;
        current.nodes[0].text = bmp_edit_snapshot(L"Replacement", 2);
    }, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
}

void timeout_cancels_unclaimed_work_and_claimed_commit_cannot_false_timeout() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto group = child(pane.get(), 1);
    auto choice = child(group.get(), 1);
    auto selection = query<ISelectionItemProvider>(choice.get());
    std::atomic<bool> done{false};
    std::atomic<HRESULT> result{E_PENDING};
    std::thread caller([&] { result.store(selection->Select()); done.store(true); });
    std::vector<Command> commands;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (commands.empty() && !done.load() && std::chrono::steady_clock::now() < deadline) {
        (void)fixture.bridge->take_commands(commands); std::this_thread::yield();
    }
    caller.join();
    require(commands.size() == 1 && result.load() == static_cast<HRESULT>(UIA_E_TIMEOUT), "unclaimed setter failed to time out");
    require(FAILED(fixture.bridge->begin_command(commands[0])), "timed-out request was claimed for a late mutation");
    BOOL selected = TRUE;
    require(selection->get_IsSelected(&selected) == S_OK && !selected, "timeout mutated the choice");
    (void)complete_request(fixture, [&] { return selection->Select(); }, [](Fixture& value, const std::atomic<bool>& finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        require(!finished.load(), "claimed action reported a false timeout before committing");
        value.nodes[7].selected = false; value.nodes[8].selected = true;
    });
    require(selection->get_IsSelected(&selected) == S_OK && selected, "claimed selection failed to commit");
}

void selected_popup_option_may_close_while_completion_stays_truthful() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto combo = child(pane.get(), 0);
    auto list = child(combo.get(), 0);
    auto large = child(list.get(), 1);
    auto option = query<ISelectionItemProvider>(large.get());
    const LONG old = runtime(large.get());
    const Command chosen = complete_request(fixture, [&] { return option->Select(); }, [](Fixture& value, const std::atomic<bool>&) {
        value.nodes[4].selected = false; value.nodes[5].selected = true;
        value.nodes[2].expanded = false;
        for (std::size_t index = 3; index <= 5; ++index) value.nodes[index].offscreen = true;
        ++value.nodes[3].generation;
    });
    require(chosen.key == 6, "popup selection changed an unrelated option");
    BOOL selected = FALSE;
    require(option->get_IsSelected(&selected) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "closed popup revived its old option provider");
    auto current_list = child(combo.get(), 0);
    auto current_large = child(current_list.get(), 1);
    require(runtime(current_large.get()) != old, "popup generation did not create a new incarnation");
    auto current = query<ISelectionItemProvider>(current_large.get());
    require(current->get_IsSelected(&selected) == S_OK && selected, "committed hidden selection was lost");
    require(current->Select() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "hidden current popup option became actionable");
    fixture.nodes[2].expanded = true;
    for (std::size_t index = 3; index <= 5; ++index) fixture.nodes[index].offscreen = false;
    ++fixture.nodes[3].generation;
    fixture.publish();
    require(current->get_IsSelected(&selected) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "reopened popup revived retained hidden references");
}

void inherited_disabled_state_and_clipped_popup_hits_are_truthful() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto language = child(pane.get(), 1);
    auto english = child(language.get(), 0);
    auto select = query<ISelectionItemProvider>(english.get());
    fixture.nodes[6].enabled = false;
    fixture.publish();
    require(select->Select() == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED), "disabled group allowed a child action");
    VARIANT value{};
    require(english->GetPropertyValue(UIA_IsEnabledPropertyId, &value) == S_OK && value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE,
            "child ignored its ancestor's disabled state");
    fixture.nodes[6].enabled = true;
    fixture.nodes[0].hit_bounds = UiaRect{20, 20, 0, 0};
    fixture.nodes[5].bounds = {20, 20, 80, 30};
    fixture.nodes[5].layer = 1;
    fixture.publish();
    auto fragment_root = query<IRawElementProviderFragmentRoot>(root.get());
    IRawElementProviderFragment* hit = nullptr;
    require(fragment_root->ElementProviderFromPoint(30, 25, &hit) == S_OK && hit, "visible popup hit missing");
    Com<IRawElementProviderFragment> hit_owner(hit);
    require(name(query<IRawElementProviderSimple>(hit).get()) == L"Large", "covered launcher replaced the top popup hit");
    auto launcher = child(root.get(), 0);
    auto invoke = query<IInvokeProvider>(launcher.get());
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "fully clipped launcher queued an action");
    auto launcher_fragment = query<IRawElementProviderFragment>(launcher.get());
    UiaRect full{};
    require(launcher_fragment->get_BoundingRectangle(&full) == S_OK && full.width == 80, "clipping changed the full bounds");
}

void rejected_or_detached_completion_wakes_the_waiting_caller() {
    Fixture fixture;
    auto root = fixture.root();
    auto pane = child(root.get(), 1);
    auto group = child(pane.get(), 1);
    auto choice = child(group.get(), 1);
    auto selection = query<ISelectionItemProvider>(choice.get());
    for (const bool detach : {false, true}) {
        std::atomic<bool> done{false};
        std::atomic<HRESULT> result{E_PENDING};
        std::thread caller([&] { result.store(selection->Select()); done.store(true); });
        std::vector<Command> commands;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (commands.empty() && !done.load() && std::chrono::steady_clock::now() < deadline) {
            (void)fixture.bridge->take_commands(commands); std::this_thread::yield();
        }
        if (commands.size() != 1) { caller.join(); throw std::runtime_error("waiting cancellation fixture did not enqueue"); }
        const HRESULT rejected = detach ? fixture.bridge->detach() : fixture.bridge->complete_command(commands[0], E_ACCESSDENIED);
        caller.join();
        require(rejected == (detach ? S_OK : E_ACCESSDENIED), "UI rejection was hidden");
        require(result.load() == (detach ? static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) : E_ACCESSDENIED), "waiting caller did not receive decisive rejection");
        require(FAILED(fixture.bridge->begin_command(commands[0])), "rejected command remained claimable");
    }
}

void bounded_queue_and_unavailable_targets_never_execute() {
    Fixture fixture;
    auto root = fixture.root();
    auto button = child(root.get(), 0);
    auto invoke = query<IInvokeProvider>(button.get());
    for (int index = 0; index < 32; ++index) require(invoke->Invoke() == S_OK, "queue rejected an in-limit request");
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_TIMEOUT), "queue grew beyond its bound");
    std::vector<Command> commands;
    require(fixture.bridge->take_commands(commands) == S_OK && commands.size() == 32, "queue lost admitted requests");
    for (const auto& command : commands)
        require(fixture.bridge->begin_command(command) == S_OK && fixture.bridge->complete_command(command, S_OK) == S_OK,
                "test consumer did not complete an admitted Invoke");
    require(invoke->Invoke() == S_OK, "queue did not become reusable");
    fixture.nodes[0].enabled = false;
    fixture.publish();
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED), "disabled control queued an action");
    require(fixture.bridge->take_commands(commands) == S_OK && commands.empty(), "previously queued disabled action executed");
    fixture.nodes[0].enabled = true;
    fixture.nodes[0].offscreen = true;
    fixture.publish();
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "invisible node queued an action");
    fixture.nodes[0].offscreen = false;
    fixture.publish();
    require(invoke->Invoke() == S_OK, "visible node remained blocked");
    ShowWindow(fixture.window, SW_HIDE);
    fixture.bridge->set_visible(false);
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_INVALIDOPERATION), "hidden HWND queued an action");
    require(fixture.bridge->take_commands(commands) == S_OK && commands.empty(), "hidden HWND retained pending actions");
    ShowWindow(fixture.window, SW_SHOWNOACTIVATE);
    fixture.publish();
    const LONG previous_runtime = runtime(button.get());
    require(invoke->Invoke() == S_OK, "pre-removal invoke failed");
    fixture.nodes.erase(fixture.nodes.begin());
    fixture.publish();
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "removed node remained actionable");
    require(fixture.bridge->take_commands(commands) == S_OK && commands.empty(), "removed target survived dispatch validation");
    fixture.nodes.insert(fixture.nodes.begin(), settings().front());
    fixture.publish();
    auto reopened = child(root.get(), 0);
    require(runtime(reopened.get()) != previous_runtime, "reopened node reused a stale incarnation");
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "stale provider acted on replacement");
    ProviderOptions options{};
    require(button->get_ProviderOptions(&options) == S_OK && options == ProviderOptions_ServerSideProvider,
            "retirement hid the immutable provider type needed for disconnect");
    require(runtime(button.get()) == previous_runtime, "retirement hid or changed the provider runtime identity needed for disconnect");
    IRawElementProviderFragmentRoot* retired_root = nullptr;
    require(query<IRawElementProviderFragment>(button.get())->get_FragmentRoot(&retired_root) == S_OK && retired_root,
            "retirement hid the fragment root needed to resolve its runtime identity");
    Com<IRawElementProviderFragmentRoot> retired_root_owner(retired_root);
    require(query<IUnknown>(retired_root).get() == query<IUnknown>(root.get()).get(), "retirement changed the fragment root identity");
    const HRESULT disconnected = fixture.bridge->disconnect_retired();
    if (FAILED(disconnected)) std::cerr << "retired disconnect HRESULT: 0x" << std::hex << static_cast<unsigned long>(disconnected) << std::dec << '\n';
    require(SUCCEEDED(disconnected), "retired provider disconnect failed");
}

void invoke_handoff_keeps_32_owned_slots_and_rejects_canceled_replay() {
    Fixture fixture;
    auto root = fixture.root(), button = child(root.get(), 0);
    auto invoke = query<IInvokeProvider>(button.get());
    for (int request = 0; request < 32; ++request) require(invoke->Invoke() == S_OK, "Invoke ownership fixture rejected admission");
    std::vector<Command> local, offered;
    require(fixture.bridge->take_commands(local) == S_OK && local.size() == 32, "Invoke handoff omitted an admitted request");
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_TIMEOUT), "Invoke handoff released live ownership and admitted an unbounded second queue");
    for (int frame = 0; frame < 64; ++frame) {
        require(fixture.bridge->take_commands(offered) == S_OK && offered.empty(), "already offered Invoke was replayed");
        const auto command = local.front(); local.erase(local.begin());
        require(fixture.bridge->begin_command(command) == S_OK && fixture.bridge->complete_command(command, S_OK) == S_OK, "owned Invoke did not complete once");
        require(fixture.bridge->begin_command(command) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "completed Invoke retained executable ownership");
        require(invoke->Invoke() == S_OK, "one completed Invoke did not release one admission slot");
        require(fixture.bridge->take_commands(offered) == S_OK && offered.size() == 1, "Invoke refill offered more than one free slot");
        local.insert(local.end(), offered.begin(), offered.end());
        require(local.size() == 32 && invoke->Invoke() == static_cast<HRESULT>(UIA_E_TIMEOUT), "sustained Invoke admission exceeded the total32 bound");
    }
    const auto canceled = local.front();
    fixture.bridge->set_visible(false); fixture.bridge->set_visible(true); fixture.publish();
    require(fixture.bridge->validate_command(canceled) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
            fixture.bridge->begin_command(canceled) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
            fixture.bridge->complete_command(canceled, S_OK) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "hide/re-show replayed an offered old Invoke token");
    require(invoke->Invoke() == S_OK && fixture.bridge->take_commands(offered) == S_OK && offered.size() == 1, "hide cancellation did not release unclaimed ownership");
    const auto applied = offered.front();
    require(fixture.bridge->begin_command(applied) == S_OK, "picker-style invocation claim failed");
    require(invoke->Invoke() == S_OK, "picker suspension fixture could not queue unclaimed work");
    require(fixture.bridge->publish(fixture.nodes, false) == S_OK, "picker-style suspension failed");
    fixture.publish();
    require(fixture.bridge->complete_command(applied, S_OK) == S_OK, "picker suspension discarded the applied Invoke before notification");
    require(fixture.bridge->complete_command(applied, S_OK) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "applied Invoke completion replayed its notification");
    require(fixture.bridge->take_commands(offered) == S_OK && offered.empty(), "unclaimed work escaped a picker-style modal cancellation");
}

void budgeted_handoff_and_projected_claims_keep_exact_owned_intent() {
    {
        Fixture fixture;
        auto root = fixture.root(), button = child(root.get(), 0);
        auto invoke = query<IInvokeProvider>(button.get());
        for (int request = 0; request < 32; ++request) require(invoke->Invoke() == S_OK, "budget fixture rejected admission");
        std::vector<Command> offered;
        require(fixture.bridge->take_commands(offered, 0) == S_OK && offered.empty(), "zero budget offered work");
        require(fixture.bridge->take_commands(offered, 3) == S_OK && offered.size() == 3, "take exceeded the remaining UI budget");
        const auto command = offered.front();
        auto forged = command; forged.request = 0;
        require(fixture.bridge->begin_command(forged) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "zero token fabricated ownership");
        forged = command; forged.value = L"unregistered payload";
        require(fixture.bridge->validate_command(forged) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
                fixture.bridge->begin_command(forged) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) &&
                fixture.bridge->complete_command(forged, S_OK) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "changed payload reused an admitted token");
        require(fixture.bridge->take_commands(offered) == S_OK && offered.size() == 29, "budgeted take lost remaining work or replayed offered work");
        require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_TIMEOUT), "budgeted handoff released ownership");
        require(fixture.bridge->begin_command(command, fixture.nodes[0], {}) == S_OK &&
                fixture.bridge->complete_command(command, S_OK) == S_OK, "root target required an unrelated completed projection");
    }
    const auto reject_invoke = [](auto change, HRESULT expected) {
        Fixture fixture;
        auto root = fixture.root(), pane = child(root.get(), 1), combo = child(pane.get(), 0);
        auto button = child(root.get(), 0); auto invoke = query<IInvokeProvider>(button.get());
        require(invoke->Invoke() == S_OK, "projected Invoke admission failed");
        std::vector<Command> offered;
        require(fixture.bridge->take_commands(offered) == S_OK && offered.size() == 1, "projected Invoke handoff missing");
        auto projected = fixture.nodes; change(projected);
        require(fixture.bridge->begin_command(offered[0], projected[0], projected) == expected, "current projection admitted obsolete Invoke intent");
        require(fixture.bridge->begin_command(offered[0]) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "rejected projection kept an executable token");
    };
    reject_invoke([](auto& nodes) { ++nodes[0].generation; }, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
    reject_invoke([](auto& nodes) { nodes[0].parent = 2; }, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
    reject_invoke([](auto& nodes) { nodes[0].protected_value = true; }, static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE));
    reject_invoke([](auto& nodes) { nodes[0].enabled = false; }, static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED));
    reject_invoke([](auto& nodes) { nodes[0].covered = true; }, static_cast<HRESULT>(UIA_E_INVALIDOPERATION));
    {
        Fixture fixture;
        fixture.nodes = {{5000,0,L"Current section",Kind::row,{10,10,200,100}},
                         {5001,5000,L"Current action",Kind::button,{20,20,80,30}}}; fixture.publish();
        auto root = fixture.root(), row = child(root.get(),0), button = child(row.get(),0);
        auto invoke = query<IInvokeProvider>(button.get()); require(invoke->Invoke() == S_OK, "ancestor replacement admission failed");
        std::vector<Command> offered; fixture.bridge->take_commands(offered);
        auto projected = fixture.nodes; ++projected[0].generation;
        require(fixture.bridge->begin_command(offered[0], projected[1], projected) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
                "current ancestor replacement did not retire intent before publication");
    }
    {
        Fixture fixture;
        fixture.nodes = {{5000,0,L"Incomplete tab set",Kind::tab,{10,10,200,60}},
                         {5001,5000,L"First",Kind::tab_item,{10,10,80,30},true,false,false,true},
                         {5002,5000,L"Second",Kind::tab_item,{90,10,80,30}}}; fixture.publish();
        auto root = fixture.root(), tabs = child(root.get(),0), second = child(tabs.get(),1);
        auto selection = query<ISelectionItemProvider>(second.get());
        std::atomic<HRESULT> result{E_PENDING};
        std::thread caller([&] { result.store(selection->Select()); });
        std::vector<Command> offered;
        while (offered.empty() && result.load() == E_PENDING) { fixture.bridge->take_commands(offered); std::this_thread::yield(); }
        const std::array<Node,1> partial{fixture.nodes[0]};
        const HRESULT claimed = offered.size() == 1 ? fixture.bridge->begin_command(offered[0], fixture.nodes[2], partial) : E_FAIL;
        if (SUCCEEDED(claimed)) { fixture.nodes[1].selected = false; fixture.nodes[2].selected = true; fixture.publish(); fixture.bridge->complete_command(offered[0],S_OK); }
        caller.join();
        require(claimed == S_OK && result.load() == S_OK, "partial draw projection required a complete selected tab tree");
    }
    {
        Fixture fixture;
        fixture.nodes = {{5001,0,L"Projected edit",Kind::edit,{20,45,110,40}}};
        fixture.nodes[0].text = bmp_edit_snapshot(L"Before",1); fixture.publish();
        auto root = fixture.root(), edit = child(root.get(),0); auto value = query<IValueProvider>(edit.get());
        std::atomic<HRESULT> result{E_PENDING}; std::thread caller([&] { result.store(value->SetValue(L"After")); });
        std::vector<Command> offered;
        while (offered.empty() && result.load() == E_PENDING) { fixture.bridge->take_commands(offered); std::this_thread::yield(); }
        auto projected = fixture.nodes[0]; ++projected.text->revision;
        const HRESULT claimed = offered.size() == 1 ? fixture.bridge->begin_command(offered[0],projected,{}) : E_FAIL;
        caller.join();
        require(claimed == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && result.load() == claimed,
                "unpublished actual edit revision admitted a stale setter");
    }
}

void invalid_tree_and_retained_references_survive_detach() {
    Fixture fixture;
    auto root = fixture.root();
    auto button = child(root.get(), 0);
    auto invoke = query<IInvokeProvider>(button.get());
    const LONG retained_runtime = runtime(button.get());
    require(invoke->Invoke() == S_OK, "pre-rejection request was not admitted");
    auto invalid = fixture.nodes;
    invalid[1].parent = 3;
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "cyclic tree admitted");
    invalid = fixture.nodes;
    invalid[1].key = 1;
    require(fixture.bridge->publish(invalid, true) == E_INVALIDARG, "duplicate semantic key admitted");
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "invalid tree retained an executable obsolete snapshot");
    std::vector<Command> rejected;
    require(fixture.bridge->take_commands(rejected) == S_OK && rejected.empty(), "invalid tree retained queued obsolete actions");
    fixture.publish();
    require(invoke->Invoke() == S_OK, "valid publication did not restore its original provider identity");
    fixture.bridge->detach();
    require(fixture.bridge->root_provider() == nullptr, "detached root was re-exposed");
    ProviderOptions options{};
    require(button->get_ProviderOptions(&options) == S_OK && options == ProviderOptions_ServerSideProvider,
            "detachment hid the immutable provider type needed for disconnect");
    require(runtime(button.get()) == retained_runtime, "detachment hid or changed the provider runtime identity needed for disconnect");
    IRawElementProviderFragmentRoot* detached_root = nullptr;
    require(query<IRawElementProviderFragment>(button.get())->get_FragmentRoot(&detached_root) == S_OK && detached_root,
            "detachment hid the fragment root needed to resolve its runtime identity");
    Com<IRawElementProviderFragmentRoot> detached_root_owner(detached_root);
    require(query<IUnknown>(detached_root).get() == query<IUnknown>(root.get()).get(), "detachment changed the fragment root identity");
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "detached pattern executed");
    std::vector<Command> commands;
    require(fixture.bridge->take_commands(commands) == S_OK && commands.empty(), "shutdown failed to cancel commands");
    VARIANT output{};
    output.vt = VT_I4;
    require(button->GetPropertyValue(UIA_NamePropertyId, &output) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && output.vt == VT_EMPTY,
            "stale query returned an uninitialized output");
    require(query<IUnknown>(invoke.get()).get() == query<IUnknown>(button.get()).get(), "detach violated IUnknown lifetime");
    require(SUCCEEDED(fixture.bridge->disconnect_retired()), "detached resources did not disconnect");
    SetWindowLongPtrW(fixture.window, GWLP_USERDATA, 0);
    fixture.bridge.reset();
    require(invoke->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "provider used destroyed context");
}

void clipped_expanded_headers_preserve_visible_children_and_logical_guards() {
    for (int condition = 0; condition < 5; ++condition) {
        Fixture fixture;
        fixture.nodes = {{5000,0,L"Expanded details",Kind::disclosure,{10,10,200,100},true,false,false,false,true},
                         {5001,5000,L"Visible action",Kind::button,{20,45,110,40}}};
        fixture.publish();
        auto root = fixture.root(), details = child(root.get(),0), button = child(details.get(),0);
        auto invoke = query<IInvokeProvider>(button.get());
        require(invoke->Invoke() == S_OK, "visible descendant request was not admitted");
        std::vector<Command> offered; fixture.bridge->take_commands(offered);
        require(offered.size() == 1, "visible descendant handoff missing");
        auto projected = fixture.nodes;
        projected[0].offscreen = true; projected[0].hit_bounds = UiaRect{};
        if (condition == 1) projected[0].enabled = false;
        if (condition == 2) projected[0].hidden = true;
        if (condition == 3) projected[0].covered = true;
        if (condition == 4) { projected[0].expanded = false; projected[1].hidden = true; }
        const HRESULT expected = condition == 0 ? S_OK : condition == 1 ? static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED) : static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
        require(fixture.bridge->begin_command(offered[0], projected[1], projected) == expected,
                "clipped header visibility or logical ancestor guard was wrong");
        if (condition == 0) require(fixture.bridge->complete_command(offered[0], S_OK) == S_OK, "visible descendant completion failed");
    }
}

void failed_cleanup_bounds_retirement_and_recovers_after_retry() {
    Fixture fixture;
    fixture.nodes.clear();
    for (Key key = 5000; key < 5512; ++key) fixture.nodes.push_back({key,0,L"Retirement node",Kind::text,{20,20,10,10}});
    fixture.publish();
    require(fixture.bridge->disconnect_retired() == S_OK, "initial retired settings cleanup failed");
    auto root = fixture.root(), original = child(root.get(),511);
    const LONG first_runtime = runtime(original.get());
    for (auto& node : fixture.nodes) node.generation = 1;
    fixture.publish();
    auto current = child(root.get(),511);
    const LONG current_runtime = runtime(current.get());
    require(current_runtime - first_runtime == 512, "whole-tree replacement did not create its exact bounded lifetimes");
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::atomic<bool> finished{false};
        std::thread caller([&] { DWORD_PTR response = 0; (void)SendMessageTimeoutW(fixture.window,kDisconnectDuringSend,1,0,SMTO_ABORTIFHUNG | SMTO_BLOCK,1000,&response); finished.store(true); });
        while (!finished.load()) { MSG message{}; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } std::this_thread::yield(); }
        caller.join();
        require(disconnect_result.load() == RPC_E_CANTCALLOUT_ININPUTSYNCCALL, "synthetic cleanup refusal did not preserve pending retirement");
        for (auto& node : fixture.nodes) ++node.generation;
        require(fixture.bridge->publish(fixture.nodes,true) == static_cast<HRESULT>(UIA_E_TIMEOUT), "refused cleanup permitted an unbounded retirement backlog");
        require(fixture.bridge->root_provider() == nullptr && runtime(current.get()) == current_runtime, "retirement pressure exposed old controls or lost their identity");
    }
    require(fixture.bridge->disconnect_retired() == S_OK, "outer-loop cleanup retry did not recover");
    fixture.publish();
    auto restored = child(root.get(),511);
    require(runtime(restored.get()) - current_runtime == 512 && name(restored.get()) == L"Retirement node", "rejected churn consumed lifetimes or prevented recovery");
    require(fixture.bridge->disconnect_retired() == S_OK, "recovered replacement cleanup failed");
}

void destroyed_hwnd_cleanup_keeps_identity_without_reviving_control_state() {
    Fixture fixture;
    auto root = fixture.root();
    auto button = child(root.get(), 0);
    IRawElementProviderSimple* original_host = nullptr;
    require(root->get_HostRawElementProvider(&original_host) == S_OK && original_host, "live HWND host identity missing");
    Com<IRawElementProviderSimple> original_host_owner(original_host);
    require(DestroyWindow(fixture.window), "owned HWND destruction failed");
    fixture.window = nullptr;
    IRawElementProviderSimple* retained_host = nullptr;
    require(root->get_HostRawElementProvider(&retained_host) == S_OK && retained_host, "HWND destruction hid host identity needed for disconnect");
    Com<IRawElementProviderSimple> retained_host_owner(retained_host);
    require(query<IUnknown>(original_host).get() == query<IUnknown>(retained_host).get(), "HWND destruction changed the retained host identity");
    require(SUCCEEDED(fixture.bridge->disconnect_retired()), "destroyed HWND providers did not disconnect");
    require(query<IInvokeProvider>(button.get())->Invoke() == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "identity cleanup revived a destroyed HWND action");
    VARIANT property{};
    require(button->GetPropertyValue(UIA_NamePropertyId, &property) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && property.vt == VT_EMPTY,
            "identity cleanup revived destroyed control metadata");
}

void pump_until(const std::atomic<bool>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}
void native_client_discovers_the_owned_fragment_and_send_shutdown_defers_disconnect() {
    Fixture fixture;
    std::atomic<bool> done{false};
    std::atomic<HRESULT> result{E_FAIL};
    std::thread client([&] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initialized)) {
            IUIAutomation2* automation = nullptr;
            HRESULT status = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                              __uuidof(IUIAutomation2), reinterpret_cast<void**>(&automation));
            if (SUCCEEDED(status)) {
                Com<IUIAutomation2> owner(automation);
                status = automation->put_ConnectionTimeout(1000);
                if (SUCCEEDED(status)) status = automation->put_TransactionTimeout(1000);
                IUIAutomationElement* element = nullptr;
                if (SUCCEEDED(status)) status = automation->ElementFromHandle(fixture.window, &element);
                if (SUCCEEDED(status)) {
                    Com<IUIAutomationElement> element_owner(element);
                    IUIAutomationCondition* condition = nullptr;
                    VARIANT expected{}; expected.vt = VT_BSTR; expected.bstrVal = SysAllocString(L"Cài đặt");
                    status = expected.bstrVal ? automation->CreatePropertyCondition(UIA_NamePropertyId, expected, &condition) : E_OUTOFMEMORY;
                    VariantClear(&expected);
                    if (SUCCEEDED(status)) {
                        Com<IUIAutomationCondition> condition_owner(condition);
                        IUIAutomationElement* pane = nullptr;
                        status = element->FindFirst(TreeScope_Children, condition, &pane);
                        Com<IUIAutomationElement> pane_owner(pane);
                        if (SUCCEEDED(status) && !pane) status = E_FAIL;
                    }
                }
            }
            result.store(status);
            CoUninitialize();
        } else result.store(initialized);
        done.store(true);
    });
    pump_until(done);
    client.join();
    require(done.load() && SUCCEEDED(result.load()), "actual UIA client failed to discover the settings pane");
    auto root = fixture.root();
    done.store(false);
    std::thread sender([&] {
        DWORD_PTR response = 0;
        (void)SendMessageTimeoutW(fixture.window, kDisconnectDuringSend, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &response);
        done.store(true);
    });
    pump_until(done);
    sender.join();
    require(disconnect_result.load() == RPC_E_CANTCALLOUT_ININPUTSYNCCALL, "incoming SendMessage made outbound disconnect calls");
    require(!fixture.bridge->root_provider(), "shutdown allowed reentrant root exposure");
    require(SUCCEEDED(fixture.bridge->disconnect_retired()), "outer-loop deferred disconnect failed");
}

#ifdef NSTU_UIA_CONTEST_PRODUCER
void actual_contest_producers_publish_all_64_rows_private_guards_and_exact_lifetimes() {
    using namespace nstu::ui;
    using namespace nstu::server;
    Fixture fixture;
    struct ImGuiContextOwner {
        ImGuiContext* context = ImGui::CreateContext();
        ~ImGuiContextOwner() { ImGui::DestroyContext(context); }
    } context;
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1800,600}; io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    unsigned char* atlas = nullptr; int atlas_width = 0, atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&atlas,&atlas_width,&atlas_height);
    require(atlas && atlas_width > 0 && atlas_height > 0, "free producer font atlas unavailable");
    std::vector<Node> captured; captured.reserve(512);
    std::map<std::string,Key,std::less<>> keys;
    Key next_key = 5001;
    ContestSemantics semantics;
    semantics.key = [&](std::string_view id) {
        const auto existing = keys.find(id);
        return existing == keys.end() ? keys.emplace(std::string(id),next_key++).first->second : existing->second;
    };
    semantics.capture = [&](Key key, Key parent, const char* label, Kind kind, ImGuiWindow* window, const ImRect& bounds, ImGuiID) -> Node& {
        require(captured.size() < 512, "actual producer exceeded the bridge capacity");
        const int length = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,label,-1,nullptr,0);
        require(length > 1, "actual producer label is not bounded Unicode");
        std::wstring name(static_cast<std::size_t>(length),L'\0');
        require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,label,-1,name.data(),length) == length, "actual producer Unicode label conversion failed");
        name.pop_back();
        captured.push_back({key,parent,std::move(name),kind,{bounds.Min.x,bounds.Min.y,
            std::max(0.0f,bounds.GetWidth()),std::max(0.0f,bounds.GetHeight())}});
        auto& node = captured.back();
        node.enabled = (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
        ImRect clip = bounds; clip.ClipWithFull(window->ClipRect);
        node.hit_bounds = UiaRect{clip.Min.x,clip.Min.y,std::max(0.0f,clip.GetWidth()),std::max(0.0f,clip.GetHeight())};
        node.offscreen = !clip.Overlaps(bounds) || clip.GetWidth() <= 0 || clip.GetHeight() <= 0;
        return node;
    };
    semantics.request = [](Node&,Action) { return std::optional<Command>{}; };
    semantics.focus = [](Node&) {};
    semantics.edit = [](Node& node, char* buffer, std::size_t capacity, TextInput input, bool) {
        // Inert producer fixture records the original operator buffer. Native product tests verify edit geometry.
        const std::string_view value(buffer);
        require(value.size() < capacity, "producer comparison escaped its original capacity");
        node.text = bmp_edit_snapshot(std::wstring(value.begin(),value.end()),1);
        node.text->utf8_capacity = capacity; node.text->input = input;
    };
    semantics.suspend = [] { throw std::runtime_error("free producer fixture reached a platform action"); };
    semantics.select = [](Key,Key) {}; semantics.invoked = [](Key) {};
    const auto frame = [&](std::string prefix, auto draw) {
        captured.clear();
        ImGui::NewFrame(); ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({1800,600});
        ImGui::Begin("Owned free producer fixture",nullptr,ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
        const auto bounds = ImGui::GetCurrentWindow()->InnerRect;
        captured.push_back({5000,0,L"Existing producer content",Kind::pane,{bounds.Min.x,bounds.Min.y,bounds.GetWidth(),bounds.GetHeight()}});
        captured[0].scrollable = true;
        semantics.parent = 5000; semantics.scroll_owner = 5000; semantics.prefix = std::move(prefix); semantics.generation = 0;
        draw(); ImGui::End(); ImGui::Render();
        require(captured.size() <= 453, "actual populated producer exceeds its approved per-view budget");
        fixture.nodes = captured; fixture.publish();
    };
    const auto rows = [&] {
        std::vector<Key> result;
        for (const auto& node : captured) if (node.kind == Kind::row) result.push_back(node.key);
        return result;
    };
    const auto row_children = [&](Key row) {
        std::vector<const Node*> result;
        for (const auto& node : captured) if (node.parent == row) result.push_back(&node);
        return result;
    };
    const auto provider = [&](Key key) {
        auto root = fixture.root(); auto content = child(root.get(),0);
        std::size_t index = 0;
        for (const auto& node : captured) if (node.parent == 5000) {
            if (node.key == key) return child(content.get(),index);
            ++index;
        }
        throw std::runtime_error("actual producer row not present in provider tree");
    };
    ContestRoomSnapshot snapshot; snapshot.running = true; snapshot.room = RoomState{};
    snapshot.room->phase = RoomPhase::prepared;
    for (std::size_t index = 0; index < kMaximumRoomMembers; ++index) {
        RoomMember member; member.display_name = "Synthetic member " + std::to_string(index); member.member_version = 1;
        member.context.package_id = "public-fixture"; member.context.candidate_id = "candidate-" + std::to_string(index);
        member.context.client_id[0] = static_cast<std::byte>(index + 1); member.context.session_id[0] = std::byte{1};
        member.context.package_digest[0] = std::byte{2};
        snapshot.room->members.push_back(member);
        snapshot.pending.push_back({index + 1,"synthetic-peer-" + std::to_string(index),"Synthetic pending " + std::to_string(index),"012345"});
    }
    frame("producer/participants",[&] {
        require(!draw_contest_participants(snapshot,OperatorRole::technician,false,1.0f,false,nullptr,&semantics), "inert participant producer performed removal");
    });
    auto member_rows = rows();
    require(member_rows.size() == 64 && captured.size() == 325, "actual participant producer omitted rows or four headers");
    for (const auto row : member_rows) {
        const auto children = row_children(row);
        require(children.size() == 4 && children[3]->kind == Kind::button && !children[3]->enabled,
                "actual participant row lost its name/package/connection/action or role guard");
        require(children[1]->name == L"Package: Awaiting verified package ACK", "synthetic unready member was silently presented as prepared");
    }
    auto retained_member = provider(member_rows.front()); const LONG member_runtime = runtime(retained_member.get());
    require(name(child(provider(member_rows.back()).get(),3).get()) == L"Remove participant (unverified name): Synthetic member 63",
            "actual64-row participant provider omitted the final removal target");
    std::reverse(snapshot.room->members.begin(),snapshot.room->members.end());
    frame("producer/participants",[&] { (void)draw_contest_participants(snapshot,OperatorRole::teacher,false,1.0f,false,nullptr,&semantics); });
    require(rows().back() == member_rows.front() && runtime(provider(member_rows.front()).get()) == member_runtime,
            "continuing actual member identity retired on reorder");
    ++snapshot.room->members.back().member_version;
    frame("producer/participants",[&] { (void)draw_contest_participants(snapshot,OperatorRole::teacher,false,1.0f,false,nullptr,&semantics); });
    require(runtime(provider(member_rows.front()).get()) != member_runtime, "replacement actual member version reused its provider runtime");
    VARIANT stale{}; require(retained_member->GetPropertyValue(UIA_NamePropertyId,&stale) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
                            "replaced member provider retained mutable state");
    ContestSasComparison comparison;
    frame("producer/admission",[&] { require(!draw_contest_admission(snapshot,OperatorRole::technician,false,nullptr,comparison,false,&semantics), "inert admission producer acted"); });
    auto peer_rows = rows();
    require(peer_rows.size() == 64 && captured.size() == 323, "actual admission producer omitted a pending row");
    for (const auto row : peer_rows) {
        const auto children = row_children(row);
        require(children.size() == 4 && children[1]->protected_value && !children[1]->text &&
                !children[2]->enabled && !children[3]->enabled, "actual pending row omitted protected SAS or role guards");
    }
    for (const auto& node : captured) require(node.name.find(L"012345") == std::wstring::npos && node.name.find(L"012 345") == std::wstring::npos &&
        (!node.text || (node.text->value.find(L"012345") == std::wstring::npos && node.text->value.find(L"012 345") == std::wstring::npos)),
        "expected SAS leaked into actual producer publication");
    auto retained_peer = provider(peer_rows.front()); const LONG peer_runtime = runtime(retained_peer.get());
    std::reverse(snapshot.pending.begin(),snapshot.pending.end());
    frame("producer/admission",[&] { (void)draw_contest_admission(snapshot,OperatorRole::teacher,false,nullptr,comparison,false,&semantics); });
    require(rows().back() == peer_rows.front() && runtime(provider(peer_rows.front()).get()) == peer_runtime,
            "continuing actual pending peer identity retired on reorder");
    snapshot.pending.back().sas = "543210";
    frame("producer/admission",[&] { (void)draw_contest_admission(snapshot,OperatorRole::teacher,false,nullptr,comparison,false,&semantics); });
    require(runtime(provider(peer_rows.front()).get()) != peer_runtime && semantics.peers.size() == 64,
            "exact protected peer replacement reused runtime or exceeded its bounded lifetime cache");
    comparison.start(snapshot.pending.back());
    frame("producer/admission",[&] { (void)draw_contest_admission(snapshot,OperatorRole::teacher,false,nullptr,comparison,false,&semantics); });
    const auto active_children = row_children(peer_rows.front());
    const auto input = std::find_if(active_children.begin(),active_children.end(),[](const Node* node) { return node->kind == Kind::edit; });
    require(input != active_children.end() && (*input)->text && (*input)->text->value.empty() && (*input)->text->utf8_capacity == 7,
            "actual comparison input was autofilled or lost the original operator buffer");
    const auto approve = std::find_if(active_children.begin(),active_children.end(),[](const Node* node) { return node->name == L"Approve matching code"; });
    require(approve != active_children.end() && !(*approve)->enabled, "empty operator comparison enabled approval");
    comparison.cancel(); snapshot.pending.clear();
    frame("producer/admission",[&] { (void)draw_contest_admission(snapshot,OperatorRole::teacher,false,nullptr,comparison,false,&semantics); });
    require(semantics.peers.empty(), "disappeared pending peers retained protected cache entries");
    std::optional<StandingsSnapshot> scores{StandingsSnapshot{}};
    scores->room_id = "synthetic-room"; scores->round_id = "synthetic-round"; scores->revision = 1;
    for (std::size_t index = 0; index < 64; ++index) scores->rows.push_back({static_cast<std::uint32_t>(index + 1),"Synthetic score " + std::to_string(index),1,index});
    const auto now = ContestClock::now();
    frame("producer/standings",[&] { draw_contest_standings(scores,now,now,false,1.0f,false,&semantics); });
    const auto score_rows = rows();
    require(score_rows.size() == 64 && captured.size() == 327, "actual standings producer omitted rows, status, age or headers");
    for (const auto row : score_rows) require(row_children(row).size() == 4, "actual aggregate row lost a column");
    auto retained_score = provider(score_rows.front()); const LONG score_runtime = runtime(retained_score.get());
    require(name(child(provider(score_rows.back()).get(),1).get()) == L"Display name (unverified): Synthetic score 63", "actual64-row standings omitted its final payload");
    ++scores->revision; ++scores->generated_time; scores->freshness = StandingsFreshness::current;
    frame("producer/standings",[&] { draw_contest_standings(scores,now,now + std::chrono::seconds(8),false,1.0f,false,&semantics); });
    require(runtime(provider(score_rows.front()).get()) == score_runtime, "unchanged ordered aggregate payload retired on heartbeat/freshness/age");
    std::swap(scores->rows[0],scores->rows[1]);
    frame("producer/standings",[&] { draw_contest_standings(scores,now,now,false,1.0f,false,&semantics); });
    require(runtime(provider(score_rows.front()).get()) != score_runtime, "changed ordered aggregate payload reused a stale provider identity");
    std::cout << "Actual free producers: participants325 admission323 standings327; all64 rows, protected SAS, role guards and exact replacement lifetimes passed\n";
}
#endif

#ifdef NSTU_UIA_PRODUCT_SERVER
void native_ok(const char* operation, HRESULT result) {
    std::cout << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec << '\n';
    require(SUCCEEDED(result), operation);
}
template<class Condition> void await_product(Condition condition, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, failure);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
struct OwnedProduct {
    PROCESS_INFORMATION process{};
    HANDLE job = nullptr;
    HWND window = nullptr;
    std::filesystem::path directory;
    bool directory_owned = false;
    OwnedProduct() {
        try {
            const std::filesystem::path executable(NSTU_UIA_PRODUCT_SERVER);
            require(std::filesystem::is_regular_file(executable), "current built server fixture missing");
            directory = std::filesystem::temp_directory_path() /
                (L"nstu-uia-product-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            require(std::filesystem::create_directory(directory), "dedicated empty product directory unavailable");
            directory_owned = true;
            job = CreateJobObjectW(nullptr, nullptr);
            require(job != nullptr, "owned product job creation failed");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "owned product job cleanup configuration failed");
            std::wstring command = L"\"" + executable.wstring() + L"\" --contests --ui-scale=1.0 --language=en --light";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                                   directory.c_str(), &startup, &process), "owned product creation failed");
            require(AssignProcessToJobObject(job, process.hProcess), "owned product job assignment failed");
            require(ResumeThread(process.hThread) != static_cast<DWORD>(-1), "owned product resume failed");
            std::wcout << L"Owned product PID=" << process.dwProcessId << L" executable=" << executable.wstring()
                       << L" empty cwd=" << directory.wstring() << L" arguments=--contests --ui-scale=1.0 --language=en --light\n";
        } catch (...) { cleanup(); throw; }
    }
    ~OwnedProduct() {
        if (process.hProcess && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
            if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0)
                std::cerr << "Owned product graceful cleanup timed out; closing its exact owned process\n";
        }
        if (process.hProcess) {
            DWORD exit = STILL_ACTIVE;
            if (GetExitCodeProcess(process.hProcess, &exit)) std::cout << "Owned product cleanup native exit=" << exit << '\n';
        }
        cleanup();
    }
    void cleanup() noexcept {
        if (process.hProcess && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, 2000);
        }
        if (process.hThread) CloseHandle(process.hThread);
        if (process.hProcess) CloseHandle(process.hProcess);
        if (job) CloseHandle(job);
        process = {}; job = nullptr;
        if (directory_owned) { std::error_code error; std::filesystem::remove_all(directory, error); directory_owned = false; }
    }
    void guard(IUIAutomationElement* element = nullptr) const {
        require(process.hProcess && GetProcessId(process.hProcess) == process.dwProcessId &&
                WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT, "owned product process exited or changed identity");
        if (window) {
            DWORD pid = 0;
            const DWORD thread = GetWindowThreadProcessId(window, &pid);
            wchar_t class_name[64]{};
            require(thread == process.dwThreadId && pid == process.dwProcessId && GetClassNameW(window, class_name, 64) &&
                    std::wcscmp(class_name, L"NstuServerWindow") == 0, "owned product HWND identity mismatch");
        }
        if (element) {
            int pid = 0;
            require(element->get_CurrentProcessId(&pid) == S_OK && static_cast<DWORD>(pid) == process.dwProcessId,
                    "UIA element belongs to another process");
        }
    }
    void discover() {
        wchar_t image[32768]{}; DWORD size = static_cast<DWORD>(std::size(image));
        require(QueryFullProcessImageNameW(process.hProcess, 0, image, &size) &&
                std::filesystem::equivalent(std::filesystem::path(image), std::filesystem::path(NSTU_UIA_PRODUCT_SERVER)),
                "owned process image differs from current built server");
        await_product([&] {
            guard();
            EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                auto& owner = *reinterpret_cast<OwnedProduct*>(context);
                DWORD pid = 0; const DWORD thread = GetWindowThreadProcessId(candidate, &pid);
                wchar_t class_name[64]{};
                if (pid == owner.process.dwProcessId && thread == owner.process.dwThreadId &&
                    GetClassNameW(candidate, class_name, 64) && std::wcscmp(class_name, L"NstuServerWindow") == 0) {
                    owner.window = candidate; return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            return window != nullptr;
        }, "owned product HWND was not created");
        guard();
    }
    void dismiss_pane() const {
        guard();
        // Popup lifecycle has no UIA close pattern; target only this fixture's verified HWND.
        require(PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 1) && PostMessageW(window, WM_KEYUP, VK_ESCAPE, 1), "owned popup Escape messages failed");
    }
    void finish() const {
        guard();
        require(PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0), "owned product graceful quit failed");
        const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
        DWORD exit = STILL_ACTIVE;
        require(GetExitCodeProcess(process.hProcess, &exit), "owned product exit status unavailable");
        std::cout << "Owned product shutdown wait=" << waited << " native exit=" << exit << '\n';
        require(waited == WAIT_OBJECT_0 && exit == 0, "owned product did not exit gracefully with zero status");
    }
};
std::wstring native_name(IUIAutomationElement* element) {
    BSTR value = nullptr;
    native_ok("CurrentName", element->get_CurrentName(&value));
    const std::wstring result(value ? value : L"", value ? SysStringLen(value) : 0);
    SysFreeString(value);
    return result;
}
std::string native_utf8(std::wstring_view text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    require(size > 0 || text.empty(), "native log name is not valid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (size) require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) == size,
                      "native log name conversion failed");
    return result;
}
std::vector<int> native_runtime(IUIAutomationElement* element) {
    SAFEARRAY* value = nullptr;
    native_ok("GetRuntimeId", element->GetRuntimeId(&value));
    require(value != nullptr, "native runtime identity missing");
    LONG lower = 0, upper = 0;
    require(SafeArrayGetLBound(value, 1, &lower) == S_OK && SafeArrayGetUBound(value, 1, &upper) == S_OK, "native runtime identity bounds unavailable");
    std::vector<int> result;
    for (LONG index = lower; index <= upper; ++index) {
        int token = 0;
        require(SafeArrayGetElement(value, &index, &token) == S_OK, "native runtime token unavailable");
        result.push_back(token);
    }
    SafeArrayDestroy(value);
    return result;
}
template<class Pattern> Com<Pattern> native_pattern(IUIAutomationElement* element, PATTERNID pattern) {
    Pattern* result = nullptr;
    native_ok("GetCurrentPatternAs", element->GetCurrentPatternAs(pattern, __uuidof(Pattern), reinterpret_cast<void**>(&result)));
    require(result != nullptr, "required native pattern missing");
    return Com<Pattern>(result);
}
Com<IUIAutomationElement> native_setting(IUIAutomation* automation, IUIAutomationElement* window, const OwnedProduct& product, int key) {
    product.guard(window);
    VARIANT value{}; value.vt = VT_BSTR;
    value.bstrVal = SysAllocString((L"nstu.settings." + std::to_wstring(key)).c_str());
    require(value.bstrVal != nullptr, "automation id allocation failed");
    IUIAutomationCondition* condition = nullptr;
    const HRESULT created = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition);
    VariantClear(&value);
    native_ok("CreatePropertyCondition", created);
    Com<IUIAutomationCondition> condition_owner(condition);
    IUIAutomationElement* result = nullptr;
    await_product([&] {
        product.guard();
        const HRESULT found = window->FindFirst(TreeScope_Subtree, condition, &result);
        return SUCCEEDED(found) && result;
    }, "owned product setting was not exposed");
    Com<IUIAutomationElement> owner(result);
    product.guard(result);
    return owner;
}
std::wstring native_selection(IUIAutomationElement* combo, const OwnedProduct& product) {
    product.guard(combo);
    auto selection = native_pattern<IUIAutomationSelectionPattern>(combo, UIA_SelectionPatternId);
    BOOL flag = TRUE;
    native_ok("Selection.CanSelectMultiple", selection->get_CurrentCanSelectMultiple(&flag));
    require(!flag, "native combo allowed multiple selection");
    native_ok("Selection.IsSelectionRequired", selection->get_CurrentIsSelectionRequired(&flag));
    require(flag, "native combo allowed empty selection");
    IUIAutomationElementArray* values = nullptr;
    native_ok("Selection.GetCurrentSelection", selection->GetCurrentSelection(&values));
    Com<IUIAutomationElementArray> values_owner(values);
    int count = 0;
    require(values && values->get_Length(&count) == S_OK && count == 1, "native combo did not delegate exactly one choice");
    IUIAutomationElement* selected = nullptr;
    native_ok("Selection.selectedElement", values->GetElement(0, &selected));
    Com<IUIAutomationElement> selected_owner(selected);
    product.guard(selected);
    auto item = native_pattern<IUIAutomationSelectionItemPattern>(selected, UIA_SelectionItemPatternId);
    native_ok("SelectionItem.IsSelected", item->get_CurrentIsSelected(&flag));
    require(flag, "native selected item reports unselected");
    const auto selected_name = native_name(selected);
    std::cout << "Native current selected Name=" << native_utf8(selected_name) << '\n';
    return selected_name;
}
struct ProductEvents final : IUIAutomationEventHandler, IUIAutomationPropertyChangedEventHandler,
                             IUIAutomationFocusChangedEventHandler, IUIAutomationStructureChangedEventHandler {
    explicit ProductEvents(DWORD owner) : pid(owner) {}
    const DWORD pid;
    std::atomic<ULONG> references{1};
    std::atomic<IUIAutomationElement*> focus_sender{nullptr};
    std::array<std::atomic<int>, 6> counts{}; // Invoke, selection, expand, name/bounds/state, focus, structure.
    std::atomic<int> text_changed{0}, text_selection{0}, value_changed{0}, live_notices{0};
    ~ProductEvents() { if (auto* sender = focus_sender.exchange(nullptr)) sender->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IUIAutomationEventHandler)) *output = static_cast<IUIAutomationEventHandler*>(this);
        else if (iid == __uuidof(IUIAutomationPropertyChangedEventHandler)) *output = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        else if (iid == __uuidof(IUIAutomationFocusChangedEventHandler)) *output = static_cast<IUIAutomationFocusChangedEventHandler*>(this);
        else if (iid == __uuidof(IUIAutomationStructureChangedEventHandler)) *output = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return references.fetch_add(1) + 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = references.fetch_sub(1) - 1;
        if (!remaining) { const std::unique_ptr<ProductEvents> self(this); }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement*, EVENTID event) noexcept override {
        if (event == UIA_Invoke_InvokedEventId) ++counts[0];
        if (event == UIA_SelectionItem_ElementSelectedEventId) ++counts[1];
        if (event == UIA_Text_TextChangedEventId) ++text_changed;
        if (event == UIA_Text_TextSelectionChangedEventId) ++text_selection;
        if (event == UIA_LiveRegionChangedEventId) ++live_notices;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID property, VARIANT) noexcept override {
        ++counts[property == UIA_ExpandCollapseExpandCollapseStatePropertyId ? 2 : 3];
        if (property == UIA_ValueValuePropertyId) ++value_changed;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* sender) noexcept override {
        int sender_pid = 0;
        if (!sender || FAILED(sender->get_CachedProcessId(&sender_pid)) || static_cast<DWORD>(sender_pid) != pid) return S_OK;
        sender->AddRef();
        if (auto* previous = focus_sender.exchange(sender)) previous->Release();
        ++counts[4];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) noexcept override {
        ++counts[5];
        return S_OK;
    }
    std::array<int, 6> snapshot() const {
        std::array<int, 6> result{};
        for (std::size_t index = 0; index < result.size(); ++index) result[index] = counts[index].load();
        return result;
    }
};
void owned_model_text_revisions_and_live_notices_deliver_change_only_events() {
    struct OwnedModel {
        PROCESS_INFORMATION process{};
        HWND window = nullptr;
        OwnedModel() {
            wchar_t executable[32768]{};
            require(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))), "model helper executable unavailable");
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --model-events-provider";
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &process), "owned external model provider creation failed");
            try {
                await_product([&] {
                    require(WaitForSingleObject(process.hProcess,0) == WAIT_TIMEOUT, "owned model provider exited during discovery");
                    EnumWindows([](HWND candidate, LPARAM context) -> BOOL {
                        auto& owner = *reinterpret_cast<OwnedModel*>(context);
                        DWORD pid = 0; const DWORD thread = GetWindowThreadProcessId(candidate,&pid);
                        wchar_t class_name[64]{};
                        if (pid == owner.process.dwProcessId && thread == owner.process.dwThreadId &&
                            GetClassNameW(candidate,class_name,64) && std::wcscmp(class_name,L"NSTUSettingsProviderTest") == 0) {
                            owner.window = candidate; return FALSE;
                        }
                        return TRUE;
                    }, reinterpret_cast<LPARAM>(this));
                    return window != nullptr;
                }, "owned external model HWND missing");
            } catch (...) { cleanup(); throw; }
        }
        void cleanup() noexcept {
            if (process.hProcess && WaitForSingleObject(process.hProcess,0) == WAIT_TIMEOUT) {
                (void)PostThreadMessageW(process.dwThreadId,WM_QUIT,0,0);
                if (WaitForSingleObject(process.hProcess,2000) != WAIT_OBJECT_0) {
                    (void)TerminateProcess(process.hProcess,1); (void)WaitForSingleObject(process.hProcess,2000);
                }
            }
            if (process.hThread) CloseHandle(process.hThread);
            if (process.hProcess) CloseHandle(process.hProcess);
            process = {};
        }
        ~OwnedModel() { cleanup(); }
        void update(WPARAM stage) const {
            DWORD pid = 0;
            require(WaitForSingleObject(process.hProcess,0) == WAIT_TIMEOUT &&
                GetWindowThreadProcessId(window,&pid) == process.dwThreadId && pid == process.dwProcessId,
                "owned model update target changed identity");
            require(PostMessageW(window,kModelEventUpdate,stage,0), "owned model update post failed");
        }
    } fixture;
    std::atomic<bool> ready{false}, stop{false}, done{false};
    std::atomic<HRESULT> result{E_PENDING};
    std::atomic<ProductEvents*> subscribed{nullptr};
    std::thread client([&] {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initialized)) {
            try {
                IUIAutomation2* instance = nullptr;
                native_ok("ModelEvents.CoCreate", CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                    __uuidof(IUIAutomation2), reinterpret_cast<void**>(&instance)));
                Com<IUIAutomation2> automation(instance);
                native_ok("ModelEvents.ConnectionTimeout", automation->put_ConnectionTimeout(1000));
                native_ok("ModelEvents.TransactionTimeout", automation->put_TransactionTimeout(1000));
                IUIAutomationElement* window = nullptr;
                native_ok("ModelEvents.ownedElement", automation->ElementFromHandle(fixture.window, &window));
                Com<IUIAutomationElement> window_owner(window);
                const auto element = [&](const wchar_t* id) {
                    VARIANT value{}; value.vt = VT_BSTR; value.bstrVal = SysAllocString(id);
                    require(value.bstrVal != nullptr, "model event target id allocation failed");
                    IUIAutomationCondition* condition = nullptr;
                    const HRESULT created = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, value, &condition); VariantClear(&value);
                    native_ok("ModelEvents.TargetCondition", created); Com<IUIAutomationCondition> condition_owner(condition);
                    IUIAutomationElement* target = nullptr;
                    native_ok("ModelEvents.TargetElement", window->FindFirst(TreeScope_Subtree, condition, &target));
                    require(target != nullptr, "owned exact model event target missing");
                    int pid = 0; require(target->get_CurrentProcessId(&pid) == S_OK && static_cast<DWORD>(pid) == fixture.process.dwProcessId, "model event target belongs to another process");
                    return Com<IUIAutomationElement>(target);
                };
                auto edit = element(L"nstu.contests.5001"), notice = element(L"nstu.contests.5002");
                auto text = native_pattern<IUIAutomationTextPattern2>(edit.get(),UIA_TextPattern2Id);
                IUIAutomationTextRange* raw_range = nullptr;
                native_ok("ExternalRange.Document",text->get_DocumentRange(&raw_range)); Com<IUIAutomationTextRange> document(raw_range);
                require(document != nullptr, "external native DocumentRange missing");
                native_ok("ExternalRange.Clone",document->Clone(&raw_range)); Com<IUIAutomationTextRange> clone(raw_range);
                BOOL equal = FALSE; int difference = 1;
                native_ok("ExternalRange.Compare",document->Compare(clone.get(),&equal)); require(equal, "external clone changed range identity or endpoints");
                native_ok("ExternalRange.CompareEndpoints",document->CompareEndpoints(TextPatternRangeEndpoint_Start,clone.get(),TextPatternRangeEndpoint_Start,&difference));
                require(difference == 0, "external range endpoint comparison rejected its owned peer");
                native_ok("ExternalRange.MoveEndpointByRange",clone->MoveEndpointByRange(TextPatternRangeEndpoint_End,document.get(),TextPatternRangeEndpoint_Start));
                native_ok("ExternalRange.CompareCollapsed",document->Compare(clone.get(),&equal)); require(!equal, "external endpoint movement did not change the range");
                int moved = 0; native_ok("ExternalRange.MoveEndpointByUnit",clone->MoveEndpointByUnit(TextPatternRangeEndpoint_End,TextUnit_Character,2,&moved));
                require(moved == 2, "external character movement omitted actual text boundaries");
                BSTR contents = nullptr; native_ok("ExternalRange.GetText",clone->GetText(-1,&contents));
                const std::wstring range_contents(contents ? contents : L"",contents ? SysStringLen(contents) : 0); SysFreeString(contents);
                require(range_contents == L"Sa", "external range operations fabricated a different substring");
                auto allocated = std::make_unique<ProductEvents>(fixture.process.dwProcessId); Com<ProductEvents> events(allocated.release());
                struct Removal { IUIAutomation* automation; ~Removal() { (void)automation->RemoveAllEventHandlers(); } } removal{automation.get()};
                for (EVENTID event : {UIA_Text_TextChangedEventId, UIA_Text_TextSelectionChangedEventId, UIA_LiveRegionChangedEventId})
                    native_ok("ModelEvents.SubscribeTextOrLive", automation->AddAutomationEventHandler(event, event == UIA_LiveRegionChangedEventId ? notice.get() : edit.get(), TreeScope_Element, nullptr, events.get()));
                PROPERTYID property = UIA_ValueValuePropertyId;
                native_ok("ModelEvents.SubscribeValue", automation->AddPropertyChangedEventHandlerNativeArray(edit.get(), TreeScope_Element, nullptr, events.get(), &property, 1));
                subscribed.store(events.get()); result.store(S_OK); ready.store(true);
                while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                native_ok("ModelEvents.Remove", automation->RemoveAllEventHandlers());
            } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result.store(E_FAIL); }
            CoUninitialize();
        } else result.store(initialized);
        done.store(true);
    });
    std::exception_ptr failure;
    try {
        pump_until(ready);
        require(ready.load() && SUCCEEDED(result.load()), "owned model event subscription did not become ready");
        auto* events = subscribed.load();
        const auto wait_events = [&](auto condition) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!condition() && std::chrono::steady_clock::now() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                (void)MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
            }
        };
        wait_events([&] { return false; });
        const int initial_text = events->text_changed.load(), initial_selection = events->text_selection.load();
        const int initial_value = events->value_changed.load(), initial_live = events->live_notices.load();
        std::cout << "Model subscription baseline text=" << initial_text << " selection=" << initial_selection
                  << " value=" << initial_value << " live=" << initial_live << '\n';
        fixture.update(1);
        wait_events([&] { return events->text_changed.load() >= initial_text + 1 && events->text_selection.load() >= initial_selection + 1 && events->live_notices.load() >= initial_live + 1; });
        std::cout << "Model edit same-value text=" << events->text_changed.load() << " selection=" << events->text_selection.load()
                  << " value=" << events->value_changed.load() << " live=" << events->live_notices.load() << '\n';
        require(events->text_changed.load() == initial_text + 1 && events->text_selection.load() == initial_selection + 1 && events->value_changed.load() == initial_value && events->live_notices.load() == initial_live + 1,
                "actual same-value edit revision or selection lacked its text event");
        fixture.update(2);
        wait_events([&] { return events->text_changed.load() >= initial_text + 2 && events->value_changed.load() >= initial_value + 1 && events->text_selection.load() >= initial_selection + 2; });
        require(events->text_changed.load() == initial_text + 2 && events->value_changed.load() == initial_value + 1 && events->text_selection.load() == initial_selection + 2,
                "actual text edit did not deliver paired Value/Text/selection events");
        fixture.update(3);
        wait_events([&] { return false; });
        require(events->text_changed.load() == initial_text + 2 && events->value_changed.load() == initial_value + 1 && events->text_selection.load() == initial_selection + 2 && events->live_notices.load() == initial_live + 1,
                "unchanged edits or recency text generated live/text chatter");
    } catch (...) { failure = std::current_exception(); }
    stop.store(true); pump_until(done); client.join();
    require(SUCCEEDED(result.load()), "owned model event client failed during cleanup");
    if (failure) std::rethrow_exception(failure);
}
void actual_product_settings_patterns_commit_and_announce() {
    OwnedProduct product;
    product.discover();
    IUIAutomation2* instance = nullptr;
    native_ok("CoCreateInstance.CUIAutomation8", CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
              __uuidof(IUIAutomation2), reinterpret_cast<void**>(&instance)));
    Com<IUIAutomation2> automation(instance);
    native_ok("ConnectionTimeout", automation->put_ConnectionTimeout(1000));
    native_ok("TransactionTimeout", automation->put_TransactionTimeout(1000));
    IUIAutomationElement* window_element = nullptr;
    native_ok("ElementFromHandle.ownedHWND", automation->ElementFromHandle(product.window, &window_element));
    Com<IUIAutomationElement> window_owner(window_element);
    product.guard(window_element);
    const auto trace_root_metadata = [&](const char* root_label, IUIAutomationElement* root_element) {
        product.guard(root_element);
        const std::array root_fields{std::pair{"Name", UIA_NamePropertyId}, std::pair{"AutomationId", UIA_AutomationIdPropertyId},
                                     std::pair{"ProviderDescription", UIA_ProviderDescriptionPropertyId}};
        for (const auto& [field_name, root_property] : root_fields) {
            VARIANT root_value{};
            std::unique_ptr<VARIANT, decltype(&VariantClear)> root_value_guard(&root_value, VariantClear);
            native_ok(field_name, root_element->GetCurrentPropertyValue(root_property, &root_value));
            std::cout << "Root metadata " << root_label << ' ' << field_name << " vt=" << root_value.vt << " value="
                      << (root_value.vt == VT_BSTR && root_value.bstrVal
                          ? native_utf8(std::wstring_view(root_value.bstrVal, SysStringLen(root_value.bstrVal))) : "<non-string>")
                      << '\n' << std::flush;
        }
    };
    trace_root_metadata("early-held", window_element);
    auto launcher = native_setting(automation.get(), window_element, product, 1);
    trace_root_metadata("held-ready", window_element);
    IUIAutomationElement* fresh_root = nullptr;
    native_ok("Root.FreshElementFromHandle", automation->ElementFromHandle(product.window, &fresh_root));
    Com<IUIAutomationElement> fresh_root_owner(fresh_root);
    require(fresh_root != nullptr, "fresh owned root missing after launcher readiness");
    trace_root_metadata("fresh-ready", fresh_root);
    BOOL same_fresh_root = FALSE;
    native_ok("Root.CompareHeldFresh", automation->CompareElements(window_element, fresh_root, &same_fresh_root));
    const auto held_root_runtime = native_runtime(window_element), fresh_root_runtime = native_runtime(fresh_root);
    std::cout << "Root held/fresh same=" << same_fresh_root << " runtimeSame=" << (held_root_runtime == fresh_root_runtime)
              << " runtimeLengths=" << held_root_runtime.size() << ',' << fresh_root_runtime.size() << '\n' << std::flush;
    {
        VARIANT ready_root_id{};
        std::unique_ptr<VARIANT, decltype(&VariantClear)> ready_root_id_guard(&ready_root_id, VariantClear);
        native_ok("Root.ReadyAutomationId", fresh_root->GetCurrentPropertyValue(UIA_AutomationIdPropertyId, &ready_root_id));
        require(ready_root_id.vt == VT_BSTR && ready_root_id.bstrVal &&
                std::wstring_view(ready_root_id.bstrVal, SysStringLen(ready_root_id.bstrVal)) == L"nstu.settings.0",
                "event subscription root is not the initialized custom product provider");
    }
    // An early HWND root can retain its MSAA proxy after the custom provider becomes available.
    window_owner = std::move(fresh_root_owner);
    window_element = window_owner.get();
    auto allocated_events = std::make_unique<ProductEvents>(product.process.dwProcessId);
    Com<ProductEvents> events(allocated_events.release());
    struct RemoveEvents { IUIAutomation* automation; ~RemoveEvents() { if (automation) (void)automation->RemoveAllEventHandlers(); } } remove_events{automation.get()};
    IUIAutomationCacheRequest* event_cache = nullptr;
    native_ok("CreateEventCacheRequest", automation->CreateCacheRequest(&event_cache));
    Com<IUIAutomationCacheRequest> event_cache_owner(event_cache);
    native_ok("EventCache.ProcessId", event_cache->AddProperty(UIA_ProcessIdPropertyId));
    native_ok("EventCache.AutomationId", event_cache->AddProperty(UIA_AutomationIdPropertyId));
    {
        IUIAutomationElement *owned_sender = nullptr, *foreign_sender = nullptr, *uncached_sender = nullptr;
        native_ok("FocusProbe.OwnedCache", window_element->BuildUpdatedCache(event_cache, &owned_sender));
        Com<IUIAutomationElement> owned_sender_owner(owned_sender);
        native_ok("FocusProbe.ForeignCache", automation->GetRootElementBuildCache(event_cache, &foreign_sender));
        Com<IUIAutomationElement> foreign_sender_owner(foreign_sender);
        require(owned_sender && foreign_sender, "native focus probe targets are missing");
        int foreign_pid = 0;
        native_ok("FocusProbe.ForeignPid", foreign_sender->get_CachedProcessId(&foreign_pid));
        require(static_cast<DWORD>(foreign_pid) != product.process.dwProcessId, "focus probe target is not foreign");
        IUIAutomationCacheRequest* empty_cache = nullptr;
        native_ok("FocusProbe.EmptyCache", automation->CreateCacheRequest(&empty_cache));
        Com<IUIAutomationCacheRequest> empty_cache_owner(empty_cache);
        native_ok("FocusProbe.UncachedTarget", window_element->BuildUpdatedCache(empty_cache, &uncached_sender));
        Com<IUIAutomationElement> uncached_sender_owner(uncached_sender);
        int uncached_pid = 0;
        require(uncached_sender && FAILED(uncached_sender->get_CachedProcessId(&uncached_pid)), "focus probe unexpectedly has a cached PID");
        ProductEvents probe(product.process.dwProcessId);
        native_ok("FocusProbe.Owned", probe.HandleFocusChangedEvent(owned_sender));
        require(probe.counts[4].load() == 1 && probe.focus_sender.load() == owned_sender, "owned cached focus was not retained once");
        native_ok("FocusProbe.Null", probe.HandleFocusChangedEvent(nullptr));
        native_ok("FocusProbe.Uncached", probe.HandleFocusChangedEvent(uncached_sender));
        native_ok("FocusProbe.Foreign", probe.HandleFocusChangedEvent(foreign_sender));
        require(probe.counts[4].load() == 1 && probe.focus_sender.load() == owned_sender, "rejected focus changed the owned count or retained sender");
    }
    native_ok("Subscribe.Invoked", automation->AddAutomationEventHandler(UIA_Invoke_InvokedEventId, launcher.get(), TreeScope_Element, nullptr, events.get()));
    native_ok("Subscribe.Selection", automation->AddAutomationEventHandler(UIA_SelectionItem_ElementSelectedEventId, window_element, TreeScope_Subtree, nullptr, events.get()));
    PROPERTYID properties[]{UIA_ExpandCollapseExpandCollapseStatePropertyId, UIA_SelectionItemIsSelectedPropertyId,
                            UIA_NamePropertyId, UIA_BoundingRectanglePropertyId, UIA_HasKeyboardFocusPropertyId};
    native_ok("Subscribe.Properties", automation->AddPropertyChangedEventHandlerNativeArray(window_element, TreeScope_Subtree, nullptr, events.get(), properties, static_cast<int>(std::size(properties))));
    native_ok("Subscribe.Focus", automation->AddFocusChangedEventHandler(event_cache, events.get()));
    native_ok("Subscribe.Structure", automation->AddStructureChangedEventHandler(window_element, TreeScope_Subtree, nullptr, events.get()));
    auto invoke = native_pattern<IUIAutomationInvokePattern>(launcher.get(), UIA_InvokePatternId);
    product.guard(launcher.get());
    native_ok("Settings.Invoke", invoke->Invoke());
    auto pane = native_setting(automation.get(), window_element, product, 2);
    const auto pane_id = native_runtime(pane.get());
    auto combo = native_setting(automation.get(), window_element, product, 4);
    require(native_name(combo.get()) == L"Reading size", "native combo name contains contents or is not English");
    require(native_selection(combo.get(), product) == L"Comfortable (100%)", "empty fixture did not start at default reading size");
    VARIANT value{};
    native_ok("FixedCombo.IsValuePatternAvailable", combo->GetCurrentPropertyValue(UIA_IsValuePatternAvailablePropertyId, &value));
    require(value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE, "fixed-choice product combo advertises editable Value");
    VariantClear(&value);
    RECT original_bounds{};
    native_ok("Reading.BoundingRectangle", combo->get_CurrentBoundingRectangle(&original_bounds));
    require(original_bounds.right > original_bounds.left && original_bounds.bottom > original_bounds.top, "native reading bounds are empty");
    auto sample = native_setting(automation.get(), window_element, product, 10);
    RECT sample_default{};
    native_ok("ReadingSample.defaultBounds", sample->get_CurrentBoundingRectangle(&sample_default));
    product.guard(combo.get()); native_ok("Reading.SetFocus", combo->SetFocus());
    BOOL focused = FALSE;
    native_ok("Reading.HasKeyboardFocus", combo->get_CurrentHasKeyboardFocus(&focused));
    require(focused, "native SetFocus did not commit actual ImGui reading focus");
    auto expand = native_pattern<IUIAutomationExpandCollapsePattern>(combo.get(), UIA_ExpandCollapsePatternId);
    IUIAutomationTreeWalker* raw_walker = nullptr;
    native_ok("Reading.RawViewWalker", automation->get_RawViewWalker(&raw_walker));
    Com<IUIAutomationTreeWalker> walker(raw_walker);
    const auto trace_ancestry = [&](const char* stage) {
        IUIAutomationElement* parent_pane = nullptr;
        native_ok("Reading.RawParentPane", walker->GetParentElement(combo.get(), &parent_pane));
        Com<IUIAutomationElement> parent_pane_owner(parent_pane);
        require(parent_pane != nullptr, "reading raw parent pane is missing");
        product.guard(parent_pane);
        IUIAutomationElement* parent_root = nullptr;
        native_ok("Reading.RawParentRoot", walker->GetParentElement(parent_pane, &parent_root));
        Com<IUIAutomationElement> parent_root_owner(parent_root);
        require(parent_root != nullptr, "reading raw parent root is missing");
        product.guard(parent_root);
        BOOL same_pane = FALSE, same_root = FALSE;
        native_ok("Reading.CompareParentPane", automation->CompareElements(parent_pane, pane.get(), &same_pane));
        native_ok("Reading.CompareParentRoot", automation->CompareElements(parent_root, window_element, &same_root));
        UIA_HWND root_handle{};
        native_ok("Reading.RawRootNativeWindow", parent_root->get_CurrentNativeWindowHandle(&root_handle));
        const auto root_runtime = native_runtime(parent_root), anchor_runtime = native_runtime(window_element);
        std::cout << "Reading ancestry " << stage << " paneSame=" << same_pane << " rootSame=" << same_root
                  << " rootRuntimeSame=" << (root_runtime == anchor_runtime) << " rootRuntimeLengths="
                  << root_runtime.size() << ',' << anchor_runtime.size() << " rootNativeWindow=" << root_handle
                  << " ownedPID=" << product.process.dwProcessId << '\n' << std::flush;
    };
    const auto change_expansion = [&](const char* operation, bool opening) {
        product.guard(combo.get());
        trace_ancestry("before");
        const auto trace_events = [&](const char* stage) {
            const auto counts = events->snapshot();
            std::cout << operation << ' ' << stage << " invoke=" << counts[0] << " selection=" << counts[1]
                      << " expand=" << counts[2] << " properties=" << counts[3] << " focus=" << counts[4]
                      << " structure=" << counts[5] << '\n' << std::flush;
        };
        ExpandCollapseState before_state{}, after_state{};
        native_ok("Reading.beforeExpansionState", expand->get_CurrentExpandCollapseState(&before_state));
        const auto before_runtime = native_runtime(combo.get());
        std::cout << operation << " beforeState=" << before_state << '\n' << std::flush;
        trace_events("before");
        const int before = events->counts[2].load();
        native_ok(operation, opening ? expand->Expand() : expand->Collapse());
        trace_ancestry("after");
        native_ok("Reading.afterExpansionState", expand->get_CurrentExpandCollapseState(&after_state));
        const auto after_runtime = native_runtime(combo.get());
        std::cout << operation << " afterState=" << after_state << " runtimeSame=" << (before_runtime == after_runtime)
                  << " runtimeLengths=" << before_runtime.size() << ',' << after_runtime.size() << '\n' << std::flush;
        trace_events("after");
        try {
            await_product([&] { return events->counts[2].load() > before; }, "actual reading expansion change did not deliver its property event");
        } catch (...) {
            trace_events("timeout");
            throw;
        }
    };
    change_expansion("Reading.Expand", true);
    require(native_selection(combo.get(), product) == L"Comfortable (100%)", "expanded product combo lost default selection");
    auto list = native_setting(automation.get(), window_element, product, 5);
    const auto first_list_id = native_runtime(list.get());
    auto large = native_setting(automation.get(), window_element, product, 7);
    const int large_expand_events = events->counts[2].load(), large_selection_events = events->counts[1].load();
    product.guard(large.get()); native_ok("Large.SelectionItem.Select", native_pattern<IUIAutomationSelectionItemPattern>(large.get(), UIA_SelectionItemPatternId)->Select());
    require(native_selection(combo.get(), product) == L"Large (125%)", "native Select returned before Large committed");
    ExpandCollapseState state = ExpandCollapseState_Expanded;
    native_ok("Reading.committedExpandState", expand->get_CurrentExpandCollapseState(&state));
    require(state == ExpandCollapseState_Collapsed, "native reading selection did not close its popup");
    await_product([&] { return events->counts[2].load() > large_expand_events && events->counts[1].load() > large_selection_events; }, "actual Large selection did not deliver selection and collapse events");
    await_product([&] { RECT bounds{}; return sample->get_CurrentBoundingRectangle(&bounds) == S_OK && bounds.bottom - bounds.top > sample_default.bottom - sample_default.top; }, "actual reading font/sample did not grow after selecting Large");
    change_expansion("Reading.ReopenLarge", true);
    require(native_selection(combo.get(), product) == L"Large (125%)", "reopened product combo lost current selection");
    auto reopened_list = native_setting(automation.get(), window_element, product, 5);
    require(native_runtime(reopened_list.get()) != first_list_id, "actual reading list reused a closed popup incarnation");
    large = native_setting(automation.get(), window_element, product, 7);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    native_ok("Large.reopenedHasKeyboardFocus", large->get_CurrentHasKeyboardFocus(&focused));
    IUIAutomationElement* focused_element = nullptr;
    native_ok("GetFocusedElement.afterReopen", automation->GetFocusedElement(&focused_element));
    Com<IUIAutomationElement> focused_owner(focused_element);
    product.guard(focused_element);
    const auto focused_name = native_name(focused_element);
    std::cout << "Actual focused Name after reopening Large=" << native_utf8(focused_name) << '\n';
    require(focused, "reopening Large focused the first option instead of the current selection");
    change_expansion("Reading.Collapse", false);
    require(native_selection(combo.get(), product) == L"Large (125%)", "Collapse changed current reading selection");
    auto vietnamese = native_setting(automation.get(), window_element, product, 13);
    product.guard(vietnamese.get()); native_ok("Vietnamese.SelectionItem.Select", native_pattern<IUIAutomationSelectionItemPattern>(vietnamese.get(), UIA_SelectionItemPatternId)->Select());
    BOOL language_selected = FALSE;
    native_ok("Vietnamese.committedSelection", native_pattern<IUIAutomationSelectionItemPattern>(vietnamese.get(), UIA_SelectionItemPatternId)->get_CurrentIsSelected(&language_selected));
    require(language_selected, "actual language selection did not commit");
    await_product([&] { return native_selection(combo.get(), product) == L"Chữ lớn (125%)" && native_name(combo.get()) == L"Cỡ chữ dễ đọc"; },
                  "actual combo localization was not published after language selection");
    require(native_selection(combo.get(), product) == L"Chữ lớn (125%)" && native_name(combo.get()) == L"Cỡ chữ dễ đọc", "actual combo did not localize its selected name separately from field name");
    auto dark = native_setting(automation.get(), window_element, product, 16);
    product.guard(dark.get()); native_ok("Dark.SelectionItem.Select", native_pattern<IUIAutomationSelectionItemPattern>(dark.get(), UIA_SelectionItemPatternId)->Select());
    BOOL selected = FALSE;
    native_ok("Dark.committedSelection", native_pattern<IUIAutomationSelectionItemPattern>(dark.get(), UIA_SelectionItemPatternId)->get_CurrentIsSelected(&selected));
    require(selected && native_name(dark.get()) == L"Tối", "actual appearance selection did not commit or localize");
    change_expansion("Reading.ExpandLargest", true);
    auto largest = native_setting(automation.get(), window_element, product, 9);
    RECT before_largest{};
    native_ok("Largest.previousSampleGeometry",sample->get_CurrentBoundingRectangle(&before_largest));
    const int largest_expand_events = events->counts[2].load(), largest_selection_events = events->counts[1].load();
    product.guard(largest.get()); native_ok("Largest.SelectionItem.Select", native_pattern<IUIAutomationSelectionItemPattern>(largest.get(), UIA_SelectionItemPatternId)->Select());
    require(native_selection(combo.get(), product) == L"Lớn nhất (200%)", "native Largest selection did not commit");
    await_product([&] { return events->counts[2].load() > largest_expand_events && events->counts[1].load() > largest_selection_events; }, "actual Largest selection did not deliver selection and collapse events");
    await_product([&] { RECT bounds{}; return sample->get_CurrentBoundingRectangle(&bounds) == S_OK && bounds.bottom - bounds.top > before_largest.bottom - before_largest.top; },
                  "actual Largest font/sample did not finish growing before reopening");
    change_expansion("Reading.ReopenLargest", true);
    largest = native_setting(automation.get(), window_element, product, 9);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    native_ok("Largest.reopenedHasKeyboardFocus", largest->get_CurrentHasKeyboardFocus(&focused));
    require(focused, "reopening Largest did not focus its current option");
    change_expansion("Reading.CollapseLargest", false);
    RECT outer{}, client{};
    product.guard();
    require(GetWindowRect(product.window,&outer) && GetClientRect(product.window,&client), "owned native geometry unavailable");
    require(SetWindowPos(product.window,nullptr,0,0,500 + (outer.right-outer.left)-(client.right-client.left),
        400 + (outer.bottom-outer.top)-(client.bottom-client.top),SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE), "owned compact fixture resize failed");
    await_product([&] { RECT actual{}; return GetClientRect(product.window,&actual) && actual.right-actual.left == 500 && actual.bottom-actual.top == 400; },
                  "owned native window did not reach the500x400 client minimum");
    // WM_SIZE precedes rendering; operate only after the resized Settings layout is published.
    await_product([&] { RECT actual{}; return pane->get_CurrentBoundingRectangle(&actual) == S_OK &&
        actual.right > actual.left && actual.bottom > actual.top && actual.right-actual.left <= 500 && actual.bottom-actual.top <= 400; }, "compact Settings layout was not published");
    native_ok("Reading.CompactFocus",combo->SetFocus());
    BOOL compact_focus = FALSE, compact_offscreen = TRUE;
    native_ok("Reading.CompactHasFocus",combo->get_CurrentHasKeyboardFocus(&compact_focus));
    native_ok("Reading.CompactIsOffscreen",combo->get_CurrentIsOffscreen(&compact_offscreen));
    require(compact_focus && !compact_offscreen, "largest compact reading focus was hidden or uncommitted");
    RECT compact_reading{}, compact_pane{}, compact_root{};
    native_ok("Reading.CompactBounds",combo->get_CurrentBoundingRectangle(&compact_reading));
    native_ok("Reading.CompactPaneBounds",pane->get_CurrentBoundingRectangle(&compact_pane));
    native_ok("Reading.CompactRootBounds",window_element->get_CurrentBoundingRectangle(&compact_root));
    const auto contains = [&](const RECT& viewport) { return compact_reading.left >= viewport.left && compact_reading.right <= viewport.right &&
        compact_reading.top >= viewport.top && compact_reading.bottom <= viewport.bottom; };
    std::cout << "Compact reading bounds " << compact_reading.left << ',' << compact_reading.top << ',' << compact_reading.right << ',' << compact_reading.bottom
              << " pane " << compact_pane.left << ',' << compact_pane.top << ',' << compact_pane.right << ',' << compact_pane.bottom << '\n';
    require(contains(compact_pane) && contains(compact_root), "largest focused reading control was only partially visible");
    const auto closed_combo_id = native_runtime(combo.get());
    IUIAutomationElement* closed_combo = nullptr;
    native_ok("Reading.CacheClosedCombo", combo->BuildUpdatedCache(event_cache, &closed_combo));
    Com<IUIAutomationElement> closed_combo_owner(closed_combo);
    require(closed_combo != nullptr, "closed reading combo cache missing");
    product.dismiss_pane();
    await_product([&] { BSTR stale = nullptr; const HRESULT result = pane->get_CurrentName(&stale); SysFreeString(stale); return result == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE); }, "owned Escape did not close and retire the settings pane");
    product.guard(launcher.get()); native_ok("Settings.Reopen.Invoke", invoke->Invoke());
    auto reopened_pane = native_setting(automation.get(), window_element, product, 2);
    require(native_runtime(reopened_pane.get()) != pane_id, "actual settings pane reused its closed incarnation");
    combo = native_setting(automation.get(), window_element, product, 4);
    const auto reopened_combo_id = native_runtime(combo.get());
    require(reopened_combo_id != closed_combo_id, "reading combo reused its closed incarnation");
    require(native_selection(combo.get(), product) == L"Lớn nhất (200%)", "pane reopening lost actual reading choice");
    const auto current_combo_focus = [&](IUIAutomationElement* sender) {
        if (!sender) return false;
        int pid = 0; BSTR id = nullptr;
        const HRESULT owner = sender->get_CachedProcessId(&pid), identity = sender->get_CachedAutomationId(&id);
        const bool matching = SUCCEEDED(owner) && SUCCEEDED(identity) && static_cast<DWORD>(pid) == product.process.dwProcessId &&
                              id && std::wcscmp(id, L"nstu.settings.4") == 0;
        SysFreeString(id);
        if (!matching) return false;
        BOOL same_current = FALSE, same_closed = FALSE;
        const HRESULT current = automation->CompareElements(sender, combo.get(), &same_current);
        const HRESULT closed = automation->CompareElements(sender, closed_combo, &same_closed);
        std::cout << "Reading focus candidate owner=" << pid << " currentHRESULT=0x" << std::hex
                  << static_cast<unsigned long>(current) << " closedHRESULT=0x" << static_cast<unsigned long>(closed)
                  << std::dec << " sameCurrent=" << same_current << " sameClosed=" << same_closed
                  << " runtimeLengths=" << reopened_combo_id.size() << ',' << closed_combo_id.size() << '\n' << std::flush;
        return SUCCEEDED(current) && same_current;
    };
    require(!current_combo_focus(closed_combo), "closed reading combo satisfied current-incarnation focus handshake");
    IUIAutomationElement* reopened_cached_combo = nullptr;
    native_ok("Reading.CacheReopenedCombo", combo->BuildUpdatedCache(event_cache, &reopened_cached_combo));
    Com<IUIAutomationElement> reopened_cached_combo_owner(reopened_cached_combo);
    require(reopened_cached_combo && current_combo_focus(reopened_cached_combo), "current reading combo failed focus identity probe");
    product.guard(combo.get()); native_ok("Reading.reopenedSetFocus", combo->SetFocus());
    await_product([&] {
        Com<IUIAutomationElement> sender(events->focus_sender.exchange(nullptr));
        return current_combo_focus(sender.get());
    }, "focus event did not identify the owned real reading combo");
    try {
        await_product([&] { const auto counts = events->snapshot(); return counts[0] == 2 && counts[1] >= 4 && counts[2] >= 8 && counts[3] > 0 && counts[4] > 0 && counts[5] > 0; }, "actual Settings invoke/selection/property/focus/structure event delivery missing");
    } catch (...) {
        const auto counts = events->snapshot();
        std::cout << "Event failure invoke=" << counts[0] << " selection=" << counts[1] << " expand=" << counts[2]
                  << " properties=" << counts[3] << " focus=" << counts[4] << " structure=" << counts[5] << '\n';
        throw;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    const auto settled = events->snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    const auto unchanged = events->snapshot();
    std::cout << "Settled Settings events invoke=" << settled[0] << " selection=" << settled[1] << " expand=" << settled[2]
              << " properties=" << settled[3] << " focus=" << settled[4] << " structure=" << settled[5] << '\n';
    std::cout << "Owned Settings events invoke=" << unchanged[0] << " selection=" << unchanged[1] << " expand=" << unchanged[2]
              << " properties=" << unchanged[3] << " focus=" << unchanged[4] << " structure=" << unchanged[5] << '\n';
    std::cout << "Settings event delta invoke=" << unchanged[0] - settled[0] << " selection=" << unchanged[1] - settled[1]
              << " expand=" << unchanged[2] - settled[2] << " properties=" << unchanged[3] - settled[3]
              << " focus=" << unchanged[4] - settled[4] << " structure=" << unchanged[5] - settled[5] << '\n' << std::flush;
    require(unchanged[0] == 2, "one launcher activation emitted duplicate Invoked events");
    require(settled == unchanged, "unchanged actual Settings frames repeatedly emitted events");
    native_ok("RemoveAllEventHandlers", automation->RemoveAllEventHandlers()); remove_events.automation = nullptr;
    product.finish();
}

Com<IUIAutomationElement> native_contest(IUIAutomation* automation, IUIAutomationElement* window,
                                       const OwnedProduct& product, const wchar_t* label, CONTROLTYPEID type) {
    product.guard(window);
    std::cout << "Contest lookup Name=" << native_utf8(label) << " ControlType=" << type << '\n' << std::flush;
    VARIANT name_value{}, type_value{}; name_value.vt = VT_BSTR; name_value.bstrVal = SysAllocString(label);
    type_value.vt = VT_I4; type_value.lVal = type;
    require(name_value.bstrVal != nullptr, "native Contest label allocation failed");
    IUIAutomationCondition *named = nullptr, *typed = nullptr, *combined = nullptr;
    const HRESULT named_result = automation->CreatePropertyCondition(UIA_NamePropertyId,name_value,&named); VariantClear(&name_value);
    native_ok("Contest.NameCondition",named_result); Com<IUIAutomationCondition> named_owner(named);
    native_ok("Contest.TypeCondition",automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type_value,&typed)); Com<IUIAutomationCondition> typed_owner(typed);
    native_ok("Contest.AndCondition",automation->CreateAndCondition(named,typed,&combined)); Com<IUIAutomationCondition> combined_owner(combined);
    IUIAutomationElement* raw = nullptr;
    await_product([&] { product.guard(); return window->FindFirst(TreeScope_Subtree,combined,&raw) == S_OK && raw; }, "owned real Contest element missing");
    Com<IUIAutomationElement> element(raw); product.guard(raw);
    BSTR id = nullptr; native_ok("Contest.AutomationId",raw->get_CurrentAutomationId(&id));
    const std::wstring automation_id(id ? id : L"",id ? SysStringLen(id) : 0); SysFreeString(id);
    require(automation_id.starts_with(L"nstu.contests."), "native Contest element used a foreign identity namespace");
    return element;
}
std::wstring native_value(IUIAutomationValuePattern* value) {
    BSTR raw = nullptr; native_ok("Contest.Value.Get",value->get_CurrentValue(&raw));
    const std::wstring result(raw ? raw : L"",raw ? SysStringLen(raw) : 0); SysFreeString(raw); return result;
}
HRESULT native_set_value(IUIAutomationValuePattern* value, std::wstring_view contents) {
    BSTR payload = SysAllocStringLen(contents.data(),static_cast<UINT>(contents.size()));
    if (!payload) return E_OUTOFMEMORY;
    const HRESULT result = value->SetValue(payload); SysFreeString(payload); return result;
}
std::wstring native_range_text(IUIAutomationTextRange* range) {
    BSTR raw = nullptr; native_ok("Contest.Range.GetText",range->GetText(-1,&raw));
    const std::wstring result(raw ? raw : L"",raw ? SysStringLen(raw) : 0); SysFreeString(raw); return result;
}
void actual_product_contest_fields_ranges_tabs_toggles_and_guards() {
    OwnedProduct product; product.discover();
    IUIAutomation2* instance = nullptr;
    native_ok("Contest.CoCreate",CoCreateInstance(__uuidof(CUIAutomation8),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IUIAutomation2),reinterpret_cast<void**>(&instance)));
    Com<IUIAutomation2> automation(instance);
    native_ok("Contest.ConnectionTimeout",automation->put_ConnectionTimeout(1000)); native_ok("Contest.TransactionTimeout",automation->put_TransactionTimeout(1000));
    IUIAutomationElement* raw_window = nullptr; native_ok("Contest.OwnedWindow",automation->ElementFromHandle(product.window,&raw_window));
    Com<IUIAutomationElement> window(raw_window);
    auto find = [&](const wchar_t* label, CONTROLTYPEID type) { return native_contest(automation.get(),window.get(),product,label,type); };
    auto show = [&](IUIAutomationElement* element) {
        product.guard(element); native_ok("Contest.ScrollItem",native_pattern<IUIAutomationScrollItemPattern>(element,UIA_ScrollItemPatternId)->ScrollIntoView());
        BOOL offscreen = TRUE; native_ok("Contest.IsOffscreen",element->get_CurrentIsOffscreen(&offscreen)); require(!offscreen, "real ScrollItem returned before the target was visible");
    };
    auto title = find(L"Room title",UIA_EditControlTypeId); show(title.get());
    auto value = native_pattern<IUIAutomationValuePattern>(title.get(),UIA_ValuePatternId);
    BOOL read_only = TRUE; native_ok("Contest.Value.IsReadOnly",value->get_CurrentIsReadOnly(&read_only)); require(!read_only, "native mutable room title was presented as read-only");
    const std::wstring unicode = L"A\u0302\u0301 \U0001F600 Việt";
    native_ok("Contest.Value.Unicode",native_set_value(value.get(),unicode)); require(native_value(value.get()) == unicode, "real title setter lost combining or supplementary Unicode");
    native_ok("Contest.Edit.SetFocus",title->SetFocus());
    BOOL focused = FALSE; native_ok("Contest.Edit.HasFocus",title->get_CurrentHasKeyboardFocus(&focused)); require(focused, "real title focus did not commit native ImGui focus");
    auto teacher_label = find(L"Teacher display label",UIA_EditControlTypeId);
    product.guard();
    require(PostMessageW(product.window,WM_KEYDOWN,VK_TAB,1) && PostMessageW(product.window,WM_KEYUP,VK_TAB,1), "owned keyboard Tab messages failed");
    await_product([&] { BOOL teacher_focus = FALSE, title_focus = TRUE; return teacher_label->get_CurrentHasKeyboardFocus(&teacher_focus) == S_OK && teacher_focus &&
        title->get_CurrentHasKeyboardFocus(&title_focus) == S_OK && !title_focus; }, "native keyboard Tab did not publish the next edit's exclusive focus");
    IUIAutomationElement* keyboard_focused = nullptr;
    native_ok("Contest.Keyboard.GetFocusedElement",automation->GetFocusedElement(&keyboard_focused)); Com<IUIAutomationElement> keyboard_owner(keyboard_focused);
    product.guard(keyboard_focused);
    require(native_runtime(keyboard_focused) == native_runtime(teacher_label.get()) && native_value(value.get()) == unicode,
            "native keyboard focus identity or prior Unicode title was wrong");
    auto teacher_text = native_pattern<IUIAutomationTextPattern2>(teacher_label.get(),UIA_TextPattern2Id);
    BOOL teacher_caret_active = FALSE; IUIAutomationTextRange* keyboard_caret = nullptr;
    native_ok("Contest.Keyboard.TeacherCaret",teacher_text->GetCaretRange(&teacher_caret_active,&keyboard_caret)); Com<IUIAutomationTextRange> keyboard_caret_owner(keyboard_caret);
    require(teacher_caret_active && native_range_text(keyboard_caret).empty(), "native keyboard focus did not expose the next edit's active caret");
    native_ok("Contest.Keyboard.ReturnTitle",title->SetFocus());
    auto text = native_pattern<IUIAutomationTextPattern2>(title.get(),UIA_TextPattern2Id);
    IUIAutomationTextRange* raw_range = nullptr; native_ok("Contest.DocumentRange",text->get_DocumentRange(&raw_range)); Com<IUIAutomationTextRange> document(raw_range);
    require(native_range_text(document.get()) == unicode, "real title DocumentRange differs from its existing input buffer");
    native_ok("Contest.Range.Clone",document->Clone(&raw_range)); Com<IUIAutomationTextRange> clone(raw_range);
    BOOL equal = FALSE; int difference = 1;
    native_ok("Contest.Range.Compare",document->Compare(clone.get(),&equal)); require(equal, "real product clone could not compare through external UIA");
    native_ok("Contest.Range.CompareEndpoints",document->CompareEndpoints(TextPatternRangeEndpoint_End,clone.get(),TextPatternRangeEndpoint_End,&difference)); require(!difference, "real product endpoint comparison changed endpoints");
    native_ok("Contest.Range.MoveEndpointByRange",clone->MoveEndpointByRange(TextPatternRangeEndpoint_End,document.get(),TextPatternRangeEndpoint_Start));
    int moved = 0; native_ok("Contest.Range.MoveCharacter",clone->MoveEndpointByUnit(TextPatternRangeEndpoint_End,TextUnit_Character,1,&moved));
    require(moved == 1 && native_range_text(clone.get()) == L"A\u0302\u0301", "real product Character unit split the stacked combining marks");
    BSTR needle = SysAllocString(L"Việt"); require(needle != nullptr, "real range search allocation failed");
    const HRESULT searched = document->FindText(needle,FALSE,FALSE,&raw_range); SysFreeString(needle);
    native_ok("Contest.Range.FindText",searched); Com<IUIAutomationTextRange> selected_range(raw_range); require(selected_range != nullptr, "real Vietnamese substring search returned no range");
    native_ok("Contest.Range.Select",selected_range->Select());
    IUIAutomationTextRangeArray* raw_selection = nullptr; native_ok("Contest.Text.GetSelection",text->GetSelection(&raw_selection)); Com<IUIAutomationTextRangeArray> selection(raw_selection);
    int selection_count = 0; native_ok("Contest.Text.SelectionCount",selection->get_Length(&selection_count)); require(selection_count == 1, "real single-line edit returned multiple selections");
    native_ok("Contest.Text.SelectedRange",selection->GetElement(0,&raw_range)); Com<IUIAutomationTextRange> actual_selection(raw_range);
    require(native_range_text(actual_selection.get()) == L"Việt", "real range selection did not match the native UTF8 edit selection");
    BOOL caret_active = FALSE; native_ok("Contest.Text.Caret",text->GetCaretRange(&caret_active,&raw_range)); Com<IUIAutomationTextRange> caret(raw_range);
    require(caret_active && native_range_text(caret.get()).empty(), "real active edit caret range was not degenerate");
    auto event_allocation = std::make_unique<ProductEvents>(product.process.dwProcessId); Com<ProductEvents> events(event_allocation.release());
    struct EventRemoval { IUIAutomation* automation; ~EventRemoval() { (void)automation->RemoveAllEventHandlers(); } } removal{automation.get()};
    native_ok("Contest.SubscribeText",automation->AddAutomationEventHandler(UIA_Text_TextChangedEventId,title.get(),TreeScope_Element,nullptr,events.get()));
    PROPERTYID property = UIA_ValueValuePropertyId;
    native_ok("Contest.SubscribeValue",automation->AddPropertyChangedEventHandlerNativeArray(title.get(),TreeScope_Element,nullptr,events.get(),&property,1));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const int baseline_text = events->text_changed.load(), baseline_value = events->value_changed.load();
    native_ok("Contest.Value.SameReplacement",native_set_value(value.get(),unicode));
    await_product([&] { return events->text_changed.load() >= baseline_text + 1; }, "same real input replacement did not raise TextChanged");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    require(events->text_changed.load() == baseline_text + 1 && events->value_changed.load() == baseline_value, "same-value real replacement emitted duplicate text or false Value events");
    BSTR stale_text = nullptr;
    require(document->GetText(-1,&stale_text) == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) && !stale_text, "same-value native replacement did not invalidate prior ranges");
    const std::wstring long_title = std::wstring(75,L'W') + L" Việt \U0001F600";
    native_ok("Contest.Value.Long",native_set_value(value.get(),long_title)); require(native_value(value.get()) == long_title, "native long edit lost exact data");
    native_ok("Contest.Long.Document",text->get_DocumentRange(&raw_range)); Com<IUIAutomationTextRange> long_document(raw_range);
    needle = SysAllocString(L"WW"); require(needle != nullptr, "long range search allocation failed");
    const HRESULT head_found = long_document->FindText(needle,FALSE,FALSE,&raw_range); SysFreeString(needle); native_ok("Contest.Long.FindHead",head_found);
    Com<IUIAutomationTextRange> head(raw_range); require(head != nullptr, "real long edit head range missing");
    native_ok("Contest.Long.ScrollHead",head->ScrollIntoView(TRUE));
    SAFEARRAY* rectangles = nullptr; native_ok("Contest.Long.HeadRectangles",head->GetBoundingRectangles(&rectangles));
    LONG lower = 0, upper = -1; require(rectangles && SafeArrayGetLBound(rectangles,1,&lower) == S_OK && SafeArrayGetUBound(rectangles,1,&upper) == S_OK && upper - lower + 1 == 4,
        "real horizontal range scroll returned without visible substring geometry"); SafeArrayDestroy(rectangles);
    auto publication = find(L"Publish aggregate standings to students",UIA_CheckBoxControlTypeId); show(publication.get());
    native_ok("Contest.Checkbox.SetFocusFromEdit",publication->SetFocus());
    BOOL checkbox_focused = FALSE;
    native_ok("Contest.Checkbox.Focused",publication->get_CurrentHasKeyboardFocus(&checkbox_focused));
    require(checkbox_focused && native_value(value.get()) == long_title, "moving focus out of the active edit lost focus or its exact text");
    auto toggle = native_pattern<IUIAutomationTogglePattern>(publication.get(),UIA_TogglePatternId);
    ToggleState initial_toggle = ToggleState_Indeterminate, changed_toggle = ToggleState_Indeterminate;
    native_ok("Contest.Toggle.Initial",toggle->get_CurrentToggleState(&initial_toggle)); native_ok("Contest.Toggle.Change",toggle->Toggle());
    native_ok("Contest.Toggle.Committed",toggle->get_CurrentToggleState(&changed_toggle)); require(changed_toggle != initial_toggle && changed_toggle != ToggleState_Indeterminate, "real publication toggle did not commit once");
    native_ok("Contest.Toggle.Restore",toggle->Toggle());
    auto details = find(L"Room details",UIA_GroupControlTypeId); show(details.get());
    auto disclosure = native_pattern<IUIAutomationExpandCollapsePattern>(details.get(),UIA_ExpandCollapsePatternId); native_ok("Contest.Details.Expand",disclosure->Expand());
    auto port = find(L"Room port (TCP and UDP)",UIA_EditControlTypeId); show(port.get()); native_ok("Contest.Port.Focus",port->SetFocus());
    auto port_value = native_pattern<IUIAutomationValuePattern>(port.get(),UIA_ValuePatternId);
    for (const auto& pair : {std::pair{L"+01",L"1"},std::pair{L"000012",L"12"},std::pair{L"-1",L"-1"}}) {
        native_ok("Contest.Port.Set",native_set_value(port_value.get(),pair.first)); require(native_value(port_value.get()) == pair.second, "native InputInt completion ignored explicit normalization");
    }
    require(native_set_value(port_value.get(),L"2147483648") == E_INVALIDARG && native_value(port_value.get()) == L"-1", "native int32 overflow altered the existing integer field");
    auto port_text = native_pattern<IUIAutomationTextPattern2>(port.get(),UIA_TextPattern2Id);
    native_ok("Contest.Port.Document",port_text->get_DocumentRange(&raw_range)); Com<IUIAutomationTextRange> port_document(raw_range);
    native_ok("Contest.Foreign.TitleDocument",text->get_DocumentRange(&raw_range)); Com<IUIAutomationTextRange> other_document(raw_range);
    const auto port_before = native_range_text(port_document.get()), title_before = native_range_text(other_document.get());
    BOOL foreign_equal = TRUE; int foreign_difference = 123;
    require(port_document->Compare(other_document.get(),&foreign_equal) == E_INVALIDARG && !foreign_equal,
            "different native edit owners admitted comparison or left output uninitialized");
    require(port_document->CompareEndpoints(TextPatternRangeEndpoint_End,other_document.get(),TextPatternRangeEndpoint_End,&foreign_difference) == E_INVALIDARG && foreign_difference == 0,
            "different native edit owners admitted endpoint comparison or left output uninitialized");
    require(port_document->MoveEndpointByRange(TextPatternRangeEndpoint_End,other_document.get(),TextPatternRangeEndpoint_Start) == E_INVALIDARG &&
            native_range_text(port_document.get()) == port_before && native_range_text(other_document.get()) == title_before,
            "different native edit owners moved either range");
    native_ok("Contest.Port.SelectAll",port_document->Select()); product.guard(port.get()); require(PostMessageW(product.window,WM_CHAR,L'-',1), "owned active partial integer input failed");
    await_product([&] { return native_value(port_value.get()) == L"-"; }, "actual native partial integer edit was not preserved in UIA");
    native_ok("Contest.Port.NormalizeActive",native_set_value(port_value.get(),L"+01")); require(native_value(port_value.get()) == L"1", "active native partial integer did not reload admitted canonical value");
    native_ok("Contest.Port.Restore",native_set_value(port_value.get(),L"47002"));
    const auto title_runtime = native_runtime(title.get());
    RECT contest_client{};
    product.guard();
    require(GetClientRect(product.window, &contest_client), "owned Contest geometry unavailable");
    std::cout << "Contest owned client=" << contest_client.right - contest_client.left << 'x' << contest_client.bottom - contest_client.top << '\n' << std::flush;
    VARIANT workspace_name{}; workspace_name.vt = VT_BSTR; workspace_name.bstrVal = SysAllocString(L"Workspace");
    require(workspace_name.bstrVal != nullptr, "Workspace condition allocation failed");
    IUIAutomationCondition* workspace_condition = nullptr;
    const HRESULT workspace_condition_result = automation->CreatePropertyCondition(UIA_NamePropertyId, workspace_name, &workspace_condition);
    VariantClear(&workspace_name);
    native_ok("Contest.Workspace.Condition", workspace_condition_result);
    Com<IUIAutomationCondition> workspace_condition_owner(workspace_condition);
    for (const wchar_t* tab_name : {L"Admission",L"Participants",L"Standings",L"Setup"}) {
        product.guard();
        IUIAutomationElement* raw_workspace = nullptr;
        native_ok("Contest.Workspace.Find", window->FindFirst(TreeScope_Subtree, workspace_condition, &raw_workspace));
        Com<IUIAutomationElement> workspace(raw_workspace);
        Com<IUIAutomationExpandCollapsePattern> workspace_disclosure;
        std::cout << "Contest Workspace present=" << static_cast<bool>(workspace) << '\n' << std::flush;
        if (workspace) {
            product.guard(workspace.get());
            CONTROLTYPEID workspace_type = 0;
            native_ok("Contest.Workspace.Type", workspace->get_CurrentControlType(&workspace_type));
            require(workspace_type == UIA_GroupControlTypeId, "Workspace is not the real disclosure group");
            BSTR workspace_id = nullptr;
            native_ok("Contest.Workspace.Identity", workspace->get_CurrentAutomationId(&workspace_id));
            const bool owned_workspace = workspace_id && std::wstring_view(workspace_id, SysStringLen(workspace_id)).starts_with(L"nstu.contests.");
            SysFreeString(workspace_id);
            require(owned_workspace, "Workspace used a foreign identity namespace");
            workspace_disclosure = native_pattern<IUIAutomationExpandCollapsePattern>(workspace.get(), UIA_ExpandCollapsePatternId);
            native_ok("Contest.Workspace.Open", workspace_disclosure->Expand());
        }
        auto tab = find(tab_name,UIA_TabItemControlTypeId);
        const auto tab_runtime = native_runtime(tab.get());
        native_ok("Contest.Tab.Select",native_pattern<IUIAutomationSelectionItemPattern>(tab.get(),UIA_SelectionItemPatternId)->Select());
        if (workspace) {
            await_product([&] { BSTR retired_name = nullptr; const HRESULT retired = tab->get_CurrentName(&retired_name); SysFreeString(retired_name);
                return retired == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE); }, "selected compact tab did not retire with its closed popup");
            native_ok("Contest.Workspace.Reopen", workspace_disclosure->Expand());
            tab = find(tab_name,UIA_TabItemControlTypeId);
            require(native_runtime(tab.get()) != tab_runtime, "compact tab reused its closed popup incarnation");
        }
        BOOL selected = FALSE; native_ok("Contest.Tab.IsSelected",native_pattern<IUIAutomationSelectionItemPattern>(tab.get(),UIA_SelectionItemPatternId)->get_CurrentIsSelected(&selected)); require(selected, "real required tab selection did not commit");
        if (workspace) native_ok("Contest.Workspace.Close", workspace_disclosure->Collapse());
    }
    auto reopened_title = find(L"Room title",UIA_EditControlTypeId);
    require(native_runtime(reopened_title.get()) != title_runtime && native_value(native_pattern<IUIAutomationValuePattern>(reopened_title.get(),UIA_ValuePatternId).get()) == long_title,
            "real tab re-entry reused stale providers or lost the existing title buffer");
    require(native_set_value(value.get(),L"stale") == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE), "old real edit provider acted on a tab replacement");
    auto launcher = native_setting(automation.get(),window.get(),product,1); auto invoke = native_pattern<IUIAutomationInvokePattern>(launcher.get(),UIA_InvokePatternId);
    native_ok("Contest.Settings.Invoke",invoke->Invoke()); auto role = find(L"Operator surface",UIA_ComboBoxControlTypeId);
    native_ok("Contest.Role.Expand",native_pattern<IUIAutomationExpandCollapsePattern>(role.get(),UIA_ExpandCollapsePatternId)->Expand());
    auto technician = find(L"Technician",UIA_ListItemControlTypeId); native_ok("Contest.Role.Technician",native_pattern<IUIAutomationSelectionItemPattern>(technician.get(),UIA_SelectionItemPatternId)->Select());
    product.dismiss_pane();
    await_product([&] { BOOL enabled = TRUE; return reopened_title->get_CurrentIsEnabled(&enabled) == S_OK && !enabled; }, "technician role did not disable real room edit");
    require(native_set_value(native_pattern<IUIAutomationValuePattern>(reopened_title.get(),UIA_ValuePatternId).get(),L"forbidden") == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED),
            "role-disabled real room edit admitted mutation");
    native_ok("Contest.RemoveEvents",automation->RemoveAllEventHandlers());
    product.finish();
    std::cout << "Actual product Contest fields/ranges/scroll/numeric partial/toggle/tabs/disabled/stale guards passed\n";
}
#endif
} // namespace

int main(int argc, char** argv) {
    const bool product_mode = argc == 2 && std::string_view(argv[1]) == "--product";
    const HRESULT initialized = CoInitializeEx(nullptr, product_mode ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { std::cerr << "COM initialization failed\n"; return 1; }
    int result = 0;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--model-events-provider") {
            Fixture fixture;
            fixture.nodes = {{5001,0,L"Title",Kind::edit,{20,45,110,40}},
                {5002,0,L"Synthetic notice initial",Kind::status,{20,100,200,30}},
                {5003,0,L"Refresh age 0s",Kind::text,{20,140,200,30}}};
            fixture.nodes[0].text = bmp_edit_snapshot(L"Same",1); fixture.nodes[1].live = true; fixture.publish();
            model_event_update = [&](WPARAM stage) {
                if (stage == 1) {
                    ++fixture.nodes[0].text->revision;
                    fixture.nodes[0].text->selection_start = 0; fixture.nodes[0].text->selection_end = fixture.nodes[0].text->caret = 2;
                    fixture.nodes[1].name = L"Synthetic notice changed"; fixture.nodes[2].name = L"Refresh age 5s"; fixture.publish();
                } else if (stage == 2) { fixture.nodes[0].text = bmp_edit_snapshot(L"Changed",3); fixture.publish(); }
                else if (stage == 3) {
                    for (int frame = 0; frame < 20; ++frame) fixture.publish();
                    fixture.nodes[2].name = L"Refresh age 10s"; fixture.publish();
                }
            };
            MSG message{};
            while (GetMessageW(&message,nullptr,0,0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            model_event_update = {};
        } else
        if (product_mode) {
#ifdef NSTU_UIA_PRODUCT_SERVER
            actual_product_settings_patterns_commit_and_announce();
            actual_product_contest_fields_ranges_tabs_toggles_and_guards();
            std::cout << "Actual product UI accessibility tests passed\n";
#else
            throw std::runtime_error("product mode requires the current built server target");
#endif
        } else {
        interface_identity_and_real_pattern_matrix();
        unicode_outputs_tree_focus_and_physical_bounds();
        selections_are_required_exclusive_and_report_committed_state();
        fixed_combo_delegates_its_current_localized_selection();
        complete_512_node_tree_rejects_513_without_truncation();
        typed_contests_choices_and_nonselecting_453_node_view();
        single_line_ranges_use_unicode_and_published_clipped_geometry();
        middle_of_clipped_wide_glyph_maps_to_an_actual_caret();
        ranges_reject_foreign_owners_invalid_points_and_same_text_revisions();
        value_and_range_commands_commit_exact_state_and_preserve_guards();
        claimed_value_does_not_certify_a_replacement_incarnation();
        timeout_cancels_unclaimed_work_and_claimed_commit_cannot_false_timeout();
        selected_popup_option_may_close_while_completion_stays_truthful();
        inherited_disabled_state_and_clipped_popup_hits_are_truthful();
        rejected_or_detached_completion_wakes_the_waiting_caller();
        bounded_queue_and_unavailable_targets_never_execute();
        invoke_handoff_keeps_32_owned_slots_and_rejects_canceled_replay();
        budgeted_handoff_and_projected_claims_keep_exact_owned_intent();
        invalid_tree_and_retained_references_survive_detach();
        clipped_expanded_headers_preserve_visible_children_and_logical_guards();
        failed_cleanup_bounds_retirement_and_recovers_after_retry();
        destroyed_hwnd_cleanup_keeps_identity_without_reviving_control_state();
        native_client_discovers_the_owned_fragment_and_send_shutdown_defers_disconnect();
#ifdef NSTU_UIA_CONTEST_PRODUCER
        actual_contest_producers_publish_all_64_rows_private_guards_and_exact_lifetimes();
#endif
#ifdef NSTU_UIA_PRODUCT_SERVER
        owned_model_text_revisions_and_live_notices_deliver_change_only_events();
#endif
        std::cout << "UI accessibility provider tests passed\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    CoUninitialize();
    return result;
}
