// TrainerPackage fixtures — plain stdlib, no framework, no network.
//
// Apply() is the step that makes a trainer install invisible to the user: the
// staging directory Task 1 extracted is copied file by file into the game
// folder, overwriting what is already there and adding what is not.
//
// These fixtures pin the contract the worker relies on:
//   - an existing file is overwritten and reported with existed == true while a
//     new one is added with existed == false (a future remove feature uses the
//     flag to delete only what the trainer added);
//   - recorded relative paths use forward slashes and record the copied byte
//     count, matching TrainerState's document shape;
//   - the copy is atomic per file: no *.tmp sibling survives a success, and a
//     failure leaves the previous destination bytes untouched;
//   - a destination that cannot be replaced makes Apply fail with ok == false
//     instead of silently reporting an install, and the records of files already
//     copied are returned (their bytes stay on disk);
//   - a missing source or game directory, an empty payload, a destination that
//     is a directory, and a game subdirectory that is a junction (a write
//     through it could land outside the game folder) are all refused rather
//     than copied;
//   - a read-only destination is overwritten (its attribute is cleared) and a
//     stale <dest>.<pid>.tmp from a crashed run is swept away, while a real file
//     that merely ends in ".tmp" (a non-numeric middle segment) is left alone.

#include "Utils/Trainers/TrainerPackage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using Trainers::TrainerPackage::Apply;
using Trainers::TrainerPackage::Result;
using Trainers::TrainerState::FileRecord;

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

// Owns one temp directory; removes it when the case ends, pass or fail.
struct TempDir {
    fs::path path;

    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() /
               (std::string("baestim_trainers_package_") + name))
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

// The record for |path|, or nullptr when Apply did not report it.
const FileRecord* FindRecord(const Result& result, std::string_view path)
{
    for (const FileRecord& record : result.files) {
        if (record.path == path)
            return &record;
    }
    return nullptr;
}

// Any "*.tmp" file left below |dir| (recursively), i.e. a copy that did not
// complete its rename. A walk that cannot finish answers "yes": an enumeration
// failure must never be read as "no leftovers".
bool HasTmpFile(const fs::path& dir)
{
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, ec);
    if (ec)
        return true;

    const fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        std::error_code statusEc;
        if (!it->is_regular_file(statusEc) || statusEc)
            continue;
        const std::string name = it->path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            return true;
    }
    if (ec)
        return true; // cannot tell: treat as a leftover
    return false;
}

// Names of the "*.tmp" regular files directly in |dir|, so a fixture can tell
// "our temp is gone" from "a real file ending in .tmp survived".
std::vector<std::string> TmpNames(const fs::path& dir)
{
    std::vector<std::string> names;

    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::error_code statusEc;
        if (!it->is_regular_file(statusEc) || statusEc)
            continue;
        const std::string name = it->path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            names.push_back(name);
    }
    return names;
}

void TestOverwriteAndAdd()
{
    TempDir tmp("copy");
    const fs::path src  = tmp.path / "src";
    const fs::path game = tmp.path / "game";

    // An empty file and one crossing the 64 KiB chunk boundary: both exercise
    // the copy loop's end-of-file handling, and the recorded size must be the
    // byte count actually written.
    const std::string big(70000, 'x');

    WriteBytes(src / "keep.txt", "new");
    WriteBytes(src / "sub" / "new.dll", "dll");
    WriteBytes(src / "empty.bin", "");
    WriteBytes(src / "big.bin", big);
    WriteBytes(game / "keep.txt", "old");

    const Result result = Apply(src, game);
    CHECK(result.ok);
    CHECK(result.error.empty());
    if (!result.ok) {
        printf("error: %s\n", result.error.c_str());
        return;
    }

    CHECK(result.files.size() == 4);

    // Overwritten: the old bytes are gone and the flag says the destination was
    // already there.
    const FileRecord* keep = FindRecord(result, "keep.txt");
    CHECK(keep != nullptr);
    if (keep != nullptr) {
        CHECK(keep->size == 3);
        CHECK(keep->existed == true);
    }
    CHECK(ReadFileContents(game / "keep.txt") == "new");

    // Added, in a subfolder that did not exist before: recorded with a forward
    // slash and existed == false.
    const FileRecord* dll = FindRecord(result, "sub/new.dll");
    CHECK(dll != nullptr);
    if (dll != nullptr) {
        CHECK(dll->size == 3);
        CHECK(dll->existed == false);
    }
    CHECK(ReadFileContents(game / "sub" / "new.dll") == "dll");

    const FileRecord* empty = FindRecord(result, "empty.bin");
    CHECK(empty != nullptr);
    if (empty != nullptr) {
        CHECK(empty->size == 0);
        CHECK(empty->existed == false);
    }
    CHECK(ReadFileContents(game / "empty.bin").empty());
    CHECK(fs::file_size(game / "empty.bin") == 0);

    const FileRecord* large = FindRecord(result, "big.bin");
    CHECK(large != nullptr);
    if (large != nullptr) {
        CHECK(large->size == big.size());
        CHECK(large->existed == false);
    }
    CHECK(ReadFileContents(game / "big.bin") == big);

    // Paths are portable: no record may carry a backslash.
    for (const FileRecord& record : result.files)
        CHECK(record.path.find('\\') == std::string::npos);

    CHECK(!HasTmpFile(game));
}

void TestLockedDestinationFails()
{
    TempDir tmp("locked");
    const fs::path src  = tmp.path / "src";
    const fs::path game = tmp.path / "game";

    WriteBytes(src / "locked.txt", "new");
    WriteBytes(game / "locked.txt", "old");

    // No sharing: the rename over this file cannot succeed while the handle is
    // open, which is how a running game holds its own binaries.
    const HANDLE handle =
        CreateFileW((game / "locked.txt").c_str(), GENERIC_READ, 0, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);

    const Result result = Apply(src, game);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.files.empty());

    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        // The destination was never touched and no temp copy was left behind.
        CHECK(ReadFileContents(game / "locked.txt") == "old");
        CHECK(!HasTmpFile(game));
    }
}

void TestMissingGameDirFails()
{
    TempDir tmp("missing_game");
    const fs::path src = tmp.path / "src";
    WriteBytes(src / "a.dll", "a");

    const Result result = Apply(src, tmp.path / "does-not-exist");
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.files.empty());
}

void TestMissingSourceDirFails()
{
    TempDir tmp("missing_src");
    const fs::path game = tmp.path / "game";
    WriteBytes(game / "keep.txt", "old");

    const Result result = Apply(tmp.path / "does-not-exist", game);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.files.empty());
    CHECK(ReadFileContents(game / "keep.txt") == "old");
}

void TestDestinationDirectoryFails()
{
    // A file cannot replace a directory: report it instead of leaving a
    // half-applied trainer recorded as installed.
    TempDir tmp("dest_dir");
    const fs::path src  = tmp.path / "src";
    const fs::path game = tmp.path / "game";

    WriteBytes(src / "conflict", "data");
    std::error_code ec;
    fs::create_directories(game / "conflict", ec);
    CHECK(!ec);

    const Result result = Apply(src, game);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.files.empty());
    CHECK(!HasTmpFile(game));
}

void TestEmptySourceFails()
{
    // Zip::ExtractGame can produce a payload with no files. Reporting success
    // would record an install with files: [], which NeedsApply treats as
    // pending forever: the trainer would re-download and re-announce on every
    // start.
    TempDir tmp("empty_src");
    const fs::path src  = tmp.path / "src";
    const fs::path game = tmp.path / "game";

    std::error_code ec;
    fs::create_directories(src, ec);
    fs::create_directories(game, ec);
    CHECK(!ec);

    const Result result = Apply(src, game);
    CHECK(!result.ok);
    CHECK(result.error.find("no files") != std::string::npos);
    CHECK(result.files.empty());

    // Nothing was created in the game folder either.
    size_t entries = 0;
    for (fs::directory_iterator it(game, ec), end; it != end && !ec;
         it.increment(ec))
        ++entries;
    CHECK(!ec);
    CHECK(entries == 0);
}

void TestReadOnlyDestinationReplaced()
{
    // MoveFileExW(REPLACE_EXISTING) refuses a read-only file; the overwrite is
    // the intent, so the attribute is cleared instead of failing the apply.
    TempDir tmp("readonly");
    const fs::path src  = tmp.path / "src";
    const fs::path game = tmp.path / "game";
    const fs::path dest = game / "ro.txt";

    WriteBytes(src / "ro.txt", "new");
    WriteBytes(dest, "old");
    CHECK(SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    CHECK((GetFileAttributesW(dest.c_str()) & FILE_ATTRIBUTE_READONLY) != 0);

    const Result result = Apply(src, game);
    CHECK(result.ok);
    CHECK(result.files.size() == 1);
    const FileRecord* record = FindRecord(result, "ro.txt");
    CHECK(record != nullptr);
    if (record != nullptr) {
        CHECK(record->size == 3);
        CHECK(record->existed == true);
    }
    CHECK(ReadFileContents(dest) == "new");

    const DWORD attributes = GetFileAttributesW(dest.c_str());
    CHECK(attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((attributes & FILE_ATTRIBUTE_READONLY) == 0);
}

void TestStaleTempSwept()
{
    // A crash in another process leaves <dest>.<pid>.tmp behind; the next apply
    // sweeps it instead of leaving it in the game folder forever. Only our own
    // naming is swept: a real file that merely ends in ".tmp" has a non-numeric
    // middle segment and must survive (no backup exists for it).
    TempDir tmp("stale_tmp");
    const fs::path src    = tmp.path / "src";
    const fs::path game   = tmp.path / "game";
    const fs::path stale  = game / "keep.txt.999999.tmp";
    const fs::path realFile = game / "keep.txt.bak.tmp";

    WriteBytes(src / "keep.txt", "new");
    WriteBytes(game / "keep.txt", "old");
    WriteBytes(stale, "half a copy from a crashed run");
    WriteBytes(realFile, "a real file ending in .tmp");

    const Result result = Apply(src, game);
    CHECK(result.ok);
    CHECK(result.files.size() == 1);
    CHECK(ReadFileContents(game / "keep.txt") == "new");

    // Swept: the PID-shaped temp.
    CHECK(!fs::exists(stale));

    // Untouched: a file whose middle segment is not a PID.
    CHECK(fs::exists(realFile));
    CHECK(ReadFileContents(realFile) == "a real file ending in .tmp");

    // The only .tmp entry left is the planted real file.
    const std::vector<std::string> temps = TmpNames(game);
    CHECK(temps.size() == 1);
    CHECK(!temps.empty() && temps.front() == "keep.txt.bak.tmp");
}

void TestPartialRecordsOnFailure()
{
    // Whatever is copied before a failure stays on disk and must be reported:
    // the caller does not advance the state, so those records are what a retry
    // (and, later, a remove feature) sees. The iteration order is not pinned, so
    // the assertions hold for either order.
    TempDir tmp("partial");
    const fs::path src     = tmp.path / "src";
    const fs::path game    = tmp.path / "game";
    const fs::path locked  = game / "b.txt";

    WriteBytes(src / "a.txt", "aaa");
    WriteBytes(src / "b.txt", "bbb");
    WriteBytes(locked, "locked-old");

    const HANDLE handle =
        CreateFileW(locked.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);

    const Result result = Apply(src, game);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.files.size() <= 1);

    for (const FileRecord& record : result.files) {
        // Every record is a real file with the recorded byte count.
        const fs::path path = game / fs::path(record.path);
        CHECK(fs::is_regular_file(path));
        CHECK(fs::file_size(path) == record.size);
        CHECK(record.path == "a.txt"); // only the unlocked file can be copied
        CHECK(ReadFileContents(path) == "aaa");
    }

    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        CHECK(ReadFileContents(locked) == "locked-old");
    }
    CHECK(!HasTmpFile(game));
}

void TestJunctionSubdirRefused()
{
    // A junction below the game folder resolves somewhere else entirely. A copy
    // through it would land outside gameDir, so it must be refused before
    // anything is written there.
    TempDir tmp("junction");
    const fs::path src     = tmp.path / "src";
    const fs::path game    = tmp.path / "game";
    const fs::path outside = tmp.path / "outside";

    WriteBytes(src / "link" / "evil.txt", "evil");
    std::error_code ec;
    fs::create_directories(game, ec);
    fs::create_directories(outside, ec);
    CHECK(!ec);

    const std::wstring command = L"cmd /c mklink /J \"" + (game / "link").wstring() +
                                 L"\" \"" + outside.wstring() + L"\" >nul 2>&1";
    const int status = _wsystem(command.c_str());
    if (status != 0) {
        printf("SKIP %s: cannot create a junction here\n", __func__);
        return;
    }

    const Result result = Apply(src, game);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(!fs::exists(outside / "evil.txt"));
}

} // namespace

int main()
{
    TestOverwriteAndAdd();
    TestLockedDestinationFails();
    TestMissingGameDirFails();
    TestMissingSourceDirFails();
    TestDestinationDirectoryFails();
    TestEmptySourceFails();
    TestReadOnlyDestinationReplaced();
    TestStaleTempSwept();
    TestPartialRecordsOnFailure();
    TestJunctionSubdirRefused();

    printf("%d checks, %d failed\n", g_checksRun, g_checksFailed);
    return g_checksFailed == 0 ? 0 : 1;
}
