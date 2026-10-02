// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef AUCTION_MARKET_HPP
#define AUCTION_MARKET_HPP

#include <memory>
#include <unordered_set>

#include <common/cbasetypes.hpp>

struct auction_data;

// Why an auction ended, stored as `auction_settlement`.`reason`.
// Bought is written by buy-now trades and is never passed to auction_settle.
enum class AuctionSettlement { Expired, Cancelled, Closed, Bought };

// Auctions whose settlement outcome is not yet known; never bid against them.
extern std::unordered_set<uint32> auction_settling;

// Auction deletion and all delivery mails form one transaction. The durable
// auction_id receipt resolves retries, including a lost COMMIT reply.
bool auction_settle(const auction_data& a, AuctionSettlement reason);
void auction_forget_settled(const std::shared_ptr<auction_data>& a);

void mapif_parse_Auction_live_query(int32 fd);   // 0x3058
void mapif_parse_Auction_live_action(int32 fd);  // 0x3059
int32 inter_auction_parse_purchase(int32 fd);    // 0x2B32

#endif /* AUCTION_MARKET_HPP */
