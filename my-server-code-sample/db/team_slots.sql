# ************************************************************
# Sequel Ace SQL dump
# バージョン 20099
#
# https://sequel-ace.com/
# https://github.com/Sequel-Ace/Sequel-Ace
#
# ホスト: 127.0.0.1 (MySQL 8.4.0)
# データベース: game_zone_1
# 生成時間: 2026-04-08 07:58:16 +0000
# ************************************************************


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
SET NAMES utf8mb4;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE='NO_AUTO_VALUE_ON_ZERO', SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;


# テーブルのダンプ team_slots
# ------------------------------------------------------------

DROP TABLE IF EXISTS `team_slots`;

CREATE TABLE `team_slots` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT,
  `role_id` varchar(32) COLLATE utf8mb4_general_ci NOT NULL COMMENT 'キャラクターID',
  `team_id` tinyint unsigned NOT NULL COMMENT '1~3(最大3編成)',
  `slot_index` tinyint unsigned NOT NULL COMMENT '0~4(最大5体)',
  `unit_id` int unsigned NOT NULL COMMENT '精霊ID',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_role_team_slot` (`role_id`,`team_id`,`slot_index`),
  KEY `idx_role_team` (`role_id`,`team_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;




/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
