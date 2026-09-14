// questdb.cpp -- SCUM.db (SQLite) quest-lockout recovery
//
// SQLite is statically linked (sqlite3.c from the amalgamation). SCUM.db is
// located by walking up from the process working directory. Queries open the
// DB read-only; mutations open read-write.
#include "questdb.h"
#include "scum_rcon.h"
#include "sqlite3.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <sstream>

namespace scum_rcon::questdb {

namespace {

std::string find_scum_db_path()
{
    namespace fs = std::filesystem;
    auto dir = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        auto candidate = dir / "SCUM.db";
        if (fs::exists(candidate)) return candidate.string();
        auto save = dir / "Saved" / "SaveFiles" / "SCUM.db";
        if (fs::exists(save)) return save.string();
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

sqlite3* open_db(bool read_write)
{
    auto path = find_scum_db_path();
    if (path.empty()) return nullptr;

    sqlite3* db = nullptr;
    int flags = SQLITE_OPEN_URI
              | (read_write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY);
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    return db;
}

bool quest_matches_blocked(std::string const& quest_path,
                           std::vector<std::string> const& stems)
{
    auto stem = quest_path;
    auto slash = stem.find_last_of("/\\");
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    auto dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    std::string stem_lower;
    for (char c : stem)
        stem_lower.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(c))));

    for (auto const& s : stems) {
        std::string sl;
        for (char c : s)
            sl.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
        if (stem_lower == sl || stem_lower.find(sl) != std::string::npos)
            return true;
    }
    return false;
}

} // anonymous namespace

// ===========================================================================
//  find_quest_lockouts
// ===========================================================================
std::vector<QuestLockoutRow> find_quest_lockouts(
    std::vector<std::string> const& blocked_stems)
{
    std::vector<QuestLockoutRow> rows;
    sqlite3* db = open_db(false);
    if (!db) return rows;

    constexpr const char* kSql =
        "SELECT up.name, up.user_id, aq.quest_data_asset_path "
        "FROM active_quest aq "
        "JOIN user_profile up ON up.id = aq.user_profile_id";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return rows;
    }

    std::size_t flagged = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QuestLockoutRow r;
        r.row_id      = 0;
        r.player_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        r.steam_id    = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.quest_path  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (quest_matches_blocked(r.quest_path, blocked_stems)) {
            rows.push_back(std::move(r));
            ++flagged;
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    log::line(L"questdb: find_quest_lockouts - " + std::to_wstring(blocked_stems.size()) +
              L" blocked quest(s), " + std::to_wstring(flagged) + L" player(s) flagged");
    return rows;
}

// ===========================================================================
//  delete_active_quests_for_user
// ===========================================================================
long long delete_active_quests_for_user(std::string const& steam_id)
{
    sqlite3* db = open_db(true);
    if (!db) return 0;

    constexpr const char* kSql =
        "DELETE FROM active_quest "
        "WHERE user_profile_id = (SELECT id FROM user_profile WHERE user_id = ?)";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, steam_id.c_str(), -1, SQLITE_TRANSIENT);

    long long deleted = 0;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        deleted = sqlite3_changes(db);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    log::line(L"questdb: deleted " + std::to_wstring(deleted) +
              L" active_quest row(s) for SteamID " + log::widen(steam_id));
    return deleted;
}

// ===========================================================================
//  run_quest_unstick
// ===========================================================================
UnstickReport run_quest_unstick(
    std::vector<std::string> const& blocked_stems,
    std::size_t max_delete,
    bool dry_run)
{
    UnstickReport rep;
    if (blocked_stems.empty()) return rep;

    sqlite3* db = open_db(!dry_run);
    if (!db) return rep;

    constexpr const char* kSql =
        "SELECT aq.id, aq.quest_data_asset_path "
        "FROM active_quest aq";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return rep;
    }

    std::vector<long long> ids_to_delete;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(stmt, 0);
        std::string quest_path =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (quest_matches_blocked(quest_path, blocked_stems)) {
            ids_to_delete.push_back(id);
            ++rep.matches;
        }
    }
    sqlite3_finalize(stmt);

    if (rep.matches > max_delete) {
        log::line(L"questdb: unstick ABORTED - " + std::to_wstring(rep.matches) +
                  L" matches exceed max_delete " + std::to_wstring(max_delete));
        rep.aborted = true;
        sqlite3_close(db);
        return rep;
    }

    if (dry_run) {
        sqlite3_close(db);
        return rep;
    }

    for (long long id : ids_to_delete) {
        sqlite3_stmt* del = nullptr;
        if (sqlite3_prepare_v2(db, "DELETE FROM active_quest WHERE id = ?", -1,
                               &del, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int64(del, 1, id);
        if (sqlite3_step(del) == SQLITE_DONE) ++rep.deleted;
        sqlite3_finalize(del);
    }

    sqlite3_close(db);
    return rep;
}

// ===========================================================================
//  format_lockouts -- human-readable FindQuestLockouts reply
// ===========================================================================
std::string format_lockouts(std::vector<QuestLockoutRow> const& rows)
{
    if (rows.empty()) return "No quest lockouts found.";
    std::ostringstream os;
    for (auto const& r : rows) {
        os << r.player_name << " (" << r.steam_id << "): " << r.quest_path << "\n";
    }
    return os.str();
}

// ===========================================================================
//  list_squads -- ListSquads command
// ===========================================================================
bool player_economy(std::string const& steam_id, PlayerEconomy& out)
{
    out = PlayerEconomy{};
    sqlite3* db = open_db(false);
    if (!db) return false;
    sqlite3_busy_timeout(db, 1500);

    sqlite3_stmt* stmt = nullptr;
    // Steam persona name.
    if (sqlite3_prepare_v2(db, "SELECT name FROM user WHERE id=?",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, steam_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* n = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 0));
            if (n) out.steam_name = n;
        }
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
    // Character profile: name, fame, money.
    long long profile_id = 0;
    if (sqlite3_prepare_v2(db, "SELECT id, name, fame_points, money_balance "
                               "FROM user_profile WHERE user_id=? LIMIT 1",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, steam_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            out.found = true;
            profile_id = sqlite3_column_int64(stmt, 0);
            const char* n = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, 1));
            if (n) out.char_name = n;
            if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
                out.fame = static_cast<long long>(
                    sqlite3_column_double(stmt, 2));
                out.has_fame = true;
            }
            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
                out.money_balance = sqlite3_column_int64(stmt, 3);
                out.has_money = true;
            }
        }
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
    // Bank balances per currency (1 = account/cash, 2 = gold).
    if (out.found && sqlite3_prepare_v2(db, "SELECT currency_type, "
            "SUM(account_balance) FROM bank_account_registry_currencies "
            "WHERE user_profile_id=? GROUP BY currency_type",
            -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, profile_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            long long ctype = sqlite3_column_int64(stmt, 0);
            long long bal = sqlite3_column_type(stmt, 1) == SQLITE_NULL
                ? 0 : sqlite3_column_int64(stmt, 1);
            if (ctype == 1) { out.account = bal; out.has_account_rows = true; }
            else if (ctype == 2) { out.gold = bal; }
        }
        sqlite3_finalize(stmt);
        stmt = nullptr;
    }
    sqlite3_close(db);
    // Account falls back to money_balance when no bank rows exist.
    if (!out.has_account_rows && out.has_money)
        out.account = out.money_balance;
    return out.found || !out.steam_name.empty();
}

std::string list_spawned_vehicles()
{
    sqlite3* db = open_db(false);
    if (!db) return "SCUM.db not found.";
    sqlite3_busy_timeout(db, 1500);

    constexpr const char* kSql =
        "SELECT ve.entity_id, vs.vehicle_asset_id, vs.vehicle_alias, "
        "vs.vehicle_last_access_time, "
        "e.location_x, e.location_y, e.location_z "
        "FROM vehicle_entity ve "
        "JOIN entity e ON e.id = ve.entity_id "
        "LEFT JOIN vehicle_spawner vs ON vs.vehicle_entity_id = ve.entity_id "
        "ORDER BY ve.entity_id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return "SCUM.db query failed.";
    }

    std::ostringstream os;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(stmt, 0);
        const char* asset = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 1));
        const char* alias = reinterpret_cast<const char*>(
            sqlite3_column_text(stmt, 2));
        bool has_ts = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
        long long ts = has_ts ? sqlite3_column_int64(stmt, 3) : 0;
        double x = sqlite3_column_type(stmt, 4) == SQLITE_NULL
            ? 0.0 : sqlite3_column_double(stmt, 4);
        double y = sqlite3_column_type(stmt, 5) == SQLITE_NULL
            ? 0.0 : sqlite3_column_double(stmt, 5);
        double z = sqlite3_column_type(stmt, 6) == SQLITE_NULL
            ? 0.0 : sqlite3_column_double(stmt, 6);

        std::string type = asset ? asset : "";
        constexpr const char* kPrefix = "Vehicle:";
        if (type.rfind(kPrefix, 0) == 0) type = type.substr(8);
        if (type.empty()) type = "Unknown";

        // Engine quirk: unix timestamp rendered in server-local time with
        // a literal ".000Z" suffix.
        std::time_t t = static_cast<std::time_t>(ts);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char tsbuf[32];
        std::strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%dT%H:%M:%S", &lt);

        std::string owner = (alias && *alias) ? alias : "No owner";

        os << "#" << id << ": " << type << "   " << tsbuf << ".000Z"
           << "   ";
        {
            std::ostringstream ls;
            ls << std::fixed;
            ls.precision(3);
            ls << "X=" << x << " Y=" << y << " Z=" << z;
            os << ls.str();
        }
        os << "      0   " << owner << "\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return os.str();
}

std::string list_squads()
{
    sqlite3* db = open_db(false);
    if (!db) return "SCUM.db not found.";

    constexpr const char* kSql =
        "SELECT id, name, score FROM squad ORDER BY id";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return "SCUM.db query failed.";
    }

    std::ostringstream os;
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ++n;
        long long id = sqlite3_column_int64(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        long long score = sqlite3_column_int64(stmt, 2);
        os << "Squad " << id << ": " << (name ? name : "") << " (score " << score << ")\n";
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (n == 0) return "No squads.";
    return os.str();
}

} // namespace scum_rcon::questdb