#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

// Hardened ZIP extraction for trainer bundles.
//
// A bundle is an archive published by the trainers backend. Only the payload
// under "game/" is ever materialised, and only inside the caller's staging
// directory: every entry name is checked before that entry is written, and the
// amount of data is bounded.
//
// No exception escapes ExtractGame: a bad archive, a hostile name, a
// filesystem error and allocation failure are all reported through
// ExtractResult, so the caller only has to check ok.

namespace Trainers::Zip {

// Budget for one extraction. The defaults are generous for a real bundle (a
// few DLLs, an injector, config files) and small enough that a hostile or
// corrupt archive cannot fill the disk.
struct Limits {
    uint64_t maxEntryBytes = 2ull * 1024 * 1024 * 1024; // 2 GiB
    uint64_t maxTotalBytes = 4ull * 1024 * 1024 * 1024; // 4 GiB
    size_t   maxEntries    = 2000;
};

struct ExtractResult {
    bool        ok = false;
    size_t      files = 0;
    std::string error;
};

// Extracts every regular file entry that lives under "game/" into destDir,
// with that prefix stripped:
//
//   game/a.dll       -> <destDir>/a.dll
//   game/sub/b.txt   -> <destDir>/sub/b.txt
//   game\win.txt     -> <destDir>/win.txt      (backslash normalised)
//
// Anything else in the archive (top-level files, __MACOSX/, explicit directory
// entries) is ignored and does not count towards limits.maxEntries or
// ExtractResult::files.
//
// Entries under "game/" are checked with IsSafeRelativePath immediately before
// that entry is written; one unsafe or non-UTF-8 name fails the rest of the
// extraction. Limits are enforced the same way, immediately before each write.
//
// On failure ok == false and error carries a short reason. Files written from
// earlier entries are left in place: the caller owns destDir (a staging
// directory) and deletes it on failure. destDir and any needed subdirectories
// are created.
ExtractResult ExtractGame(const std::filesystem::path& zipPath,
                          const std::filesystem::path& destDir,
                          const Limits& limits);

// True when rel is a non-empty, relative, portable sub-path that cannot escape
// the directory it is resolved against:
//   - no absolute paths ("/x", "\x", "C:/x", "//server/share")
//   - no "." or ".." components anywhere
//   - no empty components ("a//b") and no trailing slash
//   - no backslashes (ExtractGame normalises separators before calling this)
//   - no characters Windows refuses or reinterprets (<>:"|?*, control chars)
//   - no component ending in a space or dot (Windows silently strips those)
//   - no Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9),
//     including the padded spellings Windows also treats as devices
//     ("CON .txt" and "COM1 .log" are CON and COM1: the part before the
//     extension is compared after trailing spaces and dots are stripped)
//   - no bytes that are not well-formed UTF-8 (the name becomes a path, and
//     std::filesystem refuses to build one from malformed UTF-8)
bool IsSafeRelativePath(std::string_view rel);

} // namespace Trainers::Zip
