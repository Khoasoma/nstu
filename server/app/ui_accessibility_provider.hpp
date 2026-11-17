#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nstu::ui::accessibility {

using Key = std::uint64_t;
enum class Kind { pane, text, button, combo, list, group, radio, list_item, edit, checkbox, tab, tab_item, disclosure, row, status };
enum class Action { invoke, select, expand, collapse, focus, set_value, toggle, text_select, scroll };
enum class TextInput { plain, integer, digits };
struct TextGlyph { LONG start = 0, end = 0; UiaRect bounds{}; };
struct TextCaret { LONG offset = 0; UiaRect bounds{}; };
struct TextData {
    std::wstring value{};
    std::size_t utf8_capacity = 0;
    TextInput input = TextInput::plain;
    std::uint64_t revision = 1;
    LONG selection_start = 0, selection_end = 0, caret = 0;
    UiaRect clip{};
    std::vector<TextGlyph> glyphs{};
    std::vector<TextCaret> carets{};
};

struct Node {
    Key key = 0;
    Key parent = 0;
    std::wstring name;
    Kind kind = Kind::text;
    UiaRect bounds{}; // Full physical screen bounds, independent of clipping.
    bool enabled = true;
    bool offscreen = false;
    bool focused = false;
    bool selected = false;
    bool expanded = false;
    std::optional<UiaRect> hit_bounds = std::nullopt; // Actual visible region; empty means fully clipped.
    int layer = 0; // Popup items precede covered controls when their layer is higher.
    std::uint64_t generation = 0; // Advance when a popup/control instance closes or reopens.
    bool read_only = false, protected_value = false, checked = false;
    bool hidden = false, covered = false, live = false, scrollable = false;
    Key scroll_owner = 0;
    std::optional<TextData> text = std::nullopt;
};

struct Command {
    Key key = 0;
    LONG incarnation = 0;
    Action action = Action::invoke;
    std::uint64_t request = 0;
    std::wstring value{};
    std::uint64_t text_revision = 0;
    LONG range_start = 0, range_end = 0;
    bool align_to_top = true, target_checked = false;
};

namespace detail {
inline constexpr std::size_t kNodeLimit = 512;
inline constexpr std::size_t kQueueLimit = 32;
inline constexpr UINT kCommandMessage = WM_APP + 0x2B9;
inline constexpr HRESULT unavailable = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
inline constexpr HRESULT disabled = static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
inline constexpr HRESULT invalid_operation = static_cast<HRESULT>(UIA_E_INVALIDOPERATION);

template<class Function> HRESULT com_call(Function&& function) noexcept {
    try { return function(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

struct Provider;
struct TextRange;
struct ReleaseProvider { void operator()(Provider* value) const noexcept; };
using Owner = std::unique_ptr<Provider, ReleaseProvider>;
struct ReleaseHost { void operator()(IRawElementProviderSimple* value) const noexcept { if (value) value->Release(); } };
using Host = std::unique_ptr<IRawElementProviderSimple, ReleaseHost>;
struct Entry { Node node; LONG incarnation; Owner provider; };
struct Completion {
    enum class Stage { waiting, claimed, complete, canceled };
    std::mutex mutex;
    std::condition_variable changed;
    Stage stage = Stage::waiting;
    HRESULT result = E_PENDING;
};
struct Pending { Command command; std::shared_ptr<Completion> completion; bool offered = false; };
struct Shared {
    std::mutex mutex;
    HWND window = nullptr;
    DWORD ui_thread = 0;
    bool attached = true;
    bool ready = false;
    bool visible = false;
    UiaRect bounds{};
    LONG next_incarnation = 1;
    std::uint64_t next_request = 1;
    Owner root;
    std::vector<Entry> entries;
    std::list<Owner> retired;
    std::deque<Pending> commands;
};

inline bool selectable(Kind kind) noexcept { return kind == Kind::radio || kind == Kind::list_item || kind == Kind::tab_item; }
inline bool container(Kind kind) noexcept { return kind == Kind::group || kind == Kind::list || kind == Kind::combo || kind == Kind::tab; }
inline bool expandable(Kind kind) noexcept { return kind == Kind::combo || kind == Kind::disclosure; }
inline bool focusable(Kind kind) noexcept { return kind == Kind::button || expandable(kind) || selectable(kind) || kind == Kind::edit || kind == Kind::checkbox; }
inline bool rectangle_valid(const UiaRect& value) noexcept {
    return std::isfinite(value.left) && std::isfinite(value.top) && std::isfinite(value.width) &&
           std::isfinite(value.height) && value.width >= 0 && value.height >= 0;
}
inline bool rectangle_equal(const UiaRect& a, const UiaRect& b) noexcept {
    return a.left == b.left && a.top == b.top && a.width == b.width && a.height == b.height;
}
inline bool contains(const UiaRect& value, double x, double y) noexcept {
    return value.width > 0 && value.height > 0 && x >= value.left && y >= value.top &&
           x < value.left + value.width && y < value.top + value.height;
}
inline UiaRect intersection(const UiaRect& a, const UiaRect& b) noexcept {
    const double left = std::max(a.left, b.left), top = std::max(a.top, b.top);
    return {left, top, std::max(0.0, std::min(a.left + a.width, b.left + b.width) - left),
            std::max(0.0, std::min(a.top + a.height, b.top + b.height) - top)};
}
inline bool utf16_boundary(std::wstring_view text, LONG offset) noexcept {
    if (offset < 0 || static_cast<std::size_t>(offset) > text.size()) return false;
    return offset == 0 || static_cast<std::size_t>(offset) == text.size() ||
           !(text[static_cast<std::size_t>(offset) - 1] >= 0xD800 && text[static_cast<std::size_t>(offset) - 1] <= 0xDBFF &&
             text[static_cast<std::size_t>(offset)] >= 0xDC00 && text[static_cast<std::size_t>(offset)] <= 0xDFFF);
}
inline bool valid_value(std::wstring_view value, std::size_t capacity, TextInput input = TextInput::plain) noexcept {
    if (!capacity || capacity > 2048 || value.size() >= capacity) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const wchar_t character = value[index];
        if (!character || character == L'\r' || character == L'\n') return false;
        if (character >= 0xD800 && character <= 0xDBFF) {
            if (++index == value.size() || value[index] < 0xDC00 || value[index] > 0xDFFF) return false;
        } else if (character >= 0xDC00 && character <= 0xDFFF) return false;
    }
    const int bytes = value.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if ((!value.empty() && bytes <= 0) || static_cast<std::size_t>(bytes) >= capacity) return false;
    if (input == TextInput::plain) return true;
    if (input == TextInput::digits) return std::all_of(value.begin(), value.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    if (input != TextInput::integer || value.empty()) return false;
    std::size_t index = 0;
    const bool negative = value.front() == L'-';
    if (negative || value.front() == L'+') ++index;
    if (index == value.size()) return false;
    std::uint64_t number = 0;
    const std::uint64_t limit = negative ? 2147483648ULL : 2147483647ULL;
    for (; index < value.size(); ++index) {
        if (value[index] < L'0' || value[index] > L'9') return false;
        const auto digit = static_cast<std::uint64_t>(value[index] - L'0');
        if (number > (limit - digit) / 10) return false;
        number = number * 10 + digit;
    }
    return true;
}
inline bool valid_text(const TextData& text) noexcept {
    if ((text.input != TextInput::plain && text.input != TextInput::integer && text.input != TextInput::digits) ||
        !text.revision || !valid_value(text.value, text.utf8_capacity) || !rectangle_valid(text.clip) ||
        !utf16_boundary(text.value, text.selection_start) || !utf16_boundary(text.value, text.selection_end) ||
        !utf16_boundary(text.value, text.caret)) return false;
    LONG covered = 0;
    for (const auto& glyph : text.glyphs) {
        if (glyph.start != covered || glyph.end <= glyph.start || !utf16_boundary(text.value, glyph.end) || !rectangle_valid(glyph.bounds)) return false;
        covered = glyph.end;
    }
    if (static_cast<std::size_t>(covered) != text.value.size()) return false;
    std::size_t caret = 0;
    for (LONG offset = 0; static_cast<std::size_t>(offset) <= text.value.size(); ++offset) if (utf16_boundary(text.value, offset)) {
        if (caret == text.carets.size() || text.carets[caret].offset != offset || !rectangle_valid(text.carets[caret].bounds)) return false;
        ++caret;
    }
    return caret == text.carets.size();
}
inline LONG next_character(std::wstring_view text, LONG offset) noexcept {
    return offset + (text[static_cast<std::size_t>(offset)] >= 0xD800 && text[static_cast<std::size_t>(offset)] <= 0xDBFF ? 2 : 1);
}
inline bool combining_character(std::wstring_view text, LONG offset) noexcept {
    WORD types[2]{};
    const LONG end = next_character(text, offset);
    if (!GetStringTypeW(CT_CTYPE3, text.data() + offset, end - offset, types)) return false;
    return ((types[0] | types[1]) & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0;
}
inline std::vector<LONG> character_boundaries(std::wstring_view text) {
    std::vector<LONG> boundaries{0};
    for (LONG offset = 0; static_cast<std::size_t>(offset) < text.size();) {
        if (offset && !combining_character(text, offset)) boundaries.push_back(offset);
        offset = next_character(text, offset);
    }
    if (!text.empty()) boundaries.push_back(static_cast<LONG>(text.size()));
    return boundaries;
}
inline std::vector<LONG> text_units(std::wstring_view text, TextUnit unit) {
    auto characters = character_boundaries(text);
    if (unit == TextUnit_Character) return characters;
    if (unit >= TextUnit_Line) return text.empty() ? std::vector<LONG>{0} : std::vector<LONG>{0, static_cast<LONG>(text.size())};
    // Format is not exposed; the next supported unit is Word.
    std::vector<LONG> words{0};
    const auto classification = [&](LONG offset) {
        WORD types[2]{};
        const LONG end = next_character(text, offset);
        if (!GetStringTypeW(CT_CTYPE1, text.data() + offset, end - offset, types)) return 2;
        const WORD type = static_cast<WORD>(types[0] | types[1]);
        return (type & C1_SPACE) ? 0 : (type & (C1_ALPHA | C1_DIGIT)) ? 1 : 2;
    };
    for (std::size_t index = 0; index + 1 < characters.size();) {
        const int type = classification(characters[index]);
        std::size_t next = index + 1;
        if (type != 2) while (next + 1 < characters.size() && classification(characters[next]) == type) ++next;
        while (next + 1 < characters.size() && classification(characters[next]) == 0) ++next;
        words.push_back(characters[next]); index = next;
    }
    return words;
}
inline bool text_unit_valid(TextUnit unit) noexcept { return unit >= TextUnit_Character && unit <= TextUnit_Document; }
inline bool endpoint_valid(TextPatternRangeEndpoint endpoint) noexcept {
    return endpoint == TextPatternRangeEndpoint_Start || endpoint == TextPatternRangeEndpoint_End;
}
inline const Entry* find(const Shared& state, Key key) noexcept {
    const auto entry = std::find_if(state.entries.begin(), state.entries.end(), [key](const Entry& value) { return value.node.key == key; });
    return entry == state.entries.end() ? nullptr : &*entry;
}
inline bool descendant(const Shared& state, const Node& node, Key key) noexcept {
    Key parent = node.parent;
    for (std::size_t depth = 0; parent && depth < state.entries.size(); ++depth) {
        if (parent == key) return true;
        const auto* entry = find(state, parent);
        if (!entry) return false;
        parent = entry->node.parent;
    }
    return false;
}
inline void effective_state(const Shared& state, const Node& node, bool& enabled, bool& offscreen) noexcept {
    enabled = node.enabled;
    offscreen = !state.visible || node.offscreen || node.hidden || node.covered || node.bounds.width <= 0 || node.bounds.height <= 0 ||
                (node.hit_bounds && (node.hit_bounds->width <= 0 || node.hit_bounds->height <= 0));
    Key parent = node.parent;
    for (std::size_t depth = 0; parent && depth < state.entries.size(); ++depth) {
        const auto* entry = find(state, parent);
        if (!entry) { offscreen = true; return; }
        enabled = enabled && entry->node.enabled;
        offscreen = offscreen || entry->node.hidden || entry->node.covered;
        parent = entry->node.parent;
    }
}
inline bool blocked(const Shared& state, const Node& node) noexcept {
    const Node* current = &node;
    for (std::size_t depth = 0; current && depth <= state.entries.size(); ++depth) {
        if (current->hidden || current->covered) return true;
        if (!current->parent) return false;
        const auto* parent = find(state, current->parent);
        if (!parent) return true;
        current = &parent->node;
    }
    return true;
}
inline bool scroll_owner_valid(const Shared& state, const Node& node) noexcept {
    if (!state.visible || !node.scroll_owner || blocked(state, node)) return false;
    Key parent = node.parent;
    for (std::size_t depth = 0; parent && depth < state.entries.size(); ++depth) {
        const auto* entry = find(state, parent);
        if (!entry) return false;
        if (parent == node.scroll_owner) {
            bool enabled = false, offscreen = false; effective_state(state, entry->node, enabled, offscreen);
            return entry->node.scrollable && enabled && !offscreen;
        }
        parent = entry->node.parent;
    }
    return false;
}
inline HRESULT validate_command(const Shared& state, const Command& command) noexcept {
    if (!state.attached || !state.ready) return unavailable;
    if (!command.key) return invalid_operation;
    const auto* entry = find(state, command.key);
    if (!entry || entry->incarnation != command.incarnation) return unavailable;
    bool enabled = false, offscreen = false;
    effective_state(state, entry->node, enabled, offscreen);
    if (!enabled) return disabled;
    if (blocked(state, entry->node)) return invalid_operation;
    if (offscreen && command.action != Action::scroll && !(command.action == Action::focus && entry->node.scroll_owner)) return invalid_operation;
    if ((command.action == Action::scroll || offscreen) && !scroll_owner_valid(state, entry->node)) return invalid_operation;
    if (command.text_revision && (!entry->node.text || entry->node.protected_value || entry->node.text->revision != command.text_revision)) return unavailable;
    const Kind kind = entry->node.kind;
    switch (command.action) {
    case Action::invoke: return kind == Kind::button ? S_OK : invalid_operation;
    case Action::select: return selectable(kind) ? S_OK : invalid_operation;
    case Action::expand: case Action::collapse: return expandable(kind) ? S_OK : invalid_operation;
    case Action::focus: return focusable(kind) ? S_OK : invalid_operation;
    case Action::toggle: return kind == Kind::checkbox && entry->node.checked != command.target_checked ? S_OK : invalid_operation;
    case Action::set_value:
        if (kind != Kind::edit || entry->node.protected_value || !entry->node.text) return invalid_operation;
        if (entry->node.read_only) return invalid_operation;
        return command.text_revision && valid_value(command.value, entry->node.text->utf8_capacity, entry->node.text->input) ? S_OK : E_INVALIDARG;
    case Action::text_select:
        return kind == Kind::edit && !entry->node.protected_value && entry->node.text && command.text_revision &&
               command.range_start <= command.range_end && utf16_boundary(entry->node.text->value, command.range_start) &&
               utf16_boundary(entry->node.text->value, command.range_end) ? S_OK : E_INVALIDARG;
    case Action::scroll:
        if (command.text_revision && (command.range_start > command.range_end || !utf16_boundary(entry->node.text->value, command.range_start) ||
                                     !utf16_boundary(entry->node.text->value, command.range_end))) return E_INVALIDARG;
        return scroll_owner_valid(state, entry->node) ? S_OK : invalid_operation;
    }
    return invalid_operation;
}
inline bool canceled(const Pending& pending) {
    if (!pending.completion) return false;
    const std::lock_guard lock(pending.completion->mutex);
    return pending.completion->stage == Completion::Stage::canceled;
}
inline bool claimed(const Pending& pending) {
    const std::lock_guard lock(pending.completion->mutex);
    return pending.completion->stage == Completion::Stage::claimed;
}
inline void finish(const Pending& pending, HRESULT result) {
    if (!pending.completion) return;
    {
        const std::lock_guard lock(pending.completion->mutex);
        if (pending.completion->stage == Completion::Stage::canceled) return;
        pending.completion->result = result;
        pending.completion->stage = Completion::Stage::complete;
    }
    pending.completion->changed.notify_all();
}
inline void cancel_all(Shared& state, HRESULT result, bool preserve_claimed = false) {
    for (auto pending = state.commands.begin(); pending != state.commands.end();) {
        if (preserve_claimed && claimed(*pending)) { ++pending; continue; }
        finish(*pending, result);
        pending = state.commands.erase(pending);
    }
}

inline HRESULT rectangle_variant(const UiaRect& rectangle, VARIANT& output) noexcept {
    SAFEARRAY* array = SafeArrayCreateVector(VT_R8, 0, 4);
    if (!array) return E_OUTOFMEMORY;
    double values[]{rectangle.left, rectangle.top, rectangle.width, rectangle.height};
    for (LONG index = 0; index < 4; ++index) {
        const HRESULT result = SafeArrayPutElement(array, &index, &values[index]);
        if (FAILED(result)) { SafeArrayDestroy(array); return result; }
    }
    output.vt = VT_ARRAY | VT_R8;
    output.parray = array;
    return S_OK;
}
inline HRESULT reserved_variant(VARIANT& output, bool mixed = false) noexcept {
    IUnknown* sentinel = nullptr;
    const HRESULT result = mixed ? UiaGetReservedMixedAttributeValue(&sentinel) : UiaGetReservedNotSupportedValue(&sentinel);
    if (FAILED(result)) { if (sentinel) sentinel->Release(); return result; }
    output.vt = VT_UNKNOWN; output.punkVal = sentinel; return S_OK;
}
struct ReleaseUnknown { void operator()(IUnknown* value) const noexcept { if (value) value->Release(); } };
using Unknown = std::unique_ptr<IUnknown, ReleaseUnknown>;
using RangeOwner = std::unique_ptr<ITextRangeProvider, ReleaseUnknown>;
inline constexpr GUID kTextRangeStateId{0xe65677c1,0x3f12,0x4ac5,{0x9a,0x4f,0xc0,0x4b,0x11,0x43,0x67,0x0d}};
// Private typed peer metadata avoids casting foreign COM range implementations.
struct IRangeState : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetState(IUnknown** owner, std::uint64_t* revision, LONG* start, LONG* end) noexcept = 0;
};

struct Provider final : IRawElementProviderSimple, IRawElementProviderFragment, IRawElementProviderFragmentRoot,
                        IInvokeProvider, ISelectionProvider, ISelectionItemProvider, IExpandCollapseProvider,
                        IToggleProvider, IScrollItemProvider, IValueProvider, ITextProvider2 {
    Provider(std::weak_ptr<Shared> context, Key key, LONG incarnation, Kind kind, Owner fragment_root = {}, Host host = {},
             bool protected_value = false, bool can_scroll = false) noexcept
        : context_(std::move(context)), key_(key), incarnation_(incarnation), kind_(kind),
          protected_(protected_value), can_scroll_(can_scroll), fragment_root_(std::move(fragment_root)), host_(std::move(host)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple)) *output = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == __uuidof(IRawElementProviderFragment)) *output = static_cast<IRawElementProviderFragment*>(this);
        else if (!key_ && iid == __uuidof(IRawElementProviderFragmentRoot)) *output = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (kind_ == Kind::button && iid == __uuidof(IInvokeProvider)) *output = static_cast<IInvokeProvider*>(this);
        else if (container(kind_) && iid == __uuidof(ISelectionProvider)) *output = static_cast<ISelectionProvider*>(this);
        else if (selectable(kind_) && iid == __uuidof(ISelectionItemProvider)) *output = static_cast<ISelectionItemProvider*>(this);
        else if (expandable(kind_) && iid == __uuidof(IExpandCollapseProvider)) *output = static_cast<IExpandCollapseProvider*>(this);
        else if (kind_ == Kind::checkbox && iid == __uuidof(IToggleProvider)) *output = static_cast<IToggleProvider*>(this);
        else if (can_scroll_ && iid == __uuidof(IScrollItemProvider)) *output = static_cast<IScrollItemProvider*>(this);
        else if (kind_ == Kind::edit && !protected_ && iid == __uuidof(IValueProvider)) *output = static_cast<IValueProvider*>(this);
        else if (kind_ == Kind::edit && !protected_ && iid == __uuidof(ITextProvider)) *output = static_cast<ITextProvider*>(this);
        else if (kind_ == Kind::edit && !protected_ && iid == __uuidof(ITextProvider2)) *output = static_cast<ITextProvider2*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!remaining) { const std::unique_ptr<Provider> self(this); }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* output) noexcept override {
        if (!output) return E_POINTER;
        // UIA queries this immutable provider type while disconnecting retired elements.
        *output = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result;
            if (pattern == UIA_InvokePatternId && kind_ == Kind::button) *output = static_cast<IInvokeProvider*>(this);
            else if (pattern == UIA_SelectionPatternId && container(kind_)) *output = static_cast<ISelectionProvider*>(this);
            else if (pattern == UIA_SelectionItemPatternId && selectable(kind_)) *output = static_cast<ISelectionItemProvider*>(this);
            else if (pattern == UIA_ExpandCollapsePatternId && expandable(kind_)) *output = static_cast<IExpandCollapseProvider*>(this);
            else if (pattern == UIA_TogglePatternId && kind_ == Kind::checkbox) *output = static_cast<IToggleProvider*>(this);
            else if (pattern == UIA_ScrollItemPatternId && can_scroll_) *output = static_cast<IScrollItemProvider*>(this);
            else if (pattern == UIA_ValuePatternId && kind_ == Kind::edit && !protected_) *output = static_cast<IValueProvider*>(this);
            else if (pattern == UIA_TextPatternId && kind_ == Kind::edit && !protected_) *output = static_cast<ITextProvider*>(this);
            else if (pattern == UIA_TextPattern2Id && kind_ == Kind::edit && !protected_) *output = static_cast<ITextProvider2*>(this);
            if (*output) AddRef();
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* output) noexcept override {
        if (!output) return E_POINTER;
        VariantInit(output);
        return com_call([&] {
            Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result;
            const auto boolean = [&](bool value) { output->vt = VT_BOOL; output->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; };
            const auto integer = [&](LONG value) { output->vt = VT_I4; output->lVal = value; return S_OK; };
            const auto string = [&](const std::wstring& value) {
                BSTR text = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
                if (!text) return E_OUTOFMEMORY;
                output->vt = VT_BSTR; output->bstrVal = text; return S_OK;
            };
            switch (property) {
            case UIA_NamePropertyId: return string(snapshot.node.name);
            case UIA_AutomationIdPropertyId: return string((key_ >= 1000 ? L"nstu.contests." : L"nstu.settings.") + std::to_wstring(key_));
            case UIA_ControlTypePropertyId: {
                LONG type = UIA_PaneControlTypeId;
                switch (kind_) {
                case Kind::pane: break;
                case Kind::text: type = UIA_TextControlTypeId; break;
                case Kind::button: type = UIA_ButtonControlTypeId; break;
                case Kind::combo: type = UIA_ComboBoxControlTypeId; break;
                case Kind::list: type = UIA_ListControlTypeId; break;
                case Kind::group: type = UIA_GroupControlTypeId; break;
                case Kind::radio: type = UIA_RadioButtonControlTypeId; break;
                case Kind::list_item: type = UIA_ListItemControlTypeId; break;
                case Kind::edit: type = UIA_EditControlTypeId; break;
                case Kind::checkbox: type = UIA_CheckBoxControlTypeId; break;
                case Kind::tab: type = UIA_TabControlTypeId; break;
                case Kind::tab_item: type = UIA_TabItemControlTypeId; break;
                case Kind::disclosure: case Kind::row: type = UIA_GroupControlTypeId; break;
                case Kind::status: type = UIA_TextControlTypeId; break;
                }
                return integer(type);
            }
            case UIA_BoundingRectanglePropertyId: return rectangle_variant(snapshot.node.bounds, *output);
            case UIA_IsControlElementPropertyId: return boolean(true);
            case UIA_IsContentElementPropertyId: return boolean(kind_ != Kind::pane);
            case UIA_IsEnabledPropertyId: return boolean(snapshot.enabled);
            case UIA_IsOffscreenPropertyId: return boolean(snapshot.offscreen);
            case UIA_HasKeyboardFocusPropertyId: return boolean(snapshot.node.focused && !snapshot.offscreen);
            case UIA_IsKeyboardFocusablePropertyId: return boolean(focusable(kind_));
            case UIA_IsPasswordPropertyId: return boolean(protected_);
            case UIA_IsInvokePatternAvailablePropertyId: return boolean(kind_ == Kind::button);
            case UIA_IsSelectionPatternAvailablePropertyId: return boolean(container(kind_));
            case UIA_IsSelectionItemPatternAvailablePropertyId: return boolean(selectable(kind_));
            case UIA_IsExpandCollapsePatternAvailablePropertyId: return boolean(expandable(kind_));
            case UIA_IsTogglePatternAvailablePropertyId: return boolean(kind_ == Kind::checkbox);
            case UIA_IsScrollItemPatternAvailablePropertyId: return boolean(can_scroll_);
            case UIA_IsValuePatternAvailablePropertyId: case UIA_IsTextPatternAvailablePropertyId:
            case UIA_IsTextPattern2AvailablePropertyId: return boolean(kind_ == Kind::edit && !protected_);
            case UIA_SelectionCanSelectMultiplePropertyId: if (container(kind_)) return boolean(false); break;
            case UIA_SelectionIsSelectionRequiredPropertyId: if (container(kind_)) return boolean(true); break;
            case UIA_SelectionItemIsSelectedPropertyId: if (selectable(kind_)) return boolean(snapshot.node.selected); break;
            case UIA_ExpandCollapseExpandCollapseStatePropertyId:
                if (expandable(kind_)) return integer(snapshot.node.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed);
                break;
            case UIA_ToggleToggleStatePropertyId:
                if (kind_ == Kind::checkbox) return integer(snapshot.node.checked ? ToggleState_On : ToggleState_Off);
                break;
            case UIA_LiveSettingPropertyId: return integer(static_cast<LONG>(snapshot.node.live && !protected_ ? LiveSetting::Polite : LiveSetting::Off));
            case UIA_ValueIsReadOnlyPropertyId: if (kind_ == Kind::edit && !protected_) return boolean(snapshot.node.read_only); break;
            case UIA_ValueValuePropertyId:
                if (protected_) return reserved_variant(*output);
                if (kind_ == Kind::edit && snapshot.node.text) return string(snapshot.node.text->value);
                break;
            case UIA_HelpTextPropertyId:
                if (protected_) return reserved_variant(*output);
                break;
            default: break;
            }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** output) noexcept override {
        if (!output) return E_POINTER;
        // Retain the original HWND identity; disconnect may run after HWND destruction.
        *output = host_.get();
        if (*output) (*output)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            const std::lock_guard lock(context->mutex);
            if (!valid(*context)) return unavailable;
            const auto* current = key_ ? find(*context, key_) : nullptr;
            Provider* target = nullptr;
            switch (direction) {
            case NavigateDirection_Parent:
                if (current) target = current->node.parent ? find(*context, current->node.parent)->provider.get() : context->root.get();
                break;
            case NavigateDirection_FirstChild: case NavigateDirection_LastChild:
                for (const auto& entry : context->entries) if (entry.node.parent == key_) {
                    target = entry.provider.get();
                    if (direction == NavigateDirection_FirstChild) break;
                }
                break;
            case NavigateDirection_NextSibling: case NavigateDirection_PreviousSibling:
                if (current) {
                    Provider* previous = nullptr;
                    bool passed = false;
                    for (const auto& entry : context->entries) if (entry.node.parent == current->node.parent) {
                        if (entry.node.key == key_) { if (direction == NavigateDirection_PreviousSibling) { target = previous; break; } passed = true; }
                        else if (passed) { target = entry.provider.get(); break; }
                        else previous = entry.provider.get();
                    }
                }
                break;
            default: return E_INVALIDARG;
            }
            if (target) { target->AddRef(); *output = static_cast<IRawElementProviderFragment*>(target); }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            // Runtime identity belongs to this provider lifetime, including UIA disconnect.
            if (!key_) return S_OK;
            SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
            if (!array) return E_OUTOFMEMORY;
            LONG values[]{UiaAppendRuntimeId, incarnation_};
            for (LONG index = 0; index < 2; ++index) {
                const HRESULT stored = SafeArrayPutElement(array, &index, &values[index]);
                if (FAILED(stored)) { SafeArrayDestroy(array); return stored; }
            }
            *output = array;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* output) noexcept override {
        if (!output) return E_POINTER;
        *output = {};
        return com_call([&] { Snapshot snapshot; const HRESULT result = read(snapshot); if (SUCCEEDED(result)) *output = snapshot.node.bounds; return result; });
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot snapshot; return read(snapshot); });
    }
    HRESULT STDMETHODCALLTYPE SetFocus() noexcept override { return enqueue(Action::focus); }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** output) noexcept override {
        if (!output) return E_POINTER;
        Provider* root = key_ ? fragment_root_.get() : this;
        *output = static_cast<IRawElementProviderFragmentRoot*>(root);
        if (!root) return unavailable;
        root->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            if (!std::isfinite(x) || !std::isfinite(y)) return E_INVALIDARG;
            auto context = context_.lock(); if (!context) return unavailable;
            const std::lock_guard lock(context->mutex); if (!valid(*context)) return unavailable;
            if (!context->visible) return S_OK;
            Provider* target = nullptr;
            int layer = std::numeric_limits<int>::min();
            for (const auto& entry : context->entries) {
                bool enabled = false, offscreen = false; effective_state(*context, entry.node, enabled, offscreen);
                if (!offscreen && entry.node.layer >= layer && contains(entry.node.hit_bounds.value_or(entry.node.bounds), x, y)) {
                    target = entry.provider.get(); layer = entry.node.layer;
                }
            }
            if (target) { target->AddRef(); *output = static_cast<IRawElementProviderFragment*>(target); }
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            const std::lock_guard lock(context->mutex); if (!valid(*context)) return unavailable;
            for (const auto& entry : context->entries) {
                bool enabled = false, offscreen = false; effective_state(*context, entry.node, enabled, offscreen);
                if (entry.node.focused && !offscreen) { entry.provider->AddRef(); *output = static_cast<IRawElementProviderFragment*>(entry.provider.get()); break; }
            }
            return S_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE Invoke() noexcept override { return enqueue(Action::invoke); }
    HRESULT STDMETHODCALLTYPE Select() noexcept override { return enqueue(Action::select); }
    HRESULT STDMETHODCALLTYPE AddToSelection() noexcept override { return selection_noop(false); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() noexcept override { return selection_noop(true); }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* output) noexcept override {
        if (!output) return E_POINTER;
        *output = FALSE;
        return com_call([&] { Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result; if (!selectable(kind_)) return invalid_operation; *output = snapshot.node.selected ? TRUE : FALSE; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            const std::lock_guard lock(context->mutex); if (!valid(*context)) return unavailable;
            if (!selectable(kind_)) return invalid_operation;
            const auto* parent = find(*context, find(*context, key_)->node.parent);
            if (!parent) return unavailable;
            parent->provider->AddRef(); *output = static_cast<IRawElementProviderSimple*>(parent->provider.get()); return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** output) noexcept override {
        if (kind_ == Kind::edit) return text_selection(output);
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            Owner selected;
            {
                const std::lock_guard lock(context->mutex); if (!valid(*context)) return unavailable;
                if (!container(kind_)) return invalid_operation;
                for (const auto& entry : context->entries) if (selectable(entry.node.kind) && entry.node.selected &&
                    (entry.node.parent == key_ || (kind_ == Kind::combo && descendant(*context, entry.node, key_)))) {
                    entry.provider->AddRef(); selected.reset(entry.provider.get()); break;
                }
            }
            if (!selected) return unavailable;
            SAFEARRAY* array = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
            if (!array) return E_OUTOFMEMORY;
            LONG index = 0;
            IUnknown* unknown = static_cast<IRawElementProviderSimple*>(selected.get());
            const HRESULT result = SafeArrayPutElement(array, &index, unknown);
            if (FAILED(result)) { SafeArrayDestroy(array); return result; }
            *output = array;
            return S_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* output) noexcept override { return selection_flag(output, false); }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* output) noexcept override { return selection_flag(output, true); }
    HRESULT STDMETHODCALLTYPE Expand() noexcept override { return enqueue(Action::expand); }
    HRESULT STDMETHODCALLTYPE Collapse() noexcept override { return enqueue(Action::collapse); }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* output) noexcept override {
        if (!output) return E_POINTER;
        *output = ExpandCollapseState_Collapsed;
        return com_call([&] { Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result; if (!expandable(kind_)) return invalid_operation; *output = snapshot.node.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE Toggle() noexcept override { return enqueue(Action::toggle); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* output) noexcept override {
        if (!output) return E_POINTER;
        *output = ToggleState_Off;
        return com_call([&] { Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result;
            if (kind_ != Kind::checkbox) return invalid_operation;
            *output = snapshot.node.checked ? ToggleState_On : ToggleState_Off; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE ScrollIntoView() noexcept override { return enqueue(Action::scroll); }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) noexcept override {
        if (!value) return E_INVALIDARG;
        return com_call([&] {
            std::size_t length = 0;
            while (length < 2048 && value[length]) ++length;
            if (length == 2048) return E_INVALIDARG;
            return enqueue(Action::set_value, std::wstring(value, length));
        });
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot snapshot; const HRESULT result = text_read(snapshot); if (FAILED(result)) return result;
            *output = SysAllocStringLen(snapshot.node.text->value.data(), static_cast<UINT>(snapshot.node.text->value.size()));
            return *output ? S_OK : E_OUTOFMEMORY; });
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* output) noexcept override {
        if (!output) return E_POINTER;
        *output = TRUE;
        return com_call([&] { Snapshot snapshot; const HRESULT result = text_read(snapshot); if (FAILED(result)) return result;
            *output = snapshot.node.read_only ? TRUE : FALSE; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** output) noexcept override;
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** output) noexcept override;
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint point, ITextRangeProvider** output) noexcept override;
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider** output) noexcept override;
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection* output) noexcept override;
    HRESULT STDMETHODCALLTYPE RangeFromAnnotation(IRawElementProviderSimple*, ITextRangeProvider** output) noexcept override;
    HRESULT STDMETHODCALLTYPE GetCaretRange(BOOL* active, ITextRangeProvider** output) noexcept override;

private:
    friend struct TextRange;
    struct Snapshot { std::shared_ptr<Shared> context; Node node; bool enabled = false; bool offscreen = false; };
    std::weak_ptr<Shared> context_;
    const Key key_;
    const LONG incarnation_;
    const Kind kind_;
    const bool protected_, can_scroll_;
    // Children retain only root identity; the root's weak context prevents an ownership cycle.
    const Owner fragment_root_;
    const Host host_;
    std::atomic<ULONG> references_{1};

    bool valid(const Shared& context) const noexcept {
        if (!context.attached || !context.ready) return false;
        if (!key_) return true;
        const auto* entry = find(context, key_);
        return entry && entry->incarnation == incarnation_;
    }
    HRESULT read(Snapshot& output) {
        auto context = context_.lock(); if (!context) return unavailable;
        const std::lock_guard lock(context->mutex); if (!valid(*context)) return unavailable;
        if (!key_) { output.node = Node{0, 0, L"NSTU", Kind::pane, context->bounds}; output.enabled = true; output.offscreen = !context->visible; }
        else { output.node = find(*context, key_)->node; effective_state(*context, output.node, output.enabled, output.offscreen); }
        output.context = std::move(context);
        return S_OK;
    }
    HRESULT text_read(Snapshot& output) {
        const HRESULT result = read(output);
        if (FAILED(result)) return result;
        return kind_ == Kind::edit && !protected_ && output.node.text ? S_OK : invalid_operation;
    }
    HRESULT text_selection(SAFEARRAY** output) noexcept;
    HRESULT enqueue(Action action, std::wstring value = {}, std::uint64_t revision = 0, LONG start = 0, LONG end = 0, bool align_to_top = true) noexcept {
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            std::shared_ptr<Completion> completion;
            Command command{key_, incarnation_, action};
            command.value = std::move(value); command.text_revision = revision;
            command.range_start = start; command.range_end = end; command.align_to_top = align_to_top;
            {
                const std::lock_guard lock(context->mutex);
                const auto* entry = find(*context, key_);
                if (action == Action::toggle && entry) command.target_checked = !entry->node.checked;
                if (action == Action::set_value && entry && entry->node.text) command.text_revision = entry->node.text->revision;
                const HRESULT result = validate_command(*context, command); if (FAILED(result)) return result;
                if (action == Action::set_value && entry->node.text->input == TextInput::integer) {
                    std::size_t offset = command.value.front() == L'-' || command.value.front() == L'+' ? 1 : 0;
                    std::int64_t number = 0;
                    for (; offset < command.value.size(); ++offset) number = number * 10 + (command.value[offset] - L'0');
                    if (command.value.front() == L'-') number = -number;
                    command.value = std::to_wstring(number);
                }
                if (action != Action::invoke && GetCurrentThreadId() == context->ui_thread) return invalid_operation;
                if (context->commands.size() >= kQueueLimit) return static_cast<HRESULT>(UIA_E_TIMEOUT);
                if (context->next_request == std::numeric_limits<std::uint64_t>::max()) return E_OUTOFMEMORY;
                completion = std::make_shared<Completion>();
                command.request = context->next_request++;
                context->commands.push_back({command, completion, false});
                // PostMessage does not call the UI synchronously; failed wakes cannot escape this queue lock.
                if (!PostMessageW(context->window, kCommandMessage, 0, 0)) {
                    const DWORD error = GetLastError(); context->commands.pop_back();
                    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
                }
            }
            if (action == Action::invoke) return S_OK; // Invoke retains ownership but never waits for the UI.
            HRESULT result = S_OK;
            {
                std::unique_lock lock(completion->mutex);
                const auto finished = [&] { return completion->stage == Completion::Stage::complete || completion->stage == Completion::Stage::canceled; };
                if (!completion->changed.wait_for(lock, std::chrono::seconds(1), finished)) {
                    if (completion->stage == Completion::Stage::waiting) {
                        completion->stage = Completion::Stage::canceled;
                        completion->result = static_cast<HRESULT>(UIA_E_TIMEOUT);
                    } else {
                        // shortcut: claimed local actions must finish; use transactional cancellation before admitting blocking actions.
                        completion->changed.wait(lock, finished);
                    }
                }
                result = completion->result;
            }
            if (result == static_cast<HRESULT>(UIA_E_TIMEOUT)) {
                const std::lock_guard lock(context->mutex);
                const auto pending = std::find_if(context->commands.begin(), context->commands.end(), [&](const Pending& value) { return value.command.request == command.request; });
                if (pending != context->commands.end()) context->commands.erase(pending);
            }
            return result;
        });
    }
    HRESULT selection_noop(bool remove) noexcept {
        return com_call([&] {
            auto context = context_.lock(); if (!context) return unavailable;
            const std::lock_guard lock(context->mutex);
            const HRESULT result = validate_command(*context, {key_, incarnation_, Action::select}); if (FAILED(result)) return result;
            const bool selected = find(*context, key_)->node.selected;
            return selected == remove ? invalid_operation : S_OK;
        });
    }
    HRESULT selection_flag(BOOL* output, bool value) noexcept {
        if (!output) return E_POINTER;
        *output = FALSE;
        return com_call([&] { Snapshot snapshot; const HRESULT result = read(snapshot); if (FAILED(result)) return result; if (!container(kind_)) return invalid_operation; *output = value ? TRUE : FALSE; return S_OK; });
    }
};

inline void ReleaseProvider::operator()(Provider* value) const noexcept { if (value) value->Release(); }
inline Owner retain(Provider* provider) noexcept { if (provider) provider->AddRef(); return Owner(provider); }

struct TextRange final : ITextRangeProvider, IRangeState {
    TextRange(Owner owner, std::uint64_t revision, LONG start, LONG end) noexcept
        : owner_(std::move(owner)), revision_(revision), start_(start), end_(end) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ITextRangeProvider)) *output = static_cast<ITextRangeProvider*>(this);
        else if (iid == kTextRangeStateId) *output = static_cast<IRangeState*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return references_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        const ULONG remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!remaining) { const std::unique_ptr<TextRange> self(this); }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetState(IUnknown** owner, std::uint64_t* revision, LONG* start, LONG* end) noexcept override {
        if (owner) *owner = nullptr;
        if (revision) *revision = 0;
        if (start) *start = 0;
        if (end) *end = 0;
        if (!owner || !revision || !start || !end) return E_POINTER;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            const HRESULT identity = owner_->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(owner));
            if (FAILED(identity)) return identity;
            *revision = revision_; *start = state.start; *end = state.end; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            *output = make(state.start, state.end).release(); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* target, BOOL* output) noexcept override {
        if (!output) return E_POINTER;
        *output = FALSE;
        return com_call([&] { Snapshot state; LONG start = 0, end = 0; const HRESULT result = peer(target, state, start, end);
            if (FAILED(result)) return result;
            *output = state.start == start && state.end == end ? TRUE : FALSE; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint endpoint, ITextRangeProvider* target,
                                               TextPatternRangeEndpoint target_endpoint, int* output) noexcept override {
        if (!output) return E_POINTER;
        *output = 0;
        if (!endpoint_valid(endpoint) || !endpoint_valid(target_endpoint)) return E_INVALIDARG;
        return com_call([&] { Snapshot state; LONG start = 0, end = 0; const HRESULT result = peer(target, state, start, end);
            if (FAILED(result)) return result;
            *output = static_cast<int>((endpoint == TextPatternRangeEndpoint_Start ? state.start : state.end) -
                                      (target_endpoint == TextPatternRangeEndpoint_Start ? start : end)); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) noexcept override {
        if (!text_unit_valid(unit)) return E_INVALIDARG;
        return com_call([&] { const std::lock_guard lock(mutex_); Provider::Snapshot state; const HRESULT result = read_locked(state);
            if (FAILED(result)) return result;
            const auto units = text_units(state.node.text->value, unit);
            if (units.size() == 1) { start_ = end_ = 0; return S_OK; }
            const auto after = std::upper_bound(units.begin(), units.end(), start_);
            const std::size_t index = std::min(static_cast<std::size_t>(after - units.begin() - 1), units.size() - 2);
            start_ = units[index]; end_ = units[index + 1]; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID attribute, VARIANT value, BOOL backward, ITextRangeProvider** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            if (attribute != UIA_IsReadOnlyAttributeId && attribute != UIA_IsHiddenAttributeId) return S_OK;
            if (value.vt != VT_BOOL) return E_INVALIDARG;
            const bool wanted = value.boolVal != VARIANT_FALSE;
            if (attribute == UIA_IsReadOnlyAttributeId) {
                if (wanted == state.element.node.read_only) *output = make(state.start, state.end).release();
                return S_OK;
            }
            if (state.start == state.end) {
                if (wanted == !caret_visible(state.element, state.start)) *output = make(state.start, state.end).release();
                return S_OK;
            }
            std::vector<std::pair<LONG,LONG>> matches;
            for (const auto& glyph : state.element.node.text->glyphs) if (glyph.end > state.start && glyph.start < state.end) {
                const bool hidden = !glyph_visible(state.element, glyph);
                if (hidden != wanted) continue;
                const LONG start = std::max(state.start, glyph.start), end = std::min(state.end, glyph.end);
                if (!matches.empty() && matches.back().second == start) matches.back().second = end;
                else matches.emplace_back(start, end);
            }
            if (!matches.empty()) { const auto match = backward ? matches.back() : matches.front(); *output = make(match.first, match.second).release(); }
            return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE FindText(BSTR text, BOOL backward, BOOL ignore_case, ITextRangeProvider** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (!text) return E_INVALIDARG;
        return com_call([&] { const std::wstring_view needle(text, SysStringLen(text));
            if (needle.empty() || !valid_value(needle, 2048)) return E_INVALIDARG;
            Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            const auto& contents = state.element.node.text->value;
            std::optional<LONG> found;
            const LONG size = static_cast<LONG>(needle.size());
            for (LONG start = state.start; start + size <= state.end; ++start) {
                if (!utf16_boundary(contents, start) || !utf16_boundary(contents, start + size)) continue;
                bool match = false;
                if (ignore_case) {
                    const int compared = CompareStringOrdinal(contents.data() + start, size, needle.data(), size, TRUE);
                    if (!compared) { const DWORD error = GetLastError(); return error ? HRESULT_FROM_WIN32(error) : E_FAIL; }
                    match = compared == CSTR_EQUAL;
                } else match = std::wstring_view(contents).substr(static_cast<std::size_t>(start), needle.size()) == needle;
                if (match) { found = start; if (!backward) break; }
            }
            if (found) *output = make(*found, *found + size).release();
            return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID attribute, VARIANT* output) noexcept override {
        if (!output) return E_POINTER;
        VariantInit(output);
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            if (attribute == UIA_IsReadOnlyAttributeId) {
                output->vt = VT_BOOL; output->boolVal = state.element.node.read_only ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;
            }
            if (attribute != UIA_IsHiddenAttributeId) return reserved_variant(*output);
            bool visible = false, hidden = false;
            if (state.start == state.end) { visible = caret_visible(state.element, state.start); hidden = !visible; }
            for (const auto& glyph : state.element.node.text->glyphs) if (glyph.end > state.start && glyph.start < state.end) {
                const bool shown = glyph_visible(state.element, glyph); visible = visible || shown; hidden = hidden || !shown;
            }
            if (visible && hidden) return reserved_variant(*output, true);
            output->vt = VT_BOOL; output->boolVal = hidden ? VARIANT_TRUE : VARIANT_FALSE; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            std::optional<UiaRect> bounds;
            if (state.start != state.end) for (const auto& glyph : state.element.node.text->glyphs)
                if (glyph.end > state.start && glyph.start < state.end && glyph_visible(state.element, glyph)) {
                    const auto visible = intersection(glyph.bounds, state.element.node.text->clip);
                    if (!bounds) bounds = visible;
                    else {
                        const double right = std::max(bounds->left + bounds->width, visible.left + visible.width);
                        const double bottom = std::max(bounds->top + bounds->height, visible.top + visible.height);
                        bounds->left = std::min(bounds->left, visible.left); bounds->top = std::min(bounds->top, visible.top);
                        bounds->width = right - bounds->left; bounds->height = bottom - bounds->top;
                    }
                }
            SAFEARRAY* array = SafeArrayCreateVector(VT_R8, 0, bounds ? 4 : 0);
            if (!array) return E_OUTOFMEMORY;
            if (bounds) {
                double values[]{bounds->left, bounds->top, bounds->width, bounds->height};
                for (LONG index = 0; index < 4; ++index) {
                    const HRESULT stored = SafeArrayPutElement(array, &index, &values[index]);
                    if (FAILED(stored)) { SafeArrayDestroy(array); return stored; }
                }
            }
            *output = array; return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            owner_->AddRef(); *output = static_cast<IRawElementProviderSimple*>(owner_.get()); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetText(int maximum, BSTR* output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (maximum < -1) return E_INVALIDARG;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            const auto& text = state.element.node.text->value;
            LONG end = state.end;
            if (maximum >= 0 && maximum < state.end - state.start) {
                const auto characters = character_boundaries(text);
                const auto after = std::upper_bound(characters.begin(), characters.end(), state.start + maximum);
                end = std::max(state.start, *std::prev(after));
            }
            *output = SysAllocStringLen(text.data() + state.start, static_cast<UINT>(end - state.start));
            return *output ? S_OK : E_OUTOFMEMORY; });
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count, int* output) noexcept override {
        if (!output) return E_POINTER;
        *output = 0;
        if (!text_unit_valid(unit)) return E_INVALIDARG;
        return com_call([&] { const std::lock_guard lock(mutex_); Provider::Snapshot state; const HRESULT result = read_locked(state);
            if (FAILED(result) || !count) return result;
            const auto units = text_units(state.node.text->value, unit);
            if (units.size() == 1) return S_OK;
            if (start_ == end_) {
                LONG position = start_; const int moved = move_endpoint(units, position, count);
                start_ = end_ = position; *output = moved; return S_OK;
            }
            const auto after = std::upper_bound(units.begin(), units.end(), start_);
            const auto index = static_cast<std::int64_t>(after - units.begin() - 1);
            const auto maximum = static_cast<std::int64_t>(units.size() - 2);
            const auto target = std::clamp(index + static_cast<std::int64_t>(count), std::int64_t{0}, maximum);
            if (target == index) return S_OK;
            start_ = units[static_cast<std::size_t>(target)]; end_ = units[static_cast<std::size_t>(target + 1)];
            *output = static_cast<int>(target - index); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint, TextUnit unit, int count, int* output) noexcept override {
        if (!output) return E_POINTER;
        *output = 0;
        if (!endpoint_valid(endpoint) || !text_unit_valid(unit)) return E_INVALIDARG;
        return com_call([&] { const std::lock_guard lock(mutex_); Provider::Snapshot state; const HRESULT result = read_locked(state);
            if (FAILED(result)) return result;
            const auto units = text_units(state.node.text->value, unit);
            LONG position = endpoint == TextPatternRangeEndpoint_Start ? start_ : end_;
            *output = move_endpoint(units, position, count);
            set_endpoint(endpoint, position); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint endpoint, ITextRangeProvider* target,
                                                  TextPatternRangeEndpoint target_endpoint) noexcept override {
        if (!endpoint_valid(endpoint) || !endpoint_valid(target_endpoint)) return E_INVALIDARG;
        return com_call([&] { Snapshot state; LONG start = 0, end = 0; const HRESULT result = peer(target, state, start, end);
            if (FAILED(result)) return result;
            const std::lock_guard lock(mutex_); Provider::Snapshot current; const HRESULT checked = read_locked(current);
            if (FAILED(checked)) return checked;
            set_endpoint(endpoint, target_endpoint == TextPatternRangeEndpoint_Start ? start : end); return S_OK; });
    }
    HRESULT STDMETHODCALLTYPE Select() noexcept override {
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            return owner_->enqueue(Action::text_select, {}, revision_, state.start, state.end); });
    }
    HRESULT STDMETHODCALLTYPE AddToSelection() noexcept override { return selection_unsupported(); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() noexcept override { return selection_unsupported(); }
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL align_to_top) noexcept override {
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            return owner_->enqueue(Action::scroll, {}, revision_, state.start, state.end, align_to_top != FALSE); });
    }
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); if (FAILED(result)) return result;
            *output = SafeArrayCreateVector(VT_UNKNOWN, 0, 0); return *output ? S_OK : E_OUTOFMEMORY; });
    }
private:
    struct Snapshot { Provider::Snapshot element; LONG start = 0, end = 0; };
    const Owner owner_;
    const std::uint64_t revision_;
    std::mutex mutex_;
    LONG start_, end_;
    std::atomic<ULONG> references_{1};
    HRESULT read_locked(Provider::Snapshot& output) {
        const HRESULT result = owner_->text_read(output);
        if (FAILED(result)) return result;
        const auto& text = *output.node.text;
        return text.revision == revision_ && start_ <= end_ && utf16_boundary(text.value, start_) && utf16_boundary(text.value, end_) ? S_OK : unavailable;
    }
    HRESULT snapshot(Snapshot& output) {
        const std::lock_guard lock(mutex_);
        const HRESULT result = read_locked(output.element);
        if (SUCCEEDED(result)) { output.start = start_; output.end = end_; }
        return result;
    }
    RangeOwner make(LONG start, LONG end) const {
        auto range = std::make_unique<TextRange>(retain(owner_.get()), revision_, start, end);
        return RangeOwner(range.release());
    }
    HRESULT peer(ITextRangeProvider* target, Snapshot& state, LONG& start, LONG& end) {
        if (!target) return E_INVALIDARG;
        IRangeState* raw = nullptr;
        const HRESULT queried = target->QueryInterface(kTextRangeStateId, reinterpret_cast<void**>(&raw));
        std::unique_ptr<IRangeState, ReleaseUnknown> metadata(raw);
        if (FAILED(queried)) return queried == E_NOINTERFACE ? E_INVALIDARG : queried;
        if (!metadata) return E_INVALIDARG;
        IUnknown* element = nullptr; std::uint64_t revision = 0;
        const HRESULT acquired = metadata->GetState(&element, &revision, &start, &end);
        Unknown element_owner(element);
        if (FAILED(acquired)) return acquired;
        if (!element) return E_INVALIDARG;
        IUnknown* canonical = nullptr;
        const HRESULT identified = element->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&canonical));
        Unknown canonical_owner(canonical);
        if (FAILED(identified)) return identified;
        IUnknown* mine = nullptr;
        const HRESULT own_identity = owner_->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&mine));
        Unknown own_owner(mine);
        if (FAILED(own_identity)) return own_identity;
        if (canonical != mine) return E_INVALIDARG;
        const HRESULT result = snapshot(state);
        if (FAILED(result)) return result;
        if (revision != revision_) return unavailable;
        const auto& text = state.element.node.text->value;
        return start <= end && utf16_boundary(text, start) && utf16_boundary(text, end) ? S_OK : E_INVALIDARG;
    }
    static bool glyph_visible(const Provider::Snapshot& state, const TextGlyph& glyph) noexcept {
        if (state.offscreen) return false;
        const auto visible = intersection(glyph.bounds, state.node.text->clip);
        return visible.width > 0 && visible.height > 0;
    }
    static bool caret_visible(const Provider::Snapshot& state, LONG offset) noexcept {
        if (state.offscreen) return false;
        const auto& text = *state.node.text;
        const auto caret = std::find_if(text.carets.begin(), text.carets.end(), [&](const auto& value) { return value.offset == offset; });
        if (caret == text.carets.end()) return false;
        const auto visible = intersection(caret->bounds, text.clip);
        return visible.width > 0 && visible.height > 0;
    }
    static int move_endpoint(const std::vector<LONG>& units, LONG& position, int count) {
        int moved = 0;
        while (moved != count) {
            if (count > 0) {
                const auto next = std::upper_bound(units.begin(), units.end(), position);
                if (next == units.end()) break;
                position = *next; ++moved;
            } else {
                const auto next = std::lower_bound(units.begin(), units.end(), position);
                if (next == units.begin()) break;
                position = *std::prev(next); --moved;
            }
        }
        return moved;
    }
    void set_endpoint(TextPatternRangeEndpoint endpoint, LONG position) noexcept {
        if (endpoint == TextPatternRangeEndpoint_Start) { start_ = position; if (end_ < start_) end_ = start_; }
        else { end_ = position; if (start_ > end_) start_ = end_; }
    }
    HRESULT selection_unsupported() noexcept {
        return com_call([&] { Snapshot state; const HRESULT result = snapshot(state); return FAILED(result) ? result : invalid_operation; });
    }
};

inline RangeOwner make_range(Provider* owner, std::uint64_t revision, LONG start, LONG end) {
    auto range = std::make_unique<TextRange>(retain(owner), revision, start, end);
    return RangeOwner(range.release());
}
inline HRESULT range_array(std::span<const RangeOwner> ranges, SAFEARRAY** output) noexcept {
    SAFEARRAY* array = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(ranges.size()));
    if (!array) return E_OUTOFMEMORY;
    for (LONG index = 0; static_cast<std::size_t>(index) < ranges.size(); ++index) {
        IUnknown* range = ranges[static_cast<std::size_t>(index)].get();
        const HRESULT result = SafeArrayPutElement(array, &index, range);
        if (FAILED(result)) { SafeArrayDestroy(array); return result; }
    }
    *output = array; return S_OK;
}
inline HRESULT Provider::get_DocumentRange(ITextRangeProvider** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        *output = make_range(this, state.node.text->revision, 0, static_cast<LONG>(state.node.text->value.size())).release(); return S_OK; });
}
inline HRESULT Provider::text_selection(SAFEARRAY** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        const auto& text = *state.node.text;
        std::vector<RangeOwner> ranges;
        ranges.push_back(make_range(this, text.revision, std::min(text.selection_start, text.selection_end), std::max(text.selection_start, text.selection_end)));
        return range_array(ranges, output); });
}
inline HRESULT Provider::get_SupportedTextSelection(SupportedTextSelection* output) noexcept {
    if (!output) return E_POINTER;
    *output = SupportedTextSelection_None;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        *output = SupportedTextSelection_Single; return S_OK; });
}
inline HRESULT Provider::GetCaretRange(BOOL* active, ITextRangeProvider** output) noexcept {
    if (active) *active = FALSE;
    if (output) *output = nullptr;
    if (!active || !output) return E_POINTER;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        const auto& text = *state.node.text;
        *output = make_range(this, text.revision, text.caret, text.caret).release();
        *active = state.node.focused && !state.offscreen ? TRUE : FALSE; return S_OK; });
}
inline HRESULT Provider::GetVisibleRanges(SAFEARRAY** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        const auto& text = *state.node.text;
        std::vector<std::pair<LONG,LONG>> visible;
        if (!state.offscreen) for (const auto& glyph : text.glyphs) {
            const auto shown = intersection(glyph.bounds, text.clip);
            if (shown.width <= 0 || shown.height <= 0) continue;
            if (!visible.empty() && visible.back().second == glyph.start) visible.back().second = glyph.end;
            else visible.emplace_back(glyph.start, glyph.end);
        }
        if (!state.offscreen && text.value.empty()) {
            const auto shown = intersection(text.carets.front().bounds, text.clip);
            if (shown.width > 0 && shown.height > 0) visible.emplace_back(0,0);
        }
        std::vector<RangeOwner> ranges;
        for (const auto [start,end] : visible) ranges.push_back(make_range(this, text.revision, start, end));
        return range_array(ranges, output); });
}
inline HRESULT Provider::RangeFromPoint(UiaPoint point, ITextRangeProvider** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return E_INVALIDARG;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); if (FAILED(result)) return result;
        const auto& text = *state.node.text;
        if (state.offscreen || !contains(text.clip, point.x, point.y)) return E_INVALIDARG;
        const TextCaret* closest = nullptr; double distance = std::numeric_limits<double>::max();
        for (const auto& caret : text.carets) {
            // The visible middle of a wide glyph may have both real caret boundaries outside the clip.
            const double horizontal = std::abs(point.x - caret.bounds.left);
            if (horizontal < distance) { closest = &caret; distance = horizontal; }
        }
        if (!closest) return invalid_operation;
        *output = make_range(this, text.revision, closest->offset, closest->offset).release(); return S_OK; });
}
inline HRESULT Provider::RangeFromChild(IRawElementProviderSimple*, ITextRangeProvider** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); return FAILED(result) ? result : E_INVALIDARG; });
}
inline HRESULT Provider::RangeFromAnnotation(IRawElementProviderSimple*, ITextRangeProvider** output) noexcept {
    if (!output) return E_POINTER;
    *output = nullptr;
    return com_call([&] { Snapshot state; const HRESULT result = text_read(state); return FAILED(result) ? result : E_INVALIDARG; });
}

inline bool validate_nodes(std::span<const Node> nodes) noexcept {
    if (nodes.size() > kNodeLimit) return false;
    std::size_t focused = 0;
    const auto lookup = [&](Key key) -> const Node* {
        const auto found = std::find_if(nodes.begin(), nodes.end(), [key](const Node& value) { return value.key == key; });
        return found == nodes.end() ? nullptr : &*found;
    };
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const Node& node = nodes[index];
        if (!node.key || node.name.empty() || node.name.size() > 4096 || node.name.find(L'\0') != std::wstring::npos ||
            !rectangle_valid(node.bounds) || (node.hit_bounds && !rectangle_valid(*node.hit_bounds))) return false;
        for (std::size_t other = 0; other < index; ++other) if (nodes[other].key == node.key) return false;
        switch (node.kind) {
        case Kind::pane: case Kind::text: case Kind::button: case Kind::combo: case Kind::list: case Kind::group: case Kind::radio: case Kind::list_item:
        case Kind::edit: case Kind::checkbox: case Kind::tab: case Kind::tab_item: case Kind::disclosure: case Kind::row: case Kind::status: break;
        default: return false;
        }
        if (node.selected && !selectable(node.kind)) return false;
        if (node.checked && node.kind != Kind::checkbox) return false;
        if (node.live && (node.kind != Kind::status || node.protected_value)) return false;
        if (node.protected_value && node.text) return false;
        if (node.kind == Kind::edit && !node.protected_value && (!node.text || !valid_text(*node.text))) return false;
        if (node.kind != Kind::edit && node.text) return false;
        if (node.focused && (!focusable(node.kind) || ++focused > 1)) return false;
        Key parent = node.parent;
        std::size_t depth = 0;
        while (parent) {
            const Node* ancestor = lookup(parent);
            if (!ancestor || ++depth > nodes.size()) return false;
            if (ancestor->kind == Kind::combo && !ancestor->expanded && !node.offscreen) return false;
            if (ancestor->kind == Kind::disclosure && !ancestor->expanded && !node.hidden) return false;
            parent = ancestor->parent;
        }
        const Node* direct_parent = node.parent ? lookup(node.parent) : nullptr;
        if (node.kind == Kind::radio && (!direct_parent || direct_parent->kind != Kind::group)) return false;
        if (node.kind == Kind::list_item && (!direct_parent || direct_parent->kind != Kind::list)) return false;
        if (node.kind == Kind::tab_item && (!direct_parent || direct_parent->kind != Kind::tab)) return false;
        if (node.scroll_owner) {
            const auto* owner = lookup(node.scroll_owner);
            Key ancestor = node.parent;
            while (ancestor && ancestor != node.scroll_owner) { const auto* candidate = lookup(ancestor); if (!candidate) return false; ancestor = candidate->parent; }
            if (!owner || !owner->scrollable || ancestor != node.scroll_owner) return false;
        }
        if (node.kind == Kind::group || node.kind == Kind::list || node.kind == Kind::tab) {
            std::size_t selected = 0, children = 0;
            for (const Node& child : nodes) if (child.parent == node.key) {
                if (child.kind != (node.kind == Kind::group ? Kind::radio : node.kind == Kind::tab ? Kind::tab_item : Kind::list_item)) return false;
                ++children; if (child.selected) ++selected;
            }
            if (!children || selected != 1) return false;
        }
        if (node.kind == Kind::combo) {
            std::size_t lists = 0;
            for (const Node& child : nodes) if (child.parent == node.key) { if (child.kind != Kind::list) return false; ++lists; }
            if (lists != 1) return false;
        }
    }
    return true;
}
inline bool same_lifetime(const Shared& state, const Node& node, std::span<const Node> next) noexcept {
    const Node* current = &node;
    for (std::size_t depth = 0; current && depth <= next.size(); ++depth) {
        const auto* old = find(state, current->key);
        if (!old || old->node.kind != current->kind || old->node.parent != current->parent || old->node.generation != current->generation ||
            old->node.protected_value != current->protected_value || old->node.scroll_owner != current->scroll_owner || old->node.scrollable != current->scrollable) return false;
        if (!current->parent) return true;
        const auto parent = std::find_if(next.begin(), next.end(), [&](const Node& value) { return value.key == current->parent; });
        if (parent == next.end()) return false;
        current = &*parent;
    }
    return false;
}

// A drawing collector may be incomplete. Validate only this target and its current ancestors.
inline HRESULT validate_projected_command(const Shared& state, const Command& command, const Node& node,
                                          std::span<const Node> current_nodes) {
    if (node.key != command.key || !same_lifetime(state, node, current_nodes)) return unavailable;
    Shared projected;
    projected.attached = state.attached; projected.ready = state.ready; projected.visible = state.visible;
    const Node* current = &node;
    for (std::size_t depth = 0; current && depth <= current_nodes.size(); ++depth) {
        const auto* old = find(state, current->key);
        if (!old || !rectangle_valid(current->bounds) || (current->hit_bounds && !rectangle_valid(*current->hit_bounds))) return E_INVALIDARG;
        if ((current->protected_value && current->text) ||
            (current->kind == Kind::edit && !current->protected_value && (!current->text || !valid_text(*current->text))) ||
            (current->kind != Kind::edit && current->text)) return E_INVALIDARG;
        if (current->text && (!old->node.text || current->text->revision != old->node.text->revision ||
                             current->text->value != old->node.text->value || current->text->input != old->node.text->input ||
                             current->text->utf8_capacity != old->node.text->utf8_capacity)) return unavailable;
        projected.entries.push_back({*current, old->incarnation, {}});
        if (!current->parent) return validate_command(projected, command);
        const auto parent = std::find_if(current_nodes.begin(), current_nodes.end(), [&](const Node& value) { return value.key == current->parent; });
        if (parent == current_nodes.end()) return unavailable;
        current = &*parent;
    }
    return unavailable;
}

struct Change { Owner provider; Node before; Node after; bool existed; };
inline HRESULT raise_changes(std::span<const Change> changes, Provider* root, bool structure) noexcept {
    return com_call([&] {
        if (!UiaClientsAreListening()) return S_OK;
        HRESULT first = S_OK;
        const auto remember = [&](HRESULT result) { if (FAILED(result) && SUCCEEDED(first)) first = result; };
        if (structure) remember(UiaRaiseStructureChangedEvent(static_cast<IRawElementProviderSimple*>(root), StructureChangeType_ChildrenInvalidated, nullptr, 0));
        for (const auto& change : changes) {
            auto* provider = static_cast<IRawElementProviderSimple*>(change.provider.get());
            const auto flag = [&](PROPERTYID property, bool before, bool after) {
                if (before == after && change.existed) return;
                VARIANT old_value{}, new_value{};
                old_value.vt = new_value.vt = VT_BOOL;
                old_value.boolVal = before ? VARIANT_TRUE : VARIANT_FALSE;
                new_value.boolVal = after ? VARIANT_TRUE : VARIANT_FALSE;
                remember(UiaRaiseAutomationPropertyChangedEvent(provider, property, old_value, new_value));
            };
            flag(UIA_IsEnabledPropertyId, change.before.enabled, change.after.enabled);
            flag(UIA_IsOffscreenPropertyId, change.before.offscreen, change.after.offscreen);
            flag(UIA_HasKeyboardFocusPropertyId, change.before.focused, change.after.focused);
            if (change.after.focused && (!change.existed || !change.before.focused)) remember(UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId));
            if (selectable(change.after.kind) && (!change.existed || change.before.selected != change.after.selected)) {
                flag(UIA_SelectionItemIsSelectedPropertyId, change.before.selected, change.after.selected);
                remember(UiaRaiseAutomationEvent(provider, change.after.selected ? UIA_SelectionItem_ElementSelectedEventId : UIA_SelectionItem_ElementRemovedFromSelectionEventId));
            }
            if (expandable(change.after.kind) && (!change.existed || change.before.expanded != change.after.expanded)) {
                VARIANT before{}, after{}; before.vt = after.vt = VT_I4;
                before.lVal = change.before.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
                after.lVal = change.after.expanded ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
                remember(UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ExpandCollapseExpandCollapseStatePropertyId, before, after));
            }
            if (change.after.kind == Kind::checkbox && (!change.existed || change.before.checked != change.after.checked)) {
                VARIANT before{}, after{}; before.vt = after.vt = VT_I4;
                before.lVal = change.before.checked ? ToggleState_On : ToggleState_Off;
                after.lVal = change.after.checked ? ToggleState_On : ToggleState_Off;
                remember(UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ToggleToggleStatePropertyId, before, after));
            }
            if (change.after.kind == Kind::edit && !change.after.protected_value && change.after.text) {
                flag(UIA_ValueIsReadOnlyPropertyId, change.before.read_only, change.after.read_only);
                if (change.existed && change.before.text) {
                    const auto& before_text = *change.before.text;
                    const auto& after_text = *change.after.text;
                    if (before_text.value != after_text.value) {
                        VARIANT before{}, after{}; before.vt = after.vt = VT_BSTR;
                        before.bstrVal = SysAllocStringLen(before_text.value.data(), static_cast<UINT>(before_text.value.size()));
                        after.bstrVal = SysAllocStringLen(after_text.value.data(), static_cast<UINT>(after_text.value.size()));
                        if (!before.bstrVal || !after.bstrVal) remember(E_OUTOFMEMORY);
                        else remember(UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ValueValuePropertyId, before, after));
                        VariantClear(&before); VariantClear(&after);
                    }
                    if (before_text.revision != after_text.revision) remember(UiaRaiseAutomationEvent(provider, UIA_Text_TextChangedEventId));
                    if (before_text.selection_start != after_text.selection_start || before_text.selection_end != after_text.selection_end || before_text.caret != after_text.caret)
                        remember(UiaRaiseAutomationEvent(provider, UIA_Text_TextSelectionChangedEventId));
                }
            }
            if (change.after.live && !change.after.protected_value && (!change.existed || change.before.name != change.after.name))
                remember(UiaRaiseAutomationEvent(provider, UIA_LiveRegionChangedEventId));
            if (!change.existed || change.before.name != change.after.name) {
                VARIANT before{}, after{};
                before.vt = after.vt = VT_BSTR;
                before.bstrVal = SysAllocStringLen(change.before.name.data(), static_cast<UINT>(change.before.name.size()));
                after.bstrVal = SysAllocStringLen(change.after.name.data(), static_cast<UINT>(change.after.name.size()));
                if (!before.bstrVal || !after.bstrVal) remember(E_OUTOFMEMORY);
                else remember(UiaRaiseAutomationPropertyChangedEvent(provider, UIA_NamePropertyId, before, after));
                VariantClear(&before); VariantClear(&after);
            }
            if (!change.existed || !rectangle_equal(change.before.bounds, change.after.bounds)) {
                VARIANT before{}, after{};
                HRESULT result = rectangle_variant(change.before.bounds, before);
                if (SUCCEEDED(result)) result = rectangle_variant(change.after.bounds, after);
                if (SUCCEEDED(result)) result = UiaRaiseAutomationPropertyChangedEvent(provider, UIA_BoundingRectanglePropertyId, before, after);
                remember(result); VariantClear(&before); VariantClear(&after);
            }
        }
        return first;
    });
}
} // namespace detail

class Bridge final {
public:
    static constexpr UINT command_message = detail::kCommandMessage;

    explicit Bridge(HWND window) : state_(std::make_shared<detail::Shared>()) {
        DWORD process = 0;
        const DWORD thread = window ? GetWindowThreadProcessId(window, &process) : 0;
        if (!thread || thread != GetCurrentThreadId() || process != GetCurrentProcessId()) throw std::invalid_argument("UIA bridge requires the owning UI thread and HWND");
        state_->window = window;
        state_->ui_thread = thread;
        IRawElementProviderSimple* host_provider = nullptr;
        const HRESULT hosted = UiaHostProviderFromHwnd(window, &host_provider);
        detail::Host host(host_provider);
        if (FAILED(hosted) || !host) throw std::runtime_error("UIA HWND host provider unavailable");
        auto root = std::make_unique<detail::Provider>(state_, 0, 0, Kind::pane, detail::Owner{}, std::move(host));
        state_->root.reset(root.release());
    }
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;
    ~Bridge() { (void)detach(); }

    // UI-thread-only. Names and state are copied; no widget/backend pointers survive publication.
    HRESULT publish(std::span<const Node> nodes, bool visible) noexcept {
        if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
        bool published = false;
        const HRESULT result = detail::com_call([&] {
            if (!detail::validate_nodes(nodes)) return E_INVALIDARG;
            RECT client{};
            POINT origin{};
            HWND window = nullptr;
            { const std::lock_guard lock(state_->mutex); if (!state_->attached) return detail::unavailable; window = state_->window; }
            if (!GetClientRect(window, &client) || !ClientToScreen(window, &origin)) {
                const DWORD error = GetLastError(); return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
            const UiaRect bounds{static_cast<double>(origin.x), static_cast<double>(origin.y),
                                 static_cast<double>(client.right - client.left), static_cast<double>(client.bottom - client.top)};
            std::vector<detail::Entry> next, previous;
            std::vector<detail::Change> changes;
            std::list<detail::Owner> retiring;
            detail::Owner root;
            bool structure = false;
            {
                const std::lock_guard lock(state_->mutex);
                if (!state_->attached) return detail::unavailable;
                next.reserve(nodes.size()); changes.reserve(nodes.size());
                structure = nodes.size() != state_->entries.size();
                for (std::size_t index = 0; index < nodes.size() && index < state_->entries.size(); ++index)
                    structure = structure || nodes[index].key != state_->entries[index].node.key;
                LONG incarnation = state_->next_incarnation;
                for (const Node& node : nodes) {
                    const auto* old = detail::find(*state_, node.key);
                    const bool continuing = detail::same_lifetime(*state_, node, nodes);
                    if (continuing && node.text && old->node.text &&
                        (node.text->revision < old->node.text->revision ||
                         (node.text->value != old->node.text->value && node.text->revision == old->node.text->revision))) return E_INVALIDARG;
                    detail::Owner provider;
                    LONG token = 0;
                    if (continuing) { token = old->incarnation; provider = detail::retain(old->provider.get()); }
                    else {
                        if (incarnation == std::numeric_limits<LONG>::max()) return E_OUTOFMEMORY;
                        token = incarnation++;
                        auto created = std::make_unique<detail::Provider>(state_, node.key, token, node.kind, detail::retain(state_->root.get()),
                                                                        detail::Host{}, node.protected_value, node.scroll_owner != 0);
                        provider.reset(created.release()); structure = true;
                    }
                    Node before = continuing ? old->node : Node{};
                    if (continuing) {
                        bool enabled = false, offscreen = false;
                        detail::effective_state(*state_, before, enabled, offscreen);
                        before.enabled = enabled; before.offscreen = offscreen; before.focused = before.focused && !offscreen;
                    }
                    changes.push_back({detail::retain(provider.get()), std::move(before), node, continuing});
                    next.push_back({node, token, std::move(provider)});
                }
                for (const auto& old : state_->entries) if (std::none_of(next.begin(), next.end(), [&](const detail::Entry& entry) { return entry.provider.get() == old.provider.get(); })) {
                    retiring.emplace_back(nullptr); structure = true;
                }
                if (state_->retired.size() > detail::kNodeLimit || retiring.size() > detail::kNodeLimit - state_->retired.size())
                    return static_cast<HRESULT>(UIA_E_TIMEOUT);
                root = detail::retain(state_->root.get());
                previous.swap(state_->entries); state_->entries.swap(next);
                auto destination = retiring.begin();
                for (auto& old : previous) if (std::none_of(state_->entries.begin(), state_->entries.end(), [&](const detail::Entry& entry) { return entry.provider.get() == old.provider.get(); })) {
                    *destination++ = std::move(old.provider);
                }
                state_->retired.splice(state_->retired.end(), retiring);
                state_->next_incarnation = incarnation; state_->bounds = bounds; state_->visible = visible; state_->ready = true;
                published = true;
                if (!visible) detail::cancel_all(*state_, detail::invalid_operation, true);
                for (auto& change : changes) {
                    bool enabled = false, offscreen = false;
                    detail::effective_state(*state_, change.after, enabled, offscreen);
                    change.after.enabled = enabled; change.after.offscreen = offscreen;
                    change.after.focused = change.after.focused && !offscreen;
                }
            }
            // Snapshot is committed even if event delivery fails; all outbound UIA calls are outside locks.
            return detail::raise_changes(changes, root.get(), structure);
        });
        if (FAILED(result) && !published) {
            const HRESULT invalidated = detail::com_call([&] {
                const std::lock_guard lock(state_->mutex);
                state_->ready = false; state_->visible = false;
                detail::cancel_all(*state_, detail::unavailable);
                return S_OK;
            });
            if (FAILED(invalidated)) return invalidated;
        }
        return result;
    }

    HRESULT set_visible(bool visible) noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            std::vector<detail::Change> changes;
            detail::Owner root;
            {
                const std::lock_guard lock(state_->mutex);
                if (!state_->attached) return detail::unavailable;
                if (state_->visible == visible) return S_OK;
                changes.reserve(state_->entries.size());
                for (const auto& entry : state_->entries) {
                    Node before = entry.node;
                    bool enabled = false, offscreen = false;
                    detail::effective_state(*state_, before, enabled, offscreen);
                    before.enabled = enabled; before.offscreen = offscreen; before.focused = before.focused && !offscreen;
                    changes.push_back({detail::retain(entry.provider.get()), std::move(before), entry.node, true});
                }
                root = detail::retain(state_->root.get()); state_->visible = visible;
                if (!visible) detail::cancel_all(*state_, detail::invalid_operation, true);
                for (auto& change : changes) {
                    bool enabled = false, offscreen = false;
                    detail::effective_state(*state_, change.after, enabled, offscreen);
                    change.after.enabled = enabled; change.after.offscreen = offscreen; change.after.focused = change.after.focused && !offscreen;
                }
            }
            return detail::raise_changes(changes, root.get(), false);
        });
    }

    // Caller owns the returned reference. Null before first publication or after detachment.
    IRawElementProviderSimple* root_provider() noexcept {
        IRawElementProviderSimple* result = nullptr;
        (void)detail::com_call([&] {
            const std::lock_guard lock(state_->mutex);
            if (state_->attached && state_->ready && state_->root) { state_->root->AddRef(); result = static_cast<IRawElementProviderSimple*>(state_->root.get()); }
            return S_OK;
        });
        return result;
    }

    bool handle_getobject(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) noexcept {
        result = 0;
        if (message != WM_GETOBJECT || static_cast<LONG>(lparam) != UiaRootObjectId) return false;
        bool matches = false;
        (void)detail::com_call([&] { const std::lock_guard lock(state_->mutex); matches = state_->attached && state_->window == window; return S_OK; });
        if (!matches) return false;
        IRawElementProviderSimple* provider = root_provider();
        if (!provider) return false;
        result = UiaReturnRawElementProvider(window, wparam, lparam, provider);
        provider->Release();
        return true;
    }

    // UI-thread-only. Dispatch outside the bridge lock through the existing guarded UI handlers.
    HRESULT take_commands(std::vector<Command>& output, std::size_t max_count = detail::kQueueLimit) noexcept {
        output.clear();
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            const std::lock_guard lock(state_->mutex);
            output.reserve(std::min(state_->commands.size(), max_count));
            for (auto pending = state_->commands.begin(); pending != state_->commands.end();) {
                // A deferred edit may already have advanced its revision; its claimant owns completion.
                if (detail::claimed(*pending)) { ++pending; continue; }
                const HRESULT result = detail::validate_command(*state_, pending->command);
                if (FAILED(result) || detail::canceled(*pending)) { detail::finish(*pending, result); pending = state_->commands.erase(pending); }
                else if (!pending->offered && output.size() < max_count) {
                    output.push_back(pending->command);
                    pending->offered = true; ++pending;
                } else ++pending;
            }
            return S_OK;
        });
    }
    HRESULT validate_command(const Command& command) noexcept {
        return detail::com_call([&] {
            const std::lock_guard lock(state_->mutex);
            const auto pending = locate(command);
            if (pending == state_->commands.end()) return detail::unavailable;
            const HRESULT result = detail::validate_command(*state_, command);
            if (FAILED(result)) return result;
            return detail::canceled(*pending) ? static_cast<HRESULT>(UIA_E_TIMEOUT) : S_OK;
        });
    }

    // Claim immediately before a nonblocking local UI mutation. A timed-out unclaimed request cannot be claimed.
    HRESULT begin_command(const Command& command) noexcept {
        return begin(command, nullptr, {});
    }
    HRESULT begin_command(const Command& command, const Node& current, std::span<const Node> current_nodes) noexcept {
        return begin(command, &current, current_nodes);
    }

private:
    HRESULT begin(const Command& command, const Node* current, std::span<const Node> current_nodes) noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            const std::lock_guard lock(state_->mutex);
            const auto pending = locate(command);
            if (pending == state_->commands.end()) return detail::unavailable;
            HRESULT result = detail::validate_command(*state_, command);
            if (SUCCEEDED(result) && current) result = detail::validate_projected_command(*state_, command, *current, current_nodes);
            if (FAILED(result)) { detail::finish(*pending, result); state_->commands.erase(pending); return result; }
            const std::lock_guard completion_lock(pending->completion->mutex);
            if (pending->completion->stage != detail::Completion::Stage::waiting) return detail::invalid_operation;
            pending->completion->stage = detail::Completion::Stage::claimed;
            return S_OK;
        });
    }

public:

    // Complete after publishing the actual resulting selection/expansion/focus. Invoke raises its event here.
    HRESULT complete_command(const Command& command, HRESULT result) noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            detail::Owner invoked;
            {
                const std::lock_guard lock(state_->mutex);
                {
                    const auto pending = locate(command);
                    if (pending == state_->commands.end()) return detail::unavailable;
                    {
                        const std::lock_guard completion_lock(pending->completion->mutex);
                        if (pending->completion->stage != detail::Completion::Stage::claimed &&
                            !(pending->completion->stage == detail::Completion::Stage::waiting && FAILED(result))) return detail::invalid_operation;
                    }
                    if (SUCCEEDED(result)) {
                        const auto* entry = detail::find(*state_, command.key);
                        if (!state_->attached || !state_->ready || !entry ||
                            (command.action != Action::select && entry->incarnation != command.incarnation)) result = detail::unavailable;
                        else {
                            const Node& node = entry->node;
                            bool enabled = false, offscreen = false;
                            detail::effective_state(*state_, node, enabled, offscreen);
                            // A committed selection may close/reincarnate its list; the semantic choice still records the result.
                            bool committed = false;
                            switch (command.action) {
                            case Action::invoke:
                                committed = node.kind == Kind::button;
                                if (committed) invoked = detail::retain(entry->provider.get());
                                break;
                            case Action::select: committed = node.selected; break;
                            case Action::expand: committed = node.expanded; break;
                            case Action::collapse: committed = !node.expanded; break;
                            case Action::focus: committed = node.focused && enabled && !offscreen; break;
                            case Action::toggle: committed = node.checked == command.target_checked; break;
                            case Action::set_value:
                                committed = node.text && node.text->revision > command.text_revision && node.text->value == command.value; break;
                            case Action::text_select:
                                committed = node.text && node.text->revision == command.text_revision &&
                                    std::min(node.text->selection_start, node.text->selection_end) == command.range_start &&
                                    std::max(node.text->selection_start, node.text->selection_end) == command.range_end && node.text->caret == command.range_end; break;
                            case Action::scroll:
                                committed = enabled && !offscreen && detail::scroll_owner_valid(*state_, node);
                                if (committed && command.text_revision) {
                                    committed = node.text && node.text->revision == command.text_revision;
                                    if (committed) {
                                        const LONG target = command.align_to_top ? command.range_start : command.range_end;
                                        const auto caret = std::find_if(node.text->carets.begin(), node.text->carets.end(), [&](const auto& value) { return value.offset == target; });
                                        committed = caret != node.text->carets.end() && detail::intersection(caret->bounds, node.text->clip).width > 0 &&
                                                    detail::intersection(caret->bounds, node.text->clip).height > 0;
                                    }
                                }
                                break;
                            }
                            if (!committed) result = detail::invalid_operation;
                        }
                    }
                    detail::finish(*pending, result); state_->commands.erase(pending);
                }
            }
            if (invoked && UiaClientsAreListening()) return UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(invoked.get()), UIA_Invoke_InvokedEventId);
            return result;
        });
    }

    // The shared pointer/keyboard handler calls this only after its real Button action succeeds.
    HRESULT notify_invoked(Key key) noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            detail::Owner provider;
            {
                const std::lock_guard lock(state_->mutex);
                const auto* entry = detail::find(*state_, key);
                if (!state_->attached || !state_->ready || !entry) return detail::unavailable;
                if (entry->node.kind != Kind::button) return detail::invalid_operation;
                provider = detail::retain(entry->provider.get());
            }
            return UiaClientsAreListening() ? UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(provider.get()), UIA_Invoke_InvokedEventId) : S_OK;
        });
    }

    // State detaches before event-map cleanup; a reentrant WM_GETOBJECT cannot resurrect providers.
    HRESULT detach() noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            HWND window = nullptr;
            {
                const std::lock_guard lock(state_->mutex);
                if (!state_->attached) return S_OK;
                state_->attached = false; state_->visible = false; state_->ready = false;
                detail::cancel_all(*state_, detail::unavailable); window = std::exchange(state_->window, nullptr);
            }
            if (window) (void)UiaReturnRawElementProvider(window, 0, 0, nullptr);
            return S_OK;
        });
    }

    // Call from the outer UI loop, including once after HWND shutdown, before destroying Bridge/COM.
    HRESULT disconnect_retired() noexcept {
        return detail::com_call([&] {
            if (GetCurrentThreadId() != state_->ui_thread) return RPC_E_WRONG_THREAD;
            if (InSendMessageEx(nullptr) != ISMEX_NOSEND) return RPC_E_CANTCALLOUT_ININPUTSYNCCALL;
            std::list<detail::Owner> retired;
            std::vector<detail::Entry> detached;
            detail::Owner root;
            bool detached_at_start = false;
            {
                const std::lock_guard lock(state_->mutex);
                retired.splice(retired.end(), state_->retired);
                detached_at_start = !state_->attached;
                if (detached_at_start) { detached.swap(state_->entries); root = std::move(state_->root); }
            }
            HRESULT first = S_OK;
            const auto disconnect = [&](detail::Owner& provider) {
                if (!provider) return;
                const HRESULT result = UiaDisconnectProvider(static_cast<IRawElementProviderSimple*>(provider.get()));
                if (SUCCEEDED(result)) provider.reset();
                else if (SUCCEEDED(first)) first = result;
            };
            for (auto& provider : retired) disconnect(provider);
            for (auto& entry : detached) disconnect(entry.provider);
            disconnect(root);
            retired.remove_if([](const detail::Owner& provider) { return !provider; });
            detached.erase(std::remove_if(detached.begin(), detached.end(), [](const detail::Entry& entry) { return !entry.provider; }), detached.end());
            {
                const std::lock_guard lock(state_->mutex);
                state_->retired.splice(state_->retired.end(), retired);
                if (detached_at_start) { state_->entries.swap(detached); state_->root = std::move(root); }
            }
            return first;
        });
    }

private:
    std::shared_ptr<detail::Shared> state_;
    std::deque<detail::Pending>::iterator locate(const Command& command) noexcept {
        return std::find_if(state_->commands.begin(), state_->commands.end(), [&](const detail::Pending& pending) {
            return command.request && pending.command.request == command.request && pending.command.key == command.key &&
                   pending.command.incarnation == command.incarnation && pending.command.action == command.action &&
                   pending.command.value == command.value && pending.command.text_revision == command.text_revision &&
                   pending.command.range_start == command.range_start && pending.command.range_end == command.range_end &&
                   pending.command.align_to_top == command.align_to_top && pending.command.target_checked == command.target_checked;
        });
    }
};
} // namespace nstu::ui::accessibility
