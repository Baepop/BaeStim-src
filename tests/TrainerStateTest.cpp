// TrainerState fixtures — plain stdlib, no framework, no network.
//
// The worker keeps one JSON file next to the DLL config to remember which
// trainer version it applied to which appid:
//
//   {"version":1,"trainers":{"1361510":{"version":"1.0","status":"installed",
//      "sha256":"...","updated_at":"...",
//      "files":[{"path":"Binaries/Win64/x.dll","size":42,"existed":false}]}}}
//
// These fixtures pin the contract the rest of the feature relies on:
//   - Render/Parse roundtrip every recorded field, including the `existed`
//     flag a future remove feature needs;
//   - paths roundtrip through JSON escaping (a quote and a backslash survive);
//   - an empty or truncated document is rejected (Parse returns nullopt, which
//     the caller treats as "no state": everything is pending);
//   - a document with no trainers is a valid, empty state;
//   - entries that fail validation are skipped without costing the good ones,
//     while a fileless entry and an entry without `existed` both survive;
//   - NeedsApply is true for a new appid, a different version, a missing file
//     and a different size, and false only when every recorded file is intact;
//   - Render omits an unrepresentable appid 0, and WriteAtomic leaves the state
//     file with no *.tmp sibling behind (sweeping stale ones from a crash).

#include "Utils/Trainers/TrainerState.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using Trainers::TrainerState::Entry;
using Trainers::TrainerState::FileRecord;
using Trainers::TrainerState::NeedsApply;
using Trainers::TrainerState::Parse;
using Trainers::TrainerState::Render;
using Trainers::TrainerState::State;
using Trainers::TrainerState::WriteAtomic;

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

namespace {

constexpr const char* kSha =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

// Owns one temp directory; removes it when the case ends, pass or fail.
struct TempDir {
    fs::path path;

    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() /
               (std::string("baestim_trainers_state_") + name))
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
        CHECK(!ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string ReadFileContents(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Writes |bytes| to |path|, creating parent directories.
void WriteBytes(const fs::path& path, std::string_view bytes)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    CHECK(!ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(out));
}

State OneEntryState(const std::string& version, std::vector<FileRecord> files)
{
    Entry entry;
    entry.version   = version;
    entry.sha256    = kSha;
    entry.updatedAt = "2026-11-18T12:00:00Z";
    entry.files     = std::move(files);

    State state;
    state.trainers[1361510] = std::move(entry);
    return state;
}

// Any PID-suffixed ".tmp" sibling left behind by a failed/partial write.
bool HasTmpSibling(const fs::path& dir)
{
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            return true;
    }
    return false;
}

void TestRoundtrip()
{
    State state;
    Entry entry;
    entry.version   = "1.0";
    entry.sha256    = kSha;
    entry.updatedAt = "2026-11-18T12:00:00Z";
    entry.files.push_back(FileRecord{"Binaries/Win64/x.dll", 42, false});
    // The flag a future remove feature reads: must survive the roundtrip.
    entry.files.push_back(FileRecord{"TMNT.exe", 1234, true});
    state.trainers[1361510] = entry;

    const std::string json = Render(state);
    CHECK(!json.empty());

    const std::optional<State> parsed = Parse(json);
    CHECK(parsed.has_value());
    if (!parsed)
        return;

    CHECK(parsed->trainers.size() == 1);
    const auto it = parsed->trainers.find(1361510);
    CHECK(it != parsed->trainers.end());
    if (it == parsed->trainers.end())
        return;

    CHECK(it->second.version == "1.0");
    CHECK(it->second.sha256 == kSha);
    CHECK(it->second.updatedAt == "2026-11-18T12:00:00Z");
    CHECK(it->second.files.size() == 2);
    if (it->second.files.size() != 2)
        return;

    CHECK(it->second.files[0].path == "Binaries/Win64/x.dll");
    CHECK(it->second.files[0].size == 42);
    CHECK(it->second.files[0].existed == false);

    CHECK(it->second.files[1].path == "TMNT.exe");
    CHECK(it->second.files[1].size == 1234);
    CHECK(it->second.files[1].existed == true);

    // The spec's persisted shape: a top-level document version and an
    // explicitly "installed" status are part of the file this unit writes.
    CHECK(json.find("\"version\": 1") != std::string::npos);
    CHECK(json.find("\"status\": \"installed\"") != std::string::npos);
    CHECK(json.find("\"1361510\"") != std::string::npos);
}

void TestFilelessEntryRoundtrip()
{
    // files: [] is a shape Render can emit and Parse must accept back: dropping
    // it would silently re-apply (and re-notify) that install on every start.
    State state;
    Entry entry;
    entry.version   = "1.0";
    entry.sha256    = kSha;
    entry.updatedAt = "2026-11-18T12:00:00Z";
    state.trainers[1361510] = entry;

    const std::string json = Render(state);
    CHECK(json.find("\"files\": []") != std::string::npos);

    const std::optional<State> parsed = Parse(json);
    CHECK(parsed.has_value());
    if (!parsed)
        return;
    CHECK(parsed->trainers.size() == 1);
    const auto it = parsed->trainers.find(1361510);
    CHECK(it != parsed->trainers.end());
    if (it == parsed->trainers.end())
        return;
    CHECK(it->second.version == "1.0");
    CHECK(it->second.files.empty());
}

void TestRenderSkipsZeroAppid()
{
    // 0 is the only value the map can hold that the format cannot represent;
    // emitting it would produce a document Parse silently narrows. Skipping it
    // keeps Render's output equal to what Parse reads back.
    State state;
    Entry entry;
    entry.version = "1.0";
    entry.files.push_back(FileRecord{"a.dll", 1, false});
    state.trainers[0] = entry;

    const std::string onlyZero = Render(state);
    CHECK(onlyZero.find("\"0\":") == std::string::npos);
    CHECK(onlyZero.find("\"trainers\": {}") != std::string::npos);
    const std::optional<State> empty = Parse(onlyZero);
    CHECK(empty.has_value());
    if (empty)
        CHECK(empty->trainers.empty());

    state.trainers[7] = entry;
    const std::string mixed = Render(state);
    CHECK(mixed.find("\"0\":") == std::string::npos);
    CHECK(mixed.find("\"7\":") != std::string::npos);
    const std::optional<State> parsed = Parse(mixed);
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->trainers.size() == 1);
        CHECK(parsed->trainers.count(7) == 1);
    }
}

void TestEscaping()
{
    // A backslash and a quote are not what a well-formed bundle produces, but
    // the state file is local data: whatever path was recorded must come back
    // byte-for-byte instead of breaking the document.
    const std::string path = "some\\dir/\"quoted\".dll";
    const State state = OneEntryState("2.0", {FileRecord{path, 7, true}});

    const std::string json = Render(state);
    CHECK(json.find("\\\\") != std::string::npos); // escaped backslash
    CHECK(json.find("\\\"") != std::string::npos); // escaped quote

    const std::optional<State> parsed = Parse(json);
    CHECK(parsed.has_value());
    if (!parsed)
        return;
    const auto it = parsed->trainers.find(1361510);
    CHECK(it != parsed->trainers.end());
    if (it == parsed->trainers.end())
        return;
    CHECK(it->second.files.size() == 1);
    if (it->second.files.size() != 1)
        return;
    CHECK(it->second.files[0].path == path);
    CHECK(it->second.files[0].size == 7);
    CHECK(it->second.files[0].existed == true);
}

void TestRejectsUnusableDocuments()
{
    CHECK(!Parse("").has_value());
    CHECK(!Parse("{").has_value());
    CHECK(!Parse("not json").has_value());
    CHECK(!Parse("[]").has_value());
    // A complete object with no "trainers" object is not a state file.
    CHECK(!Parse(R"({"version":1})").has_value());
    CHECK(!Parse(R"({"version":1,"trainers":[]})").has_value());
    // Truncated inside the trainers object.
    CHECK(!Parse(R"({"version":1,"trainers":{"1":{"version":"1.0"}})").has_value());
    // Trailing garbage after the top-level object.
    CHECK(!Parse(R"({"version":1,"trainers":{}} trailing)").has_value());

    // The minimal valid document is an empty state.
    const std::optional<State> empty = Parse(R"({"version":1,"trainers":{}})");
    CHECK(empty.has_value());
    if (empty)
        CHECK(empty->trainers.empty());

    // Pretty-printed input (the file is meant to be readable) parses too.
    const std::optional<State> pretty = Parse(
        "{\n"
        "  \"version\": 1,\n"
        "  \"trainers\": {\n"
        "    \"1361510\": {\n"
        "      \"version\": \"1.0\",\n"
        "      \"status\": \"installed\",\n"
        "      \"sha256\": \"" + std::string(kSha) + "\",\n"
        "      \"updated_at\": \"2026-11-18T12:00:00Z\",\n"
        "      \"files\": [\n"
        "        { \"path\": \"a.dll\", \"size\": 3, \"existed\": true }\n"
        "      ]\n"
        "    }\n"
        "  }\n"
        "}\n");
    CHECK(pretty.has_value());
    if (pretty) {
        CHECK(pretty->trainers.size() == 1);
        const auto it = pretty->trainers.find(1361510);
        CHECK(it != pretty->trainers.end());
        if (it != pretty->trainers.end()) {
            CHECK(it->second.version == "1.0");
            CHECK(it->second.sha256 == kSha);
            CHECK(it->second.updatedAt == "2026-11-18T12:00:00Z");
            CHECK(it->second.files.size() == 1);
            if (it->second.files.size() == 1) {
                CHECK(it->second.files[0].path == "a.dll");
                CHECK(it->second.files[0].size == 3);
                CHECK(it->second.files[0].existed == true);
            }
        }
    }
}

void TestInvalidEntriesAreSkipped()
{
    // One valid entry first, one last, and every rejected shape between them:
    // one corrupt row must never cost the good ones.
    const std::string body =
        std::string(R"({"version":1,"trainers":{)") +
        R"("1361510":{"version":"1.0","status":"installed","files":[{"path":"a.dll","size":1,"existed":true}]},)"
        // not a decimal appid
        R"("abc":{"version":"1.0","files":[{"path":"x","size":1,"existed":false}]},)"
        // appid 0
        R"("0":{"version":"1.0","files":[{"path":"x","size":1,"existed":false}]},)"
        // past UINT32_MAX
        R"("4294967296":{"version":"1.0","files":[{"path":"x","size":1,"existed":false}]},)"
        // leading zero: ambiguous with "42"
        R"("042":{"version":"1.0","files":[{"path":"x","size":1,"existed":false}]},)"
        // empty version
        R"("42":{"version":"","files":[{"path":"x","size":1,"existed":false}]},)"
        // no files key at all: not a usable entry (unlike "files": [])
        R"("43":{"version":"1.0"},)"
        // empty files array: accepted; NeedsApply treats it as pending
        R"("44":{"version":"1.0","files":[]},)"
        // file without a path
        R"("45":{"version":"1.0","files":[{"size":1,"existed":false}]},)"
        // negative size
        R"("46":{"version":"1.0","files":[{"path":"x","size":-1,"existed":false}]},)"
        // entry value is not an object
        R"("47":"nope",)"
        // file value is not an object
        R"("48":{"version":"1.0","files":["x"]},)"
        // no existed flag: unknown is read as true (keep), never delete
        R"("49":{"version":"1.0","files":[{"path":"noflag.dll","size":3}]},)"
        // trailing valid entry
        R"("730":{"version":"2.0","files":[{"path":"b.dll","size":2,"existed":false}]}}})";

    const std::optional<State> parsed = Parse(body);
    CHECK(parsed.has_value());
    if (!parsed)
        return;

    CHECK(parsed->trainers.size() == 4);
    CHECK(parsed->trainers.count(1361510) == 1);
    CHECK(parsed->trainers.count(44) == 1);
    CHECK(parsed->trainers.count(49) == 1);
    CHECK(parsed->trainers.count(730) == 1);
    const auto first = parsed->trainers.find(1361510);
    if (first != parsed->trainers.end()) {
        CHECK(first->second.files.size() == 1);
        if (first->second.files.size() == 1)
            CHECK(first->second.files[0].existed == true);
    }
    const auto fileless = parsed->trainers.find(44);
    if (fileless != parsed->trainers.end())
        CHECK(fileless->second.files.empty());
    const auto unflagged = parsed->trainers.find(49);
    if (unflagged != parsed->trainers.end()) {
        CHECK(unflagged->second.files.size() == 1);
        if (unflagged->second.files.size() == 1)
            CHECK(unflagged->second.files[0].existed == true);
    }
}

void TestNeedsApply()
{
    TempDir dir("needs");
    const fs::path gameDir = dir.path / "game";
    const fs::path dllPath = gameDir / "Binaries" / "Win64" / "x.dll";
    WriteBytes(dllPath, std::string(42, 'x'));

    const State state =
        OneEntryState("1.0", {FileRecord{"Binaries/Win64/x.dll", 42, false}});

    // Installed version, files intact.
    CHECK(!NeedsApply(state, 1361510, "1.0", gameDir));

    // No entry for this appid.
    CHECK(NeedsApply(state, 730, "1.0", gameDir));
    CHECK(NeedsApply(State{}, 1361510, "1.0", gameDir));

    // A different version is pending even when the files look right.
    CHECK(NeedsApply(state, 1361510, "2.0", gameDir));

    // Same version, different size (a repair/update reverted the file).
    WriteBytes(dllPath, std::string(41, 'x'));
    CHECK(NeedsApply(state, 1361510, "1.0", gameDir));

    // Same version, file deleted.
    std::error_code ec;
    fs::remove(dllPath, ec);
    CHECK(!ec);
    CHECK(NeedsApply(state, 1361510, "1.0", gameDir));

    // A missing game directory is pending, never an exception.
    CHECK(NeedsApply(state, 1361510, "1.0", dir.path / "nope"));

    // An unset game directory cannot vouch for anything: pending (and no
    // relative path is ever resolved against the process CWD).
    CHECK(NeedsApply(state, 1361510, "1.0", fs::path{}));

    // An entry Parse accepts but that records no files cannot vouch for the
    // install either: pending, same as a missing entry.
    State filelessState;
    Entry fileless;
    fileless.version = "1.0";
    filelessState.trainers[1361510] = fileless;
    CHECK(NeedsApply(filelessState, 1361510, "1.0", gameDir));

    // A directory where the recorded file should be is not an intact file.
    const fs::path otherGame = dir.path / "other";
    std::error_code mkEc;
    fs::create_directories(otherGame / "dir.dll", mkEc);
    CHECK(!mkEc);
    const State dirState = OneEntryState("1.0", {FileRecord{"dir.dll", 0, false}});
    CHECK(NeedsApply(dirState, 1361510, "1.0", otherGame));
}

void TestWriteAtomic()
{
    TempDir dir("write");
    const fs::path path = dir.path / "baestim" / "trainers" / "state.json";
    const fs::path parent = path.parent_path();

    const State state =
        OneEntryState("1.0", {FileRecord{"Binaries/Win64/x.dll", 42, false}});

    CHECK(WriteAtomic(path, state));
    CHECK(fs::exists(path));
    CHECK(!HasTmpSibling(parent));

    const std::optional<State> parsed = Parse(ReadFileContents(path));
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(parsed->trainers.size() == 1);
        const auto it = parsed->trainers.find(1361510);
        CHECK(it != parsed->trainers.end());
        if (it != parsed->trainers.end()) {
            CHECK(it->second.version == "1.0");
            CHECK(it->second.files.size() == 1);
            if (it->second.files.size() == 1)
                CHECK(it->second.files[0].existed == false);
        }
    }

    // A crash between write and rename leaves a PID-suffixed sibling; the next
    // writer has a different PID, so it must sweep the stale one before use.
    const fs::path stale = parent / "state.json.999999.tmp";
    WriteBytes(stale, "stale");
    CHECK(fs::exists(stale));
    CHECK(WriteAtomic(path, state));
    CHECK(!fs::exists(stale));
    CHECK(!HasTmpSibling(parent));

    // Replacing an existing file goes through the same temp + rename path and
    // still leaves no temp behind.
    CHECK(WriteAtomic(path, State{}));
    CHECK(!HasTmpSibling(parent));
    const std::optional<State> emptied = Parse(ReadFileContents(path));
    CHECK(emptied.has_value());
    if (emptied)
        CHECK(emptied->trainers.empty());

    // An empty path is refused instead of writing somewhere unintended.
    CHECK(!WriteAtomic(fs::path{}, state));
}

} // namespace

int main()
{
    TestRoundtrip();
    TestFilelessEntryRoundtrip();
    TestRenderSkipsZeroAppid();
    TestEscaping();
    TestRejectsUnusableDocuments();
    TestInvalidEntriesAreSkipped();
    TestNeedsApply();
    TestWriteAtomic();

    printf("%d/%d checks passed\n", g_checksRun - g_checksFailed, g_checksRun);
    return g_checksFailed == 0 ? 0 : 1;
}
