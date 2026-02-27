CREATE TABLE IF NOT EXISTS `shared_professions_account_skills` (
    `account_id` INT UNSIGNED NOT NULL,
    `skill_id` SMALLINT UNSIGNED NOT NULL,
    `value` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `max_value` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `step` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`, `skill_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
