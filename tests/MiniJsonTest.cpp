// MiniJson fixture tests — plain stdlib, no framework.
//
// Fixtures mirror the REAL response shapes of the tracker APIs (verified
// 2026-11-16):
//   - GitHub:  GET /repos/madoiscool/steam-monitor/commits?sha=pattern&path=steamclient
//   - GitHub:  GET /repos/.../commits/<sha>            (files[].filename)
//   - Supabase: GET /functions/v1/config?steamid=...   (appids[].appid, key, token)
// PatternFallback consumes MiniJson::ExtractStrings with keys
// "sha", "filename" and "name" on the GitHub payloads; RemoteConfig will
// consume MiniJson::ExtractNumbers with key "appid" on the Edge Function one.

#include "Utils/Json/MiniJson.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Simple CHECK macro: counts failures, main() returns 1 if any failed.
static int g_checksFailed = 0;
static int g_checksRun    = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checksRun;                                                           \
        if (!(cond)) {                                                           \
            ++g_checksFailed;                                                    \
            wprintf(L"FAIL %hs:%d: %hs\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

// Real shape of the commits-list response: GET /repos/madoiscool/steam-monitor/commits?sha=pattern&path=steamclient
static const char kCommitsList[] = R"([
 {"sha":"6b287b3be5dae23a75c5a6d19e5bd74e5bc77412",
  "commit":{"tree":{"sha":"263a890afe9b07c50455da8abc379ffcc302d675"}},
  "parents":[{"sha":"6786d808a359cfa820048b1474d75f3a342e63d2"}]}
])";
// Real shape of a commit-detail response: GET /repos/.../commits/<sha> (files[].filename)
static const char kCommitDetail[] = R"({"files":[
 {"filename":"steamclient/0156589a0747e298c64be5902cc8934a99d468ddb8f7249f8be87f6fa50f15a6.toml"},
 {"filename":"steamui/067e232ff8dea355142f5d8065924f5078afbb907fcf10ad6700fa44106ab67f.toml"},
 {"filename":"README.md"}]})";
// Real shape of the config Edge Function response (supabase/functions/config/
// index.ts): appid is a JSON number; key/token travel as strings (or null) so
// no precision is lost. token is a steamid-like uint64, key is 64 hex chars.
static const char kEdgeConfig[] =
    R"({"steamid":"76561198000000000","appids":[)"
    R"({"appid":1361510,"key":null,"token":null},)"
    R"({"appid":1361511,)"
    R"("key":"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90",)"
    R"("token":"76561198000000000"}]})";

// Real shape of the full-Lua response (supabase/functions/config/index.ts): the
// owner pastes a whole client .lua into users.lua_text and the function returns
// it as one JSON string, so every newline and every Lua string quote is escaped
// with \n and \". The client must hand the file back byte-for-byte, which is
// exactly what ExtractJsonString has to undo.
static const char kVerbatimLuaResponse[] =
    R"({"steamid":"76561198000000000","lua":")"
    R"(-- 1888930\n)"
    R"(addappid(228988, 0, \"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90\")\n)"
    R"(addappid(1888930)\n)"
    R"(setManifestid(228988, \"1825453925180843246\")\n)"
    R"(addtoken(1888930, \"76561198000000000\")\n)"
    R"("})";

static void ExtractStrings_FindsCommitShaFirst()
{
    const std::vector<std::string> values =
        MiniJson::ExtractStrings(kCommitsList, "sha");

    CHECK(values.size() == 3);
    if (values.size() == 3) {
        // Document order: the commit sha first, then tree, then parent.
        CHECK(values[0] == "6b287b3be5dae23a75c5a6d19e5bd74e5bc77412");
        CHECK(values[1] == "263a890afe9b07c50455da8abc379ffcc302d675");
        CHECK(values[2] == "6786d808a359cfa820048b1474d75f3a342e63d2");
    }
}

static void ExtractStrings_FindsFilenames()
{
    const std::vector<std::string> values =
        MiniJson::ExtractStrings(kCommitDetail, "filename");

    CHECK(values.size() == 3);
    if (values.size() == 3) {
        CHECK(values[0] ==
              "steamclient/0156589a0747e298c64be5902cc8934a99d468ddb8f7249f8be87f6fa50f15a6.toml");
        CHECK(values[1] ==
              "steamui/067e232ff8dea355142f5d8065924f5078afbb907fcf10ad6700fa44106ab67f.toml");
        CHECK(values[2] == "README.md");
    }
}

static void ExtractStrings_IgnoresKeysInsideStringValues()
{
    // The word "sha" appears inside a string VALUE (escaped quotes); only the
    // real key must match.
    const std::vector<std::string> values = MiniJson::ExtractStrings(
        R"({"note":"fake \"sha\" inside","sha":"real"})", "sha");

    CHECK(values.size() == 1);
    if (values.size() == 1) {
        CHECK(values[0] == "real");
    }
}

static void ExtractStrings_TruncatedJsonIsSafe()
{
    const std::string_view full(kCommitsList);
    const std::string_view truncated = full.substr(0, full.size() / 2);

    // Malformed/truncated input must not crash, and the result must be
    // deterministic: the matches of the parsed prefix, if any.
    const std::vector<std::string> first =
        MiniJson::ExtractStrings(truncated, "sha");
    const std::vector<std::string> second =
        MiniJson::ExtractStrings(truncated, "sha");

    CHECK(first == second);
    CHECK(first.size() <= 3);
    for (const std::string& value : first) {
        const bool knownSha =
            value == "6b287b3be5dae23a75c5a6d19e5bd74e5bc77412" ||
            value == "263a890afe9b07c50455da8abc379ffcc302d675" ||
            value == "6786d808a359cfa820048b1474d75f3a342e63d2";
        CHECK(knownSha);
    }
}

static void ExtractStrings_NoMatchReturnsEmpty()
{
    const std::vector<std::string> values =
        MiniJson::ExtractStrings(kCommitsList, "no_such_key");

    CHECK(values.empty());
}

// --- ExtractNumbers -------------------------------------------------------

static void CheckNumbers(const std::vector<int64_t>& values, std::initializer_list<int64_t> expected)
{
    CHECK(values.size() == expected.size());
    if (values.size() != expected.size())
        return;
    std::size_t i = 0;
    for (const int64_t want : expected) {
        CHECK(values[i] == want);
        ++i;
    }
}

static void ExtractNumbers_FindsAppidsInEdgeResponse()
{
    // Contract fixture: the appids array of the Edge Function response. Only
    // the numeric field is picked up; the steamid and token strings are not.
    const std::vector<int64_t> values =
        MiniJson::ExtractNumbers(kEdgeConfig, "appid");

    CheckNumbers(values, {1361510, 1361511});
}

static void ExtractNumbers_IgnoresStringsAndFloats()
{
    // A string that looks like a number, a float, and a key embedded inside a
    // string value must all be discarded; only true integers match.
    // (token travels as a string precisely so it is NOT seen as a number.)
    const char* json =
        R"({"appid":1361510,"x":"1361511","y":13.7,"token":"76561198000000000",)"
        R"("note":"fake \"appid\": 99","z":-42,"w":1e3})";

    CheckNumbers(MiniJson::ExtractNumbers(json, "appid"), {1361510});

    // Sibling fields with numeric values are matched only under their own key.
    CheckNumbers(MiniJson::ExtractNumbers(json, "z"), {-42});
    CheckNumbers(MiniJson::ExtractNumbers(json, "y"), {});
    CheckNumbers(MiniJson::ExtractNumbers(json, "w"), {});
    CheckNumbers(MiniJson::ExtractNumbers(json, "token"), {});
}

static void ExtractNumbers_TruncatedJsonIsSafe()
{
    const std::string_view full(kEdgeConfig);
    const std::string_view truncated = full.substr(0, full.size() / 2);

    const std::vector<int64_t> first = MiniJson::ExtractNumbers(truncated, "appid");
    const std::vector<int64_t> second = MiniJson::ExtractNumbers(truncated, "appid");

    CHECK(first == second); // deterministic
    for (const int64_t value : first) {
        const bool knownAppid = value == 1361510 || value == 1361511;
        CHECK(knownAppid); // never a half-parsed prefix of a number
    }

    // Cut in the middle of a number: the partial digits must not be emitted.
    // A number cut off by the end of the input is discarded as well — we
    // cannot tell whether more digits were coming, and a silently wrong appid
    // is worse than a missing one.
    CHECK(MiniJson::ExtractNumbers(R"({"appid":136)", "appid").empty());
    CHECK(MiniJson::ExtractNumbers(R"({"appid":1361510)", "appid").empty());
    // Complete value with its terminator intact: still extracted.
    CHECK(MiniJson::ExtractNumbers(R"({"appid":1361510})", "appid").size() == 1);
    // Cut right after the key, before the colon or value.
    CHECK(MiniJson::ExtractNumbers(R"({"appid")", "appid").empty());
    CHECK(MiniJson::ExtractNumbers(R"({"appid":)", "appid").empty());
    // Truncated array: the complete first entry survives, the cut-off second
    // is discarded instead of yielding a bogus appid.
    CheckNumbers(MiniJson::ExtractNumbers(R"({"appids":[{"appid":10},{"appid":11)", "appid"), {10});
}

static void ExtractNumbers_NoMatchReturnsEmpty()
{
    CHECK(MiniJson::ExtractNumbers(kEdgeConfig, "no_such_key").empty());
    CHECK(MiniJson::ExtractNumbers("", "appid").empty());
    CHECK(MiniJson::ExtractNumbers("{}", "appid").empty());
}

static void ExtractNumbers_HugeNumberDoesNotOverflow()
{
    // Out-of-int64-range integers are skipped rather than wrapping around
    // (no signed-overflow UB), and neighbours keep working.
    const char* json =
        R"({"appid":9223372036854775808,"appids":[)"
        R"({"appid":1361510},{"appid":-9223372036854775809},)"
        R"({"appid":99999999999999999999999999999999999999}]})";

    CheckNumbers(MiniJson::ExtractNumbers(json, "appid"), {1361510});

    // The int64 boundaries themselves are still exact.
    CHECK(MiniJson::ExtractNumbers(
              R"({"appid":9223372036854775807})", "appid") ==
          std::vector<int64_t>{INT64_MAX});
    CHECK(MiniJson::ExtractNumbers(
              R"({"appid":-9223372036854775808})", "appid") ==
          std::vector<int64_t>{INT64_MIN});
}

// --- ExtractJsonString ----------------------------------------------------

static void ExtractJsonString_UnescapesLuaBody()
{
    // The escaped body of kVerbatimLuaResponse, unescaped, must be exactly the
    // Lua source the owner pasted: real newlines, real quotes, nothing added
    // and nothing lost.
    const std::string lua = MiniJson::ExtractJsonString(kVerbatimLuaResponse, "lua");

    const std::string expected =
        "-- 1888930\n"
        "addappid(228988, 0, \"a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90\")\n"
        "addappid(1888930)\n"
        "setManifestid(228988, \"1825453925180843246\")\n"
        "addtoken(1888930, \"76561198000000000\")\n";

    CHECK(!lua.empty());
    CHECK(lua == expected);

    // Only the FIRST value bound to the key is returned (the contract has one),
    // and the remaining escape sequences are decoded too: \\ -> \, \t, \r and
    // the BMP \uXXXX form (U+00E9 -> C3 A9).
    CHECK(MiniJson::ExtractJsonString(R"({"lua":"a\\b"})", "lua") == "a\\b");
    CHECK(MiniJson::ExtractJsonString(R"({"lua":"a\tb"})", "lua") == "a\tb");
    CHECK(MiniJson::ExtractJsonString(R"({"lua":"a\rb"})", "lua") == "a\rb");
    // (Plain literals below: MSVC reads \uXXXX as a UCN even inside a raw
    // string, so the escape has to reach the parser via \\u.)
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"a\\u00e9b\"}", "lua") ==
          "a\xC3\xA9"
          "b");
    // A surrogate pair decodes to one 4-byte UTF-8 code point (U+1F600).
    CHECK(MiniJson::ExtractJsonString(
              "{\"lua\":\"\\ud83d\\ude00\"}", "lua") == "\xF0\x9F\x98\x80");

    CHECK(MiniJson::ExtractJsonString(R"({"lua":"first","lua":"second"})", "lua") ==
          "first");
}

static void ExtractJsonString_NoMatchReturnsEmpty()
{
    // Absent key, key used as a string value only, and empty documents.
    CHECK(MiniJson::ExtractJsonString(kEdgeConfig, "lua").empty());
    CHECK(MiniJson::ExtractJsonString(R"({"note":"lua"})", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("{}", "lua").empty());

    // Malformed input yields "" rather than a guessed body: a verbatim file
    // built from half a JSON string would break the game's Lua.
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"abc", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"abc\\\"def", "lua").empty());
    CHECK(MiniJson::ExtractJsonString(R"({"lua":123})", "lua").empty());
    CHECK(MiniJson::ExtractJsonString(R"({"lua":null})", "lua").empty());
    // Unknown escape and incomplete \u escape are not valid JSON.
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"a\\qb\"}", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"a\\u12\"}", "lua").empty());
    CHECK(MiniJson::ExtractJsonString("{\"lua\":\"a\\ud83d\"}", "lua").empty());
}

int main()
{
    ExtractStrings_FindsCommitShaFirst();
    ExtractStrings_FindsFilenames();
    ExtractStrings_IgnoresKeysInsideStringValues();
    ExtractStrings_TruncatedJsonIsSafe();
    ExtractStrings_NoMatchReturnsEmpty();

    ExtractNumbers_FindsAppidsInEdgeResponse();
    ExtractNumbers_IgnoresStringsAndFloats();
    ExtractNumbers_TruncatedJsonIsSafe();
    ExtractNumbers_NoMatchReturnsEmpty();
    ExtractNumbers_HugeNumberDoesNotOverflow();

    ExtractJsonString_UnescapesLuaBody();
    ExtractJsonString_NoMatchReturnsEmpty();

    wprintf(L"%d/%d checks passed\n", g_checksRun - g_checksFailed, g_checksRun);
    return g_checksFailed == 0 ? 0 : 1;
}
