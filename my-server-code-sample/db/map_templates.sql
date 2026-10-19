# ************************************************************
# Sequel Ace SQL dump
# バージョン 20099
#
# https://sequel-ace.com/
# https://github.com/Sequel-Ace/Sequel-Ace
#
# ホスト: 127.0.0.1 (MySQL 8.4.0)
# データベース: game_zone_1
# 生成時間: 2026-04-08 07:56:43 +0000
# ************************************************************


/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
SET NAMES utf8mb4;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE='NO_AUTO_VALUE_ON_ZERO', SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;


# テーブルのダンプ map_templates
# ------------------------------------------------------------

DROP TABLE IF EXISTS `map_templates`;

CREATE TABLE `map_templates` (
  `map_id` int unsigned NOT NULL AUTO_INCREMENT COMMENT 'マップテンプレートID',
  `map_name` varchar(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci DEFAULT '{プレイヤー名}のメインシティ' COMMENT 'マップ名',
  `map_mode` tinyint(1) DEFAULT '0' COMMENT '0:メインシティ 1:ダンジョン',
  `max_players` tinyint DEFAULT '1' COMMENT 'このマップに入れる最大プレイヤー数',
  `respawn_x` float DEFAULT '0' COMMENT 'マップのリスポーン地点のX座標',
  `respawn_y` float DEFAULT '-64' COMMENT 'マップのリスポーン地点のY座標',
  PRIMARY KEY (`map_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;

LOCK TABLES `map_templates` WRITE;
/*!40000 ALTER TABLE `map_templates` DISABLE KEYS */;

INSERT INTO `map_templates` (`map_id`, `map_name`, `map_mode`, `max_players`, `respawn_x`, `respawn_y`)
VALUES
	(1,'{プレイヤー名}のメインシティ',0,1,0,-64);

/*!40000 ALTER TABLE `map_templates` ENABLE KEYS */;
UNLOCK TABLES;



/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;
/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
