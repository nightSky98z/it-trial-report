# ************************************************************
# Sequel Ace SQL dump
# バージョン 20099
#
# https://sequel-ace.com/
# https://github.com/Sequel-Ace/Sequel-Ace
#
# ホスト: 127.0.0.1 (MySQL 8.4.0)
# データベース: game_zone_1
# 生成時間: 2026-04-08 07:58:03 +0000
# ************************************************************


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
SET NAMES utf8mb4;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE='NO_AUTO_VALUE_ON_ZERO', SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;


# テーブルのダンプ roles
# ------------------------------------------------------------

DROP TABLE IF EXISTS `roles`;

CREATE TABLE `roles` (
  `role_id` varchar(32) COLLATE utf8mb4_general_ci NOT NULL COMMENT 'キャラクターID',
  `user_id` varchar(64) COLLATE utf8mb4_general_ci NOT NULL COMMENT 'ユーザーID',
  `role_name` varchar(32) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci NOT NULL DEFAULT '' COMMENT 'キャラクター名',
  `gold` int unsigned NOT NULL DEFAULT '0' COMMENT 'ゴールド',
  `stamina` int unsigned NOT NULL DEFAULT '100' COMMENT 'スタミナ',
  `crystal` int unsigned NOT NULL DEFAULT '0' COMMENT 'クリスタル',
  `combat_power` int unsigned NOT NULL DEFAULT '0' COMMENT '戦闘力',
  `current_map_id` tinyint unsigned NOT NULL DEFAULT '1' COMMENT 'プレイヤーが現在いるマップ（デフォルトはメインシティ。初回入場時もメインシティ）',
  `selected_team_id` tinyint unsigned NOT NULL DEFAULT '1' COMMENT 'プレイヤーの現在の編成番号',
  `created_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP COMMENT '作成日時',
  `updated_at` timestamp NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '更新日時',
  PRIMARY KEY (`role_id`),
  UNIQUE KEY `uq_roles_user_id` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;




/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
