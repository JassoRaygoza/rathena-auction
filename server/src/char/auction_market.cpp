// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

// Modern Auction market (char-server): live catalog, durable trading and
// transactional settlement. The classic Auction protocol stays in int_auction.

#include "auction_market.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <vector>

#include <common/auction_service.hpp>
#include <common/mmo.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>
#include <common/utilities.hpp>

#include "char.hpp"
#include "char_mapif.hpp"
#include "inter.hpp"
#include "int_auction.hpp"
#include "int_mail.hpp"

using namespace rathena;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// conf/msg_conf/char_msg.conf
enum e_auction_msg : int32 {
	AUCTION_MSG_SENDER = 200,     // Auction Manager
	AUCTION_MSG_TITLE = 201,      // Auction
	AUCTION_MSG_WON = 202,        // Thanks, you won the auction!
	AUCTION_MSG_PAYMENT = 203,    // Payment for your auction!
	AUCTION_MSG_NO_BUYER = 204,   // No buyers have been found for your auction.
	AUCTION_MSG_CANCELED = 205,   // Auction canceled.
	AUCTION_MSG_CLOSED = 206,     // Auction closed.
	AUCTION_MSG_WINNER = 207,     // Auction winner.
};

static const char* const AUCTION_TRADE_MAIL_BODY = "Auction transaction completed. Your items or zeny are attached.";

// A transaction must not silently reconnect between its statements. This
// restores reconnection when the transaction scope ends.
struct AuctionReconnectGuard {
	~AuctionReconnectGuard() { Sql_SetReconnect(sql_handle, true); }
};

// Reads the single COUNT(*) of the last query and compares it.
static bool auction_count_result_is(int32 expected) {
	char* data = nullptr;
	const bool matches = SQL_SUCCESS == Sql_NextRow(sql_handle) &&
	                     SQL_SUCCESS == Sql_GetData(sql_handle, 0, &data, nullptr) && data && atoi(data) == expected;
	Sql_FreeResult(sql_handle);
	return matches;
}

// Settlement needs the auction and mail tables, plus the receipt table, on InnoDB.
static bool auction_settlement_ready() {
	if (SQL_ERROR == Sql_Query(sql_handle,
		"SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND ENGINE='InnoDB' "
		"AND TABLE_NAME IN ('%s','%s','%s','auction_settlement')",
		schema_config.auction_db, schema_config.mail_db, schema_config.mail_attachment_db))
		return false;
	return auction_count_result_is(4);
}

// Trading also writes the character's zeny and inventory and journals operations.
static bool auction_trading_ready() {
	if (SQL_ERROR == Sql_Query(sql_handle,
		"SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND ENGINE='InnoDB' "
		"AND TABLE_NAME IN ('%s','%s','%s','%s','%s','auction_settlement','auction_operation')",
		schema_config.char_db, schema_config.inventory_db, schema_config.auction_db, schema_config.mail_db,
		schema_config.mail_attachment_db))
		return false;
	return auction_count_result_is(7);
}

// Delivery mails saved inside the current transaction. They are announced to
// the map-server only after COMMIT.
struct AuctionMailBatch {
	mail_message mails[3]{};
	int32 count = 0;

	bool add(int32 recipient, const char* name, const char* body, int32 zeny, const item* attachment) {
		if (count >= 3)
			return false;
		mail_message& m = mails[count++];
		m.dest_id = recipient;
		safestrncpy(m.dest_name, name, NAME_LENGTH);
		safestrncpy(m.send_name, msg_txt(AUCTION_MSG_SENDER), NAME_LENGTH);
		safestrncpy(m.title, msg_txt(AUCTION_MSG_TITLE), MAIL_TITLE_LENGTH);
		safestrncpy(m.body, body, MAIL_BODY_LENGTH);
		m.status = MAIL_NEW;
		m.type = MAIL_INBOX_NORMAL;
		m.timestamp = time(nullptr);
		m.zeny = zeny;
		if (attachment)
			m.item[0] = *attachment;
		return mail_savemessage(&m, false) != 0;
	}

	void announce() {
		for (int32 i = 0; i < count; ++i)
			mapif_Mail_new(&mails[i]);
	}
};

// Writes the settlement receipt and removes the auction.
static bool auction_finish_settlement(uint32 auction_id, int32 seller_id, int32 buyer_id, AuctionSettlement reason,
                                      int32 item_mail_id, int32 zeny_mail_id) {
	if (SQL_ERROR == Sql_Query(sql_handle,
		"INSERT INTO `auction_settlement` (`auction_id`,`seller_id`,`buyer_id`,`reason`,`item_mail_id`,`zeny_mail_id`) VALUES (%u,%d,%d,%u,%d,%d)",
		auction_id, seller_id, buyer_id, static_cast<unsigned>(reason), item_mail_id, zeny_mail_id))
		return false;
	return SQL_ERROR != Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `auction_id`=%u", schema_config.auction_db, auction_id);
}

// ---------------------------------------------------------------------------
// Settlement
// ---------------------------------------------------------------------------
// Auction deletion and all delivery mails form one transaction. The durable
// auction_id receipt resolves retries, including a lost COMMIT reply. No map
// notification or cache/timer removal may happen until the outcome is known.
// An ambiguous outcome must not be followed by a bid against the stale cache.

std::unordered_set<uint32> auction_settling;

// Locks the auction row and checks that it still matches the cached copy.
static bool auction_row_matches(const auction_data& a) {
	if (SQL_ERROR == Sql_Query(sql_handle,
		"SELECT `seller_id`,`buyer_id`,`price`,`buynow`,`timestamp` FROM `%s` WHERE `auction_id`=%u FOR UPDATE",
		schema_config.auction_db, a.auction_id))
		return false;

	const uint64 expected[] = {
		static_cast<uint64>(a.seller_id),
		static_cast<uint64>(a.buyer_id),
		static_cast<uint64>(a.price),
		static_cast<uint64>(a.buynow),
		static_cast<uint64>(a.timestamp),
	};
	bool matches = SQL_SUCCESS == Sql_NextRow(sql_handle);
	char* data = nullptr;
	for (int32 i = 0; matches && i < 5; ++i) {
		if (SQL_SUCCESS != Sql_GetData(sql_handle, i, &data, nullptr) || !data || strtoull(data, nullptr, 10) != expected[i])
			matches = false;
	}
	Sql_FreeResult(sql_handle);
	return matches;
}

bool auction_settle(const auction_data& a, AuctionSettlement reason) {
	if (!a.auction_id || a.seller_id <= 0 || a.price < 0 || a.buyer_id < 0 || !a.item.nameid || a.item.amount != 1)
		return false;
	if (reason == AuctionSettlement::Cancelled && a.buyer_id)
		return false;
	if (reason == AuctionSettlement::Closed && !a.buyer_id)
		return false;

	auction_settling.insert(a.auction_id);
	if (!auction_settlement_ready())
		return false;
	if (SQL_ERROR == Sql_SetReconnect(sql_handle, false))
		return false;
	AuctionReconnectGuard reconnect_guard;
	if (SQL_ERROR == Sql_QueryStr(sql_handle, "START TRANSACTION"))
		return false;
	auto rollback = []() {
		Sql_QueryStr(sql_handle, "ROLLBACK");
		return false;
	};

	// The cache can lag SQL after an ambiguous commit. A receipt is authoritative
	// even if a subsequent cancellation/expiry retry names a different reason.
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id` FROM `auction_settlement` WHERE `auction_id`=%u FOR UPDATE", a.auction_id))
		return rollback();
	const bool settled = SQL_SUCCESS == Sql_NextRow(sql_handle);
	Sql_FreeResult(sql_handle);
	if (settled) {
		Sql_QueryStr(sql_handle, "ROLLBACK");
		return true;
	}

	if (!auction_row_matches(a))
		return rollback();

	// With a buyer: item to the buyer, payment to the seller. Otherwise the item
	// returns to the seller.
	AuctionMailBatch mail;
	if (a.buyer_id) {
		const bool closed = reason == AuctionSettlement::Closed;
		if (!mail.add(a.buyer_id, a.buyer_name, msg_txt(closed ? AUCTION_MSG_WINNER : AUCTION_MSG_WON), 0, &a.item) ||
		    !mail.add(a.seller_id, a.seller_name, msg_txt(closed ? AUCTION_MSG_CLOSED : AUCTION_MSG_PAYMENT), a.price, nullptr))
			return rollback();
	} else {
		const bool canceled = reason == AuctionSettlement::Cancelled;
		if (!mail.add(a.seller_id, a.seller_name, msg_txt(canceled ? AUCTION_MSG_CANCELED : AUCTION_MSG_NO_BUYER), 0, &a.item))
			return rollback();
	}

	const int32 zeny_mail_id = mail.count == 2 ? mail.mails[1].id : 0;
	if (!auction_finish_settlement(a.auction_id, a.seller_id, a.buyer_id, reason, mail.mails[0].id, zeny_mail_id))
		return rollback();
	if (SQL_ERROR == Sql_QueryStr(sql_handle, "COMMIT"))
		return rollback();

	mail.announce();
	return true;
}

void auction_forget_settled(const std::shared_ptr<auction_data>& a) {
	if (a->auction_end_timer != INVALID_TIMER)
		delete_timer(a->auction_end_timer, auction_end_timer);
	auction_db.erase(a->auction_id);
	auction_settling.erase(a->auction_id);
}

// ---------------------------------------------------------------------------
// Live catalog (0x3058 -> 0x3858)
// ---------------------------------------------------------------------------

static bool auction_live_matches(const auction_data& a, uint16 type, int32 char_id, const char* text,
                                 uint32 maximum_price, const std::vector<uint32>& items, time_t now) {
	using namespace auction_live;
	if (a.timestamp <= now)
		return false;
	if (is_category_query(type) && !std::binary_search(items.begin(), items.end(), a.item.nameid))
		return false;
	if (type == QuerySales && a.seller_id != char_id)
		return false;
	if (type == QueryBids && a.buyer_id != char_id)
		return false;
	if (type == QueryPrice)
		return a.price >= 0 && static_cast<uint32>(a.price) <= maximum_price;
	return contains_name(a.item_name, text);
}

void mapif_parse_Auction_live_query(int32 fd) {
	using namespace auction_live;
	const uint16 length = RFIFOW(fd, 2);
	if (length < AUCTION_QUERY_HEADER)
		return;
	const uint16 type = RFIFOW(fd, 12), requested = RFIFOW(fd, 14), item_count = RFIFOW(fd, 40);
	if (type > QueryCostume || !requested || length != AUCTION_QUERY_HEADER + item_count * 4)
		return;
	const int32 char_id = static_cast<int32>(RFIFOL(fd, 4));
	const uint32 request = RFIFOL(fd, 8);
	if (char_id <= 0 || !request)
		return;

	char text[NAME_LENGTH];
	memcpy(text, RFIFOP(fd, 16), NAME_LENGTH);
	text[NAME_LENGTH - 1] = 0;
	u32 maximum_price = 0;
	if (type == QueryPrice && !parse_price(text, maximum_price))
		return;

	// Item ids of the requested category, resolved by the map-server.
	std::vector<uint32> items;
	for (uint16 i = 0; i < item_count; ++i)
		items.push_back(RFIFOL(fd, AUCTION_QUERY_HEADER + 4 * i));
	std::sort(items.begin(), items.end());

	const time_t now = time(nullptr);
	std::vector<std::shared_ptr<auction_data>> found;
	for (const auto& pair : auction_db) {
		if (auction_live_matches(*pair.second, type, char_id, text, maximum_price, items, now))
			found.push_back(pair.second);
	}

	// Newest first, max_rows per page.
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a->auction_id > b->auction_id; });
	const size_t pages = std::min<size_t>(65535, std::max<size_t>(1, (found.size() + max_rows - 1) / max_rows));
	const uint16 page = static_cast<uint16>(std::min<size_t>(requested, pages));
	const size_t start = (page - 1) * max_rows;
	const uint16 count = static_cast<uint16>(std::min<size_t>(max_rows, found.size() - start));

	static_assert(AUCTION_RESULT_HEADER + max_rows * sizeof(auction_data) <= 65535, "Auction response must fit its length field");
	const uint16 out_length = static_cast<uint16>(AUCTION_RESULT_HEADER + count * sizeof(auction_data));
	WFIFOHEAD(fd, out_length);
	WFIFOW(fd, 0) = AUCTION_LIVE_RESULT;
	WFIFOW(fd, 2) = out_length;
	WFIFOL(fd, 4) = char_id;
	WFIFOL(fd, 8) = request;
	WFIFOW(fd, 12) = count;
	WFIFOW(fd, 14) = static_cast<uint16>(pages);
	WFIFOW(fd, 16) = page;
	WFIFOW(fd, 18) = 0;
	for (uint16 i = 0; i < count; ++i)
		memcpy(WFIFOP(fd, AUCTION_RESULT_HEADER + i * sizeof(auction_data)), found[start + i].get(), sizeof(auction_data));
	WFIFOSET(fd, out_length);
}

// ---------------------------------------------------------------------------
// Durable trading (0x2B32 -> 0x2B33)
// ---------------------------------------------------------------------------
// One durable transaction spans character balance/inventory, Auction, refunds,
// deliveries and the operation receipt. The map keeps resources reserved until
// it receives this receipt. A lost COMMIT reply is resolved by replaying the
// same random operation ID and hash, never by guessing or refunding in memory.

// The `auction_operation` row of a request, locked for this transaction.
struct AuctionJournalRow {
	uint32 account_id = 0, char_id = 0;
	uint16 result = auction_live::Retry;
	uint32 auction_id = 0;
	uint8 same_request = 0;
};

// Records the operation (if new) and locks its row. Fails when the row cannot
// be read or belongs to another character or a different request.
static bool auction_journal_open(SqlStmt& journal, const s_auction_request& r, AuctionJournalRow& row) {
	auto* request = const_cast<s_auction_request*>(&r);
	if (SQL_ERROR == journal.Prepare("INSERT IGNORE INTO `auction_operation` (`operation_id`,`account_id`,`char_id`,`request_hash`,`result`,`auction_id`) VALUES (?,?,?,UNHEX(SHA2(?,256)),4,0)") ||
	    SQL_ERROR == journal.BindParam(0, SQLDT_STRING, request->operation, 32) ||
	    SQL_ERROR == journal.BindParam(1, SQLDT_UINT32, &request->account_id, sizeof(r.account_id)) ||
	    SQL_ERROR == journal.BindParam(2, SQLDT_UINT32, &request->char_id, sizeof(r.char_id)) ||
	    SQL_ERROR == journal.BindParam(3, SQLDT_BLOB, request, sizeof(r)) ||
	    SQL_ERROR == journal.Execute())
		return false;

	if (SQL_ERROR == journal.Prepare("SELECT `account_id`,`char_id`,`result`,`auction_id`,`request_hash`=UNHEX(SHA2(?,256)) FROM `auction_operation` WHERE `operation_id`=? FOR UPDATE") ||
	    SQL_ERROR == journal.BindParam(0, SQLDT_BLOB, request, sizeof(r)) ||
	    SQL_ERROR == journal.BindParam(1, SQLDT_STRING, request->operation, 32) ||
	    SQL_ERROR == journal.Execute() ||
	    SQL_ERROR == journal.BindColumn(0, SQLDT_UINT32, &row.account_id) ||
	    SQL_ERROR == journal.BindColumn(1, SQLDT_UINT32, &row.char_id) ||
	    SQL_ERROR == journal.BindColumn(2, SQLDT_UINT16, &row.result) ||
	    SQL_ERROR == journal.BindColumn(3, SQLDT_UINT32, &row.auction_id) ||
	    SQL_ERROR == journal.BindColumn(4, SQLDT_UINT8, &row.same_request) ||
	    SQL_SUCCESS != journal.NextRow())
		return false;

	return row.account_id == r.account_id && row.char_id == r.char_id && row.same_request;
}

// Stores the final result in the operation row.
static bool auction_journal_close(SqlStmt& journal, const s_auction_request& r, uint16& result, uint32& auction_id) {
	return SQL_ERROR != journal.Prepare("UPDATE `auction_operation` SET `result`=?,`auction_id`=? WHERE `operation_id`=?") &&
	       SQL_ERROR != journal.BindParam(0, SQLDT_UINT16, &result, sizeof result) &&
	       SQL_ERROR != journal.BindParam(1, SQLDT_UINT32, &auction_id, sizeof auction_id) &&
	       SQL_ERROR != journal.BindParam(2, SQLDT_STRING, const_cast<char*>(r.operation), 32) &&
	       SQL_ERROR != journal.Execute();
}

// Locks the character row and reads its saved zeny and name.
static bool auction_lock_character(SqlStmt& person, const s_auction_request& r, uint32& zeny, char* name, size_t name_size) {
	auto* request = const_cast<s_auction_request*>(&r);
	return SQL_ERROR != person.Prepare("SELECT `zeny`,`name` FROM `%s` WHERE `account_id`=? AND `char_id`=? FOR UPDATE", schema_config.char_db) &&
	       SQL_ERROR != person.BindParam(0, SQLDT_UINT32, &request->account_id, sizeof(r.account_id)) &&
	       SQL_ERROR != person.BindParam(1, SQLDT_UINT32, &request->char_id, sizeof(r.char_id)) &&
	       SQL_ERROR != person.Execute() &&
	       SQL_ERROR != person.BindColumn(0, SQLDT_UINT32, &zeny) &&
	       SQL_ERROR != person.BindColumn(1, SQLDT_STRING, name, name_size);
}

// Validates a new listing and fills in its end time and seller name.
static uint16 auction_check_listing(const s_auction_request& r, const char* seller_name, auction_data& next) {
	using namespace auction_live;
	next = r.listing;
	if (next.seller_id != static_cast<int32>(r.char_id) || next.buyer_id || next.auction_id)
		return Malformed;
	if (next.hours < AUCTION_MIN_HOURS || next.hours > AUCTION_MAX_HOURS)
		return Malformed;
	if (next.price <= 0 || next.price >= next.buynow || static_cast<uint32>(next.buynow) > r.maximum_price ||
	    r.maximum_price > MAX_ZENY)
		return Malformed;
	if (static_cast<uint64>(next.hours) * r.fee_per_hour != r.charge)
		return Malformed;
	const item& listed = next.item;
	if (!listed.nameid || listed.amount != 1 || !listed.identify || listed.equip || listed.equipSwitch || listed.expire_time)
		return Malformed;
	if (auction_count(r.char_id, false) >= AUCTION_MAX_SALES)
		return LimitReached;

	next.timestamp = time(nullptr) + next.hours * 3600;
	safestrncpy(next.seller_name, seller_name, NAME_LENGTH);
	return Done;
}

// Validates a bid or buy-now against the auction the player was shown.
// Returns Retry while the auction's settlement outcome is unknown.
static uint16 auction_check_purchase(const s_auction_request& r, auction_data& next) {
	using namespace auction_live;
	auto found = util::umap_find(auction_db, r.target);
	if (!found)
		return Missing;
	if (auction_settling.count(r.target))
		return Retry;
	if (found->seller_id == static_cast<int32>(r.char_id))
		return Denied;
	if (found->timestamp <= time(nullptr) || static_cast<uint32>(found->price) != r.expected_price ||
	    static_cast<uint32>(found->buyer_id) != r.expected_buyer)
		return Changed;

	const uint32 bid = static_cast<uint32>(found->price), buynow = static_cast<uint32>(found->buynow);
	const bool valid_amount = r.action == BuySale ? r.charge == buynow : r.charge > bid && r.charge < buynow;
	if (!valid_amount)
		return Changed;
	if (r.action == BidSale && found->buyer_id != static_cast<int32>(r.char_id) && auction_count(r.char_id, true) >= AUCTION_MAX_BIDS)
		return LimitReached;

	next = *found;
	return Done;
}

// Writes the debit, the reserved inventory and the auction change.
static bool auction_apply_trade(const s_auction_request& r, auction_data& next, const char* character_name,
                                uint32& result_id, AuctionMailBatch& mail) {
	using namespace auction_live;
	// The saved snapshot and the debit are written together. Ordinary map
	// saves are deferred while this request is outstanding.
	if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `%s` SET `zeny`=%u WHERE `account_id`=%u AND `char_id`=%u",
		schema_config.char_db, r.zeny_before - r.charge, r.account_id, r.char_id))
		return false;
	if (char_memitemdata_to_sql(r.inventory, MAX_INVENTORY, r.char_id, TABLE_INVENTORY, 0) != 0)
		return false;

	if (r.action == RegisterSale) {
		result_id = auction_insert_sql(next);
		return result_id != 0;
	}

	result_id = r.target;
	// Refund the previous leading bid.
	if (next.buyer_id && !mail.add(next.buyer_id, next.buyer_name, AUCTION_TRADE_MAIL_BODY, next.price, nullptr))
		return false;
	next.buyer_id = r.char_id;
	next.price = r.charge;
	safestrncpy(next.buyer_name, character_name, NAME_LENGTH);

	if (r.action == BidSale)
		return auction_save(std::make_shared<auction_data>(next));

	// Buy now: item to the buyer, payment to the seller, and the auction ends.
	const int32 item_mail = mail.count;
	if (!mail.add(next.buyer_id, next.buyer_name, AUCTION_TRADE_MAIL_BODY, 0, &next.item) ||
	    !mail.add(next.seller_id, next.seller_name, AUCTION_TRADE_MAIL_BODY, next.buynow, nullptr))
		return false;
	return auction_finish_settlement(next.auction_id, next.seller_id, next.buyer_id, AuctionSettlement::Bought,
	                                 mail.mails[item_mail].id, mail.mails[item_mail + 1].id);
}

static uint16 auction_trade(const s_auction_request& r, uint32& result_id) {
	using namespace auction_live;
	if (!auction_trading_ready() || !inter_auctions_fromsql())
		return Retry;
	if (SQL_ERROR == Sql_SetReconnect(sql_handle, false))
		return Retry;
	AuctionReconnectGuard reconnect_guard;
	if (SQL_ERROR == Sql_QueryStr(sql_handle, "START TRANSACTION"))
		return Retry;
	auto retry = []() {
		Sql_QueryStr(sql_handle, "ROLLBACK");
		return static_cast<uint16>(Retry);
	};

	SqlStmt journal{ *sql_handle };
	AuctionJournalRow operation;
	const bool journal_open = auction_journal_open(journal, r, operation);
	result_id = operation.auction_id;
	if (!journal_open)
		return retry();

	// Replay of a finished operation: answer with the recorded result.
	if (operation.result != Retry) {
		Sql_QueryStr(sql_handle, "ROLLBACK");
		char_get_chardb().erase(r.char_id);
		return inter_auctions_fromsql() ? operation.result : static_cast<uint16>(Retry);
	}

	uint32 saved_zeny = 0;
	char character_name[NAME_LENGTH]{};
	SqlStmt person{ *sql_handle };
	if (!auction_lock_character(person, r, saved_zeny, character_name, sizeof character_name))
		return retry();

	uint16 result = Done;
	if (person.NumRows() != 1 || person.NextRow() != SQL_SUCCESS)
		result = Denied;
	else if (saved_zeny != r.zeny_before)
		result = Changed;
	else if (r.charge > saved_zeny || saved_zeny > MAX_ZENY)
		result = InsufficientFunds;

	auction_data next{};
	if (result == Done) {
		switch (r.action) {
		case RegisterSale:
			result = auction_check_listing(r, character_name, next);
			break;
		case BidSale:
		case BuySale:
			result = auction_check_purchase(r, next);
			if (result == Retry)
				return retry();
			break;
		default:
			result = Malformed;
			break;
		}
	}

	AuctionMailBatch mail;
	if (result == Done && !auction_apply_trade(r, next, character_name, result_id, mail))
		return retry();
	if (!auction_journal_close(journal, r, result, result_id))
		return retry();

	char_get_chardb().erase(r.char_id);
	if (SQL_ERROR == Sql_QueryStr(sql_handle, "COMMIT")) {
		// The outcome is unknown until the retry reads the receipt.
		if (r.action != RegisterSale)
			auction_settling.insert(r.target);
		return retry();
	}

	mail.announce();
	return inter_auctions_fromsql() ? result : static_cast<uint16>(Retry);
}

int32 inter_auction_parse_purchase(int32 fd) {
	if (RFIFOREST(fd) < 4)
		return 0;
	const uint16 length = RFIFOW(fd, 2);
	if (length != sizeof(s_auction_request)) {
		set_eof(fd);
		return 0;
	}
	if (RFIFOREST(fd) < length)
		return 0;

	s_auction_request request{};
	memcpy(&request, RFIFOP(fd, 0), sizeof request);
	RFIFOSKIP(fd, length);
	if (request.operation[32] || strspn(request.operation, "0123456789abcdef") != 32 || !request.client_request ||
	    !request.account_id || !request.char_id) {
		set_eof(fd);
		return 0;
	}

	s_auction_reply reply{};
	reply.header = AUCTION_SERVICE_REPLY;
	memcpy(reply.operation, request.operation, sizeof reply.operation);
	reply.account_id = request.account_id;
	reply.char_id = request.char_id;
	reply.client_request = request.client_request;
	reply.target = request.target;
	reply.action = request.action;
	reply.result = auction_trade(request, reply.auction_id);

	WFIFOHEAD(fd, sizeof reply);
	memcpy(WFIFOP(fd, 0), &reply, sizeof reply);
	WFIFOSET(fd, sizeof reply);
	return 1;
}

// ---------------------------------------------------------------------------
// Seller actions and rules (0x3059 -> 0x3859)
// ---------------------------------------------------------------------------
// Modern requests carry the exact bid/buyer the seller confirmed. A changed
// auction requires a new confirmation; a durable receipt makes retries safe.

static void mapif_Auction_live_action_result(int32 fd, uint32 char_id, uint32 request, uint32 id, uint16 action, uint16 result) {
	WFIFOHEAD(fd, AUCTION_ACTION_RESULT_LENGTH);
	WFIFOW(fd, 0) = AUCTION_ACTION_RESULT;
	WFIFOW(fd, 2) = AUCTION_ACTION_RESULT_LENGTH;
	WFIFOL(fd, 4) = char_id;
	WFIFOL(fd, 8) = request;
	WFIFOL(fd, 12) = id;
	WFIFOW(fd, 16) = action;
	WFIFOW(fd, 18) = result;
	WFIFOSET(fd, AUCTION_ACTION_RESULT_LENGTH);
}

// Capabilities follow the tables that are actually ready.
static uint16 auction_capabilities() {
	using namespace auction_live;
	uint16 capabilities = Catalog;
	if (auction_settlement_ready())
		capabilities |= Cancel | Close;
	if (auction_trading_ready())
		capabilities |= Register | Bid | BuyNow;
	return capabilities;
}

// Reads the settlement receipt of an auction, if any, and whether it belongs to the seller.
static bool auction_find_receipt(uint32 id, uint32 seller_id, bool& settled, bool& owner) {
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `seller_id` FROM `auction_settlement` WHERE `auction_id`=%u", id))
		return false;
	char* data = nullptr;
	settled = SQL_SUCCESS == Sql_NextRow(sql_handle);
	owner = settled && SQL_SUCCESS == Sql_GetData(sql_handle, 0, &data, nullptr) && data && strtoul(data, nullptr, 10) == seller_id;
	Sql_FreeResult(sql_handle);
	return true;
}

void mapif_parse_Auction_live_action(int32 fd) {
	using namespace auction_live;
	if (RFIFOW(fd, 2) != AUCTION_ACTION_LENGTH || RFIFOW(fd, 26))
		return;
	const uint32 character = RFIFOL(fd, 4), request = RFIFOL(fd, 8), id = RFIFOL(fd, 12);
	const uint32 expected_price = RFIFOL(fd, 16), expected_buyer = RFIFOL(fd, 20);
	const uint16 action = RFIFOW(fd, 24);
	if (!character || character > 0x7fffffffu || !request)
		return;
	auto reply = [&](uint16 result) { mapif_Auction_live_action_result(fd, character, request, id, action, result); };

	if (action == ReadRules) {
		reply(auction_capabilities());
		return;
	}
	if ((action != CancelSale && action != CloseSale) || !id) {
		reply(Malformed);
		return;
	}
	if (!auction_settlement_ready()) {
		reply(Retry);
		return;
	}

	// An earlier attempt may have settled the auction already.
	bool settled = false, owner = false;
	if (!auction_find_receipt(id, character, settled, owner)) {
		reply(Retry);
		return;
	}
	auto a = util::umap_find(auction_db, id);
	if (settled) {
		if (owner && a)
			auction_forget_settled(a);
		reply(owner ? Done : Denied);
		return;
	}

	if (!a) {
		reply(Missing);
		return;
	}
	if (a->seller_id != static_cast<int32>(character)) {
		reply(Denied);
		return;
	}
	if (a->timestamp <= time(nullptr) || static_cast<uint32>(a->price) != expected_price ||
	    static_cast<uint32>(a->buyer_id) != expected_buyer || (action == CancelSale && a->buyer_id) ||
	    (action == CloseSale && !a->buyer_id)) {
		reply(Changed);
		return;
	}
	if (!auction_settle(*a, action == CancelSale ? AuctionSettlement::Cancelled : AuctionSettlement::Closed)) {
		reply(Retry);
		return;
	}
	if (a->buyer_id)
		mapif_Auction_message(a->buyer_id, 6);  // You have won the auction
	auction_forget_settled(a);
	reply(Done);
}
