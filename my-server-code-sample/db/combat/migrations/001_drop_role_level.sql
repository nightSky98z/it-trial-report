-- プレイヤー（キャラクター）自体にはレベルがない。レベルを持つのは船（role_units.level）だけ。
ALTER TABLE `roles` DROP COLUMN IF EXISTS `level`;
