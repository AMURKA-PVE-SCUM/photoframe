// questdb.h -- SCUM.db (SQLite) quest-lockout recovery
#pragma once
#include <string>
#include <vector>

namespace scum_rcon::questdb {

struct QuestLockoutRow {
    long long   row_id;
    std::string player_name;
    std::string steam_id;          // up.user_id (17-digit SteamID)
    std::string quest_path;        // aq.quest_data_asset_path
};

std::vector<QuestLockoutRow> find_quest_lockouts(
    std::vector<std::string> const& blocked_stems);

long long delete_active_quests_for_user(std::string const& steam_id);

struct UnstickReport {
    std::size_t matches = 0;
    std::size_t deleted = 0;
    bool        aborted = false;
};
UnstickReport run_quest_unstick(
    std::vector<std::string> const& blocked_stems,
    std::size_t max_delete,
    bool dry_run);

std::string format_lockouts(std::vector<QuestLockoutRow> const& rows);
std::string list_squads();

// Per-player economy/identity snapshot for the ListPlayers reply.
// Sources: user.name (Steam persona), user_profile (character name, fame,
// money), bank_account_registry_currencies (1 = account/cash, 2 = gold).
struct PlayerEconomy {
    bool        found = false;   // user_profile row exists
    std::string steam_name;      // user.name
    std::string char_name;       // user_profile.name
    long long   fame = 0;        // (long long)fame_points
    bool        has_fame = false;
    long long   money_balance = 0;
    bool        has_money = false;
    long long   account = 0;     // SUM currency_type=1
    bool        has_account_rows = false;
    long long   gold = 0;        // SUM currency_type=2
};
bool player_economy(std::string const& steam_id, PlayerEconomy& out);

// Native ListSpawnedVehicles reply in the exact engine text format:
//   #<id>: <Type>   <localtime>.000Z   X=<%.3f> Y=<%.3f> Z=<%.3f>      0   <alias|No owner>
// Sources: vehicle_entity + entity + vehicle_spawner. The engine renders
// the unix timestamp in SERVER-LOCAL time with a literal ".000Z" suffix.
std::string list_spawned_vehicles();

} // namespace scum_rcon::questdb
