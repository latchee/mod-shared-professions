# mod-shared-professions

AzerothCore module that shares profession progression account-wide. All characters on the same account benefit from the highest skill level, tier, and learned recipes across all characters.

## How it works

- On **login**, the character's profession data is saved to an account-wide store, then the best values from the store are applied back
- On **skill gain**, the new value is saved to the account store
- On **learning a recipe**, the spell is saved to the account store
- On **training a profession** (learning from a trainer), account-wide data is immediately synced — no relog needed

Characters must have the profession learned to receive synced data. The module does not grant new professions.

Data persists in the account store even if a character abandons a profession.

## Installation

1. Place this module in `modules/mod-shared-professions/`
2. Re-run cmake and rebuild
3. Copy `conf/mod_shared_professions.conf.dist` to your server's config directory
4. Run the SQL files from `data/sql/db-characters/` against your `acore_characters` database

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `SharedProfessions.Enable` | `1` | Enable the module |
| `SharedProfessions.SyncSecondary` | `1` | Also sync Cooking, First Aid, and Fishing |
