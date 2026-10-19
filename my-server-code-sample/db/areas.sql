-- エリアの矩形境界
-- (map_id, area_id) ごとに 1 行で、そのエリアの活動範囲となる矩形を定義する。
-- 制約："area_spawn_points" の各スポーン地点は、対応するエリアの境界内になければならない。
-- 外れていると起動時のチェックで失敗する。
-- 実行時の制約："CAreaBounds" コンポーネントはこの設定からコピーされ、徘徊先の抽選・追跡対象の選択・
-- 位置の強制的な押し戻しのすべてがこれを参照する。

CREATE TABLE IF NOT EXISTS `areas` (
  `map_id`  INT UNSIGNED     NOT NULL COMMENT 'マップID',
  `area_id` TINYINT UNSIGNED NOT NULL COMMENT 'エリアID',
  `min_x`   FLOAT            NOT NULL COMMENT '矩形の左下 X',
  `max_x`   FLOAT            NOT NULL COMMENT '矩形の右上 X',
  `min_y`   FLOAT            NOT NULL COMMENT '矩形の左下 Y',
  `max_y`   FLOAT            NOT NULL COMMENT '矩形の右上 Y',
  PRIMARY KEY (`map_id`, `area_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- map_id = 1：メインシティのマップの 4 エリア（十字型の配置。各エリア 800×800 で、メインシティが中央）
-- エリア 1 メインシティ  中心 (   0,    0) —— モンスターなし
-- エリア 2 南            中心 (   0, -800) —— モンスターあり
-- エリア 3 西            中心 (-800,    0) —— モンスターあり
-- エリア 4 北            中心 (   0,  800) —— モンスターあり
INSERT IGNORE INTO `areas` (`map_id`, `area_id`, `min_x`, `max_x`, `min_y`, `max_y`)
VALUES
  (1, 1,  -400,   400,  -400,   400),
  (1, 2,  -400,   400, -1200,  -400),
  (1, 3, -1200,  -400,  -400,   400),
  (1, 4,  -400,   400,   400,  1200);
