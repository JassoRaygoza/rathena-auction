-- Modern Auction System: durable settlement and trading.
-- Back up the database and run with map-server and char-server stopped.

-- Part 1: durable cancellation / closure / expiry delivery.
-- Durable Auction cancellation / closure / expiry delivery.
-- Back up the character database and deploy with map/char stopped.
-- For custom table names, substitute auction/mail/mail_attachments below.
-- ALTER TABLE commits implicitly; this migration is not one transaction.
ALTER TABLE `auction` ENGINE=InnoDB;
ALTER TABLE `mail` ENGINE=InnoDB;
ALTER TABLE `mail_attachments` ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS `auction_settlement` (
  `auction_id` bigint unsigned NOT NULL,
  `seller_id` int unsigned NOT NULL,
  `buyer_id` int unsigned NOT NULL,
  `reason` tinyint unsigned NOT NULL,
  `item_mail_id` int unsigned NOT NULL,
  `zeny_mail_id` int unsigned NOT NULL DEFAULT 0,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`auction_id`)
) ENGINE=InnoDB;
-- Keep receipts for the lifetime of these auction IDs, including on rollback.
-- Never reset/reuse auction AUTO_INCREMENT while receipts remain.
-- This covers settlement only; map-side registration/bid reservations are separate.

-- Part 2: durable trading (registration, bids, buy-now).
-- Conditional DDL also permits a fresh main.sql schema and safe reruns.
SET @auction_bound_ddl = IF(
  EXISTS(SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='auction' AND COLUMN_NAME='bound'),
  'SELECT 1', 'ALTER TABLE `auction` ADD COLUMN `bound` tinyint unsigned NOT NULL DEFAULT 0');
PREPARE auction_bound_stmt FROM @auction_bound_ddl;
EXECUTE auction_bound_stmt;
DEALLOCATE PREPARE auction_bound_stmt;
ALTER TABLE `char` ENGINE=InnoDB;
ALTER TABLE `inventory` ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS `auction_operation` (
  `operation_id` char(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `account_id` int unsigned NOT NULL,
  `char_id` int unsigned NOT NULL,
  `request_hash` binary(32) NOT NULL,
  `result` smallint unsigned NOT NULL,
  `auction_id` int unsigned NOT NULL DEFAULT 0,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`operation_id`),
  KEY `character_history` (`char_id`,`created_at`)
) ENGINE=InnoDB;
-- Preserve operation IDs on restores/rollbacks; outstanding retries depend on them.
