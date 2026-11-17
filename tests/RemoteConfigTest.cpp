// RemoteConfig fixture tests — plain stdlib, no framework.
//
// Covers the unit's two pure layers through RemoteConfig::Test:
//   - ParseResponse (tolerant "appids" scanner over real Edge Function bodies,
//     supabase/functions/config/index.ts);
//   - RenderLua (exact Lua line shapes the LuaConfig reader accepts) and the
//     atomic-write no-churn path of WriteRemoteLua.
//
// These fixtures are the permanent form of the implementation probe; it caught
// two scanner bugs before the first commit, so the cases live on here.

#include "Utils/Remote/RemoteConfig.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// Simple CHECK macro: counts failures, main() returns 1 if any failed.
static int g_checksFailed = 0;
static int g_checksRun    = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checksRun;                                                           \
        if (!(cond)) {                                                           \
            ++g_checksFailed;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

namespace fs = std::filesystem;

// 64 hex chars: the only depot-key shape the Lua reader keeps.
static const char kKey64[] =
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90";

static const uint64_t kSteamId = 76561198000000000ull;

// Real shape of the config Edge Function response: appid is a JSON number,
// key/token travel as strings or null, steamid is always present.
static const char kFullBody[] =
    R"({"steamid":"76561198000000000","appids":[)"
    R"({"appid":1361510,"key":null,"token":null},)"
    R"({"appid":1361511,)"
    R"("key":"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90",)"
    R"("token":"76561198000000000"}]})";

// ── full-Lua (verbatim) mode ────────────────────────────────────────────────
//
// users.lua_text holds a whole client .lua (depot keys, setManifestid,
// addtoken...) and the function returns it as one JSON string, so the DLL must
// write exactly these bytes: no header, no re-render, no trailing newline it
// did not have. kVerbatimLua is the unescaped body of kVerbatimBody.
static const char kVerbatimLua[] =
    "-- 1888930\n"
    "addappid(228988, 0, \"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90\")\n"
    "addappid(1888930)\n"
    "setManifestid(228988, \"1825453925180843246\")\n"
    "addtoken(1888930, \"76561198000000000\")\n";

static const char kVerbatimBody[] =
    R"({"steamid":"76561198000000000","lua":")"
    R"(-- 1888930\n)"
    R"(addappid(228988, 0, \"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90\")\n)"
    R"(addappid(1888930)\n)"
    R"(setManifestid(228988, \"1825453925180843246\")\n)"
    R"(addtoken(1888930, \"76561198000000000\")\n)"
    R"("})";

// ── parser ─────────────────────────────────────────────────────────────────

static void Parse_FullBody()
{
    const RemoteConfig::Test::ParseResult parsed =
        RemoteConfig::Test::ParseResponse(kFullBody);

    CHECK(parsed.keyPresent);
    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 2);
    if (parsed.entries.size() == 2) {
        CHECK(parsed.entries[0].appid == 1361510);
        CHECK(parsed.entries[0].key.empty());
        CHECK(parsed.entries[0].token.empty());
        CHECK(parsed.entries[1].appid == 1361511);
        CHECK(parsed.entries[1].key == kKey64);
        CHECK(parsed.entries[1].token == "76561198000000000");
    }
}

static void Parse_NullFields()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        R"({"steamid":"76561198000000000",)"
        R"("appids":[{"appid":10,"key":null,"token":null}]})");

    CHECK(parsed.keyPresent);
    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 1);
    if (!parsed.entries.empty()) {
        CHECK(parsed.entries[0].appid == 10);
        CHECK(parsed.entries[0].key.empty());
        CHECK(parsed.entries[0].token.empty());
    }
}

static void Parse_KeyOnly()
{
    const std::string body =
        std::string(R"({"steamid":"76561198000000000","appids":[{"appid":20,"key":")") +
        kKey64 + R"(","token":null}]})";
    const RemoteConfig::Test::ParseResult parsed =
        RemoteConfig::Test::ParseResponse(body);

    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 1);
    if (!parsed.entries.empty()) {
        CHECK(parsed.entries[0].key == kKey64);
        CHECK(parsed.entries[0].token.empty());
    }
}

static void Parse_TokenOnly()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        R"({"steamid":"76561198000000000",)"
        R"("appids":[{"appid":21,"key":null,"token":"76561198000000000"}]})");

    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 1);
    if (!parsed.entries.empty()) {
        CHECK(parsed.entries[0].key.empty());
        CHECK(parsed.entries[0].token == "76561198000000000");
    }
}

static void Parse_EmptyArray()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        R"({"steamid":"76561198000000000","appids":[]})");

    // An authoritative empty list: present key + closed array, zero entries.
    CHECK(parsed.keyPresent);
    CHECK(parsed.complete);
    CHECK(parsed.entries.empty());
}

static void Parse_Truncated()
{
    const RemoteConfig::Test::ParseResult parsed =
        RemoteConfig::Test::ParseResponse(R"({"appids":[{"appid":10},{"appid":11)");

    // Cut off inside the array: not authoritative (never cached), but the one
    // complete entry is still usable in memory.
    CHECK(parsed.keyPresent);
    CHECK(!parsed.complete);
    CHECK(parsed.entries.size() == 1);
    if (!parsed.entries.empty())
        CHECK(parsed.entries[0].appid == 10);
}

static void Parse_AppidsAsStringValue()
{
    // "appids" inside a string value is not the key...
    const RemoteConfig::Test::ParseResult noKey =
        RemoteConfig::Test::ParseResponse(R"({"note":"appids"})");
    CHECK(!noKey.keyPresent);
    CHECK(!noKey.complete);
    CHECK(noKey.entries.empty());

    // ...and the real key after it is still found.
    const RemoteConfig::Test::ParseResult found = RemoteConfig::Test::ParseResponse(
        R"({"note":"appids","appids":[{"appid":30}]})");
    CHECK(found.keyPresent);
    CHECK(found.complete);
    CHECK(found.entries.size() == 1);
    if (!found.entries.empty())
        CHECK(found.entries[0].appid == 30);
}

static void Parse_MalformedAppidsAreDropped()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        R"({"appids":[{"appid":0},{"appid":-5},{"appid":4294967296},)"
        R"({"key":"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"},)"
        R"({"appid":40},{"appid":41}]})");

    // Only the two well-formed appids survive; the key-only entry has no appid.
    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 2);
    if (parsed.entries.size() == 2) {
        CHECK(parsed.entries[0].appid == 40);
        CHECK(parsed.entries[1].appid == 41);
    }
}

static void Parse_FieldValidation()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        R"({"appids":[{"appid":50,"key":"short","token":"12a"},)"
        R"({"appid":51,"token":"18446744073709551615"},)"
        R"({"appid":52,"token":"18446744073709551616"}]})");

    CHECK(parsed.entries.size() == 3);
    if (parsed.entries.size() == 3) {
        // A bad key/token only costs that argument; the appid stays unlocked.
        CHECK(parsed.entries[0].appid == 50);
        CHECK(parsed.entries[0].key.empty());
        CHECK(parsed.entries[0].token.empty());
        CHECK(parsed.entries[1].token == "18446744073709551615"); // uint64 max
        CHECK(parsed.entries[2].token.empty());                   // overflow
    }
}

static void Parse_WhitespaceAndNesting()
{
    const RemoteConfig::Test::ParseResult parsed = RemoteConfig::Test::ParseResponse(
        " \r\n { \"appids\" : [\n  { \"appid\" : 60 , \"extra\":[1,2] } ,\n"
        "  { \"appid\" : 61 }\n ] } \n");

    CHECK(parsed.keyPresent);
    CHECK(parsed.complete);
    CHECK(parsed.entries.size() == 2);
    if (parsed.entries.size() == 2) {
        CHECK(parsed.entries[0].appid == 60);
        CHECK(parsed.entries[1].appid == 61);
    }
}

static void Parse_VerbatimLuaBody()
{
    const RemoteConfig::Test::ParseResult parsed =
        RemoteConfig::Test::ParseResponse(kVerbatimBody);

    // Exact body: the newlines and quotes are decoded, nothing is added (no
    // header) and nothing is trimmed.
    CHECK(parsed.luaPresent);
    CHECK(parsed.lua == kVerbatimLua);

    // Full-Lua mode owns the file: the structured list is ignored even when a
    // body carries both keys.
    CHECK(parsed.entries.empty());
    CHECK(!parsed.keyPresent);
    CHECK(!parsed.complete);

    const RemoteConfig::Test::ParseResult both = RemoteConfig::Test::ParseResponse(
        R"({"steamid":"76561198000000000","lua":"addappid(1)\n",)"
        R"("appids":[{"appid":9}]})");
    CHECK(both.luaPresent);
    CHECK(both.lua == "addappid(1)\n");
    CHECK(both.entries.empty());

    // A structured response keeps the old path: no "lua" anywhere.
    const RemoteConfig::Test::ParseResult structured =
        RemoteConfig::Test::ParseResponse(kFullBody);
    CHECK(!structured.luaPresent);
    CHECK(structured.lua.empty());
    CHECK(structured.keyPresent);
    CHECK(structured.complete);
    CHECK(structured.entries.size() == 2);

    // "lua" as a string value, not a key: still the structured answer.
    CHECK(!RemoteConfig::Test::ParseResponse(
               R"({"steamid":"76561198000000000","appids":[],"note":"lua"})")
               .luaPresent);
    // Empty lua body falls back to the structured contract (never a blank file).
    CHECK(!RemoteConfig::Test::ParseResponse(
               R"({"steamid":"76561198000000000","lua":"","appids":[]})")
               .luaPresent);
    // Truncated lua value is unusable: not cached, not written.
    CHECK(!RemoteConfig::Test::ParseResponse(
               "{\"steamid\":\"76561198000000000\",\"lua\":\"addappid(1)")
               .luaPresent);
}

// ── renderer ───────────────────────────────────────────────────────────────
static const char kHeader[] =
    "-- BaeStim remote config for 76561198000000000 \xE2\x80\x94 generated; do not edit\n";

static void Render_LineShapes()
{
    std::vector<RemoteConfig::Entry> entries;
    entries.push_back({1361510, {}, {}});
    entries.push_back({1361511, kKey64, "76561198000000000"});

    size_t emitted = 0;
    const std::string lua =
        RemoteConfig::Test::RenderLua(kSteamId, entries, emitted);

    CHECK(emitted == 2);
    CHECK(lua == std::string(kHeader) +
                     "addappid(1361510)\n"
                     "addappid(1361511, 0, \"" + kKey64 + "\")\n"
                     "addtoken(1361511, \"76561198000000000\")\n");

    // The Lua reader takes the key as argument 3; a 2-arg form would be ignored.
    CHECK(lua.find("addappid(1361511, 0, \"") != std::string::npos);
    CHECK(lua.find("addappid(1361511, \"") == std::string::npos);
}

static void Render_RejectsInvalidEntries()
{
    std::vector<RemoteConfig::Entry> entries;
    entries.push_back({0, {}, {}});                          // appid 0: no Lua
    entries.push_back({70, "short", "12a"});                // bad key + token
    entries.push_back({71, std::string(64, 'z'), {}});      // 64 non-hex chars

    size_t emitted = 0;
    const std::string lua =
        RemoteConfig::Test::RenderLua(kSteamId, entries, emitted);

    CHECK(emitted == 2);
    CHECK(lua.find("addappid(0)") == std::string::npos);
    CHECK(lua.find("addappid(70)\n") != std::string::npos);
    CHECK(lua.find("addtoken(70") == std::string::npos);
    CHECK(lua.find("addappid(71)\n") != std::string::npos);
    CHECK(lua.find("addappid(71, ") == std::string::npos);
    CHECK(lua.find("zzzz") == std::string::npos);
}

static void Render_EmptyListIsHeaderOnly()
{
    size_t emitted = 99;
    const std::string lua =
        RemoteConfig::Test::RenderLua(kSteamId, {}, emitted);

    CHECK(emitted == 0);
    CHECK(lua == kHeader);
}

// ── atomic write ───────────────────────────────────────────────────────────

static bool ReadFile(const fs::path& path, std::string& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

static void Write_AtomicAndNoChurn()
{
    const fs::path root = fs::temp_directory_path() /
        ("baestim_rc_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path luaPath = root / "config" / "baestim" / "baestim-remote.lua";

    std::vector<RemoteConfig::Entry> entries;
    entries.push_back({80, kKey64, {}});

    CHECK(RemoteConfig::WriteRemoteLua(root.string(), kSteamId, entries));

    std::string contents;
    CHECK(ReadFile(luaPath, contents));
    size_t emitted = 0;
    CHECK(contents == RemoteConfig::Test::RenderLua(kSteamId, entries, emitted));

    // No temp file survives a successful write, and the name is PID-suffixed
    // so concurrent Steam child processes never share it.
    bool tmpLeft = false;
    for (const auto& item : fs::directory_iterator(luaPath.parent_path())) {
        if (item.path().filename().string().find(".tmp") != std::string::npos)
            tmpLeft = true;
    }
    CHECK(!tmpLeft);

    // Identical bytes must not touch the file (the watcher would reparse and
    // re-notify every 30 min).
    const fs::file_time_type first = fs::last_write_time(luaPath);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(RemoteConfig::WriteRemoteLua(root.string(), kSteamId, entries));
    CHECK(fs::last_write_time(luaPath) == first);

    // A changed list does rewrite.
    entries.push_back({81, {}, "76561198000000000"});
    CHECK(RemoteConfig::WriteRemoteLua(root.string(), kSteamId, entries));
    std::string rewritten;
    CHECK(ReadFile(luaPath, rewritten));
    CHECK(rewritten.find("addappid(81)") != std::string::npos);
    CHECK(rewritten.find("addtoken(81, \"76561198000000000\")") != std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
}

static void Write_VerbatimExactAndNoChurn()
{
    const fs::path root = fs::temp_directory_path() /
        ("baestim_rc_verbatim_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path luaPath = root / "config" / "baestim" / "baestim-remote.lua";

    // Byte-for-byte the pasted body: no "-- BaeStim remote config for ..."
    // header, no addappid() re-render, no added or removed trailing newline.
    CHECK(RemoteConfig::WriteRemoteLuaVerbatim(root.string(), kVerbatimLua));
    std::string contents;
    CHECK(ReadFile(luaPath, contents));
    CHECK(contents == kVerbatimLua);
    CHECK(contents.size() == sizeof(kVerbatimLua) - 1);

    // Same atomic-write machinery as the structured path: no temp file left.
    bool tmpLeft = false;
    for (const auto& item : fs::directory_iterator(luaPath.parent_path())) {
        if (item.path().filename().string().find(".tmp") != std::string::npos)
            tmpLeft = true;
    }
    CHECK(!tmpLeft);

    // Identical bytes must not wake the watcher.
    const fs::file_time_type first = fs::last_write_time(luaPath);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(RemoteConfig::WriteRemoteLuaVerbatim(root.string(), kVerbatimLua));
    CHECK(fs::last_write_time(luaPath) == first);

    // A changed body (the owner edited users.lua_text) does rewrite.
    const std::string changed = std::string(kVerbatimLua) + "addappid(99)\n";
    CHECK(RemoteConfig::WriteRemoteLuaVerbatim(root.string(), changed));
    std::string rewritten;
    CHECK(ReadFile(luaPath, rewritten));
    CHECK(rewritten == changed);

    // An empty body never truncates the file: it is refused outright.
    CHECK(!RemoteConfig::WriteRemoteLuaVerbatim(root.string(), ""));
    std::string untouched;
    CHECK(ReadFile(luaPath, untouched));
    CHECK(untouched == changed);

    std::error_code ec;
    fs::remove_all(root, ec);
}

int main()
{
    Parse_FullBody();
    Parse_NullFields();
    Parse_KeyOnly();
    Parse_TokenOnly();
    Parse_EmptyArray();
    Parse_Truncated();
    Parse_AppidsAsStringValue();
    Parse_MalformedAppidsAreDropped();
    Parse_FieldValidation();
    Parse_WhitespaceAndNesting();
    Parse_VerbatimLuaBody();

    Render_LineShapes();
    Render_RejectsInvalidEntries();
    Render_EmptyListIsHeaderOnly();

    Write_AtomicAndNoChurn();
    Write_VerbatimExactAndNoChurn();

    if (g_checksFailed != 0) {
        printf("FAILED (%d of %d checks)\n", g_checksFailed, g_checksRun);
        return 1;
    }
    printf("ALL OK (%d checks)\n", g_checksRun);
    return 0;
}
