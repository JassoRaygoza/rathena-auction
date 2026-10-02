// Private map-server <-> char-server Auction contract (same build on both sides).
// Client fields are validated on map; the authoritative character transaction
// owns all durable effects.
#pragma once

#include <algorithm>
#include <string>

#include "mmo.hpp"
#include "auction_protocol.hpp"

namespace auction_live {

// Costume items: costume equip slots, or the "C_" name prefix.
inline bool is_costume(unsigned equip, const std::string& name) {
	return (equip & 0x3c00u) != 0 || name.compare(0, 2, "C_") == 0;
}

// Case-insensitive (ASCII) substring search.
inline bool contains_name(const std::string& name, const std::string& query) {
	const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
	const auto equal = [&](unsigned char a, unsigned char b) { return lower(a) == lower(b); };
	return std::search(name.begin(), name.end(), query.begin(), query.end(), equal) != name.end();
}

}  // namespace auction_live

// Limits enforced by both servers and advertised to the client in the rules.
constexpr uint16 AUCTION_MIN_HOURS = 1;
constexpr uint16 AUCTION_MAX_HOURS = 48;
constexpr uint16 AUCTION_MAX_SALES = 5;
constexpr uint16 AUCTION_MAX_BIDS = 5;

// Live catalog and seller actions (map -> char -> map).
//   0x3058 <length>.W <char id>.L <request>.L <type>.W <page>.W <text>.24B <item count>.W {<item id>.L}*
//   0x3858 <length>.W <char id>.L <request>.L <count>.W <pages>.W <page>.W <status>.W {auction_data}*
//   0x3059 <length>.W <char id>.L <request>.L <auction id>.L <price>.L <buyer>.L <action>.W <zero>.W
//   0x3859 <length>.W <char id>.L <request>.L <auction id>.L <action>.W <result>.W
// For action 0 (read rules) the 0x3859 result carries the capabilities.
constexpr uint16 AUCTION_LIVE_QUERY = 0x3058, AUCTION_LIVE_RESULT = 0x3858;
constexpr uint16 AUCTION_LIVE_ACTION = 0x3059, AUCTION_ACTION_RESULT = 0x3859;
constexpr uint16 AUCTION_QUERY_HEADER = 42, AUCTION_RESULT_HEADER = 20;
constexpr uint16 AUCTION_ACTION_LENGTH = 28, AUCTION_ACTION_RESULT_LENGTH = 20;

// Durable trading (map -> char -> map), fixed-size structs below.
constexpr uint16 AUCTION_SERVICE_REQUEST = 0x2b32, AUCTION_SERVICE_REPLY = 0x2b33;

#pragma pack(push, 1)
struct s_auction_request {
	uint16 header, length;
	char operation[33];  // random 32-digit hex id, identifies retries
	uint32 account_id, char_id, client_request;
	uint16 action;
	uint32 target, expected_price, expected_buyer, charge, zeny_before;
	uint32 fee_per_hour, maximum_price;
	auction_data listing;
	item inventory[MAX_INVENTORY];  // authoritative inventory after reservation
};

struct s_auction_reply {
	uint16 header;
	char operation[33];
	uint32 account_id, char_id, client_request, target, auction_id;
	uint16 action, result;
};
#pragma pack(pop)

static_assert(sizeof(s_auction_request) <= UINT16_MAX, "Auction private packet size");
