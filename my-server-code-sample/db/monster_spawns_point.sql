-- メインシティのモンスター出現設定
-- unit/camp とスポーン地点の対応だけを表す。座標はすべて area_spawn_points から取る。

CREATE TABLE IF NOT EXISTS `monster_spawns_point` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '設定ID',
  `unit_id` INT UNSIGNED NOT NULL COMMENT 'モンスターのテンプレートID',
  `camp_id` INT UNSIGNED NOT NULL COMMENT '陣営ID',
  `spawn_point_id` BIGINT UNSIGNED NOT NULL COMMENT 'スポーン地点ID(area_spawn_points.idを参照)',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_spawn_point_unit` (`spawn_point_id`, `unit_id`),
  KEY `idx_spawn_point_id` (`spawn_point_id`),
  KEY `idx_unit_id` (`unit_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `monster_spawns_point` (`id`, `unit_id`, `camp_id`, `spawn_point_id`)
VALUES
  (1, 1, 1, 1),
  (2, 2, 1, 2),
  (3, 3, 1, 3);
