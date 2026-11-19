// Trainers::Zip fixtures — plain stdlib, no framework, no network.
//
// Covers the hardened extraction primitive later trainer tasks will call on a
// downloaded bundle:
//   - IsSafeRelativePath: the relative-name contract that keeps a hostile
//     archive inside the staging directory (no traversal, no absolute paths, no
//     drive letters, no Windows-reserved device names, no invalid characters);
//   - ExtractGame: only entries under "game/" are materialised, that prefix is
//     stripped, "/" and "\" in entry names are both accepted, and everything
//     else (outside.txt, __MACOSX/) is ignored;
//   - the limits: per-entry and total uncompressed bytes as well as the entry
//     count abort the extraction before the oversized file is written;
//   - a traversal name and a truncated archive fail closed with ok == false.
//
// Fixtures are written with miniz's writer and live under
// temp_directory_path(); every case cleans its temp directory up.

#include "Utils/Trainers/Zip.h"

#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

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

// ── fixtures ───────────────────────────────────────────────────────────────

struct Entry {
    std::string name;
    std::string data;
};

// Owns one temp directory; removes it (and everything the extraction wrote)
// when the case ends, pass or fail.
struct TempDir {
    fs::path path;

    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() /
               (std::string("baestim_trainers_zip_") + name))
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

static std::string ReadFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};

    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}

static void WriteFile(const fs::path& p, std::string_view bytes)
{
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(out));
}

// Builds a ZIP at zipPath from the given entries, using miniz's writer (the
// same library the extractor reads with). A failed init means there is no
// archive to add to, so the case must not keep using a zeroed writer.
static void BuildZip(const fs::path& zipPath, const std::vector<Entry>& entries)
{
    mz_zip_archive w{};
    const std::string zipName = zipPath.string();
    const mz_bool init = mz_zip_writer_init_file(&w, zipName.c_str(), 0);
    CHECK(init == MZ_TRUE);
    if (init != MZ_TRUE)
        return;

    for (const Entry& e : entries) {
        CHECK(mz_zip_writer_add_mem(&w, e.name.c_str(), e.data.data(),
                                    e.data.size(), MZ_BEST_SPEED) == MZ_TRUE);
    }

    CHECK(mz_zip_writer_finalize_archive(&w) == MZ_TRUE);
    CHECK(mz_zip_writer_end(&w) == MZ_TRUE);
}

static Trainers::Zip::Limits DefaultLimits()
{
    return Trainers::Zip::Limits{};
}

// ── IsSafeRelativePath ─────────────────────────────────────────────────────

static void SafePath_AcceptsNestedFile()
{
    CHECK(Trainers::Zip::IsSafeRelativePath("sub/a.dll"));
    CHECK(Trainers::Zip::IsSafeRelativePath("a.dll"));
    CHECK(Trainers::Zip::IsSafeRelativePath("sub/deeper/b.txt"));
    CHECK(Trainers::Zip::IsSafeRelativePath("data.pak"));
}

static void SafePath_RejectsTraversal()
{
    CHECK(!Trainers::Zip::IsSafeRelativePath("../a"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a/../b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath(".."));
    CHECK(!Trainers::Zip::IsSafeRelativePath("./a"));
}

static void SafePath_RejectsAbsolute()
{
    CHECK(!Trainers::Zip::IsSafeRelativePath("/abs"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("\\abs"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("C:\\x"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("C:/x"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("//server/share"));
}

static void SafePath_RejectsWindowsTraps()
{
    CHECK(!Trainers::Zip::IsSafeRelativePath("CON"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("con"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("sub/NUL"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("com1"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("lpt9.log"));
    // Windows ignores trailing spaces before the extension when matching a
    // device name, so these are CON / COM1 too.
    CHECK(!Trainers::Zip::IsSafeRelativePath("CON .txt"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("COM1 .log"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("sub/nul .dll"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a<b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a:b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a|b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a?b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("a*b"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("trailing."));
    CHECK(!Trainers::Zip::IsSafeRelativePath("trailing "));
    CHECK(!Trainers::Zip::IsSafeRelativePath("sub//b.txt"));
}

static void SafePath_RejectsEmpty()
{
    CHECK(!Trainers::Zip::IsSafeRelativePath(""));
}

static void SafePath_Utf8()
{
    // Valid multi-byte UTF-8 is a legitimate name; malformed sequences are not
    // (std::filesystem refuses to build a path from them).
    CHECK(Trainers::Zip::IsSafeRelativePath("sub/\xC3\xBC.txt"));     // "ü"
    CHECK(Trainers::Zip::IsSafeRelativePath("sub/\xE6\xB5\x8B.txt")); // "测"
    CHECK(!Trainers::Zip::IsSafeRelativePath("bad\xFF.txt"));
    CHECK(!Trainers::Zip::IsSafeRelativePath("bad\xC3"));     // truncated
    CHECK(!Trainers::Zip::IsSafeRelativePath("\xC0\xAF"));    // overlong '/'
    CHECK(!Trainers::Zip::IsSafeRelativePath("\xED\xA0\x80")); // surrogate
}

// ── ExtractGame: payload selection ─────────────────────────────────────────

static void Extract_GameFolderOnly()
{
    TempDir tmp("game_only");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, {
        { "game/a.txt",      "alpha"   },
        { "game/sub/b.txt",  "bravo!"  },
        // A literal backslash inside the entry name: some zippers emit it.
        { "game\\win.txt",   "charlie" },
        { "outside.txt",     "nope"    },
        { "__MACOSX/x",      "junk"    },
    });

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.files == 3);

    CHECK(ReadFile(dest / "a.txt") == "alpha");
    CHECK(ReadFile(dest / "sub" / "b.txt") == "bravo!");
    CHECK(ReadFile(dest / "win.txt") == "charlie");

    // The selected entries land at the root of dest, not under a "game" dir.
    CHECK(!fs::exists(dest / "game"));
    CHECK(!fs::exists(dest / "outside.txt"));
    CHECK(!fs::exists(dest / "__MACOSX"));
}

static void Extract_IgnoresDirectoryEntries()
{
    TempDir tmp("dir_entries");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    // Explicit directory entries (trailing slash) are not files and must not
    // count towards r.files nor trip IsSafeRelativePath on an empty remainder.
    BuildZip(zipPath, {
        { "game/",       ""  },
        { "game/sub/",   ""  },
        { "game/a.txt",  "x" },
    });

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(r.ok);
    CHECK(r.files == 1);
    CHECK(ReadFile(dest / "a.txt") == "x");
}

static void Extract_NestedBackslashName()
{
    TempDir tmp("backslash_nested");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    // Every separator in the entry name is a backslash, not just the one after
    // "game": the whole name is normalised before the prefix is stripped.
    BuildZip(zipPath, { { "game\\sub\\b.txt", "nested" } });

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(r.ok);
    CHECK(r.files == 1);
    CHECK(ReadFile(dest / "sub" / "b.txt") == "nested");
    // The "game" component was consumed, not kept as a directory.
    CHECK(!fs::exists(dest / "game"));
}

static void Extract_RejectsInvalidUtf8EntryName()
{
    TempDir tmp("invalid_utf8");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    // A raw 0xFF byte in the central-directory name: not valid UTF-8, so no
    // path can be built from it. The extraction must fail cleanly, not throw.
    std::string name = "game/bad";
    name.push_back(static_cast<char>(0xFF));
    name += ".txt";

    BuildZip(zipPath, { { name, "pwned" } });

    CHECK(!Trainers::Zip::IsSafeRelativePath(name.substr(5)));

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.files == 0);
    // Named precisely: this is the encoding refusal, not the catch-all.
    CHECK(r.error.find("not valid UTF-8") != std::string::npos);
    CHECK(r.error.find("bad") != std::string::npos);
    CHECK(fs::is_directory(dest));
    CHECK(fs::is_empty(dest));
    CHECK(!fs::exists(tmp.path / "bad.txt"));
}

static void Extract_NoGameEntriesIsSuccess()
{
    TempDir tmp("no_game");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    // An archive with nothing to extract is not an error: a bundle whose
    // payload never shipped yields an empty staging dir and files == 0.
    BuildZip(zipPath, {
        { "readme.txt", "" },
        { "__MACOSX/x", "junk" },
    });

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.files == 0);
    CHECK(fs::is_directory(dest));
    CHECK(fs::is_empty(dest));
}

// ── ExtractGame: refusals ──────────────────────────────────────────────────

static void Extract_RejectsTraversalEntry()
{
    TempDir tmp("traversal");
    const fs::path zipPath = tmp.path / "evil.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, {
        { "game/a.txt",        "alpha" },
        { "game/../evil.txt",  "pwned" },
    });

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, DefaultLimits());

    CHECK(!r.ok);
    CHECK(!r.error.empty());
    // Nothing escaped dest: no evil.txt next to it (miniz may normalise the
    // name, in which case the entry is refused just the same).
    CHECK(!fs::exists(tmp.path / "evil.txt"));
    CHECK(!fs::exists(dest.parent_path().parent_path() / "evil.txt"));
}

static void Extract_RejectsEntryOverByteLimit()
{
    TempDir tmp("entry_limit");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, { { "game/a.txt", "12345678" } });

    Trainers::Zip::Limits limits = DefaultLimits();
    limits.maxEntryBytes = 4;

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, limits);

    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(!fs::exists(dest / "a.txt"));
}

static void Extract_RejectsTotalOverByteLimit()
{
    TempDir tmp("total_limit");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, {
        { "game/a.txt", "1234" },
        { "game/b.txt", "1234" },
    });

    Trainers::Zip::Limits limits = DefaultLimits();
    limits.maxTotalBytes = 6;

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, limits);

    CHECK(!r.ok);
    CHECK(!r.error.empty());
    // The first file fit and stays; the one that would exceed the total does
    // not exist.
    CHECK(r.files == 1);
    CHECK(ReadFile(dest / "a.txt") == "1234");
    CHECK(!fs::exists(dest / "b.txt"));
}

static void Extract_RejectsTooManyEntries()
{
    TempDir tmp("entry_count");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, {
        { "game/a.txt", "a" },
        { "game/b.txt", "b" },
    });

    Trainers::Zip::Limits limits = DefaultLimits();
    limits.maxEntries = 1;

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(zipPath, dest, limits);

    CHECK(!r.ok);
    CHECK(!r.error.empty());
    // The first file is within the count and stays; the one that would exceed
    // maxEntries does not exist.
    CHECK(r.files == 1);
    CHECK(ReadFile(dest / "a.txt") == "a");
    CHECK(!fs::exists(dest / "b.txt"));
}

static void Extract_RejectsTruncatedArchive()
{
    TempDir tmp("truncated");
    const fs::path zipPath = tmp.path / "bundle.zip";
    const fs::path dest    = tmp.path / "dest";

    BuildZip(zipPath, {
        { "game/a.txt", "alpha" },
        { "game/b.txt", "bravo" },
    });

    const std::string whole = ReadFile(zipPath);
    CHECK(whole.size() > 8);
    const fs::path truncatedPath = tmp.path / "truncated.zip";
    WriteFile(truncatedPath, std::string_view(whole).substr(0, whole.size() / 2));

    const Trainers::Zip::ExtractResult r =
        Trainers::Zip::ExtractGame(truncatedPath, dest, DefaultLimits());

    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

static void Extract_RejectsMissingArchive()
{
    TempDir tmp("missing");
    const Trainers::Zip::ExtractResult r = Trainers::Zip::ExtractGame(
        tmp.path / "does-not-exist.zip", tmp.path / "dest", DefaultLimits());

    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

int main()
{
    SafePath_AcceptsNestedFile();
    SafePath_RejectsTraversal();
    SafePath_RejectsAbsolute();
    SafePath_RejectsWindowsTraps();
    SafePath_RejectsEmpty();
    SafePath_Utf8();

    Extract_GameFolderOnly();
    Extract_IgnoresDirectoryEntries();
    Extract_NestedBackslashName();
    Extract_NoGameEntriesIsSuccess();
    Extract_RejectsInvalidUtf8EntryName();
    Extract_RejectsTraversalEntry();
    Extract_RejectsEntryOverByteLimit();
    Extract_RejectsTotalOverByteLimit();
    Extract_RejectsTooManyEntries();
    Extract_RejectsTruncatedArchive();
    Extract_RejectsMissingArchive();

    if (g_checksFailed != 0) {
        printf("FAILED (%d of %d checks)\n", g_checksFailed, g_checksRun);
        return 1;
    }
    printf("ALL OK (%d checks)\n", g_checksRun);
    return 0;
}
