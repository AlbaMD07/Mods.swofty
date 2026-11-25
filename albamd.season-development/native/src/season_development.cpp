// Season Development: AI teams develop their cars through a career season.
//
// Between rounds (at Team HQ) every AI team may bring an upgrade package. Packages can work, half work or fail
// (and get rolled back), big packages cluster around the early-season and summer upgrade windows, and teams out
// of the fight switch focus to next year's car late in the season. The running total of each team's development
// is applied to its cars in every session of the career as per-car layers through the core's car operations:
// ai.grip_scale (cornering, AI only) and engine.power_scale (power). The player's car is never touched.
//
// State is kept per career through the core's career saves (fallback: this addon's storage folder).
//
// SPDX-License-Identifier: MIT

#include <swofty/career.hpp>
#include <swofty/events.hpp>
#include <swofty/live.hpp>
#include <swofty/mixin.hpp>
#include <swofty/popup.hpp>
#include <swofty/storage.hpp>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

static constexpr const char *kAddon = "albamd.season-development";
static constexpr uint32_t kDataVersion = 1;

// ---------------------------------------------------------------------------------------------------- settings

SWOFTY_SETTING(bool, enabled, true);
SWOFTY_SETTING(int, strength, 100);                       // percent: how much a pace step changes the car
SWOFTY_SETTING(std::string_view, frequency, "normal");    // low / normal / high: how often packages arrive
SWOFTY_SETTING(std::string_view, failures, "normal");     // rare / normal / often: packages that do not work
SWOFTY_SETTING(bool, include_player_team, false);         // develop your team-mate's car too
SWOFTY_SETTING(std::string_view, reports, "summary");     // off / summary / detailed
SWOFTY_SETTING(bool, verbose, false);

// ---------------------------------------------------------------------------------------------------- core tables

namespace core {

template <class T, class Fn> const T *lookup(const char *name)
{
    HMODULE m = GetModuleHandleW(L"swofty_core.dll");
    auto fn = m ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(m, name))) : nullptr;
    return fn ? fn() : nullptr;
}

inline const SwoftyCarOps *cars()
{
    static const SwoftyCarOps *t = lookup<SwoftyCarOps, SwoftyCoreCarOpsFn>("SwoftyCoreCarOps");
    return t && t->version >= 2 && t->available && t->available() ? t : nullptr;
}

inline const SwoftyStandings *standings()
{
    static const SwoftyStandings *t = lookup<SwoftyStandings, SwoftyCoreStandingsFn>("SwoftyCoreStandings");
    return t && t->version >= 1 && t->available && t->available() ? t : nullptr;
}

inline const SwoftyCareerSaves *saves()
{
    static const SwoftyCareerSaves *t = lookup<SwoftyCareerSaves, SwoftyCoreCareerSavesFn>("SwoftyCoreCareerSaves");
    return t && t->version >= 1 && t->available && t->available() ? t : nullptr;
}

}

// ---------------------------------------------------------------------------------------------------- random

static uint64_t hash_str(std::string_view s, uint64_t h = 1469598103934665603ull)
{
    for (char c : s)
        h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
    return h;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    uint64_t next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 2685821657736338717ull;
    }
    float uniform() { return static_cast<float>(next() >> 40) / static_cast<float>(1ull << 24); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
};

// ---------------------------------------------------------------------------------------------------- state

struct Team {
    std::string key;           // the live team badge key ("mclaren"), or "team<udp id>"
    float resources = 1.0f;    // 0.70 .. 1.35: how fast the team develops
    float dev_chassis = 0.0f;  // pace percent from aero and chassis packages
    float dev_power = 0.0f;    // pace percent from power-unit packages
    int packages = 0;          // packages brought this season
    int frozen = 0;            // 1: development stopped for next year's car
    int rollback_round = 0;    // a failed package is taken off at this round
    float rollback = 0.0f;     // pace percent given back by the rollback
    int finish_sum = 0;        // sum of the team's best race finish this season
    int races = 0;
};

struct State {
    bool loaded = false;
    std::string career;        // career id from the core's career index
    int season = -1;           // season_index
    int round = 0;             // the last round processed (the next race to run, 1-based)
    std::vector<Team> teams;
};

static State g;

static Team *team(std::string_view key)
{
    for (auto &t : g.teams)
        if (t.key == key)
            return &t;
    return nullptr;
}

static Team &team_add(std::string_view key)
{
    if (auto *t = team(key))
        return *t;
    Team t;
    t.key = std::string(key);
    Rng r(hash_str(key, hash_str(g.career)) ^ static_cast<uint64_t>(g.season + 17));
    t.resources = r.range(0.85f, 1.15f);
    g.teams.push_back(t);
    return g.teams.back();
}

static std::string sanitize(std::string_view s)
{
    std::string out;
    for (char c : s) {
        char l = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_')
            out.push_back(l);
    }
    return out.empty() ? std::string("unknown") : out;
}

// "v1|season|round;key,res,dc,dp,pkgs,frozen,rbround,rb,finsum,races;..."
static std::string serialize()
{
    std::string s = "v1|" + std::to_string(g.season) + "|" + std::to_string(g.round);
    char buf[256];
    for (const auto &t : g.teams) {
        std::snprintf(buf, sizeof buf, ";%s,%.4f,%.4f,%.4f,%d,%d,%d,%.4f,%d,%d", t.key.c_str(), t.resources,
                      t.dev_chassis, t.dev_power, t.packages, t.frozen, t.rollback_round, t.rollback, t.finish_sum,
                      t.races);
        s += buf;
    }
    return s;
}

static bool deserialize(std::string_view s)
{
    if (s.rfind("v1|", 0) != 0)
        return false;
    std::string text(s);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++)
        if (i == text.size() || text[i] == ';') {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    int season = -1, round = 0;
    if (std::sscanf(parts[0].c_str(), "v1|%d|%d", &season, &round) != 2)
        return false;
    g.season = season;
    g.round = round;
    g.teams.clear();
    for (size_t i = 1; i < parts.size(); i++) {
        char key[64] = {};
        Team t;
        if (std::sscanf(parts[i].c_str(), "%63[^,],%f,%f,%f,%d,%d,%d,%f,%d,%d", key, &t.resources, &t.dev_chassis,
                        &t.dev_power, &t.packages, &t.frozen, &t.rollback_round, &t.rollback, &t.finish_sum,
                        &t.races) == 10) {
            t.key = key;
            g.teams.push_back(t);
        }
    }
    return true;
}

// JSON wrapper for the career saves data file: {"state":"<serialized>"}; the serialized text holds no quotes or
// backslashes (keys are sanitised), so it needs no escaping.
static void save()
{
    if (g.career.empty())
        return;
    std::string json = "{\"state\":\"" + serialize() + "\"}";
    if (auto *cs = core::saves()) {
        int r = cs->data_set(g.career.c_str(), kAddon, kDataVersion, SwoftyText{json.data(), json.size()});
        if (r == SWOFTY_ASSET_OK)
            return;
        swofty::log("[season-dev] career save refused (%d); using addon storage", r);
    }
    swofty::storage::write("careers/" + sanitize(g.career) + ".json", json);
}

static bool load_json(const std::string &json)
{
    auto at = json.find("\"state\":\"");
    if (at == std::string::npos)
        return false;
    at += 9;
    auto end = json.find('"', at);
    return end != std::string::npos && deserialize(std::string_view(json).substr(at, end - at));
}

static void load()
{
    g = State{};
    char id[128] = {};
    size_t len = 0;
    if (auto *cs = core::saves())
        if (cs->current(id, sizeof id, &len) == SWOFTY_ASSET_OK)
            g.career.assign(id, strnlen(id, sizeof id));
    if (g.career.empty()) {
        SwoftyCareerState st{};
        if (swofty::career::state(&st) == swofty::career::Result::ok && st.loaded)
            g.career = std::string("key_") + sanitize(std::string_view(st.key, strnlen(st.key, sizeof st.key)));
    }
    if (g.career.empty())
        return;
    g.loaded = true;

    bool ok = false;
    if (auto *cs = core::saves()) {
        std::string buf(64 * 1024, '\0');
        uint32_t version = 0;
        if (cs->data_get(g.career.c_str(), kAddon, buf.data(), buf.size(), &len, &version) == SWOFTY_ASSET_OK) {
            buf.resize(std::min(len, buf.size()));
            ok = load_json(buf);
        }
    }
    if (!ok) {
        std::string json;
        if (swofty::storage::read("careers/" + sanitize(g.career) + ".json", json) == swofty::storage::Result::ok)
            ok = load_json(json);
    }
    swofty::log("[season-dev] career %s: %s (%zu teams, season %d, round %d)", g.career.c_str(),
                ok ? "state loaded" : "new state", g.teams.size(), g.season, g.round);
}

// ---------------------------------------------------------------------------------------------------- team names

static std::vector<std::pair<std::string, std::string>> g_names;

static void load_names()
{
    std::string text;
    if (swofty::storage::read("teams.txt", text) != swofty::storage::Result::ok)
        return;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++)
        if (i == text.size() || text[i] == '\n') {
            std::string line = text.substr(start, i - start);
            start = i + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;
            g_names.emplace_back(sanitize(line.substr(0, eq)), line.substr(eq + 1));
        }
}

static std::string display_name(const std::string &key)
{
    for (const auto &[k, v] : g_names)
        if (k == key)
            return v;
    std::string n = key;
    if (!n.empty() && n[0] >= 'a' && n[0] <= 'z')
        n[0] = static_cast<char>(n[0] - 'a' + 'A');
    return n;
}

// ---------------------------------------------------------------------------------------------------- development

static float freq_base()
{
    std::string_view f = frequency;
    return f == "low" ? 0.16f : f == "high" ? 0.38f : 0.26f;
}

static float fail_scale()
{
    std::string_view f = failures;
    return f == "rare" ? 0.5f : f == "often" ? 1.6f : 1.0f;
}

static std::string g_player_team;   // the player's team key, from the last session with cars

static bool is_player_team(const Team &t) { return !include_player_team && !g_player_team.empty() && t.key == g_player_team; }

// The team's average best finish rank among teams this season (lower is better); 0 without results.
static float avg_finish(const Team &t) { return t.races ? static_cast<float>(t.finish_sum) / t.races : 0.0f; }

static const char *size_word(float pct)
{
    float a = std::fabs(pct);
    return a >= 0.25f ? "a big step" : a >= 0.12f ? "a solid step" : "a small step";
}

static void rollover(int new_season, std::vector<std::string> &news)
{
    std::vector<Team *> ranked;
    for (auto &t : g.teams)
        if (t.races)
            ranked.push_back(&t);
    std::sort(ranked.begin(), ranked.end(), [](Team *a, Team *b) { return avg_finish(*a) < avg_finish(*b); });
    for (size_t i = 0; i < ranked.size(); i++) {
        float third = static_cast<float>(i) / static_cast<float>(ranked.size());
        ranked[i]->resources += third >= 0.66f ? 0.08f : third < 0.33f ? -0.05f : 0.0f;   // wind-tunnel catch-up
    }
    for (auto &t : g.teams) {
        Rng r(hash_str(t.key, hash_str(g.career)) ^ (static_cast<uint64_t>(new_season) * 7919ull));
        t.dev_chassis *= 0.4f;
        t.dev_power *= 0.4f;
        if (t.frozen) {
            float head = r.range(0.10f, 0.30f);
            t.dev_chassis += head;
            news.push_back(display_name(t.key) + " starts the year strong after switching to this car early");
        }
        t.resources = std::clamp(t.resources + r.range(-0.05f, 0.05f), 0.70f, 1.35f);
        t.frozen = 0;
        t.packages = 0;
        t.rollback_round = 0;
        t.rollback = 0.0f;
        t.finish_sum = 0;
        t.races = 0;
    }
    swofty::log("[season-dev] season %d -> %d: development carried over at 40%%", g.season, new_season);
}

// Development for the weekend at `round` of `rounds`; writes the paddock news lines.
static void develop(int round, int rounds, std::vector<std::string> &news)
{
    if (round <= 1 || rounds <= 0)
        return;   // round 1: the cars run as launched
    float progress = static_cast<float>(round) / static_cast<float>(rounds);
    bool window = (progress >= 0.18f && progress <= 0.30f) || (progress >= 0.48f && progress <= 0.60f);
    bool detailed = std::string_view(reports) == "detailed";

    // teams' order by results, for the late-season focus switch
    std::vector<Team *> ranked;
    for (auto &t : g.teams)
        if (t.races)
            ranked.push_back(&t);
    std::sort(ranked.begin(), ranked.end(), [](Team *a, Team *b) { return avg_finish(*a) < avg_finish(*b); });

    for (auto &t : g.teams) {
        if (is_player_team(t))
            continue;
        Rng r(hash_str(t.key, hash_str(g.career)) ^ (static_cast<uint64_t>(g.season + 1) * 104729ull) ^
              (static_cast<uint64_t>(round) * 2654435761ull));
        char line[200];

        if (t.rollback_round == round && t.rollback != 0.0f) {
            t.dev_chassis += t.rollback;
            std::snprintf(line, sizeof line, "%s goes back to its old specification", display_name(t.key).c_str());
            news.push_back(line);
            t.rollback_round = 0;
            t.rollback = 0.0f;
        }
        if (t.frozen)
            continue;

        if (progress >= 0.72f) {
            auto rank = std::find(ranked.begin(), ranked.end(), &t) - ranked.begin();
            bool out_of_fight = !ranked.empty() && rank >= 3;
            if (out_of_fight && r.uniform() < 0.30f) {
                t.frozen = 1;
                std::snprintf(line, sizeof line, "%s stops developing this car to focus on next year",
                              display_name(t.key).c_str());
                news.push_back(line);
                continue;
            }
        }

        float chance = freq_base() * t.resources * (window ? 1.8f : 1.0f) * (progress > 0.8f ? 0.5f : 1.0f);
        if (r.uniform() >= chance)
            continue;

        bool major = window && r.uniform() < 0.5f;
        bool power = r.uniform() < 0.2f;
        float total = t.dev_chassis + t.dev_power;
        float diminish = 1.0f / (1.0f + std::max(0.0f, total) / 0.8f);
        float size = (major ? r.range(0.18f, 0.40f) : r.range(0.06f, 0.16f)) * t.resources * diminish;

        float roll = r.uniform();
        float p_fail = 0.15f * fail_scale(), p_partial = 0.20f;
        const char *part = power ? "power unit update" : major ? (r.uniform() < 0.5f ? "new floor" : "major aero package")
                                                       : (r.uniform() < 0.5f ? "front wing update" : "sidepod and bodywork update");
        float gain;
        const char *verdict;
        if (roll < p_fail) {
            gain = -size * 0.35f;
            verdict = "isn't working: the car is harder to drive";
            if (!power) {
                t.rollback_round = std::min(rounds, round + 1 + static_cast<int>(r.uniform() * 2.0f));
                t.rollback = -gain;
            }
        } else if (roll < p_fail + p_partial) {
            gain = size * 0.4f;
            verdict = "works, but less than the team hoped";
        } else {
            gain = size;
            verdict = size_word(size);
        }
        (power ? t.dev_power : t.dev_chassis) += gain;
        t.packages++;

        if (detailed)
            std::snprintf(line, sizeof line, "%s brings a %s: %s (%+.2f%%)", display_name(t.key).c_str(), part, verdict, gain);
        else
            std::snprintf(line, sizeof line, "%s brings a %s: %s", display_name(t.key).c_str(), part, verdict);
        news.push_back(line);
    }

    for (auto &t : g.teams) {
        float cap = 1.2f;
        t.dev_chassis = std::clamp(t.dev_chassis, -cap, cap);
        t.dev_power = std::clamp(t.dev_power, -cap * 0.5f, cap * 0.5f);
    }
}

static void show_report(int round, const std::vector<std::string> &news)
{
    if (std::string_view(reports) == "off" || news.empty())
        return;
    std::string body;
    for (const auto &n : news)
        body += "- " + n + "\n";
    if (std::string_view(reports) == "detailed") {
        std::vector<const Team *> order;
        for (const auto &t : g.teams)
            order.push_back(&t);
        std::sort(order.begin(), order.end(), [](const Team *a, const Team *b) {
            return a->dev_chassis + a->dev_power > b->dev_chassis + b->dev_power;
        });
        body += "\nDevelopment this season:\n";
        char line[120];
        for (const auto *t : order) {
            std::snprintf(line, sizeof line, "%s %+.2f%%%s\n", display_name(t->key).c_str(), t->dev_chassis + t->dev_power,
                          t->frozen ? " (stopped)" : "");
            body += line;
        }
    }
    char title[64];
    std::snprintf(title, sizeof title, "PADDOCK UPGRADES - ROUND %d", round);
    auto r = swofty::popup::show(title, body);
    if (r != swofty::popup::Result::ok)
        swofty::log("[season-dev] report popup %s", swofty::popup::name(r));
}

// At Team HQ: process the weekend the career is at, once.
static void process_hub()
{
    if (!g.loaded)
        load();
    if (!g.loaded)
        return;
    auto *st = core::standings();
    SwoftyStandingsSeason season{};
    if (!st || st->season(&season) != SWOFTY_ASSET_OK || season.round <= 0)
        return;
    if (season.season_index == g.season && season.round == g.round)
        return;

    std::vector<std::string> news;
    if (g.season >= 0 && season.season_index != g.season)
        rollover(season.season_index, news);
    bool new_season = season.season_index != g.season;
    g.season = season.season_index;
    if (new_season || season.round > g.round)
        develop(season.round, season.rounds, news);
    g.round = season.round;
    save();
    swofty::log("[season-dev] season %d round %d of %d: %zu news line(s), %zu teams", g.season, g.round, season.rounds,
                news.size(), g.teams.size());
    if (verbose)
        for (const auto &n : news)
            swofty::log("[season-dev]   %s", n.c_str());
    show_report(g.round, news);
}

// ---------------------------------------------------------------------------------------------------- applying

struct Applied {
    uint32_t generation = 0xffffffffu;
    uint32_t cars = 0;
    std::vector<uint32_t> tokens;
};
static Applied g_applied;
static bool g_dirty = false;

static void unapply()
{
    if (auto *ops = core::cars())
        for (uint32_t tok : g_applied.tokens)
            ops->unlayer(tok);
    g_applied = Applied{};
}

static std::string team_key_of(uint32_t vehicle)
{
    if (auto badge = swofty::live::team_badge(vehicle); badge && !badge->empty())
        return sanitize(*badge);
    if (auto d = swofty::live::car_detail(vehicle); d && swofty::live::has(*d, SWOFTY_LIVE_HAVE_IDENTITY))
        return "team" + std::to_string(d->team_id);
    return {};
}

static uint32_t field_id(const SwoftyCarOps *ops, const char *path, uint32_t fallback)
{
    uint32_t id = 0;
    return ops->field && ops->field(path, &id) == SWOFTY_ASSET_OK ? id : fallback;
}

static void apply_session(const swofty::live::Session &s)
{
    auto *ops = core::cars();
    if (!ops)
        return;
    unapply();
    g_applied.generation = s.generation;
    g_applied.cars = s.cars;

    if (s.player >= 0)
        if (auto key = team_key_of(static_cast<uint32_t>(s.player)); !key.empty())
            g_player_team = key;

    const uint32_t f_grip = field_id(ops, "ai.grip_scale", SWOFTY_CAR_AI_GRIP_SCALE);
    const uint32_t f_power = field_id(ops, "engine.power_scale", SWOFTY_CAR_ENGINE_POWER_SCALE);
    const float k = std::clamp(static_cast<int>(strength), 0, 200) / 100.0f;

    uint32_t count = 0;
    ops->count(&count);
    int applied = 0;
    bool added = false;
    for (uint32_t v = 0; v < count; v++) {
        auto car = swofty::live::car(v);
        if (!car || swofty::live::is_player(*car))
            continue;
        std::string key = team_key_of(v);
        if (key.empty())
            continue;
        if (!team(key)) {
            team_add(key);
            added = true;
        }
        const Team &t = *team(key);
        if (is_player_team(t))
            continue;
        // pace percent -> scales: about 1.6 % AI grip per 1 % chassis pace, 3 % power per 1 % power pace
        float grip = 1.0f + (t.dev_chassis / 100.0f) * 1.6f * k;
        float power = 1.0f + (t.dev_power / 100.0f) * 3.0f * k;
        uint32_t tok = 0;
        if (std::fabs(grip - 1.0f) > 1e-5f && ops->layer(kAddon, v, f_grip, SWOFTY_CAR_OP_SCALE, grip, 0, &tok) == SWOFTY_ASSET_OK)
            g_applied.tokens.push_back(tok);
        if (std::fabs(power - 1.0f) > 1e-5f && ops->layer(kAddon, v, f_power, SWOFTY_CAR_OP_SCALE, power, 0, &tok) == SWOFTY_ASSET_OK)
            g_applied.tokens.push_back(tok);
        applied++;
        if (verbose)
            swofty::log("[season-dev] car %u (%s): grip x%.4f power x%.4f", v, key.c_str(), grip, power);
    }
    if (added)
        save();
    swofty::log("[season-dev] session %u: %d AI car(s) set, %zu layer(s)", s.generation, applied, g_applied.tokens.size());
}

// ---------------------------------------------------------------------------------------------------- race results

struct RaceSnap {
    bool race = false;
    bool chequered = false;
    std::vector<std::string> order;   // team keys in classification order
};
static RaceSnap g_race;

static void snapshot_race(const swofty::live::Session &s)
{
    g_race.race = s.type >= 15 && s.type <= 17;
    if (!g_race.race || !s.chequered)
        return;
    g_race.chequered = true;
    g_race.order.clear();
    for (uint32_t v : swofty::live::order().ids)
        g_race.order.push_back(team_key_of(v));
}

static void commit_race()
{
    if (!g.loaded || !g_race.race || !g_race.chequered || g_race.order.empty())
        return;
    std::vector<std::string> seen;
    for (const auto &key : g_race.order) {
        if (key.empty() || std::find(seen.begin(), seen.end(), key) != seen.end())
            continue;
        seen.push_back(key);
        Team &t = team_add(key);
        t.finish_sum += static_cast<int>(seen.size());   // the team's rank by its best car
        t.races++;
    }
    save();
    swofty::log("[season-dev] race recorded: %zu teams classified", seen.size());
}

// ---------------------------------------------------------------------------------------------------- events

static bool g_hub_pending = false;
static uint32_t g_frames = 0;

static bool career_loaded()
{
    SwoftyCareerState st{};
    return swofty::career::state(&st) == swofty::career::Result::ok && st.loaded;
}

static void on_frame(double, void *)
{
    if (!enabled) {
        if (!g_applied.tokens.empty())
            unapply();
        return;
    }
    if (++g_frames % 15)
        return;   // about four checks a second
    if (!career_loaded())
        return;
    if (g_hub_pending) {
        g_hub_pending = false;
        process_hub();
    }
    auto s = swofty::live::session();
    if (!s || !swofty::live::has(*s, SWOFTY_LIVE_SHAVE_CARS) || s->cars == 0)
        return;
    if (!g.loaded)
        load();
    if (!g.loaded)
        return;
    if (g_dirty || s->generation != g_applied.generation || s->cars != g_applied.cars) {
        g_dirty = false;
        apply_session(*s);
    }
    snapshot_race(*s);
}

static void on_hub(const SwoftyEvent *, void *) { g_hub_pending = true; }

static void on_session_end(const SwoftyEvent *, void *)
{
    commit_race();
    g_race = RaceSnap{};
}

static void on_session_unload(const SwoftyEvent *, void *)
{
    unapply();
    g_race = RaceSnap{};
}

static void on_career_loaded(const SwoftyEvent *, void *)
{
    load();
    g_hub_pending = true;
}

static void on_career_unloaded(const SwoftyEvent *, void *)
{
    unapply();
    g = State{};
    g_player_team.clear();
}

SWOFTY_ON_CHANGE(strength, int) { g_dirty = true; }
SWOFTY_ON_CHANGE(include_player_team, bool) { g_dirty = true; }
SWOFTY_ON_CHANGE(enabled, bool on)
{
    if (on)
        g_dirty = true;
}

SWOFTY_ON_LOAD
{
    load_names();
    namespace ev = swofty::events;
    ev::subscribe(SWOFTY_EVENT_CAREER_LOADED, on_career_loaded);
    ev::subscribe(SWOFTY_EVENT_CAREER_UNLOADED, on_career_unloaded);
    ev::subscribe(SWOFTY_EVENT_CAREER_HUB, on_hub);
    ev::subscribe(SWOFTY_EVENT_SESSION_END, on_session_end);
    ev::subscribe(SWOFTY_EVENT_SESSION_UNLOAD, on_session_unload);
    ev::on_frame(on_frame);
    swofty::log("[season-dev] loaded: car ops %s, standings %s, career saves %s, popup %s, %zu team names",
                core::cars() ? "on" : "off", core::standings() ? "on" : "off", core::saves() ? "on" : "off",
                swofty::popup::available() ? "on" : "off", g_names.size());
}

SWOFTY_ADDON()
