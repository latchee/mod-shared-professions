CREATE TABLE IF NOT EXISTS `shared_professions_account_spells` (
    `account_id` INT UNSIGNED NOT NULL,
    `spell_id` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`account_id`, `spell_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
