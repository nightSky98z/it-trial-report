# ************************************************************
# Sequel Ace SQL dump
# バージョン 20099
#
# https://sequel-ace.com/
# https://github.com/Sequel-Ace/Sequel-Ace
#
# ホスト: 127.0.0.1 (MySQL 8.4.0)
# データベース: game_zone_1
# 生成時間: 2026-04-08 07:57:43 +0000
# ************************************************************


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
SET NAMES utf8mb4;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE='NO_AUTO_VALUE_ON_ZERO', SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;


# テーブルのダンプ role_units
# ------------------------------------------------------------

DROP TABLE IF EXISTS `role_units`;

CREATE TABLE `role_units` (
  `id` bigint unsigned NOT NULL AUTO_INCREMENT COMMENT '所持レコードID',
  `role_id` varchar(32) COLLATE utf8mb4_general_ci NOT NULL COMMENT '所有者ID(キャラクターID)',
  `unit_id` int unsigned NOT NULL COMMENT '精霊ID',
  `level` int unsigned NOT NULL DEFAULT '1' COMMENT '精霊のレベル',
  `exp` bigint unsigned NOT NULL DEFAULT '0' COMMENT '精霊の経験値',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_role_unit` (`role_id`,`unit_id`),
  KEY `idx_role_id` (`role_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;




/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
