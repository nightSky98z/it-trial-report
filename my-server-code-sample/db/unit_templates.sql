# ************************************************************
# Sequel Ace SQL dump
# バージョン 20099
#
# https://sequel-ace.com/
# https://github.com/Sequel-Ace/Sequel-Ace
#
# ホスト: 127.0.0.1 (MySQL 8.4.0)
# データベース: game_zone_1
# 生成時間: 2026-04-08 07:58:35 +0000
# ************************************************************


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
SET NAMES utf8mb4;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE='NO_AUTO_VALUE_ON_ZERO', SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;


# テーブルのダンプ unit_templates
# ------------------------------------------------------------

DROP TABLE IF EXISTS `unit_templates`;

CREATE TABLE `unit_templates` (
  `unit_id` int unsigned NOT NULL COMMENT '精霊のテンプレートID(精霊の種類と同じ)',
  `unit_name` varchar(32) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '' COMMENT '精霊の名前',
  `camp_id` int unsigned NOT NULL DEFAULT '0' COMMENT '精霊が属する陣営',
  `camp_name` varchar(32) COLLATE utf8mb4_general_ci NOT NULL DEFAULT '' COMMENT '精霊が属する陣営の名前',
  `description` text COLLATE utf8mb4_general_ci COMMENT '説明',
  `base_hp` int unsigned NOT NULL DEFAULT '100' COMMENT '基礎HP',
  `base_atk` int unsigned NOT NULL DEFAULT '10' COMMENT '基礎攻撃力',
  `base_def` int unsigned NOT NULL DEFAULT '5' COMMENT '基礎防御力',
  `base_speed` float NOT NULL DEFAULT '120' COMMENT '基礎速度',
  `attack_range` float NOT NULL DEFAULT '1' COMMENT '基礎攻撃範囲',
  PRIMARY KEY (`unit_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

LOCK TABLES `unit_templates` WRITE;
/*!40000 ALTER TABLE `unit_templates` DISABLE KEYS */;

INSERT INTO `unit_templates` (`unit_id`, `unit_name`, `camp_id`, `camp_name`, `description`, `base_hp`, `base_atk`, `base_def`, `base_speed`, `attack_range`)
VALUES
	(1,'駆逐艦',1,'戦艦','駆逐艦',100,10,5,120,1),
	(2,'巡洋艦',1,'巡洋艦','巡洋艦',100,10,5,120,1),
	(3,'空母',1,'空母','空母',100,10,5,120,1);

/*!40000 ALTER TABLE `unit_templates` ENABLE KEYS */;
UNLOCK TABLES;



/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
