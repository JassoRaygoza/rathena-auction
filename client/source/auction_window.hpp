// Native Auction window: layout, text fitting and the catalog controller.
// Freestanding: the client section links without a C/C++ runtime.
#pragma once
// From the emulator checkout (EmulatorDir in auction_section.vcxproj).
#include <common/auction_protocol.hpp>

// ============================================================================
// Runtime
// ============================================================================
//
// Without a CRT the compiler still emits calls to memset/memcpy for zeroing
// and copying structs, so this translation unit provides them.

using size_type = decltype(sizeof 0);

extern "C" void* __cdecl memset(void* destination, int value, size_type size);
#pragma function(memset)
extern "C" void* __cdecl memset(void* destination, int value, size_type size) {
    auto* bytes = static_cast<unsigned char*>(destination);
    while (size--) *bytes++ = static_cast<unsigned char>(value);
    return destination;
}

extern "C" void* __cdecl memcpy(void* destination, const void* source, size_type size);
#pragma function(memcpy)
extern "C" void* __cdecl memcpy(void* destination, const void* source, size_type size) {
    auto* out = static_cast<unsigned char*>(destination);
    const auto* in = static_cast<const unsigned char*>(source);
    while (size--) *out++ = *in++;
    return destination;
}

// ============================================================================
// Window geometry (logical pixels, measured on auction_bg_0.bmp)
// ============================================================================

namespace auction::ui {
constexpr int window_width = 550, window_height = 300;
constexpr int close_x = 534;
constexpr int first_row_y = 64, row_height = 27, row_pitch = 34;

struct Column { int x, width; };
// Item | bid/buy now (two lines) | seller | buyer | remaining.
constexpr Column columns[] = {{69, 84}, {158, 148}, {311, 69}, {385, 68}, {458, 89}};

constexpr const char* titles[] = {
    "Auction - Product List",
    "Auction - Register Item",
    "Auction - My Sales",
    "Auction - My Bids",
};

// "유저인터페이스\basic_interface\" in CP949, as referenced by the client executables.
constexpr char texture_prefix[] = "\xc0\xaf\xc0\xfa\xc0\xce\xc5\xcd\xc6\xe4\xc0\xcc\xbd\xba\\basic_interface\\";

// Returns the row index under a window-local point, or -1.
constexpr int hit_row_index(int x, int y) {
    if (x < 0 || x >= window_width || y < first_row_y) return -1;
    const int offset = y - first_row_y;
    const int row = offset / row_pitch;
    return row < auction_live::max_rows && offset % row_pitch < row_height ? row : -1;
}

// The title bar, used for dragging the window.
constexpr bool hit_title(int x, int y) { return x >= 0 && x < close_x && y >= 0 && y < 18; }
}  // namespace auction::ui

// ============================================================================
// Text fitting
// ============================================================================
//
// Fits CP949 UI strings without cutting a multibyte character. Width must
// measure with the renderer's actual font. ASCII dots are CP949-compatible.

namespace auction::ui {
inline int character_bytes(const char* text) {
    const unsigned char first = static_cast<unsigned char>(text[0]);
    return first >= 0x81 && first <= 0xfe && text[1] ? 2 : 1;
}

// Copies source into output, ending it with "..." when it is wider than
// max_width. Returns the number of bytes written (without the terminator).
template<class Width>
int fit_text(const char* source, char* output, int capacity, int max_width, Width width) {
    if (capacity < 4) {
        if (capacity > 0) output[0] = 0;
        return 0;
    }

    // Copy whole characters, leaving room for "...".
    int used = 0;
    while (*source) {
        const int bytes = character_bytes(source);
        if (used + bytes >= capacity - 3) break;
        for (int i = 0; i < bytes; ++i) output[used++] = *source++;
    }
    output[used] = 0;
    if (!*source && width(output, used) <= max_width) return used;

    // Drop one character at a time until the text plus "..." fits.
    while (true) {
        output[used] = '.';
        output[used + 1] = '.';
        output[used + 2] = '.';
        output[used + 3] = 0;
        if (width(output, used + 3) <= max_width) return used + 3;
        if (!used) {
            output[0] = 0;
            return 0;
        }
        int previous = 0;
        for (int i = 0; i < used; i += character_bytes(output + i)) previous = i;
        used = previous;
    }
}
}  // namespace auction::ui

// ============================================================================
// Skin layout
// ============================================================================

namespace auction_live {
// The skin is drawn at 2x: 1100x600 physical pixels. Control geometry keeps
// the original logical coordinates and is scaled at the input/render boundary.
// Text is redrawn at 18 px rather than scaling 12 px raster text to 24 px.
constexpr int skin_scale = 2;

struct Rect {
    int x, y, w, h;
    constexpr bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

inline int maximum(int a, int b) { return a > b ? a : b; }

enum class Filter { All, Armor, Weapon, Card, Misc, Costume };
constexpr const char* filter_names[] = {"All", "Armor", "Weapon", "Card", "Misc", "Costume"};
// Query type sent for each filter.
constexpr unsigned short filter_types[] = {4, 0, 1, 2, 3, 8};

struct Layout {
    int width = auction::ui::window_width * skin_scale;
    int height = auction::ui::window_height * skin_scale;
    int font = 18;
    int row_pitch = auction::ui::row_pitch;
    int rows_y = auction::ui::first_row_y;

    Rect close{534, 2, 11, 11};

    // Catalog
    Rect filters[6]{{8, 238, 27, 18}, {39, 238, 45, 18}, {88, 238, 57, 18},
                    {149, 238, 35, 18}, {188, 238, 35, 18}, {227, 238, 61, 18}};
    Rect tabs[4]{{8, 263, 67, 30}, {78, 263, 67, 30}, {148, 263, 67, 30}, {218, 263, 67, 30}};
    Rect previous{303, 238, 19, 18};
    Rect next{526, 238, 19, 18};
    Rect search_mode{298, 263, 36, 16};
    Rect search_edit{334, 263, 151, 16};
    Rect search{490, 263, 55, 16};
    Rect refresh{298, 281, 61, 16};
    Rect rules{368, 281, 110, 16};
    Rect details{484, 281, 61, 16};

    // Details and registration
    Rect sale_action{69, 234, 105, 20};
    Rect buy_action{300, 234, 65, 20};
    Rect confirm_action{368, 234, 65, 20};
    Rect abort_action{436, 234, 65, 20};
    Rect more{436, 267, 53, 26};
    Rect back{493, 267, 52, 26};

    // Server rules
    Rect rules_refresh{8, 263, 265, 30};
    Rect rules_back{280, 263, 265, 30};

    // Inventory picker
    Rect inventory_refresh{298, 263, 61, 30};
    Rect inventory_cancel{368, 263, 110, 30};
    Rect inventory_choose{484, 263, 61, 30};

    // Returns the listing row under (x, y), or -1.
    int row_at(int x, int y, unsigned count) const {
        const int row = auction::ui::hit_row_index(x, y);
        return row >= 0 && row < static_cast<int>(count) ? row : -1;
    }
};

// The layout does not depend on the resolution yet.
inline Layout layout_for(int, int) { return Layout{}; }
}  // namespace auction_live

// ============================================================================
// Item categories and rules
// ============================================================================

namespace auction_live {
// rAthena item_types values received from the authoritative item database.
inline bool auction_category(u16 type) {
    switch (type) {
    case TypeEtc:
    case TypeArmor:
    case TypeWeapon:
    case TypeCard:
    case TypePetArmor:
    case TypeShadowGear:
        return true;
    default:
        return false;
    }
}

inline const char* category_name(u16 type) {
    switch (type) {
    case TypeEtc:        return "ETC";
    case TypeArmor:      return "Armor";
    case TypeWeapon:     return "Weapon";
    case TypeCard:       return "Card";
    case TypePetArmor:   return "Pet equipment";
    case TypeShadowGear: return "Shadow equipment";
    default:             return "Not eligible";
    }
}

// rAthena e_enchantgrade: 1 D, 2 C, 3 B, 4 A. Callers skip 0 (no grade).
inline const char* grade_name(u8 grade) {
    switch (grade) {
    case 1:  return "D";
    case 2:  return "C";
    case 3:  return "B";
    case 4:  return "A";
    default: return "?";
    }
}

// The listing fee (fee_per_hour * hours) must fit in a signed 32-bit zeny value.
inline bool fee_overflows(const Rules& r, u32 hours) { return hours && r.fee_per_hour > 0x7fffffffu / hours; }

inline bool valid_hours(const Rules& r, u32 hours) {
    return hours >= r.minimum_hours && hours <= r.maximum_hours && !fee_overflows(r, hours);
}
}  // namespace auction_live

// ============================================================================
// Catalog controller
// ============================================================================

namespace auction_live {
enum class Phase { Closed, Loading, Ready, Error };
enum class View { List, Sell, Buy, Inventory };

// Client request (opcode 0x251), 34 bytes:
//   +0 opcode   +2 0x8000 | type   +4 request token
//   +8 payload (query text or action fields)   +32 page number
struct QueryPacket { u8 bytes[34]; };

namespace query {
constexpr u16 opcode = 0x251;
constexpr u16 flag = 0x8000;
constexpr int token = 4, payload = 8, page = 32;
constexpr int text_size = 24;
}  // namespace query

// Owned by one UI-thread window. The request sequence belongs to the client
// process, not the window: reopening must not reuse an earlier query token.
struct Controller {
    // Milliseconds.
    static constexpr u32 refresh_interval = 3000, retry_interval = 5000, request_timeout = 10000;

    Phase phase = Phase::Closed;
    View view = View::List;
    Filter filter = Filter::All;
    Page page{};
    int selected = -1;

    // Page request in flight (token), when it started and when the page arrived.
    u32 pending = 0, started_at = 0, received_at = 0;
    char query[24]{};
    bool price_search = false;

    Rules rules{};
    u32 rules_pending = 0, rules_started = 0;

    // Economic action in flight. The packet is kept for retransmission.
    u32 action_pending = 0, action_id = 0, action_sent = 0;
    u16 action_kind = 0, action_result = Done;
    QueryPacket action_packet{};
    bool action_finished = false;

    // Background refresh of the visible page.
    bool background = false, refresh_failed = false;
    u32 refresh_at = 0;

    // ---- Requests ----

    bool open(u32& sequence, u32 tick, QueryPacket& packet) {
        phase = Phase::Ready;
        view = View::List;
        filter = Filter::All;
        query[0] = 0;
        return request(sequence, tick, 1, packet);
    }

    void close() {
        phase = Phase::Closed;
        pending = rules_pending = action_pending = 0;
        selected = -1;
        page.count = 0;
        rules = Rules{};
    }

    // Loads a page from scratch: clears the rows and the selection.
    bool request(u32& sequence, u32 tick, u16 number, QueryPacket& packet) {
        if (phase == Phase::Closed || !number) return false;
        if (!action_pending) action_finished = false;
        if (exhausted(sequence)) {
            phase = Phase::Error;
            pending = 0;
            page.count = 0;
            selected = -1;
            page.status = Unavailable;
            return false;
        }

        background = refresh_failed = false;
        pending = ++sequence;
        started_at = refresh_at = tick;
        selected = -1;
        page.count = 0;
        phase = Phase::Loading;

        begin_packet(packet, query_type(), pending, number);
        if (view == View::Inventory)
            packet.bytes[query::payload] = static_cast<u8>(filter);
        else
            copy_string(query_text(packet), query, query::text_size);
        return true;
    }

    // Refreshes the current query without clearing rows or moving the selection.
    // Inventory tokens belong to a frozen offer and must not be refreshed here.
    bool refresh(u32& sequence, u32 tick, QueryPacket& packet, bool force = false) {
        if (phase != Phase::Ready || view == View::Inventory || pending || action_pending || exhausted(sequence))
            return false;
        if (!force && tick - refresh_at < refresh_interval) return false;

        pending = ++sequence;
        started_at = refresh_at = tick;
        background = true;
        begin_packet(packet, query_type(), pending, page.number ? page.number : 1);
        copy_string(query_text(packet), query, query::text_size);
        return true;
    }

    bool request_rules(u32& sequence, u32 tick, QueryPacket& packet) {
        if (phase == Phase::Closed || exhausted(sequence)) return false;
        rules_pending = ++sequence;
        rules_started = tick;
        rules = Rules{};
        begin_packet(packet, QueryRules, rules_pending, 1);
        return true;
    }

    bool change_filter(Filter value, u32& sequence, u32 tick, QueryPacket& packet) {
        if (phase == Phase::Closed || (view != View::List && view != View::Inventory) ||
            static_cast<unsigned>(value) > 5)
            return false;
        filter = value;
        price_search = false;
        return request(sequence, tick, 1, packet);
    }

    bool change_view(View value, u32& sequence, u32 tick, QueryPacket& packet) {
        if (phase == Phase::Closed || static_cast<unsigned>(value) > 3) return false;
        view = value;
        if (view == View::Inventory) filter = Filter::All;
        query[0] = 0;
        price_search = false;
        return request(sequence, tick, 1, packet);
    }

    bool search(const char* text, u32& sequence, u32 tick, QueryPacket& packet) {
        if (phase == Phase::Closed) return false;
        u32 price = 0;
        if (price_search && !parse_price(text, price)) return false;
        copy_string(query, text, query::text_size);
        return request(sequence, tick, 1, packet);
    }

    bool previous_enabled() const { return phase == Phase::Ready && page.number > 1; }
    bool next_enabled() const { return phase == Phase::Ready && page.number < page.pages; }

    bool navigate(int direction, u32& sequence, u32 tick, QueryPacket& packet) {
        if (direction == -1 && previous_enabled()) return request(sequence, tick, u16(page.number - 1), packet);
        if (direction == 1 && next_enabled()) return request(sequence, tick, u16(page.number + 1), packet);
        return false;
    }

    // ---- Economic actions ----

    // Cancel, close, bid on or buy the selected auction.
    bool act(u16 action, u32& sequence, u32 tick, QueryPacket& packet, u32 amount = 0) {
        if (action_pending || phase != Phase::Ready || view == View::Inventory || selected < 0 ||
            selected >= page.count || !time_left(selected, tick) || exhausted(sequence) || rules.status != Ok)
            return false;

        const auto& row = page.rows[selected];
        const bool own = row.seller_id == rules.character_id;
        const bool selling = view == View::Sell;
        switch (action) {
        case CancelSale:
            if (!selling || !own || row.buyer_id != 0 || !(rules.capabilities & Cancel)) return false;
            break;
        case CloseSale:
            if (!selling || !own || row.buyer_id == 0 || !(rules.capabilities & Close)) return false;
            break;
        case BidSale:
            if (own || selling || !(rules.capabilities & Bid)) return false;
            if (amount <= row.bid || amount >= row.buy) return false;
            break;
        case BuySale:
            if (own || selling || !(rules.capabilities & BuyNow)) return false;
            amount = row.buy;
            break;
        default:
            return false;
        }

        pending = 0;
        background = false;
        start_action(action, row.id, sequence, tick);

        // The server checks the price and buyer it was shown against the current ones.
        begin_packet(packet, action, action_pending, 1);
        u8* payload = packet.bytes + query::payload;
        put32(payload, row.id);
        if (action == BidSale || action == BuySale) {
            put32(payload + 4, amount);
            put32(payload + 8, row.bid);
            put32(payload + 12, row.buyer_id);
        } else {
            put32(payload + 4, row.bid);
            put32(payload + 8, row.buyer_id);
        }
        action_packet = packet;
        return true;
    }

    // Lists the selected inventory item. Only valid on the inventory view.
    bool register_sale(u32 price, u32 buy, u32 hours, u32& sequence, u32 tick, QueryPacket& packet) {
        if (action_pending || phase != Phase::Ready || view != View::Inventory || selected < 0 ||
            selected >= page.count || exhausted(sequence) || rules.status != Ok ||
            !(rules.capabilities & Register) || !auction_category(page.rows[selected].type))
            return false;
        if (!price || price >= buy || buy > rules.maximum_price || !valid_hours(rules, hours)) return false;

        start_action(RegisterSale, page.rows[selected].id, sequence, tick);

        begin_packet(packet, RegisterSale, action_pending, 1);
        u8* payload = packet.bytes + query::payload;
        put32(payload, action_id);
        put32(payload + 4, price);
        put32(payload + 8, buy);
        put32(payload + 12, hours);
        put32(payload + 16, page.request);  // the inventory offer the item came from
        action_packet = packet;
        return true;
    }

    // Resends the pending action until the server answers.
    bool retry_action(u32 tick, QueryPacket& packet) {
        if (phase == Phase::Closed || !action_pending || tick - action_sent < retry_interval) return false;
        action_sent = tick;
        packet = action_packet;
        return true;
    }

    // ---- Responses ----

    bool receive(const u8* data, int length, u32 tick) {
        const u32 kind = length >= 8 ? get32(data + 4) : 0;
        if (phase != Phase::Closed && kind == action_magic) return receive_action(data, length);
        if (phase != Phase::Closed && kind == rules_magic) return receive_rules(data, length);
        if (background && phase == Phase::Ready) return receive_refresh(data, length, tick);
        return receive_page(data, length, tick);
    }

    // Expires requests that got no answer. Returns true when the view changed.
    bool timeout(u32 tick) {
        if (rules_pending && tick - rules_started >= request_timeout) {
            rules_pending = 0;
            rules = Rules{};
        }
        if (background && tick - started_at >= request_timeout) {
            pending = 0;
            background = false;
            refresh_failed = true;
            refresh_at = tick;
            return true;
        }
        if (phase != Phase::Loading || tick - started_at < request_timeout) return false;

        phase = Phase::Error;
        pending = 0;
        page.count = 0;
        page.status = Unavailable;
        return true;
    }

    // ---- Queries ----

    bool select(int row) {
        if (phase != Phase::Ready || row < 0 || row >= page.count) return false;
        selected = row;
        return true;
    }

    // Seconds left for a row, counted down locally since the page arrived.
    u32 time_left(int row, u32 tick) const {
        if (phase != Phase::Ready || row < 0 || row >= page.count) return 0;
        const u32 expires = page.rows[row].expires;
        if (expires <= page.server_time) return 0;
        const u32 initial = expires - page.server_time;
        const u32 elapsed = (tick - received_at) / 1000;
        return elapsed >= initial ? 0 : initial - elapsed;
    }

private:
    static bool exhausted(u32 sequence) { return sequence == 0xffffffffu; }

    static char* query_text(QueryPacket& packet) { return reinterpret_cast<char*>(packet.bytes + query::payload); }

    static void begin_packet(QueryPacket& packet, u16 type, u32 token, u16 page_number) {
        for (auto& b : packet.bytes) b = 0;
        put16(packet.bytes, query::opcode);
        put16(packet.bytes + 2, u16(query::flag | type));
        put32(packet.bytes + query::token, token);
        put16(packet.bytes + query::page, page_number);
    }

    u16 query_type() const {
        switch (view) {
        case View::Inventory: return QueryInventory;
        case View::Sell:      return QuerySales;
        case View::Buy:       return QueryBids;
        default:              return price_search ? QueryPrice : filter_types[static_cast<int>(filter)];
        }
    }

    void start_action(u16 kind, u32 id, u32& sequence, u32 tick) {
        action_pending = ++sequence;
        action_sent = tick;
        action_id = id;
        action_kind = kind;
        action_result = Retry;
        action_finished = false;
    }

    bool receive_action(const u8* data, int length) {
        u16 status = Retry;
        if (!decode_action(data, length, action_pending, action_id, action_kind, status)) return false;
        action_result = status;
        if (status != Retry) {
            action_pending = 0;
            action_finished = true;
        }
        return true;
    }

    bool receive_rules(const u8* data, int length) {
        if (!decode_rules(data, length, rules_pending, rules)) return false;
        rules_pending = 0;
        return true;
    }

    // Replaces the visible page, keeping the same auction selected if it is
    // still there.
    bool receive_refresh(const u8* data, int length, u32 tick) {
        Page fresh{};
        if (!decode(data, length, pending, fresh)) return false;
        pending = 0;
        background = false;
        refresh_at = tick;
        if (fresh.status != Ok) {
            refresh_failed = true;
            return true;
        }

        const u32 id = selected >= 0 && selected < page.count ? page.rows[selected].id : 0;
        page = fresh;
        selected = -1;
        for (int i = 0; i < page.count; ++i)
            if (page.rows[i].id == id) selected = i;
        received_at = tick;
        refresh_failed = false;
        return true;
    }

    bool receive_page(const u8* data, int length, u32 tick) {
        const u32 expected_magic = view == View::Inventory ? inventory_magic : magic;
        if (phase != Phase::Loading || !decode(data, length, pending, page, expected_magic)) return false;
        pending = 0;
        received_at = refresh_at = tick;
        phase = page.status == Ok ? Phase::Ready : Phase::Error;
        return true;
    }
};
}  // namespace auction_live
