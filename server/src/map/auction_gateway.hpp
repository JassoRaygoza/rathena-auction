// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef AUCTION_GATEWAY_HPP
#define AUCTION_GATEWAY_HPP

#include <common/auction_service.hpp>
#include <common/cbasetypes.hpp>

class map_session_data;
struct PACKET_CZ_AUCTION_ITEM_SEARCH;

// Client requests (0x0251 with type & 0x8000) and their 0x0252 replies.
void clif_Auction_live_query(map_session_data* sd, const PACKET_CZ_AUCTION_ITEM_SEARCH& p);
void clif_Auction_live_result(map_session_data* sd, uint32 request, uint16 page, uint16 pages, uint16 count, const uint8* data, uint16 status);
void clif_Auction_action_result(map_session_data* sd, uint32 request, uint32 id, uint16 action, uint16 status);
void clif_Auction_live_rules(map_session_data* sd, uint32 request, uint16 capabilities);

// Inter-server catalog and seller actions.
int32 intif_Auction_requestlist_live(uint32 char_id, uint32 request, uint16 type, uint16 page, const char* text);
int32 intif_Auction_action(uint32 char_id, uint32 request, uint32 id, uint32 price, uint32 buyer, uint16 action);
void intif_parse_Auction_live_result(int32 fd);  // 0x3858
void intif_parse_Auction_action_result(int32 fd); // 0x3859

// Durable purchases. While sd.auction_purchase is set, the reserved zeny and
// item belong to the pending operation and must not be spent elsewhere.
struct s_auction_pending;
bool auction_purchase(map_session_data& sd, s_auction_request request, int index = -1);
uint32 auction_reserved_zeny(const map_session_data& sd);
int32 auction_reserved_item(const map_session_data& sd, int index);
void auction_retry_all();
void auction_defer_save(map_session_data& sd, int flags);
void auction_purchase_reply(int fd); // 0x2B33

#endif /* AUCTION_GATEWAY_HPP */
