// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

// Modern Auction gateway (map-server): handles CZ_AUCTION_ITEM_SEARCH requests
// flagged with 0x8000, builds inventory offers, reserves zeny/items until the
// char-server confirms, and answers the client with versioned 0x0252 frames.

#include "auction_gateway.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include <common/auction_protocol.hpp>
#include <common/auction_service.hpp>
#include <common/mmo.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>

#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "map.hpp"
#include "packets.hpp"
#include "pc.hpp"
#include "pc_groups.hpp"

extern int32 char_fd; // inter server Fd used for char_fd
#define inter_fd char_fd	// alias

// Minimum time between client requests, and between resends of a pending purchase (ms).
static constexpr t_tick AUCTION_QUERY_INTERVAL = 250;
static constexpr t_tick AUCTION_ACTION_INTERVAL = 500;
static constexpr t_tick AUCTION_RESEND_INTERVAL = 5000;

// Inventory offers identify items by inventory index + 2.
static constexpr uint32 AUCTION_OFFER_BASE = 2;

static_assert(static_cast<unsigned>(auction_live::TypeArmor) == IT_ARMOR && static_cast<unsigned>(auction_live::TypeWeapon) == IT_WEAPON &&
	static_cast<unsigned>(auction_live::TypeEtc) == IT_ETC && static_cast<unsigned>(auction_live::TypeCard) == IT_CARD &&
	static_cast<unsigned>(auction_live::TypePetArmor) == IT_PETARMOR && static_cast<unsigned>(auction_live::TypeShadowGear) == IT_SHADOWGEAR,
	"Auction item types must match the authoritative rAthena item_types enum");

// ---------------------------------------------------------------------------
// Client requests and 0x0252 replies
// ---------------------------------------------------------------------------

// Copies the item fields shared by auction and inventory rows.
static void auction_row_item(auction_live::Row& r, const item& it) {
	r.item_id = it.nameid;
	r.amount = it.amount;
	r.refine = it.refine;
	r.grade = it.enchantgrade;
	r.identified = it.identify;
	r.attribute = it.attribute;
	r.unique_low = static_cast<uint32>(it.unique_id);
	r.unique_high = static_cast<uint32>(it.unique_id >> 32);
	for (int32 i = 0; i < 4; ++i)
		r.cards[i] = it.card[i];
	for (int32 i = 0; i < 5; ++i) {
		r.options[i].id = it.option[i].id;
		r.options[i].value = it.option[i].value;
		r.options[i].parameter = it.option[i].param;
	}
}

void clif_Auction_live_result(map_session_data* sd, uint32 request, uint16 page, uint16 pages, uint16 count, const uint8* data, uint16 status) {
	using namespace auction_live;
	if (!sd || !request || sd->auction.live_request != request || count > max_rows || !page || page > pages ||
	    status > Invalid || (status && count) || (count && !data))
		return;

	const int32 fd = sd->fd, length = header_size + row_size * count;
	WFIFOHEAD(fd, length);
	auto* out = static_cast<uint8*>(WFIFOP(fd, 0));
	memset(out, 0, length);
	encode_header(out, request, status, page, pages, count, static_cast<uint32>(time(nullptr)));

	for (uint16 i = 0; i < count; ++i) {
		auction_data a;
		memcpy(&a, data + i * sizeof(a), sizeof(a));

		Row r{};
		auction_row_item(r, a.item);
		r.id = a.auction_id;
		r.bid = a.price;
		r.buy = a.buynow;
		r.expires = static_cast<uint32>(a.timestamp);
		r.seller_id = a.seller_id;
		r.buyer_id = a.buyer_id;
		r.type = a.type;
		copy_string(r.seller, a.seller_name, sizeof(r.seller));
		copy_string(r.buyer, a.buyer_name, sizeof(r.buyer));
		const auto item = item_db.find(a.item.nameid);
		copy_string(r.name, item ? item->ename.c_str() : a.item_name, sizeof(r.name));
		encode_row(out + header_size + row_size * i, r);
	}

	WFIFOSET(fd, length);
	sd->auction.live_request = 0;
}

static bool auction_eligible_item(map_session_data* sd, int index) {
	if (index < 0 || index >= MAX_INVENTORY)
		return false;
	const auto& item = sd->inventory.u.items_inventory[index];
	auto id = item_db.find(item.nameid);
	if (!id || item.amount <= 0 || item.equip || item.equipSwitch || item.expire_time || !item.identify)
		return false;
	if (item.bound && !pc_can_give_bounded_items(sd))
		return false;
	if (!itemdb_available(item.nameid) || !itemdb_canauction(&item, pc_get_group_level(sd)))
		return false;
	return id->type == IT_ARMOR || id->type == IT_PETARMOR || id->type == IT_WEAPON || id->type == IT_CARD ||
	       id->type == IT_ETC || id->type == IT_SHADOWGEAR;
}

// Lists the player's eligible items as an inventory offer. The offer token and
// the exact offered items are remembered so a registration can be verified.
static void clif_Auction_inventory(map_session_data* sd, const PACKET_CZ_AUCTION_ITEM_SEARCH& p) {
	using namespace auction_live;
	unsigned filter = 0;
	uint16 status = Ok;
	if (!inventory_filter(p.text, filter))
		status = Invalid;
	else if (!battle_config.feature_auction)
		status = Disabled;
	else if (!pc_can_give_items(sd))
		status = Unavailable;

	// Filtered before pagination.
	std::vector<int> indices;
	if (status == Ok) {
		for (int i = 0; i < MAX_INVENTORY; ++i) {
			if (!auction_eligible_item(sd, i))
				continue;
			const auto id = item_db.find(sd->inventory.u.items_inventory[i].nameid);
			if (matches_category(filter, id->type, is_costume(id->equip, id->name)))
				indices.push_back(i);
		}
	}

	const uint16 pages = static_cast<uint16>(std::max<size_t>(1, (indices.size() + max_rows - 1) / max_rows));
	const uint16 page = std::min<uint16>(p.page, pages);
	if (!page)
		return;
	const size_t start = (page - 1) * max_rows;
	const uint16 count = static_cast<uint16>(std::min<size_t>(max_rows, indices.size() - start));

	const int32 fd = sd->fd, length = header_size + row_size * count;
	WFIFOHEAD(fd, length);
	auto* out = static_cast<uint8*>(WFIFOP(fd, 0));
	encode_header(out, p.auction_id, status, page, pages, count, static_cast<uint32>(time(nullptr)));
	put32(out + 4, inventory_magic);

	sd->auction.inventory_request = p.auction_id;
	for (int i = 0; i < max_rows; ++i)
		sd->auction.inventory_indices[i] = -1;
	for (int i = 0; i < count; ++i) {
		const int index = indices[start + i];
		const auto& item = sd->inventory.u.items_inventory[index];
		sd->auction.inventory_indices[i] = index;
		sd->auction.inventory_items[i] = item;

		Row r{};
		auction_row_item(r, item);
		r.id = index + AUCTION_OFFER_BASE;
		r.buy = 1;  // rows need a buy-now price to be valid
		const auto id = item_db.find(item.nameid);
		r.type = id->type;
		copy_string(r.name, id->ename.c_str(), sizeof r.name);
		copy_string(r.seller, "Inventory", sizeof r.seller);
		encode_row(out + header_size + i * row_size, r);
	}

	WFIFOSET(fd, length);
}

void clif_Auction_live_rules(map_session_data* sd, uint32 request, uint16 capabilities) {
	using namespace auction_live;
	if (!sd || !request || sd->auction.rules_request != request)
		return;

	Rules r{};
	r.request = request;
	r.status = battle_config.feature_auction ? Ok : Disabled;
	// Economic capabilities remain off until their durable service is ready.
	r.capabilities = r.status == Ok ? (capabilities & all_capabilities) : 0;
	r.character_id = sd->status.char_id;
	r.fee_per_hour = battle_config.auction_feeperhour;
	r.maximum_price = battle_config.auction_maximumprice;
	r.minimum_hours = AUCTION_MIN_HOURS;
	r.maximum_hours = AUCTION_MAX_HOURS;
	r.maximum_sales = AUCTION_MAX_SALES;
	r.maximum_bids = AUCTION_MAX_BIDS;
	r.server_time = static_cast<uint32>(time(nullptr));

	const int32 fd = sd->fd;
	WFIFOHEAD(fd, rules_size);
	encode_rules(static_cast<uint8*>(WFIFOP(fd, 0)), r);
	WFIFOSET(fd, rules_size);
	sd->auction.rules_request = 0;
}

void clif_Auction_action_result(map_session_data* sd, uint32 request, uint32 id, uint16 action, uint16 status) {
	using namespace auction_live;
	if (!sd || !request || request != sd->auction.action_request || id != sd->auction.action_id ||
	    action != sd->auction.action_kind || status > LimitReached)
		return;

	const int32 fd = sd->fd;
	WFIFOHEAD(fd, action_size);
	encode_action(static_cast<uint8*>(WFIFOP(fd, 0)), request, id, action, status);
	WFIFOSET(fd, action_size);
	sd->auction.action_result = status;
}

// Builds a registration from the client payload:
//   +4 starting bid  +8 buy now  +12 hours  +16 inventory offer token  +20 zero
// The item must come from the last inventory offer and be unchanged since.
static uint16 auction_prepare_listing(map_session_data* sd, uint32 id, const uint8* data, s_auction_request& r, int& index) {
	using namespace auction_live;
	const uint32 price = get32(data + 4), buy = get32(data + 8), hours = get32(data + 12), offer = get32(data + 16);
	if (get32(data + 20) || !price || price >= buy || buy > r.maximum_price || hours < AUCTION_MIN_HOURS || hours > AUCTION_MAX_HOURS)
		return Malformed;
	if (offer != sd->auction.inventory_request || id < AUCTION_OFFER_BASE || id >= MAX_INVENTORY + AUCTION_OFFER_BASE)
		return ItemChanged;

	index = static_cast<int>(id - AUCTION_OFFER_BASE);
	int offered = -1;
	for (int i = 0; i < max_rows; ++i)
		if (sd->auction.inventory_indices[i] == index)
			offered = i;
	const item& current = sd->inventory.u.items_inventory[index];
	if (offered < 0 || !auction_eligible_item(sd, index) || memcmp(&current, &sd->auction.inventory_items[offered], sizeof(item)))
		return ItemChanged;

	const uint64 fee = static_cast<uint64>(hours) * r.fee_per_hour;
	if (fee > MAX_ZENY)
		return Malformed;
	r.charge = static_cast<uint32>(fee);

	auction_data& a = r.listing;
	a.seller_id = sd->status.char_id;
	safestrncpy(a.seller_name, sd->status.name, NAME_LENGTH);
	a.item = current;
	a.item.amount = 1;
	const auto item_data = item_db.find(a.item.nameid);
	a.type = item_data->type;
	safestrncpy(a.item_name, item_data->ename.c_str(), sizeof a.item_name);
	a.price = price;
	a.buynow = buy;
	a.hours = static_cast<uint16>(hours);
	return Done;
}

// Builds a bid or buy-now from the client payload:
//   +4 amount to pay  +8 bid shown  +12 leading buyer shown  +16 zero  +20 zero
static uint16 auction_prepare_purchase(const uint8* data, s_auction_request& r) {
	using namespace auction_live;
	r.charge = get32(data + 4);
	r.expected_price = get32(data + 8);
	r.expected_buyer = get32(data + 12);
	if (!r.charge || r.charge > r.maximum_price || get32(data + 16) || get32(data + 20))
		return Malformed;
	return Done;
}

// Economic actions. The client payload (24 bytes in the query text) starts with
// the auction or offer id; the rest depends on the action.
static void clif_Auction_live_action(map_session_data* sd, const PACKET_CZ_AUCTION_ITEM_SEARCH& p, uint16 action) {
	using namespace auction_live;
	const auto* data = reinterpret_cast<const u8*>(p.text);
	const uint32 id = get32(data);
	if (!id || p.page != 1 || p.auction_id < sd->auction.action_request)
		return;

	// The client resends an action with the same request token until it gets
	// an answer. A replay must repeat the original request exactly.
	const bool replay = p.auction_id == sd->auction.action_request;
	if (replay && (id != sd->auction.action_id || action != sd->auction.action_kind || memcmp(data, sd->auction.action_payload, 24)))
		return;
	if (sd->auction_purchase && !replay)
		return;
	if (gettick() < sd->auction.action_next_request)
		return;
	sd->auction.action_next_request = gettick() + AUCTION_ACTION_INTERVAL;
	if (replay && (sd->auction.action_result != Retry || sd->auction_purchase)) {
		clif_Auction_action_result(sd, p.auction_id, id, action, sd->auction.action_result);
		return;
	}

	sd->auction.action_request = p.auction_id;
	sd->auction.action_id = id;
	sd->auction.action_kind = action;
	memcpy(sd->auction.action_payload, data, 24);
	sd->auction.action_result = Retry;
	auto reject = [&](uint16 status) { clif_Auction_action_result(sd, p.auction_id, id, action, status); };

	if (!battle_config.feature_auction || !pc_can_give_items(sd)) {
		reject(Denied);
		return;
	}

	// Cancel / close: +4 bid shown  +8 buyer shown, rest zero. Settled by the char-server.
	if (action == CancelSale || action == CloseSale) {
		for (int i = 12; i < 24; ++i) {
			if (data[i]) {
				reject(Malformed);
				return;
			}
		}
		if (!intif_Auction_action(sd->status.char_id, p.auction_id, id, get32(data + 4), get32(data + 8), action))
			reject(Retry);
		return;
	}

	s_auction_request r{};
	r.client_request = p.auction_id;
	r.target = id;
	r.action = action;
	r.fee_per_hour = battle_config.auction_feeperhour;
	r.maximum_price = battle_config.auction_maximumprice;

	int index = -1;
	uint16 status = Malformed;
	if (action == RegisterSale)
		status = auction_prepare_listing(sd, id, data, r, index);
	else if (action == BidSale || action == BuySale)
		status = auction_prepare_purchase(data, r);
	if (status != Done) {
		reject(status);
		return;
	}

	if (r.charge > static_cast<uint32>(sd->status.zeny)) {
		reject(InsufficientFunds);
		return;
	}
	if (!auction_purchase(*sd, r, index))
		reject(Retry);
}

void clif_Auction_live_query(map_session_data* sd, const PACKET_CZ_AUCTION_ITEM_SEARCH& p) {
	using namespace auction_live;
	const uint16 type = p.type & 0x7fff;
	if (!p.auction_id)
		return;

	if (type == QueryRules && p.page == 1) {
		sd->auction.rules_request = p.auction_id;
		// Without the char-server only the catalog is offered.
		if (!intif_Auction_action(sd->status.char_id, p.auction_id, 0, 0, 0, ReadRules))
			clif_Auction_live_rules(sd, p.auction_id, Catalog);
		return;
	}
	if (type >= CancelSale && type <= BuySale) {
		clif_Auction_live_action(sd, p, type);
		return;
	}
	if (type == QueryInventory) {
		clif_Auction_inventory(sd, p);
		return;
	}

	// Catalog listing, answered by the char-server.
	sd->auction.live_request = p.auction_id;
	uint16 status = Ok;
	if (!battle_config.feature_auction)
		status = Disabled;
	else if (type > QueryCostume || !p.page)
		status = Invalid;
	else if (gettick() < sd->auction.live_next_request)
		status = Unavailable;
	if (status) {
		clif_Auction_live_result(sd, p.auction_id, 1, 1, 0, nullptr, status);
		return;
	}

	sd->auction.live_next_request = gettick() + AUCTION_QUERY_INTERVAL;
	char text[NAME_LENGTH];
	safestrncpy(text, p.text, sizeof(text));
	if (!intif_Auction_requestlist_live(sd->status.char_id, p.auction_id, type, p.page, text))
		clif_Auction_live_result(sd, p.auction_id, 1, 1, 0, nullptr, Unavailable);
}

// ---------------------------------------------------------------------------
// Inter-server catalog and seller actions (0x3058/0x3059 -> 0x3858/0x3859)
// ---------------------------------------------------------------------------

int32 intif_Auction_requestlist_live(uint32 char_id, uint32 request, uint16 type, uint16 page, const char* text) {
	using namespace auction_live;
	if (CheckForCharServer())
		return 0;

	// The char-server does not know item categories, so send the matching ids.
	std::vector<uint32> items;
	if (is_category_query(type)) {
		// Query types 0-3 are UI filters 1-4; Costume (8) is filter 5.
		const unsigned filter = type == QueryCostume ? 5 : type + 1;
		for (const auto& pair : item_db) {
			const auto& item = *pair.second;
			if (matches_category(filter, item.type, is_costume(item.equip, item.name)))
				items.push_back(item.nameid);
		}
	}
	if (items.size() > (65535 - AUCTION_QUERY_HEADER) / 4)
		return 0;  // Never silently truncate category IDs.
	std::sort(items.begin(), items.end());

	const int32 length = AUCTION_QUERY_HEADER + static_cast<int32>(items.size()) * 4;
	WFIFOHEAD(inter_fd, length);
	WFIFOW(inter_fd, 0) = AUCTION_LIVE_QUERY;
	WFIFOW(inter_fd, 2) = length;
	WFIFOL(inter_fd, 4) = char_id;
	WFIFOL(inter_fd, 8) = request;
	WFIFOW(inter_fd, 12) = type;
	WFIFOW(inter_fd, 14) = page;
	safestrncpy(WFIFOCP(inter_fd, 16), text, NAME_LENGTH);
	WFIFOW(inter_fd, 40) = static_cast<uint16>(items.size());
	for (size_t i = 0; i < items.size(); ++i)
		WFIFOL(inter_fd, AUCTION_QUERY_HEADER + 4 * i) = items[i];
	WFIFOSET(inter_fd, length);
	return 1;
}

void intif_parse_Auction_live_result(int32 fd) {
	const int32 length = RFIFOW(fd, 2);
	if (length < AUCTION_RESULT_HEADER)
		return;
	const uint16 count = RFIFOW(fd, 12);
	if (count > auction_live::max_rows || length != AUCTION_RESULT_HEADER + count * sizeof(auction_data))
		return;
	auto* sd = map_charid2sd(RFIFOL(fd, 4));
	if (!sd)
		return;
	clif_Auction_live_result(sd, RFIFOL(fd, 8), RFIFOW(fd, 16), RFIFOW(fd, 14), count, RFIFOP(fd, AUCTION_RESULT_HEADER), RFIFOW(fd, 18));
}

int32 intif_Auction_action(uint32 char_id, uint32 request, uint32 id, uint32 price, uint32 buyer, uint16 action) {
	if (CheckForCharServer())
		return 0;
	WFIFOHEAD(inter_fd, AUCTION_ACTION_LENGTH);
	WFIFOW(inter_fd, 0) = AUCTION_LIVE_ACTION;
	WFIFOW(inter_fd, 2) = AUCTION_ACTION_LENGTH;
	WFIFOL(inter_fd, 4) = char_id;
	WFIFOL(inter_fd, 8) = request;
	WFIFOL(inter_fd, 12) = id;
	WFIFOL(inter_fd, 16) = price;
	WFIFOL(inter_fd, 20) = buyer;
	WFIFOW(inter_fd, 24) = action;
	WFIFOW(inter_fd, 26) = 0;
	WFIFOSET(inter_fd, AUCTION_ACTION_LENGTH);
	return 1;
}

void intif_parse_Auction_action_result(int32 fd) {
	if (RFIFOW(fd, 2) != AUCTION_ACTION_RESULT_LENGTH)
		return;
	auto* sd = map_charid2sd(RFIFOL(fd, 4));
	if (!sd)
		return;
	// For ReadRules the result field carries the capabilities.
	if (RFIFOW(fd, 16) == auction_live::ReadRules)
		clif_Auction_live_rules(sd, RFIFOL(fd, 8), RFIFOW(fd, 18));
	else
		clif_Auction_action_result(sd, RFIFOL(fd, 8), RFIFOL(fd, 12), RFIFOW(fd, 16), RFIFOW(fd, 18));
}

// ---------------------------------------------------------------------------
// Durable purchases with reserved resources (0x2B32 -> 0x2B33)
// ---------------------------------------------------------------------------
// Resources stay owned by the player until the durable char-server receipt.
// No inventory removal or zeny debit occurs before the durable char receipt.

struct s_auction_pending {
	s_auction_request request{};
	int index = -1;                    // reserved inventory slot, or -1
	int save_flags = CSAVE_INVENTORY;  // saves deferred until the receipt
	item original_item{};
	t_tick last_send = 0;
};

static std::unordered_map<uint32, std::weak_ptr<s_auction_pending>> auction_pending;

uint32 auction_reserved_zeny(const map_session_data& sd) {
	return sd.auction_purchase ? sd.auction_purchase->request.charge : 0;
}

int32 auction_reserved_item(const map_session_data& sd, int index) {
	return sd.auction_purchase && sd.auction_purchase->index == index ? 1 : 0;
}

// Online or still in the char-server's session list (e.g. while logging out).
static map_session_data* auction_find_player(uint32 account_id, uint32 char_id) {
	auto* sd = map_charid2sd(char_id);
	if (!sd) {
		auto* node = chrif_search(account_id);
		if (node && node->char_id == char_id)
			sd = node->sd;
	}
	return sd;
}

static void auction_purchase_send(map_session_data& sd) {
	if (!chrif_isconnected() || !sd.auction_purchase)
		return;
	auto& pending = *sd.auction_purchase;
	if (pending.last_send && DIFF_TICK(gettick(), pending.last_send) < AUCTION_RESEND_INTERVAL)
		return;
	pending.last_send = gettick();
	chrif_char_online(&sd);
	WFIFOHEAD(char_fd, sizeof pending.request);
	memcpy(WFIFOP(char_fd, 0), &pending.request, sizeof pending.request);
	WFIFOSET(char_fd, sizeof pending.request);
}

void auction_retry_all() {
	for (auto it = auction_pending.begin(); it != auction_pending.end();) {
		auto pending = it->second.lock();
		if (!pending) {
			it = auction_pending.erase(it);
			continue;
		}
		auto* sd = auction_find_player(pending->request.account_id, pending->request.char_id);
		if (sd && sd->auction_purchase == pending)
			auction_purchase_send(*sd);
		++it;
	}
}

bool auction_purchase(map_session_data& sd, s_auction_request request, int index) {
	if (!chrif_isconnected() || sd.auction_purchase || request.charge > static_cast<uint32>(sd.status.zeny))
		return false;

	auto pending = std::make_shared<s_auction_pending>();
	request.header = AUCTION_SERVICE_REQUEST;
	request.length = sizeof request;
	// Random operation id: 32 hex digits.
	std::random_device entropy;
	for (int i = 0; i < 4; ++i)
		snprintf(request.operation + i * 8, 9, "%08x", static_cast<uint32>(entropy()));
	request.account_id = sd.status.account_id;
	request.char_id = sd.status.char_id;
	request.zeny_before = sd.status.zeny;
	memcpy(request.inventory, sd.inventory.u.items_inventory, sizeof request.inventory);

	// A listing reserves one unit of the item; the char-server saves the
	// inventory without it.
	if (index >= 0) {
		if (index >= MAX_INVENTORY || request.inventory[index].amount < 1)
			return false;
		pending->original_item = request.inventory[index];
		pending->index = index;
		if (--request.inventory[index].amount == 0)
			request.inventory[index] = {};
	}

	pending->request = request;
	chrif_save(&sd, CSAVE_INVENTORY);
	sd.auction_purchase = pending;
	auction_pending[sd.status.char_id] = pending;
	auction_purchase_send(sd);
	return true;
}

void auction_defer_save(map_session_data& sd, int flags) {
	sd.auction_purchase->save_flags |= flags;
	auction_purchase_send(sd);
}

// The reserved zeny and item must still be there when the receipt arrives.
static bool auction_reservation_intact(const map_session_data& sd, const s_auction_pending& pending) {
	bool intact = static_cast<uint32>(sd.status.zeny) >= pending.request.charge;
	if (pending.index >= 0) {
		item current_item = sd.inventory.u.items_inventory[pending.index];
		intact = intact && current_item.amount >= 1 && sd.inventory_data[pending.index];
		current_item.amount = pending.original_item.amount;
		intact = intact && !memcmp(&current_item, &pending.original_item, sizeof current_item);
	}
	return intact;
}

void auction_purchase_reply(int fd) {
	using namespace auction_live;
	s_auction_reply reply{};
	memcpy(&reply, RFIFOP(fd, 0), sizeof reply);
	auto* sd = auction_find_player(reply.account_id, reply.char_id);
	if (!sd || sd->status.account_id != reply.account_id || !sd->auction_purchase)
		return;

	auto pending = sd->auction_purchase;
	const auto& r = pending->request;
	if (memcmp(reply.operation, r.operation, sizeof reply.operation) || reply.client_request != r.client_request ||
	    reply.action != r.action || reply.target != r.target)
		return;
	// Retry keeps the reservation; the request is resent.
	if (reply.result == Retry || reply.result > LimitReached)
		return;
	if (reply.result == Done && !auction_reservation_intact(*sd, *pending)) {
		ShowError("Auction reservation invariant failed char=%u operation=%s; reconciliation required.\n", reply.char_id, reply.operation);
		return;
	}

	sd->auction_purchase.reset();
	auction_pending.erase(reply.char_id);
	if (reply.result == Done) {
		if (r.charge)
			pc_payzeny(sd, r.charge, LOG_TYPE_AUCTION);
		if (pending->index >= 0)
			pc_delitem(sd, pending->index, 1, 0, 0, LOG_TYPE_AUCTION);
	}
	sd->auction.action_result = reply.result;
	if (sd->fd > 0)
		clif_Auction_action_result(sd, reply.client_request, reply.target, reply.action, reply.result);
	chrif_save(sd, pending->save_flags);
}
