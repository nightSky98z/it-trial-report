-- メインシティのエリア別スポーン地点
-- 各スポーン地点は一意の ID を持つ。異なる map_id で同じ area_id を使ってもよいが、座標はそれぞれ独立している。

CREATE TABLE IF NOT EXISTS `area_spawn_points` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT 'スポーン地点ID',
  `map_id` INT UNSIGNED NOT NULL COMMENT 'マップID',
  `area_id` TINYINT UNSIGNED NOT NULL COMMENT 'エリアID(1=メインシティ,2=南,3=西,4=北)',
  `x` FLOAT NOT NULL DEFAULT 0 COMMENT 'スポーン地点のX座標',
  `y` FLOAT NOT NULL DEFAULT 0 COMMENT 'スポーン地点のY座標',
  PRIMARY KEY (`id`),
  KEY `idx_map_area` (`map_id`, `area_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `area_spawn_points` (`id`, `map_id`, `area_id`, `x`, `y`)
VALUES
  (1, 1, 2, 0, -800),
  (2, 1, 2, 320, -1024),
  (3, 1, 2, -320, -1024);
