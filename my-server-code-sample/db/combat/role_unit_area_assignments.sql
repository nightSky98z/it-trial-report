-- プレイヤー船のエリア割り当てテーブル
-- role_id + unit_id は一意。area_id は 1〜4。
CREATE TABLE IF NOT EXISTS `role_unit_area_assignments` (
  `role_id` varchar(32) NOT NULL,
  `unit_id` int unsigned NOT NULL,
  `area_id` int unsigned NOT NULL DEFAULT 1,
  `updated_at` timestamp DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`role_id`, `unit_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
