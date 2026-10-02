// Auction wire contract shared by map-server, char-server and the client window.
// CRT-free: the client section is built without a C/C++ runtime, so this header
// must not include standard or rAthena headers.
#pragma once

namespace auction_live {

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned;
static_assert(sizeof(u32) == 4 && sizeof(u16) == 2, "wire widths");

// Every server reply is a 0x0252 frame carrying this protocol version.
constexpr u16 reply_opcode = 0x252;
constexpr u16 protocol_version = 2;

// ---------------------------------------------------------------------------
// Little-endian helpers
// ---------------------------------------------------------------------------

inline u16 get16(const u8* p) { return u16(p[0] | p[1] << 8); }
inline u32 get32(const u8* p) { return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24; }

inline void put16(u8* p, u16 v) {
	p[0] = u8(v);
	p[1] = u8(v >> 8);
}

inline void put32(u8* p, u32 v) {
	for (int i = 0; i < 4; ++i)
		p[i] = u8(v >> (8 * i));
}

// Copies a string and pads the rest of the field with zeros.
inline void copy_string(char* out, const char* in, int capacity) {
	int i = 0;
	for (; i + 1 < capacity && in[i]; ++i)
		out[i] = in[i];
	for (; i < capacity; ++i)
		out[i] = 0;
}

// ---------------------------------------------------------------------------
// Queries (client request 0x0251, type | 0x8000)
// ---------------------------------------------------------------------------

// Types 0-4 and 8 are category filters (0 Armor, 1 Weapon, 2 Card, 3 Misc,
// 4 All, 8 Costume). The rest select a different listing. Actions use their
// Action value as the type.
enum QueryType : u16 {
	QueryAll = 4,
	QueryPrice = 5,
	QuerySales = 6,
	QueryBids = 7,
	QueryCostume = 8,
	QueryRules = 9,
	QueryInventory = 10,
};

// Category queries only list auctions whose item belongs to the category.
inline bool is_category_query(u16 type) { return type < QueryAll || type == QueryCostume; }

// ---------------------------------------------------------------------------
// Catalog and inventory pages (AUC2 / AUI2)
// ---------------------------------------------------------------------------

constexpr u32 magic = 0x32435541;            // AUC2
constexpr u32 inventory_magic = 0x32495541;  // AUI2, inventory offers, never auctions
constexpr int header_size = 32, row_size = 200, max_rows = 5, max_packet = 1032;

// Numeric item types carried unchanged from rAthena, not filter positions.
enum ItemType : u16 { TypeEtc = 3, TypeArmor = 4, TypeWeapon = 5, TypeCard = 6, TypePetArmor = 8, TypeShadowGear = 12 };
enum Status : u16 { Ok, Disabled, Unavailable, Invalid };

struct Row {
	u32 id, item_id, amount, bid, buy, expires, seller_id, buyer_id, unique_low, unique_high;
	u8 refine, grade, identified, attribute;
	u32 cards[4];
	struct Option {
		u16 id;
		short value;
		u8 parameter;
	} options[5];
	char seller[24], buyer[24], name[64];
	u16 type;
};

struct Page {
	u32 request, server_time;
	u16 status, number, pages, count;
	Row rows[max_rows];
};

// Page header, 32 bytes:
//   +0 opcode  +2 length  +4 magic  +8 request  +12 version  +14 status
//   +16 page  +18 pages  +20 row count  +22 row size  +24 server time  +28 zero
inline void encode_header(u8* p, u32 request, u16 status, u16 number, u16 pages, u16 count, u32 now) {
	put16(p, reply_opcode);
	put16(p + 2, u16(header_size + row_size * count));
	put32(p + 4, magic);
	put32(p + 8, request);
	put16(p + 12, protocol_version);
	put16(p + 14, status);
	put16(p + 16, number);
	put16(p + 18, pages);
	put16(p + 20, count);
	put16(p + 22, row_size);
	put32(p + 24, now);
	put32(p + 28, 0);
}

// Row, 200 bytes:
//   +0 ten u32 fields, in Row order (id ... unique_high)
//   +40 refine  +41 grade  +42 identified  +43 attribute  +44 cards (4 x u32)
//   +60 options (5 x id.W value.W parameter.B)
//   +85 seller[24]  +109 buyer[24]  +133 name[64]  +197 type.W  +199 zero
namespace row_offset {
constexpr int refine = 40, cards = 44, options = 60, option_size = 5;
constexpr int seller = 85, buyer = 109, name = 133, type = 197, end = 199;
}  // namespace row_offset

inline void encode_row(u8* p, const Row& r) {
	const u32 fields[] = {r.id, r.item_id, r.amount, r.bid, r.buy, r.expires, r.seller_id, r.buyer_id, r.unique_low, r.unique_high};
	for (int i = 0; i < 10; ++i)
		put32(p + 4 * i, fields[i]);
	p[row_offset::refine] = r.refine;
	p[row_offset::refine + 1] = r.grade;
	p[row_offset::refine + 2] = r.identified;
	p[row_offset::refine + 3] = r.attribute;
	for (int i = 0; i < 4; ++i)
		put32(p + row_offset::cards + 4 * i, r.cards[i]);
	for (int i = 0; i < 5; ++i) {
		u8* option = p + row_offset::options + row_offset::option_size * i;
		put16(option, r.options[i].id);
		put16(option + 2, u16(r.options[i].value));
		option[4] = r.options[i].parameter;
	}
	copy_string(reinterpret_cast<char*>(p + row_offset::seller), r.seller, 24);
	copy_string(reinterpret_cast<char*>(p + row_offset::buyer), r.buyer, 24);
	copy_string(reinterpret_cast<char*>(p + row_offset::name), r.name, 64);
	put16(p + row_offset::type, r.type);
	p[row_offset::end] = 0;
}

inline void decode_row(const u8* p, Row& r) {
	r.id = get32(p);
	r.item_id = get32(p + 4);
	r.amount = get32(p + 8);
	r.bid = get32(p + 12);
	r.buy = get32(p + 16);
	r.expires = get32(p + 20);
	r.seller_id = get32(p + 24);
	r.buyer_id = get32(p + 28);
	r.unique_low = get32(p + 32);
	r.unique_high = get32(p + 36);
	r.refine = p[row_offset::refine];
	r.grade = p[row_offset::refine + 1];
	r.identified = p[row_offset::refine + 2];
	r.attribute = p[row_offset::refine + 3];
	for (int i = 0; i < 4; ++i)
		r.cards[i] = get32(p + row_offset::cards + 4 * i);
	for (int i = 0; i < 5; ++i) {
		const u8* option = p + row_offset::options + row_offset::option_size * i;
		r.options[i].id = get16(option);
		r.options[i].value = short(get16(option + 2));
		r.options[i].parameter = option[4];
	}
	copy_string(r.seller, reinterpret_cast<const char*>(p + row_offset::seller), 24);
	copy_string(r.buyer, reinterpret_cast<const char*>(p + row_offset::buyer), 24);
	copy_string(r.name, reinterpret_cast<const char*>(p + row_offset::name), 64);
	r.type = get16(p + row_offset::type);
}

// A row must name an auction, an item, an amount and a buy-now price not below
// the bid, with a seller and an item name; all strings stay terminated.
inline bool valid_row(const u8* r) {
	if (!get32(r) || !get32(r + 4) || !get32(r + 8) || !get32(r + 16) || get32(r + 12) > get32(r + 16))
		return false;
	if (!r[row_offset::seller] || !r[row_offset::name])
		return false;
	return !r[row_offset::seller + 23] && !r[row_offset::buyer + 23] && !r[row_offset::name + 63] && !r[row_offset::end];
}

// Validates a whole page frame and decodes it into out. Nothing is written on failure.
inline bool decode(const u8* p, int length, u32 expected, Page& out, u32 expected_magic = magic) {
	if (length < header_size || length > max_packet)
		return false;
	if (get16(p) != reply_opcode || get16(p + 2) != length || get32(p + 4) != expected_magic ||
	    get16(p + 12) != protocol_version || !expected || get32(p + 8) != expected || get16(p + 22) != row_size ||
	    get32(p + 28))
		return false;

	const u16 status = get16(p + 14), number = get16(p + 16), pages = get16(p + 18), count = get16(p + 20);
	if (count > max_rows || length != header_size + row_size * count || status > Invalid)
		return false;
	if (!pages || !number || number > pages || (status && count))
		return false;

	for (int i = 0; i < count; ++i) {
		const u8* r = p + header_size + row_size * i;
		if (!valid_row(r))
			return false;
		// Auction ids are unique within a page.
		for (int j = 0; j < i; ++j)
			if (get32(p + header_size + row_size * j) == get32(r))
				return false;
	}

	out.request = expected;
	out.status = status;
	out.number = number;
	out.pages = pages;
	out.count = count;
	out.server_time = get32(p + 24);
	for (int i = 0; i < count; ++i)
		decode_row(p + header_size + row_size * i, out.rows[i]);
	return true;
}

// ---------------------------------------------------------------------------
// Server rules (AUR2)
// ---------------------------------------------------------------------------

constexpr u32 rules_magic = 0x32525541;  // AUR2
constexpr int rules_size = 64;

enum Capability : u32 { Catalog = 1, Register = 2, Bid = 4, BuyNow = 8, Cancel = 16, Close = 32 };
constexpr u32 all_capabilities = 63;

struct Rules {
	u32 request = 0, capabilities = 0, fee_per_hour = 0, maximum_price = 0, server_time = 0, character_id = 0;
	u16 status = Unavailable, minimum_hours = 0, maximum_hours = 0, maximum_sales = 0, maximum_bids = 0;
};

// Rules frame, 64 bytes:
//   +0 opcode  +2 length  +4 magic  +8 request  +12 version  +14 status
//   +16 capabilities  +20 fee per hour  +24 maximum price  +28 minimum hours
//   +30 maximum hours  +32 maximum sales  +34 maximum bids  +36 server time
//   +40 character id  +44 zero
inline void encode_rules(u8* p, const Rules& r) {
	for (int i = 0; i < rules_size; ++i)
		p[i] = 0;
	put16(p, reply_opcode);
	put16(p + 2, rules_size);
	put32(p + 4, rules_magic);
	put32(p + 8, r.request);
	put16(p + 12, protocol_version);
	put16(p + 14, r.status);
	put32(p + 16, r.capabilities);
	put32(p + 20, r.fee_per_hour);
	put32(p + 24, r.maximum_price);
	put16(p + 28, r.minimum_hours);
	put16(p + 30, r.maximum_hours);
	put16(p + 32, r.maximum_sales);
	put16(p + 34, r.maximum_bids);
	put32(p + 36, r.server_time);
	put32(p + 40, r.character_id);
}

inline bool decode_rules(const u8* p, int length, u32 expected, Rules& r) {
	if (length != rules_size || !expected)
		return false;
	if (get16(p) != reply_opcode || get16(p + 2) != rules_size || get32(p + 4) != rules_magic ||
	    get32(p + 8) != expected || get16(p + 12) != protocol_version || get16(p + 14) > Invalid)
		return false;
	for (int i = 44; i < rules_size; ++i)
		if (p[i])
			return false;

	// Capabilities are only granted with Ok status, and then every limit must be set.
	const u32 capabilities = get32(p + 16);
	if ((capabilities & ~all_capabilities) || (get16(p + 14) != Ok && capabilities))
		return false;
	if (capabilities && (!get32(p + 24) || !get16(p + 28) || get16(p + 28) > get16(p + 30) || !get16(p + 32) ||
	                     !get16(p + 34) || !get32(p + 40)))
		return false;

	r.request = expected;
	r.status = get16(p + 14);
	r.capabilities = capabilities;
	r.fee_per_hour = get32(p + 20);
	r.maximum_price = get32(p + 24);
	r.minimum_hours = get16(p + 28);
	r.maximum_hours = get16(p + 30);
	r.maximum_sales = get16(p + 32);
	r.maximum_bids = get16(p + 34);
	r.server_time = get32(p + 36);
	r.character_id = get32(p + 40);
	return true;
}

// Decimal input is bounded before multiplication; whitespace/signs are invalid.
inline bool parse_price(const char* s, u32& value) {
	if (!s || !*s)
		return false;
	u32 result = 0;
	for (int i = 0; s[i]; ++i) {
		if (i >= 10 || s[i] < '0' || s[i] > '9')
			return false;
		const u32 digit = s[i] - '0';
		if (result > (0x7fffffffu - digit) / 10)
			return false;
		result = result * 10 + digit;
	}
	value = result;
	return true;
}

// ---------------------------------------------------------------------------
// Economic actions (AUA2)
// ---------------------------------------------------------------------------

constexpr u32 action_magic = 0x32415541;  // AUA2
constexpr int action_size = 32;

enum Action : u16 { ReadRules = 0, CancelSale = 16, CloseSale = 17, RegisterSale = 18, BidSale = 19, BuySale = 20 };
enum ActionResult : u16 { Done, Denied, Missing, Changed, Retry, Malformed, InsufficientFunds, ItemChanged, LimitReached };

// Action frame, 32 bytes:
//   +0 opcode  +2 length  +4 magic  +8 request  +12 version  +14 result
//   +16 auction id  +20 action  +22 zero
inline void encode_action(u8* p, u32 request, u32 id, u16 action, u16 status) {
	for (int i = 0; i < action_size; ++i)
		p[i] = 0;
	put16(p, reply_opcode);
	put16(p + 2, action_size);
	put32(p + 4, action_magic);
	put32(p + 8, request);
	put16(p + 12, protocol_version);
	put16(p + 14, status);
	put32(p + 16, id);
	put16(p + 20, action);
}

// Accepts only the reply to this exact request, auction and action.
inline bool decode_action(const u8* p, int length, u32 request, u32 id, u16 action, u16& status) {
	if (length != action_size || !request)
		return false;
	if (get16(p) != reply_opcode || get16(p + 2) != action_size || get32(p + 4) != action_magic ||
	    get32(p + 8) != request || get16(p + 12) != protocol_version || get16(p + 14) > LimitReached ||
	    get32(p + 16) != id || get16(p + 20) != action)
		return false;
	for (int i = 22; i < action_size; ++i)
		if (p[i])
			return false;
	status = get16(p + 14);
	return true;
}

// ---------------------------------------------------------------------------
// Category filters
// ---------------------------------------------------------------------------

// UI order: All, Armor, Weapon, Card, Misc, Costume. Shared by catalog and inventory.
inline bool matches_category(unsigned filter, unsigned type, bool costume) {
	switch (filter) {
	case 0: return true;
	case 1: return !costume && (type == TypeArmor || type == TypePetArmor || type == TypeShadowGear);
	case 2: return !costume && type == TypeWeapon;
	case 3: return type == TypeCard;
	case 4: return !costume && type == TypeEtc;
	case 5: return costume;
	default: return false;
	}
}

// Inventory queries carry the filter in the first text byte; the rest is zero.
inline bool inventory_filter(const char* text, unsigned& filter) {
	filter = static_cast<unsigned char>(text[0]);
	if (filter > 5)
		return false;
	for (int i = 1; i < 24; ++i)
		if (text[i])
			return false;
	return true;
}

}  // namespace auction_live
