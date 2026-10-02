// Live auction adapter. Client addresses are not compiled in: the patcher fills
// the exported auction_imports table with the target client's addresses.
// No fixtures: only validated AUC2 server responses can produce listing rows.
#include "auction_window.hpp"

static_assert(sizeof(void*) == 4, "32-bit client only");

// ============================================================================
// Client addresses (import table)
// ============================================================================

// Filled by the patcher with this client's addresses before the code runs.
struct ClientImports {
    using u32 = auction_live::u32;
    // functions
    u32 allocate, release, destroy_string, assign_string, construct, destruct, base_on_event,
        base_refresh, size_window, move_window, add_child, move_child, dirty, set_window_id,
        drag, drag_move, drag_end, add_window, remove_window, focus, make_window, capture_mouse,
        captured_window, release_mouse, edit_construct, fill, resource_manager, resolve_path,
        get_bitmap, draw_bitmap, measure, print_text, item_descriptor, item_construct,
        item_set_id, network, send_packet;
    // globals and data
    u32 tick_import, manager, screen, frame_vtable;
    // hook continuations
    u32 measure_resume, text_resume, packet_buffer, dispatch_continue;
};
extern "C" __declspec(dllexport) ClientImports auction_imports = {};
extern "C" __declspec(dllexport) const auction_live::u32 auction_imports_size = sizeof(ClientImports);

namespace {
using namespace auction_live;

constexpr int vtable_slots = 53;

#define CLIENT(type, field) reinterpret_cast<type>(auction_imports.field)

// Memory
#define allocate         CLIENT(void* (__cdecl*)(u32), allocate)
#define release          CLIENT(void (__cdecl*)(void*, u32), release)
#define destroy_string   CLIENT(void (__fastcall*)(void*, void*), destroy_string)
#define assign_string    CLIENT(void (__fastcall*)(void*, void*, const char*, u32), assign_string)

// Frame window
#define construct        CLIENT(void* (__fastcall*)(void*, void*, int), construct)
#define destruct         CLIENT(void (__fastcall*)(void*, void*), destruct)
#define base_on_event    CLIENT(int (__fastcall*)(void*, void*, void*, int, int, int, int, int), base_on_event)
#define base_refresh     CLIENT(int (__fastcall*)(void*, void*, int), base_refresh)
#define size_window      CLIENT(void (__fastcall*)(void*, void*, int, int), size_window)
#define move_window      CLIENT(void (__fastcall*)(void*, void*, int, int), move_window)
#define add_child        CLIENT(void (__fastcall*)(void*, void*, void*), add_child)
#define move_child       CLIENT(void (__fastcall*)(void*, void*, int, int), move_child)
#define dirty            CLIENT(void (__fastcall*)(void*, void*), dirty)
#define set_window_id    CLIENT(void (__fastcall*)(void*, void*, int), set_window_id)
#define drag             CLIENT(void (__fastcall*)(void*, void*, int, int), drag)
#define drag_move        CLIENT(void (__fastcall*)(void*, void*, int, int), drag_move)
#define drag_end         CLIENT(void (__fastcall*)(void*, void*, int, int), drag_end)

// Window manager
#define add_window       CLIENT(void (__fastcall*)(void*, void*, void*), add_window)
#define remove_window    CLIENT(void (__fastcall*)(void*, void*, void*), remove_window)
#define focus            CLIENT(void (__fastcall*)(void*, void*, void*), focus)
#define make_window      CLIENT(void* (__fastcall*)(void*, void*, int), make_window)
#define capture_mouse    CLIENT(void (__fastcall*)(void*, void*, void*), capture_mouse)
#define captured_window  CLIENT(void* (__fastcall*)(void*, void*), captured_window)
#define release_mouse    CLIENT(void (__fastcall*)(void*, void*), release_mouse)

// Edit control
#define edit_construct   CLIENT(void* (__fastcall*)(void*, void*), edit_construct)

// Drawing and resources
#define fill             CLIENT(void (__fastcall*)(void*, void*, int, int, int, int, u32), fill)
#define resource_manager CLIENT(void* (__cdecl*)(), resource_manager)
#define resolve_path     CLIENT(const char* (__cdecl*)(const char*), resolve_path)
#define get_bitmap       CLIENT(void* (__fastcall*)(void*, void*, const char*), get_bitmap)
#define draw_bitmap      CLIENT(void (__fastcall*)(void*, void*, int, int, void*, int), draw_bitmap)
#define measure          CLIENT(int (__fastcall*)(void*, void*, const char*, int, int, int, int, int), measure)
#define print_text       CLIENT(void (__fastcall*)(void*, void*, int, int, const char*, int, int, int, u32, int, int), print_text)

// Items
#define item_descriptor  CLIENT(const u8* (__cdecl*)(u32), item_descriptor)
#define item_construct   CLIENT(void* (__fastcall*)(void*, void*), item_construct)
#define item_set_id      CLIENT(void (__fastcall*)(void*, void*, u32), item_set_id)

// Network
#define network          CLIENT(void* (__cdecl*)(), network)
#define send_packet      CLIENT(void (__fastcall*)(void*, void*, int, const void*), send_packet)

// timeGetTime through the import table.
u32 tick() { return (*reinterpret_cast<u32 (__stdcall**)()>(auction_imports.tick_import))(); }

void* manager() { return reinterpret_cast<void*>(auction_imports.manager); }

// Reads or writes a field of a native object by byte offset.
template<class T> T& at(void* base, u32 offset) {
    return *reinterpret_cast<T*>(static_cast<u8*>(base) + offset);
}
template<class T> const T& at(const void* base, u32 offset) {
    return *reinterpret_cast<const T*>(static_cast<const u8*>(base) + offset);
}

// UIEditCtrl field offsets.
namespace edit_offset {
constexpr u32 size       = 0x11c;
constexpr u32 visible    = 0x28;
constexpr u32 max_length = 0x88;
constexpr u32 text       = 0xd8;  // std::string, inline buffer while capacity < 16
constexpr u32 length     = 0xe8;
constexpr u32 capacity   = 0xec;
}

// "유저인터페이스\item\" in CP949.
constexpr char item_texture_prefix[] = "\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba\\item\\";

// ============================================================================
// Window state
// ============================================================================

// Registration inputs, in tab order.
constexpr int input_price = 0, input_buy = 1, input_hours = 2;

struct Window {
    void** table;
    u8 base[0xB4 - sizeof(void*)];  // native UIFrameWnd state

    Controller data;
    Layout layout;
    void* edit;       // search box
    void* inputs[3];  // starting bid, buy now, duration
    int screen_w, screen_h;
    u32 last_second;

    // Which screen is showing.
    bool closing, details, registration, item_info, rules, price_mode, confirming, picking;

    // Values awaiting confirmation.
    u32 form_price, form_buy, form_hours;
    u16 proposed_action;
    const char* notice;
    bool form_initialized;
    const char* field_errors[3];

    // Buttons drawn in the last frame, for hover and press hit-testing.
    struct Hit { Rect rect; bool enabled; } buttons[32];
    int button_count;

    int mouse_x, mouse_y;
    Rect pressed;
    bool mouse_down, dragging;
    u32 press_context;
};

Window* current;
void* window_vtable[vtable_slots];
u32 sequence;

// ============================================================================
// Small helpers
// ============================================================================

void clear_focus() { focus(manager(), nullptr, nullptr); }
void focus_on(void* control) { focus(manager(), nullptr, control); }

void send_query(const QueryPacket& p) {
    void* net = network();
    if (net) send_packet(net, nullptr, sizeof p.bytes, p.bytes);
}

// Physical pixels to logical skin coordinates; negative means outside.
int to_logical(int value) { return value < 0 ? -1 : value / skin_scale; }

void place(void* control, Rect r) {
    size_window(control, nullptr, r.w * skin_scale, r.h * skin_scale);
    move_child(control, nullptr, r.x * skin_scale, r.y * skin_scale);
}

u32 digit_count(u32 value) {
    u32 digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// ============================================================================
// Text formatting (every buffer here is text_capacity bytes)
// ============================================================================

constexpr int text_capacity = 180;

void number(char* out, u32 value, bool commas = false) {
    char reverse[16];
    int n = 0, digits = 0;
    do {
        if (commas && digits && digits % 3 == 0) reverse[n++] = ',';
        reverse[n++] = char('0' + value % 10);
        value /= 10;
        ++digits;
    } while (value);
    for (int i = 0; i < n; ++i) out[i] = reverse[n - i - 1];
    out[n] = 0;
}

void append(char* out, const char* value) {
    int n = 0;
    while (out[n]) ++n;
    copy_string(out + n, value, text_capacity - n);
}

void append_number(char* out, u32 value, bool commas = false) {
    char digits[16];
    number(digits, value, commas);
    append(out, digits);
}

// "1,234 zeny"
void zeny(char* out, u32 value, const char* suffix = " zeny") {
    number(out, value, true);
    append(out, suffix);
}

// "1 - 72"
void hours_range(char* out, const Rules& r) {
    number(out, r.minimum_hours);
    append(out, " - ");
    append_number(out, r.maximum_hours);
}

// "[A] +7 Name". The grade comes first so it survives column clipping;
// ungraded items get no marker.
void item_label(char* out, const Row& r, bool with_refine) {
    out[0] = 0;
    if (r.grade) {
        append(out, "[");
        append(out, grade_name(r.grade));
        append(out, "] ");
    }
    if (with_refine && r.refine) {
        append(out, "+");
        append_number(out, r.refine);
        append(out, " ");
    }
    append(out, r.name);
}

// "2h 5m", "4m 30s" or "Ended".
void remaining(char* out, u32 left) {
    if (left >= 3600) {
        number(out, left / 3600);
        append(out, "h ");
        append_number(out, (left % 3600) / 60);
        append(out, "m");
    } else if (left) {
        number(out, left / 60);
        append(out, "m ");
        append_number(out, left % 60);
        append(out, "s");
    } else {
        copy_string(out, "Ended", text_capacity);
    }
}

const char* status_label(const Controller& c) {
    if (c.action_pending) return "Processing";
    if (c.phase == Phase::Loading) return "Loading";
    if (c.refresh_failed) return "Reconnecting";
    if (c.background) return "Updating";
    return "Live";
}

const char* action_error(u16 result) {
    switch (result) {
    case InsufficientFunds: return "Not enough zeny. Your entered values have been kept.";
    case LimitReached:      return "You reached the server's auction limit.";
    case ItemChanged:       return "The item changed. Choose it again; your prices have been kept.";
    case Missing:           return "This auction has ended or was purchased.";
    case Changed:           return "The auction changed. Review the updated price.";
    default:                return "The server rejected this action. Review the item and values.";
    }
}

// ============================================================================
// Drawing primitives
// ============================================================================

constexpr u32 ink = 0x000000, muted = 0x808080, selected = 0x0000c0;

// Client font key: bold selects CreateFont weight 700 (400 otherwise); measured with the same weight.
void text(Window* w, int x, int y, int width, const char* value, u32 color = ink, int bold = 0) {
    const int font = w->layout.font;
    const auto text_width = [&](const char* s, int n) { return measure(w, nullptr, s, n, 0, font, bold, 0); };
    char fitted[text_capacity];
    const int count = auction::ui::fit_text(value, fitted, sizeof fitted, width * skin_scale, text_width);
    if (count) print_text(w, nullptr, x * skin_scale, y * skin_scale, fitted, count, 0, font, color, bold, 0);
}

void heading(Window* w, int x, int y, int width, const char* value, u32 color = ink) {
    text(w, x, y, width, value, color, 1);
}

// Label in the left column, value in the right one.
void field(Window* w, int y, const char* label, const char* value) {
    text(w, 69, y, 102, label, muted);
    text(w, 174, y, 365, value);
}

void draw_image(Window* w, int x, int y, const char* path) {
    const char* resolved = resolve_path(path);
    if (!resolved) return;
    void* image = get_bitmap(resource_manager(), nullptr, resolved);
    if (image) draw_bitmap(w, nullptr, x * skin_scale, y * skin_scale, image, 1);
}

void bitmap(Window* w, int x, int y, const char* filename) {
    char path[text_capacity];
    copy_string(path, auction::ui::texture_prefix, sizeof path);
    append(path, "auction_x2\\");
    append(path, filename);
    draw_image(w, x, y, path);
}

void item_icon(Window* w, int x, int y, const Row& row) {
    const u8* descriptor = item_descriptor(row.item_id);
    const char* name = descriptor ? at<const char*>(descriptor, 8) : nullptr;
    if (!name || !name[0]) {
        text(w, x, y, 20, "?", muted);
        return;
    }
    char path[text_capacity];
    copy_string(path, item_texture_prefix, sizeof path);
    append(path, name);
    append(path, ".bmp");
    draw_image(w, x, y, path);
}

// Draws a skinned button and records it for hit-testing.
// Bitmaps are named btn_<w>x<h>_<label>_<state>.bmp.
void button(Window* w, Rect r, const char* label, bool enabled = true, bool active = false) {
    // While an action is pending only Close stays usable.
    const bool is_close = r.x == w->layout.close.x && r.y == w->layout.close.y;
    enabled = enabled && (!w->data.action_pending || is_close);
    if (w->button_count < 32) w->buttons[w->button_count++] = {r, enabled};

    const bool over = r.contains(w->mouse_x, w->mouse_y);
    const bool pressed = w->mouse_down && r.x == w->pressed.x && r.y == w->pressed.y && over;
    const char* state = !enabled ? "disabled" : pressed || active ? "pressed" : over ? "hover" : "normal";

    char key[80];
    if (label[0] == '<' || label[0] == '>') {
        copy_string(key, label[0] == '<' ? "previous" : "next", sizeof key);
    } else {
        // "Place bid" -> "place_bid"
        int k = 0;
        for (; label[k] && k < 79; ++k) {
            const char ch = label[k];
            key[k] = ch == ' ' ? '_' : ch >= 'A' && ch <= 'Z' ? char(ch + 32) : ch;
        }
        key[k] = 0;
    }

    char file[text_capacity];
    copy_string(file, "btn_", sizeof file);
    append_number(file, r.w);
    append(file, "x");
    append_number(file, r.h);
    append(file, "_");
    append(file, key);
    append(file, "_");
    append(file, state);
    append(file, ".bmp");
    bitmap(w, r.x, r.y, file);
}

void confirm_buttons(Window* w) {
    button(w, w->layout.confirm_action, "Confirm");
    button(w, w->layout.abort_action, "Back");
}

void filters(Window* w) {
    const auto& c = w->data;
    const bool enabled = c.phase != Phase::Loading && (c.view == View::List || c.view == View::Inventory);
    for (int i = 0; i < 6; ++i)
        button(w, w->layout.filters[i], filter_names[i], enabled, static_cast<int>(c.filter) == i);
}

// Previous / next buttons and the "Page N / M" label.
void pager(Window* w) {
    const auto& c = w->data;
    button(w, w->layout.previous, "<", c.previous_enabled());
    button(w, w->layout.next, ">", c.next_enabled());

    const bool ready = c.phase == Phase::Ready;
    char page[text_capacity];
    copy_string(page, "Page ", sizeof page);
    append_number(page, ready ? c.page.number : 0);
    append(page, " / ");
    append_number(page, ready ? c.page.pages : 0);
    text(w, 327, 241, 194, page);
}

// Opens the native item tooltip window for a listing row.
void item_popup(const Row& row) {
    // Same CItem layout and synchronous copy used by the native inventory popup.
    alignas(4) u8 item[0xf8]{};
    item_construct(item, nullptr);
    put32(item, row.type);
    put32(item + 0x10, row.amount);
    for (int i = 0; i < 4; ++i) put32(item + 0x1c + i * 4, row.cards[i]);
    item_set_id(item, nullptr, row.item_id);
    item[0x5c] = row.identified;
    item[0x5d] = row.attribute;
    put32(item + 0x60, row.refine);
    put16(item + 0x88, row.grade);

    // Random options are packed: only non-empty slots, 5 bytes each.
    int count = 0;
    for (int i = 0; i < 5; ++i) {
        const auto& option = row.options[i];
        if (!option.id) continue;
        u8* slot = item + 0x9c + count++ * 5;
        put16(slot, option.id);
        put16(slot + 2, static_cast<u16>(option.value));
        slot[4] = option.parameter;
    }
    put32(item + 0x98, count);

    void* popup = make_window(manager(), nullptr, 12);
    if (popup) {
        using Handler = int (__fastcall*)(void*, void*, void*, int, void*, int, int, int);
        auto** vtable = *reinterpret_cast<void***>(popup);
        reinterpret_cast<Handler>(vtable[0x94 / 4])(popup, nullptr, nullptr, 24, item, 0, 0, 0);
    }
    destroy_string(item + 0x44, nullptr);
    destroy_string(item + 0x2c, nullptr);
}

// ============================================================================
// Edit controls
// ============================================================================

void set_visible(void* control, bool visible) { at<int>(control, edit_offset::visible) = visible ? 1 : 0; }

// Returns the edit's text (not NUL-terminated) and stores its length.
const char* edit_text(const void* edit, u32& length) {
    length = at<u32>(edit, edit_offset::length);
    const u32 capacity = at<u32>(edit, edit_offset::capacity);
    return capacity < 16 ? &at<char>(edit, edit_offset::text) : at<const char*>(edit, edit_offset::text);
}

void set_edit_text(void* edit, const char* value, u32 length) {
    assign_string(static_cast<u8*>(edit) + edit_offset::text, nullptr, value, length);
}

void edit_visible(Window* w, bool visible) {
    if (!w->edit) return;
    if (!visible) clear_focus();
    set_visible(w->edit, visible);
}

void clear_search(Window* w) {
    if (w->edit) set_edit_text(w->edit, "", 0);
}

bool edit_number(void* edit, u32& value) {
    if (!edit) return false;
    u32 length = 0;
    const char* s = edit_text(edit, length);
    if (!length || length > 10) return false;
    char digits[12];
    memcpy(digits, s, length);
    digits[length] = 0;
    return parse_price(digits, value);
}

void set_number(void* edit, u32 value) {
    if (!edit) return;
    char digits[16];
    number(digits, value);
    u32 length = 0;
    while (digits[length]) ++length;
    set_edit_text(edit, digits, length);
}

void clear_edit(void* edit) {
    set_edit_text(edit, "", 0);
    // Native cursor state, reset so the caret returns to the start.
    at<u32>(edit, 0x7c) = at<u32>(edit, 0x80) = at<u32>(edit, 0xb0) = 0;
    dirty(edit, nullptr);
}

void* create_edit(u32 max_length) {
    void* edit = allocate(edit_offset::size);
    if (!edit) return nullptr;
    edit_construct(edit, nullptr);
    at<u32>(edit, edit_offset::max_length) = max_length;
    return edit;
}

void create_edits(Window* w) {
    w->edit = create_edit(23);
    if (w->edit) add_child(w, nullptr, w->edit);

    for (int i = 0; i < 3; ++i) {
        void* input = create_edit(10);
        w->inputs[i] = input;
        if (!input) continue;
        // Native style fields, same values as the original setup.
        at<u32>(input, 0x8c) = 0;
        at<u32>(input, 0x9c) = 255;
        at<u32>(input, 0xa0) = 184;
        at<u32>(input, 0xa4) = 194;
        add_child(w, nullptr, input);
    }
}

// Validates the registration inputs. Invalid fields are cleared and the first
// one gets focus; valid values are stored as the form to confirm.
bool validate_form(Window* w) {
    if (!w->inputs[0] || !w->inputs[1] || !w->inputs[2]) {
        w->notice = "Input controls unavailable. Reopen Auction.";
        return false;
    }

    const auto& r = w->data.rules;
    u32 values[3]{};
    bool good[3]{};
    for (int i = 0; i < 3; ++i) {
        good[i] = edit_number(w->inputs[i], values[i]);
        w->field_errors[i] = nullptr;
    }
    const u32 price = values[input_price], buy = values[input_buy], hours = values[input_hours];

    if (!good[input_price] || !price || price >= r.maximum_price)
        w->field_errors[input_price] = "Starting bid is outside the allowed range.";
    if (!good[input_buy] || buy < 2 || buy > r.maximum_price)
        w->field_errors[input_buy] = "Buy now is outside the allowed range.";
    if (!good[input_hours] || !valid_hours(r, hours))
        w->field_errors[input_hours] = "Duration is outside the allowed range.";
    if (!w->field_errors[input_price] && !w->field_errors[input_buy] && price >= buy)
        w->field_errors[input_buy] = "Buy now must exceed the starting bid.";

    int first = -1;
    for (int i = 0; i < 3; ++i) {
        if (!w->field_errors[i]) continue;
        if (first < 0) first = i;
        clear_edit(w->inputs[i]);
    }
    if (first >= 0) {
        w->notice = w->field_errors[first];
        focus_on(w->inputs[first]);
        return false;
    }

    w->form_price = price;
    w->form_buy = buy;
    w->form_hours = hours;
    return true;
}

// Shows and positions the edit boxes for the current screen: all three on the
// registration form, only the bid box on item details.
void sync_inputs(Window* w) {
    const auto& c = w->data;
    const bool ready = c.phase == Phase::Ready && c.selected >= 0 && !c.action_pending && !c.action_finished &&
                       !w->confirming;
    const bool form = w->registration && !w->picking && ready && (c.rules.capabilities & Register);
    const bool bid = w->details && !w->item_info && ready && c.view != View::Sell &&
                     c.page.rows[c.selected].seller_id != c.rules.character_id &&
                     (c.rules.capabilities & Bid) && c.time_left(c.selected, tick());

    for (int i = 0; i < 3; ++i) {
        void* input = w->inputs[i];
        if (!input) continue;

        const bool visible = form || (i == input_price && bid);
        set_visible(input, visible);
        if (!visible) continue;

        const u32 limit = i == input_hours ? c.rules.maximum_hours : c.rules.maximum_price;
        at<u32>(input, edit_offset::max_length) = digit_count(limit);
        place(input, bid ? Rect{174, 234, 105, 20} : Rect{174, 108 + 27 * i, i == input_hours ? 60 : 151, 16});
    }
}

// ============================================================================
// Window lifecycle and requests
// ============================================================================

void send_action(Window* w, const QueryPacket& p) {
    send_query(p);
    clear_focus();
    sync_inputs(w);
    dirty(w, nullptr);
}

// Sends a catalog request (when one was produced) and shows that catalog.
// Without a network connection the request fails immediately.
void transmit(Window* w, const QueryPacket& p, bool ready) {
    if (!ready) return;
    void* net = network();
    if (net) {
        send_packet(net, nullptr, sizeof p.bytes, p.bytes);
    } else {
        w->data.phase = Phase::Error;
        w->data.pending = 0;
        w->data.page.status = Unavailable;
    }

    w->details = false;
    w->registration = w->data.view == View::Inventory;
    w->picking = w->registration;
    w->item_info = false;
    w->rules = false;
    w->confirming = false;
    w->notice = nullptr;
    edit_visible(w, !w->registration);
    sync_inputs(w);
    dirty(w, nullptr);
}

void return_to_list(Window* w) {
    QueryPacket p{};
    const bool ready = w->data.change_view(View::List, sequence, tick(), p);
    transmit(w, p, ready);
}

void request_rules(Window* w) {
    QueryPacket p{};
    if (w->data.request_rules(sequence, tick(), p)) send_query(p);
}

// Keeps the window centered on the current screen resolution.
void resize(Window* w) {
    const auto* display = *reinterpret_cast<const u8* const*>(auction_imports.screen);
    const int width = display ? at<int>(display, 0x28) : 800;
    const int height = display ? at<int>(display, 0x2c) : 600;
    if (w->screen_w == width && w->screen_h == height) return;

    w->screen_w = width;
    w->screen_h = height;
    w->layout = layout_for(width, height);
    size_window(w, nullptr, w->layout.width, w->layout.height);
    move_window(w, nullptr, maximum(0, (width - w->layout.width) / 2), maximum(0, (height - w->layout.height) / 2));
    if (w->edit) place(w->edit, w->layout.search_edit);
    dirty(w, nullptr);
}

void close(Window* w) {
    if (w->closing) return;
    w->closing = true;
    w->data.close();
    clear_focus();
    remove_window(manager(), nullptr, w);
}

void* __fastcall on_delete(Window* w, void*, u32 flags) {
    if (current == w) current = nullptr;
    destruct(w, nullptr);
    if (flags & 1) release(w, sizeof(Window));
    return w;
}

void __fastcall on_pre_delete(Window* w, void*) {
    w->data.close();
    if (current == w) current = nullptr;
}

int __fastcall on_refresh(Window* w, void*, int force) {
    if (!w->closing) {
        resize(w);
        const u32 now = tick();
        QueryPacket p{};
        if (w->data.retry_action(now, p)) send_query(p);
        if (w->data.refresh(sequence, now, p)) send_query(p);
        // Repaint at least once a second so countdowns stay current.
        if (w->data.timeout(now) || now / 1000 != w->last_second) {
            w->last_second = now / 1000;
            dirty(w, nullptr);
        }
    }
    return base_refresh(w, nullptr, force);
}

// ============================================================================
// Screens
// ============================================================================

void detail_auction_fields(Window* w, const Row& r, u32 now) {
    char value[text_capacity];
    field(w, 98, "Seller", r.seller);
    field(w, 124, "Buyer", r.buyer[0] ? r.buyer : "No bids");
    zeny(value, r.bid);
    field(w, 150, "Current bid", value);
    zeny(value, r.buy);
    field(w, 176, "Buy now", value);
    remaining(value, w->data.time_left(w->data.selected, now));
    field(w, 202, "Time left", value);
}

void detail_item_fields(Window* w, const Row& r) {
    char value[text_capacity];
    number(value, r.item_id);
    append(value, " / Amount ");
    append_number(value, r.amount);
    field(w, 98, "Item ID", value);

    number(value, r.refine);
    if (r.grade) {
        append(value, " / ");
        append(value, grade_name(r.grade));
    }
    field(w, 120, r.grade ? "Refine / Grade" : "Refine", value);

    value[0] = 0;
    for (int i = 0; i < 4; ++i) {
        if (i) append(value, ", ");
        append_number(value, r.cards[i]);
    }
    field(w, 142, "Cards (IDs)", value);

    for (int i = 0; i < 5; ++i) {
        const auto& option = r.options[i];
        number(value, option.id);
        append(value, ": ");
        int amount = option.value;
        if (amount < 0) {
            append(value, "-");
            amount = -amount;
        }
        append_number(value, static_cast<u32>(amount));
        field(w, 164 + i * 14, i == 0 ? "Options" : "", value);
    }
}

// Status line and trade buttons at the bottom of the details screen.
void detail_actions(Window* w, const Row& r, u32 now) {
    const auto& c = w->data;
    const auto& rules = c.rules;
    if (w->notice) text(w, 69, 221, 465, w->notice, selected);

    if (c.action_pending) {
        text(w, 69, 238, 450, "Waiting for the server to confirm...", muted);
        return;
    }
    if (c.action_finished && c.action_result != Done) {
        text(w, 69, 238, 450, "Action rejected. Refresh and review the auction again.", muted);
        return;
    }
    if (w->confirming) {
        const u16 action = w->proposed_action;
        char prompt[text_capacity];
        copy_string(prompt,
                    action == CancelSale  ? "Return item to your mailbox?"
                    : action == CloseSale ? "Deliver item and collect payment?"
                                          : "Confirm payment: ",
                    sizeof prompt);
        if (action == BidSale || action == BuySale) {
            append_number(prompt, w->form_price, true);
            append(prompt, " zeny?");
        }
        text(w, 69, 239, 294, prompt);
        confirm_buttons(w);
        return;
    }
    if (w->item_info || !c.time_left(c.selected, now)) {
        text(w, 69, 238, 450, w->item_info ? "Review the auction to trade." : "This auction has ended.", muted);
        return;
    }

    const bool own = r.seller_id == rules.character_id;
    if (c.view == View::Sell && own && (rules.capabilities & (r.buyer_id ? Close : Cancel))) {
        button(w, w->layout.sale_action, r.buyer_id ? "Close sale" : "Cancel sale");
    } else if (!own) {
        if (rules.capabilities & Bid) button(w, w->layout.sale_action, "Place bid");
        if (rules.capabilities & BuyNow) button(w, w->layout.buy_action, "Buy now");
        if (!(rules.capabilities & (Bid | BuyNow)))
            text(w, 69, 238, 450, "Bidding and buying are not available yet.", muted);
    }
}

void detail_view(Window* w, const Row& r, u32 now) {
    char value[text_capacity];
    heading(w, 18, 33, 500, w->item_info ? "Item information" : "Selected item");
    item_label(value, r, true);
    text(w, 69, 71, 470, value);
    item_icon(w, 26, 78, r);
    text(w, 69, 84, 400, category_name(r.type), muted);

    if (w->item_info)
        detail_item_fields(w, r);
    else
        detail_auction_fields(w, r, now);
    detail_actions(w, r, now);

    button(w, w->layout.more, w->item_info ? "Auction" : "Info");
    button(w, w->layout.back, "Back");
}

void registration_form(Window* w, const Row& item) {
    const auto& c = w->data;
    const auto& r = c.rules;
    char value[text_capacity];

    item_icon(w, 26, 78, item);
    field(w, 93, "Category", category_name(item.type));
    text(w, 69, 110, 102, "Starting bid", muted);
    text(w, 69, 137, 102, "Buy now", muted);
    text(w, 69, 164, 102, "Duration (hours)", muted);

    // Hints beside the inputs.
    text(w, 335, 110, 205, "Starting bid < Buy now", muted);
    number(value, r.maximum_price, true);
    append(value, " max");
    text(w, 335, 137, 205, value, muted);
    hours_range(value, r);
    append(value, " hours");
    text(w, 244, 164, 290, value, muted);

    u32 hours = 0;
    bool valid;
    if (w->confirming) {
        hours = w->form_hours;
        valid = true;
    } else {
        valid = edit_number(w->inputs[input_hours], hours);
    }
    if (valid && valid_hours(r, hours)) {
        zeny(value, r.fee_per_hour * hours);
        field(w, 191, "Listing fee", value);
    } else {
        field(w, 191, "Listing fee", "Enter a valid duration");
    }

    // The inputs are hidden now; show the submitted values instead.
    if (w->confirming || c.action_pending) {
        number(value, w->form_price, true);
        field(w, 110, "Starting bid", value);
        number(value, w->form_buy, true);
        field(w, 137, "Buy now", value);
        number(value, w->form_hours);
        field(w, 164, "Duration (hours)", value);
    }

    if (c.action_pending) {
        text(w, 69, 238, 450, "Registering item. Waiting for the server...", muted);
    } else if (w->confirming) {
        text(w, 69, 239, 294, "Confirm listing and pay the fee?");
        confirm_buttons(w);
    } else if (w->notice) {
        text(w, 69, 226, 450, w->notice, selected);
    } else if (c.action_finished) {
        text(w, 69, 226, 450, "Registration rejected. Choose the item again.", muted);
    } else {
        hours_range(value, r);
        append(value, " hours. One item per listing.");
        text(w, 69, 226, 450, value, muted);
    }
}

void registration_unavailable(Window* w) {
    const auto& r = w->data.rules;
    char value[text_capacity];
    field(w, 110, "Starting bid", "-- zeny");
    field(w, 137, "Buy now", "-- zeny");
    if (r.status == Ok) {
        hours_range(value, r);
        append(value, " hours");
        field(w, 164, "Duration", value);
        zeny(value, r.fee_per_hour, " zeny / hour");
        field(w, 191, "Listing fee", value);
    }
    text(w, 69, 226, 450, "Registration is not available yet.", muted);
}

void registration_view(Window* w) {
    const auto& c = w->data;
    const bool available = (c.rules.capabilities & Register) && c.phase == Phase::Ready && c.selected >= 0;
    heading(w, 18, 33, 500, "Register one item for auction");

    if (available) {
        const Row& item = c.page.rows[c.selected];
        char value[text_capacity];
        item_label(value, item, false);
        field(w, 77, "Item", value);
        registration_form(w, item);
    } else {
        field(w, 77, "Item", "No item selected");
        registration_unavailable(w);
    }

    button(w, w->layout.more, "Register", available && !w->confirming && !c.action_pending && !c.action_finished);
    button(w, w->layout.back, "Cancel");
}

void inventory_view(Window* w) {
    const auto& c = w->data;
    const auto& l = w->layout;
    const auto* columns = auction::ui::columns;

    heading(w, 19, 33, 43, "Item");
    heading(w, 69, 33, 84, "Item name");
    heading(w, 158, 33, 148, "Available amount");
    heading(w, 311, 33, 69, "Grade");
    heading(w, 385, 33, 68, "Refine");
    heading(w, 458, 33, 89, "Item ID");

    if (c.phase == Phase::Ready) {
        if (!c.page.count) text(w, 76, 133, 450, "No eligible items. Unequip the item before selling.", muted);
        for (int i = 0; i < c.page.count; ++i) {
            const auto& r = c.page.rows[i];
            const int y = l.rows_y + i * l.row_pitch + 7;
            const u32 color = i == c.selected ? selected : ink;
            char n[text_capacity];

            item_icon(w, 26, y, r);
            text(w, columns[0].x, y - 4, columns[0].width, r.name, color);
            text(w, columns[0].x, y + 7, columns[0].width, category_name(r.type), muted);
            number(n, r.amount);
            text(w, 158, y, 148, n, color);
            if (r.grade) text(w, 311, y, 69, grade_name(r.grade), color);
            number(n, r.refine);
            text(w, 385, y, 68, n, color);
            number(n, r.item_id);
            text(w, 458, y, 89, n, color);
        }
    } else {
        text(w, 76, 133, 450, c.phase == Phase::Loading ? "Loading inventory..." : "Inventory unavailable. Press Refresh.", muted);
    }

    filters(w);
    pager(w);
    button(w, l.inventory_refresh, "Refresh", c.phase != Phase::Loading);
    button(w, l.inventory_cancel, "Cancel");
    button(w, l.inventory_choose, "Choose", c.selected >= 0);
}

void rules_view(Window* w) {
    const auto& r = w->data.rules;
    char value[text_capacity];
    heading(w, 18, 33, 500, "Auction House - Server rules");

    if (r.status == Ok) {
        zeny(value, r.maximum_price);
        field(w, 77, "Maximum price", value);
        zeny(value, r.fee_per_hour, " zeny / hour");
        field(w, 110, "Listing fee", value);
        hours_range(value, r);
        append(value, " hours");
        field(w, 137, "Duration", value);
        number(value, r.maximum_sales);
        field(w, 164, "Active sales", value);
        number(value, r.maximum_bids);
        field(w, 191, "Active bids", value);
        text(w, 69, 226, 450,
             (r.capabilities & (Cancel | Close)) ? "Own sales: cancellation and delivery enabled."
                                                 : "Trading is not available yet.",
             muted);
    } else {
        const char* message = w->data.rules_pending ? "Loading server rules..."
                              : r.status == Disabled  ? "Auction House is disabled."
                                                      : "Server rules unavailable. Press Refresh to retry.";
        text(w, 69, 110, 450, message, muted);
    }

    button(w, w->layout.rules_refresh, "Refresh", !w->data.rules_pending);
    button(w, w->layout.rules_back, "Back");
}

void catalog_row(Window* w, int i, u32 now) {
    const auto& c = w->data;
    const auto& r = c.page.rows[i];
    const auto* columns = auction::ui::columns;
    const int top = w->layout.rows_y + i * w->layout.row_pitch;
    const int y = top + 7;
    const bool is_selected = i == c.selected;
    const u32 color = is_selected ? selected : ink;

    char name[text_capacity], bid[text_capacity], buy[text_capacity], time[text_capacity];
    item_label(name, r, true);
    number(bid, r.bid, true);
    number(buy, r.buy, true);
    remaining(time, c.time_left(i, now));

    item_icon(w, 26, y, r);
    text(w, columns[0].x, y - 4, columns[0].width, name, color);
    text(w, columns[0].x, y + 7, columns[0].width, category_name(r.type), muted);
    text(w, columns[1].x, top, columns[1].width, bid, color);
    text(w, columns[1].x, top + 14, columns[1].width, buy, is_selected ? selected : muted);
    text(w, columns[2].x, y, columns[2].width, r.seller, color);
    text(w, columns[3].x, y, columns[3].width, r.buyer[0] ? r.buyer : "-", color);
    text(w, columns[4].x, y, columns[4].width, time, color);
}

// List / My sales / My bids.
void catalog_view(Window* w, u32 now) {
    const auto& c = w->data;
    const auto& l = w->layout;
    const auto* columns = auction::ui::columns;

    heading(w, 19, 33, 43, "Item");
    heading(w, columns[0].x, 33, columns[0].width, "Item name");
    heading(w, columns[1].x, 25, columns[1].width, "Current bid");
    heading(w, columns[1].x, 40, columns[1].width, "Buy now (zeny)", muted);
    heading(w, columns[2].x, 33, columns[2].width, "Seller");
    heading(w, columns[3].x, 33, columns[3].width, "Buyer");
    heading(w, columns[4].x, 33, columns[4].width, "Time left");

    if (c.phase == Phase::Ready) {
        if (!c.page.count) {
            const char* empty = c.view == View::List   ? "No auctions found."
                                : c.view == View::Sell ? "No items listed for sale."
                                                       : "No leading bids.";
            text(w, 76, 133, 450, empty, muted);
        }
        for (int i = 0; i < c.page.count; ++i) catalog_row(w, i, now);
    } else {
        const char* message = c.phase == Phase::Loading        ? "Loading auctions..."
                              : c.page.status == Disabled ? "Auction House is disabled on this server."
                                                          : "Unable to load auctions. Press Refresh to retry.";
        text(w, 76, 133, 450, message, muted);
    }

    filters(w);
    pager(w);
    button(w, l.search_mode, w->price_mode ? "Max bid" : "Name", c.phase != Phase::Loading && c.view == View::List);
    button(w, l.search, "Search", w->edit != nullptr && c.phase != Phase::Loading);
    button(w, l.refresh, "Refresh", c.phase != Phase::Loading);
    button(w, l.rules, "Rules");
    button(w, l.details, "Details", c.selected >= 0);
    if (w->notice) text(w, 10, 226, 535, w->notice, selected);
}

void __fastcall on_draw(Window* w, void*) {
    w->button_count = 0;
    const auto& l = w->layout;
    const auto& c = w->data;
    const u32 now = tick();
    const int active_tab = w->registration ? 1 : c.view == View::List ? 0 : c.view == View::Sell ? 2 : 3;

    const bool form_background = (w->registration && !w->picking) || w->details || w->rules;
    bitmap(w, 0, 0, form_background ? "auction_bg_1.bmp" : "auction_bg_0.bmp");
    button(w, l.close, "Close");
    if (!w->rules) {
        const char* labels[] = {"List", "Register", "Sell", "Buy"};
        for (int i = 0; i < 4; ++i)
            button(w, l.tabs[i], labels[i], !w->confirming && c.phase != Phase::Loading, i == active_tab);
    }
    heading(w, 8, 3, 430, w->details ? "Auction - Item Details" : auction::ui::titles[active_tab]);
    text(w, 456, 3, 69, status_label(c), muted);

    if (w->rules) {
        rules_view(w);
        return;
    }
    sync_inputs(w);
    if (w->registration) {
        if (w->picking)
            inventory_view(w);
        else
            registration_view(w);
        return;
    }
    if (w->details && c.selected >= 0 && c.selected < c.page.count) {
        detail_view(w, c.page.rows[c.selected], now);
        return;
    }
    catalog_view(w, now);
}

// ============================================================================
// Input
// ============================================================================

void start_drag(Window* w, int x, int y) { drag(w, nullptr, x * skin_scale, y * skin_scale); }

void click_rules(Window* w, int x, int y) {
    const auto& l = w->layout;
    if (l.rules_refresh.contains(x, y)) {
        if (!w->data.rules_pending) request_rules(w);
        dirty(w, nullptr);
    } else if (l.rules_back.contains(x, y)) {
        w->rules = false;
        edit_visible(w, true);
        dirty(w, nullptr);
    } else if (auction::ui::hit_title(x, y)) {
        start_drag(w, x, y);
    }
}

void click_confirmation(Window* w, int x, int y) {
    const auto& l = w->layout;
    auto& c = w->data;
    if (l.abort_action.contains(x, y)) {
        w->confirming = false;
        sync_inputs(w);
        dirty(w, nullptr);
        return;
    }
    if (!l.confirm_action.contains(x, y)) return;

    QueryPacket p{};
    const bool ready = w->proposed_action == RegisterSale
                           ? c.register_sale(w->form_price, w->form_buy, w->form_hours, sequence, tick(), p)
                           : c.act(w->proposed_action, sequence, tick(), p, w->form_price);
    w->confirming = false;
    if (ready)
        send_action(w, p);
    else
        w->notice = "Action unavailable. Refresh and review it again.";
    sync_inputs(w);
    dirty(w, nullptr);
}

// The functions below return true when the click was consumed.

bool click_registration(Window* w, int x, int y) {
    const auto& l = w->layout;
    auto& c = w->data;
    if (l.back.contains(x, y)) {
        return_to_list(w);
        return true;
    }
    if (!l.more.contains(x, y) || !(c.rules.capabilities & Register) || c.selected < 0 || c.action_finished)
        return false;

    w->notice = nullptr;
    if (!auction_category(c.page.rows[c.selected].type)) {
        w->notice = "This item category cannot be registered.";
    } else if (validate_form(w)) {
        w->proposed_action = RegisterSale;
        w->confirming = true;
        clear_focus();
    }
    sync_inputs(w);
    dirty(w, nullptr);
    return true;
}

// Returns the trade action under (x, y), or 0. A rejected bid sets a notice.
u16 trade_action_at(Window* w, int x, int y) {
    const auto& l = w->layout;
    const auto& c = w->data;
    const auto& row = c.page.rows[c.selected];
    const auto capabilities = c.rules.capabilities;
    const bool own = row.seller_id == c.rules.character_id;

    if (c.view == View::Sell && own && (capabilities & (row.buyer_id ? Close : Cancel)) && l.sale_action.contains(x, y))
        return row.buyer_id ? CloseSale : CancelSale;
    if (own) return 0;

    u16 action = 0;
    if ((capabilities & BuyNow) && l.buy_action.contains(x, y)) {
        action = BuySale;
        w->form_price = row.buy;
    }
    if ((capabilities & Bid) && l.sale_action.contains(x, y)) {
        if (edit_number(w->inputs[input_price], w->form_price) && w->form_price > row.bid && w->form_price < row.buy)
            action = BidSale;
        else
            w->notice = "Bid must exceed the current bid and be below Buy now.";
    }
    return action;
}

bool click_details(Window* w, int x, int y) {
    const auto& l = w->layout;
    const auto& c = w->data;
    if (l.more.contains(x, y)) {
        w->item_info = !w->item_info;
        w->notice = nullptr;
        sync_inputs(w);
        dirty(w, nullptr);
        return true;
    }
    if (l.back.contains(x, y)) {
        w->details = false;
        w->notice = nullptr;
        edit_visible(w, true);
        sync_inputs(w);
        dirty(w, nullptr);
        return true;
    }
    if (w->item_info || c.selected < 0 || c.action_finished || !c.time_left(c.selected, tick())) return false;

    const u16 action = trade_action_at(w, x, y);
    if (action) {
        w->notice = nullptr;
        w->proposed_action = action;
        w->confirming = true;
        clear_focus();
    }
    sync_inputs(w);
    dirty(w, nullptr);
    return false;  // tabs and title dragging still apply
}

bool click_inventory_buttons(Window* w, int x, int y) {
    const auto& l = w->layout;
    const auto& c = w->data;
    if (l.inventory_cancel.contains(x, y)) {
        return_to_list(w);
        return true;
    }
    if (l.inventory_choose.contains(x, y) && c.selected >= 0) {
        w->picking = false;
        if (!w->form_initialized) {
            set_number(w->inputs[input_price], 1);
            set_number(w->inputs[input_buy], 2);
            set_number(w->inputs[input_hours], c.rules.minimum_hours);
            w->form_initialized = true;
        }
        sync_inputs(w);
        dirty(w, nullptr);
        return true;
    }
    return false;
}

bool click_list_buttons(Window* w, int x, int y) {
    const auto& l = w->layout;
    const auto& c = w->data;
    if (l.rules.contains(x, y)) {
        w->rules = true;
        edit_visible(w, false);
        dirty(w, nullptr);
        return true;
    }
    if (l.details.contains(x, y) && c.selected >= 0) {
        w->details = true;
        w->item_info = false;
        w->notice = nullptr;
        set_number(w->inputs[input_price], c.page.rows[c.selected].bid + 1);
        edit_visible(w, false);
        sync_inputs(w);
        dirty(w, nullptr);
        return true;
    }
    if (c.phase != Phase::Loading && c.view == View::List && l.search_mode.contains(x, y)) {
        w->price_mode = !w->price_mode;
        clear_search(w);
        dirty(w, nullptr);
        return true;
    }
    return false;
}

void submit_search(Window* w) {
    auto& c = w->data;
    u32 length = 0;
    const char* value = edit_text(w->edit, length);
    if (length > 23) return;

    char query[24];
    memcpy(query, value, length);
    query[length] = 0;

    const bool old_price_search = c.price_search;
    c.price_search = w->price_mode;
    QueryPacket p{};
    const bool ready = c.search(query, sequence, tick(), p);
    if (!ready) {
        c.price_search = old_price_search;
        w->notice = "Enter a valid price.";
    }
    transmit(w, p, ready);
}

// Filters, paging, refresh and search. Only called while not loading.
bool click_query_buttons(Window* w, int x, int y) {
    const auto& l = w->layout;
    auto& c = w->data;
    QueryPacket p{};

    for (int i = 0; i < 6; ++i) {
        if (!l.filters[i].contains(x, y)) continue;
        w->price_mode = false;
        const bool ready = c.change_filter(static_cast<Filter>(i), sequence, tick(), p);
        transmit(w, p, ready);
        return true;
    }
    if (l.previous.contains(x, y) || l.next.contains(x, y)) {
        const bool ready = c.navigate(l.previous.contains(x, y) ? -1 : 1, sequence, tick(), p);
        transmit(w, p, ready);
        return true;
    }
    if ((w->picking ? l.inventory_refresh : l.refresh).contains(x, y)) {
        const bool ready = c.request(sequence, tick(), c.page.number ? c.page.number : 1, p);
        transmit(w, p, ready);
        return true;
    }
    if (!w->picking && w->edit && l.search.contains(x, y)) {
        submit_search(w);
        return true;
    }
    return false;
}

// Catalog and inventory picker.
bool click_catalog(Window* w, int x, int y) {
    auto& c = w->data;
    if (w->picking ? click_inventory_buttons(w, x, y) : click_list_buttons(w, x, y)) return true;
    if (c.phase != Phase::Loading && click_query_buttons(w, x, y)) return true;
    if (c.select(w->layout.row_at(x, y, c.page.count))) {
        dirty(w, nullptr);
        return true;
    }
    return false;
}

bool click_tabs(Window* w, int x, int y) {
    auto& c = w->data;
    if (c.phase == Phase::Loading) return false;

    for (int i = 0; i < 4; ++i) {
        if (!w->layout.tabs[i].contains(x, y)) continue;

        // Without the Register capability, show the form as unavailable
        // instead of loading the inventory.
        if (i == 1 && !(c.rules.capabilities & Register)) {
            w->registration = true;
            w->picking = false;
            w->details = false;
            edit_visible(w, false);
            sync_inputs(w);
            dirty(w, nullptr);
            return true;
        }

        constexpr View views[] = {View::List, View::Inventory, View::Sell, View::Buy};
        QueryPacket p{};
        const bool ready = c.change_view(views[i], sequence, tick(), p);
        w->price_mode = false;
        clear_search(w);
        transmit(w, p, ready);
        return true;
    }
    return false;
}

// Receives physical pixel coordinates.
void __fastcall on_click(Window* w, void*, int x, int y) {
    if (x < 0 || y < 0) return;
    x /= skin_scale;
    y /= skin_scale;
    if (w->closing) return;

    if (w->layout.close.contains(x, y)) {
        close(w);
        return;
    }
    if (w->data.action_pending) return;

    if (w->rules) {
        click_rules(w, x, y);
        return;
    }
    if (w->confirming) {
        click_confirmation(w, x, y);
        return;
    }

    bool handled;
    if (w->registration && !w->picking)
        handled = click_registration(w, x, y);
    else if (w->details)
        handled = click_details(w, x, y);
    else
        handled = click_catalog(w, x, y);
    if (handled) return;

    if (click_tabs(w, x, y)) return;
    if (auction::ui::hit_title(x, y)) start_drag(w, x, y);
}

// Simulates a click in the corner of a logical rectangle.
void click_at(Window* w, Rect r) { on_click(w, nullptr, (r.x + 2) * skin_scale, (r.y + 2) * skin_scale); }

// UIEditCtrl forwards accept/cancel (events 0/1) to its parent through
// vtable+0x94. Keep the original six-argument ABI and all other events.
int __fastcall on_event(Window* w, void*, void* source, int event, int a, int b, int c, int d) {
    constexpr int accept = 0, cancel = 1;
    if (event != accept && event != cancel) return base_on_event(w, nullptr, source, event, a, b, c, d);
    if (w->closing) return 0;

    const auto& l = w->layout;
    if (event == cancel) {
        if (w->confirming)
            click_at(w, l.abort_action);
        else if (w->rules)
            click_at(w, l.rules_back);
        else if (w->details || (w->registration && !w->picking))
            click_at(w, l.back);
        else
            close(w);
        return 0;
    }

    // Held Enter must never confirm a charge. Confirmation requires its
    // separate visible button after the terms have been reviewed.
    if (w->data.action_pending || w->confirming || w->rules) return 0;

    Rect target = l.details;
    if (w->registration && !w->picking) {
        // Enter moves from one input to the next.
        for (int i = 0; i < 2; ++i) {
            if (source == w->inputs[i]) {
                focus_on(w->inputs[i + 1]);
                return 0;
            }
        }
        target = l.more;
    } else if (w->details) {
        if (w->item_info) return 0;
        target = l.sale_action;
    } else if (source == w->edit && !w->picking) {
        target = l.search;
    }
    click_at(w, target);
    return 0;
}

void __fastcall on_right_click(Window* w, void*, int x, int y) {
    if (w->closing || x < 0 || y < 0 || w->rules || w->data.phase != Phase::Ready) return;
    x /= skin_scale;
    y /= skin_scale;

    if (w->details || (w->registration && !w->picking)) {
        // On these screens only the item header opens the popup.
        constexpr Rect item_header{18, 64, 521, 36};
        if (!item_header.contains(x, y)) return;
    } else if (!w->data.select(w->layout.row_at(x, y, w->data.page.count))) {
        return;
    }

    const int i = w->data.selected;
    if (i >= 0 && i < w->data.page.count) item_popup(w->data.page.rows[i]);
    dirty(w, nullptr);
}

// Fingerprint of the screen. A press only activates on release when the
// screen did not change in between (for example, a server update arrived).
u32 input_context(const Window* w) {
    return w->data.page.request
         ^ (static_cast<u32>(w->data.view) << 28)
         ^ (w->details ? 0x1000000u : 0)
         ^ (w->rules ? 0x2000000u : 0)
         ^ (w->confirming ? 0x4000000u : 0)
         ^ (w->picking ? 0x8000000u : 0)
         ^ w->data.action_pending;
}

void track_mouse(Window* w, int mx, int my) {
    if (mx == w->mouse_x && my == w->mouse_y) return;
    w->mouse_x = mx;
    w->mouse_y = my;
    dirty(w, nullptr);
}

void __fastcall on_mouse_down(Window* w, void*, int x, int y) {
    if (w->closing) return;
    w->mouse_x = to_logical(x);
    w->mouse_y = to_logical(y);

    // Buttons are pressed on mouse down and activated on mouse up. Search
    // from the last drawn, which is the topmost.
    for (int i = w->button_count - 1; i >= 0; --i) {
        const auto& hit = w->buttons[i];
        if (!hit.rect.contains(w->mouse_x, w->mouse_y)) continue;
        if (hit.enabled) {
            w->pressed = hit.rect;
            w->mouse_down = true;
            w->press_context = input_context(w);
            capture_mouse(manager(), nullptr, w);
            dirty(w, nullptr);
        }
        return;
    }

    w->dragging = auction::ui::hit_title(w->mouse_x, w->mouse_y);
    on_click(w, nullptr, x, y);
}

void __fastcall on_mouse_move(Window* w, void*, int x, int y) {
    track_mouse(w, to_logical(x), to_logical(y));
    if (w->dragging) drag_move(w, nullptr, x, y);
}

void __fastcall on_mouse_outside(Window* w, void*, int x, int y) {
    // Parent-space mouse notification also arrives while an edit child is under
    // the cursor. Clear the old button hover even when still inside this frame.
    const int left = at<int>(w, 0x1c), top = at<int>(w, 0x20);
    track_mouse(w, to_logical(x - left), to_logical(y - top));
}

void __fastcall on_mouse_up(Window* w, void*, int x, int y) {
    if (w->closing) return;
    if (w->dragging) {
        w->dragging = false;
        drag_end(w, nullptr, x, y);
        return;
    }

    const bool activate = w->mouse_down && w->press_context == input_context(w) && x >= 0 && y >= 0 &&
                          w->pressed.contains(x / skin_scale, y / skin_scale);
    w->mouse_down = false;
    if (captured_window(manager(), nullptr) == w) release_mouse(manager(), nullptr);
    if (activate) on_click(w, nullptr, x, y);
    dirty(w, nullptr);
}

// ============================================================================
// Opening and server packets
// ============================================================================

// Copies the native frame vtable once and overrides the slots we handle.
void install_vtable() {
    if (window_vtable[0]) return;
    const auto* native = reinterpret_cast<void* const*>(auction_imports.frame_vtable);
    for (int i = 0; i < vtable_slots; ++i) window_vtable[i] = native[i];

    window_vtable[0x00 / 4] = reinterpret_cast<void*>(&on_delete);
    window_vtable[0x2c / 4] = reinterpret_cast<void*>(&on_pre_delete);
    window_vtable[0x50 / 4] = reinterpret_cast<void*>(&on_draw);
    window_vtable[0x64 / 4] = reinterpret_cast<void*>(&on_mouse_down);
    window_vtable[0x6c / 4] = reinterpret_cast<void*>(&on_mouse_move);
    window_vtable[0x74 / 4] = reinterpret_cast<void*>(&on_mouse_outside);
    window_vtable[0x7c / 4] = reinterpret_cast<void*>(&on_mouse_up);
    window_vtable[0x80 / 4] = reinterpret_cast<void*>(&on_right_click);
    window_vtable[0x94 / 4] = reinterpret_cast<void*>(&on_event);
    window_vtable[0xa4 / 4] = reinterpret_cast<void*>(&on_refresh);
}

void open() {
    if (current && !current->closing) {
        dirty(current, nullptr);
        return;
    }
    install_vtable();

    auto* w = static_cast<Window*>(allocate(sizeof(Window)));
    if (!w) return;
    memset(w, 0, sizeof(Window));
    construct(w, nullptr, 0);
    w->table = window_vtable;
    w->data.selected = -1;
    w->mouse_x = w->mouse_y = -1;

    create_edits(w);
    sync_inputs(w);
    resize(w);
    add_window(manager(), nullptr, w);
    clear_focus();
    set_window_id(w, nullptr, 0x2b80);
    current = w;

    QueryPacket p{};
    const bool ready = w->data.open(sequence, tick(), p);
    transmit(w, p, ready);
    request_rules(w);
}

// A background refresh replaced the page. Tell the player when the item they
// were looking at moved away or changed price.
void on_background_refresh(Window* w, bool had_selection, u32 old_bid, u32 old_buyer) {
    auto& c = w->data;
    if (had_selection && c.selected < 0) {
        w->details = w->item_info = w->confirming = false;
        edit_visible(w, true);
        w->notice = "This auction is no longer on this page. The list has been updated.";
    } else if (c.selected >= 0 &&
               (c.page.rows[c.selected].bid != old_bid || c.page.rows[c.selected].buyer_id != old_buyer)) {
        w->confirming = false;
        w->notice = "A new bid arrived. Review the updated price.";
    }

    // Deletions can make the last page disappear. Follow the new last page.
    if (!c.page.count && c.page.number > 1) {
        QueryPacket q{};
        const bool ready = c.request(sequence, tick(), c.page.number - 1, q);
        transmit(w, q, ready);
    }
    sync_inputs(w);
}

void on_action_result(Window* w) {
    auto& c = w->data;
    w->confirming = false;
    const auto result = c.action_result;

    if (result == Done) {
        w->form_initialized = false;
        QueryPacket query{};
        const bool ready = c.action_kind == RegisterSale
                               ? c.change_view(View::Sell, sequence, tick(), query)
                               : c.request(sequence, tick(), c.page.number ? c.page.number : 1, query);
        transmit(w, query, ready);
        w->notice = "Completed. The catalog has been updated.";
        return;
    }

    c.action_finished = false;
    w->notice = action_error(result);
    if (c.action_kind == RegisterSale && result == ItemChanged) {
        // Reload the inventory. transmit() clears the notice, so keep it.
        QueryPacket q{};
        const bool ready = c.request(sequence, tick(), 1, q);
        const char* note = w->notice;
        transmit(w, q, ready);
        w->notice = note;
    } else if (c.view != View::Inventory) {
        QueryPacket q{};
        if (c.refresh(sequence, tick(), q, true)) send_query(q);
    }
    sync_inputs(w);
}

void on_open_packet(const u8* p) {
    if (get32(p + 2) == 0)
        open();
    else if (current)
        close(current);
}

void on_list_packet(const u8* p) {
    if (!current || current->closing) return;
    Window* w = current;
    auto& c = w->data;

    // Remember the selected row so changes can be reported after the update.
    const bool was_background = c.background;
    const bool had_selection = c.selected >= 0 && c.selected < c.page.count;
    const u32 old_bid = had_selection ? c.page.rows[c.selected].bid : 0;
    const u32 old_buyer = had_selection ? c.page.rows[c.selected].buyer_id : 0;

    if (!c.receive(p, get16(p + 2), tick())) return;

    const u32 kind = get32(p + 4);
    if (was_background && kind == magic && !c.background && !c.refresh_failed)
        on_background_refresh(w, had_selection, old_bid, old_buyer);
    if (kind == action_magic && c.action_finished) on_action_result(w);
    dirty(w, nullptr);
}
}  // namespace

extern "C" void __cdecl auction_on_open_packet(const unsigned char* p) { on_open_packet(p); }
extern "C" void __cdecl auction_on_list_packet(const unsigned char* p) { on_list_packet(p); }

// Scope Arial and edit sizing to Auction, including native caret/hit testing.
extern "C" int __cdecl auction_uses_font(void* p) {
    return current && (p == current || p == current->edit || p == current->inputs[0] || p == current->inputs[1] ||
                       p == current->inputs[2]);
}

// ============================================================================
// Detours (jumped to from patched client code)
// ============================================================================
//
// The font hooks replace the 5-byte prologue of measure/print_text. For our
// windows they overwrite the font face (1 = Arial) and size (18) arguments on
// the stack, then replay the prologue (push ebp; mov ebp, esp; push -1) and
// resume at the original address + 5. pushfd + pushad put the caller's return
// address at [esp+36].

extern "C" __declspec(dllexport) __declspec(naked) void auction_measure_hook() {
    __asm {
        pushfd
        pushad
        push ecx
        call auction_uses_font
        add esp, 4
        test eax, eax
        jz unchanged
        mov dword ptr [esp+48], 1
        mov dword ptr [esp+52], 18
    unchanged:
        popad
        popfd
        push ebp
        mov ebp, esp
        push -1
        push auction_imports.measure_resume
        ret
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void auction_text_hook() {
    __asm {
        pushfd
        pushad
        push ecx
        call auction_uses_font
        add esp, 4
        test eax, eax
        jz unchanged
        mov dword ptr [esp+56], 1
        mov dword ptr [esp+60], 18
    unchanged:
        popad
        popfd
        push ebp
        mov ebp, esp
        push -1
        push auction_imports.text_resume
        ret
    }
}

// Packet receive hooks: pass the client's receive buffer (packet_buffer) to the
// handler, then continue in the client's packet dispatcher.
extern "C" __declspec(dllexport) __declspec(naked) void auction_recv_025f() {
    __asm {
        push auction_imports.packet_buffer
        call auction_on_open_packet
        add esp, 4
        push auction_imports.dispatch_continue
        ret
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void auction_recv_0252() {
    __asm {
        push auction_imports.packet_buffer
        call auction_on_list_packet
        add esp, 4
        push auction_imports.dispatch_continue
        ret
    }
}
