#include "Utils/Trainers/TrainerPackage.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <new>
#include <string>
#include <system_error>
#include <utility>

// GetCurrentProcessId for the temp-file suffix. NOMINMAX keeps windows.h's
// "max" macro from eating std::...::max elsewhere.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Implementation notes
//
// - The source is walked with recursive_directory_iterator, which by default
//   does not descend into links, and every entry is inspected with
//   GetFileAttributesW: the reparse-point bit means a junction or symlink, and
//   such an entry is refused instead of followed. That check is repeated for
//   every parent directory of the destination, so a junction planted inside the
//   game folder cannot redirect a write outside it.
// - The relative path comes from std::filesystem::relative and is then checked
//   component by component: absolute, rooted, drive-qualified, "." and ".."
//   are refused. The destination is joined below gameDir only after that.
// - Each file is streamed in 64 KiB chunks into <dest>.<pid>.tmp and moved over
//   the destination with MoveFileExW(MOVEFILE_REPLACE_EXISTING |
//   MOVEFILE_WRITE_THROUGH). The temp lives next to the destination, so the
//   move is a same-volume rename: a reader sees the previous file or the
//   complete new one, never a truncated one. A failed copy removes the temp and
//   keeps the previous destination bytes.
// - The temp name is PID-suffixed for the same reason TrainerState's is: Steam
//   hosts the DLL in more than one process, and a fixed ".tmp" could let a
//   second writer truncate the first one's file. Apply itself is called from one
//   worker, and a retry after a crash overwrites the stale temp; temps left by
//   another PID are swept before each copy (best effort, regular files only).
// - A read-only destination has that attribute cleared just before the move,
//   because MoveFileExW(REPLACE_EXISTING) would otherwise fail with
//   ERROR_ACCESS_DENIED and the overwrite is the intent.
// - The walk must be complete and the payload must contain at least one file.
//   An increment error empties recursive_directory_iterator, so it is checked in
//   the loop condition and again after the loop; a partial walk or an empty
//   payload fails the apply instead of being recorded as an install (a zero-file
//   entry is pending forever in TrainerState::NeedsApply).
// - No backup is taken: overwriting the game's file is the intent, and the
//   `existed` flag recorded for the state is enough for a future remove.
// - No exception escapes Apply: every expected failure is reported through
//   Result, and the catch-all is the backstop for allocation failure or a path
//   the standard library refuses to build.

namespace Trainers::TrainerPackage {
namespace {

namespace fs = std::filesystem;

constexpr size_t kBufferBytes       = 64 * 1024;
constexpr size_t kMaxErrorPathBytes = 120;

// What the filesystem says about one path, without following links:
// GetFileAttributesW reports the reparse point itself.
struct Probe {
    bool  exists    = false;
    bool  directory = false;
    bool  reparse   = false;
    DWORD error     = 0; // 0 also means "not found": see ProbePath
};

Probe ProbePath(const fs::path& path)
{
    Probe probe;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        probe.error = GetLastError();
        // Absent is a normal answer (the caller creates it); anything else
        // (access denied, an unusable name) must not be mistaken for it.
        if (probe.error == ERROR_FILE_NOT_FOUND ||
            probe.error == ERROR_PATH_NOT_FOUND)
            probe.error = 0;
        return probe;
    }

    probe.exists    = true;
    probe.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    probe.reparse   = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    return probe;
}

// UTF-8 bytes with forward slashes, matching the state format's relative paths.
std::string PortablePath(const fs::path& path)
{
    const std::u8string u8 = path.generic_u8string();
    std::string out;
    out.reserve(u8.size());
    for (const char8_t c : u8)
        out.push_back(static_cast<char>(c));
    return out;
}

// A path for an error message: shortened and never an exception source (a name
// the narrow/UTF-8 conversion refuses must not turn into a crash).
std::string DescribePath(const fs::path& path)
{
    try {
        std::string out = PortablePath(path);
        if (out.size() > kMaxErrorPathBytes) {
            out.resize(kMaxErrorPathBytes);
            out += "...";
        }
        return out;
    } catch (...) {
        return "<unprintable path>";
    }
}

// A relative payload path that cannot leave the directory it is joined to:
// non-empty, not absolute or rooted, no "." or ".." component and no empty one.
bool IsSafeRelative(const fs::path& rel)
{
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() ||
        rel.has_root_directory())
        return false;

    for (const fs::path& component : rel) {
        if (component.empty() || component == "." || component == "..")
            return false;
    }
    return true;
}

// Creates the destination's parent directories below |gameDir|, refusing an
// existing link on the way, and reports whether a file already occupies the
// destination (sampled before anything is written). |rel| passed IsSafeRelative.
bool PrepareDestination(const fs::path& gameDir, const fs::path& rel,
                        fs::path& dest, bool& existed, std::string& error)
{
    fs::path current = gameDir;
    const fs::path parentRel = rel.parent_path();

    for (const fs::path& component : parentRel) {
        current /= component;

        const Probe probe = ProbePath(current);
        if (probe.error != 0) {
            error = "package: cannot inspect directory " + DescribePath(current);
            return false;
        }
        if (probe.reparse) {
            // A write below this directory would land wherever the link points.
            error = "package: unsafe directory (link): " + DescribePath(current);
            return false;
        }
        if (!probe.exists) {
            std::error_code ec;
            fs::create_directory(current, ec);
            if (ec) {
                error = "package: cannot create directory " + DescribePath(current);
                return false;
            }
            continue;
        }
        if (!probe.directory) {
            error = "package: not a directory: " + DescribePath(current);
            return false;
        }
    }

    dest = current / rel.filename();

    const Probe probe = ProbePath(dest);
    if (probe.error != 0) {
        error = "package: cannot inspect destination " + DescribePath(dest);
        return false;
    }
    if (probe.reparse) {
        error = "package: unsafe destination (link): " + DescribePath(dest);
        return false;
    }
    if (probe.directory) {
        error = "package: destination is a directory: " + DescribePath(dest);
        return false;
    }

    existed = probe.exists;
    return true;
}

// Removes <dest>.<pid>.tmp siblings left by a crashed run in another process
// (this process's own temp is recreated below). Same shape as
// TrainerState::WriteAtomic's sweep: regular files only, so a directory named
// like a temp is not ours to delete; native (wide) comparison, so a name the
// narrow encoding cannot represent cannot throw; best effort, because a sweep
// that cannot list or remove is not a reason to fail the copy. A live writer's
// temp may be swept too; its rename then fails and it reports a failure without
// touching the published file, the same outcome as any other failed write.
// The middle segment must be decimal digits only: that is the only name this
// unit writes, so a genuine file of the payload or the game ("config.ini.bak.tmp")
// is never treated as our leftover.
void RemoveStaleTemps(const fs::path& dest)
{
    const fs::path dir = dest.parent_path();
    if (dir.empty())
        return; // Apply always joins below gameDir, so this is only defensive

    const std::wstring prefix = dest.filename().native() + L".";

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return;

    const fs::directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const std::wstring name = it->path().filename().native();
        if (name.size() <= prefix.size() + 4) // no room for a non-empty pid
            continue;
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (name.compare(name.size() - 4, 4, L".tmp") != 0)
            continue;

        const auto middleBegin =
            name.begin() + static_cast<std::ptrdiff_t>(prefix.size());
        const auto middleEnd = name.end() - 4;
        const bool pidShaped =
            std::all_of(middleBegin, middleEnd, [](wchar_t c) {
                return c >= L'0' && c <= L'9';
            });
        if (!pidShaped)
            continue;

        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc)
            continue;

        std::error_code removeEc;
        fs::remove(it->path(), removeEc);
    }
}

// Streams |source| into <dest>.<pid>.tmp and moves it over |dest|. On success
// |size| is the number of bytes written, which is what the state records. On
// failure the temp is removed, |dest| is left as it was, and |error| is set.
bool CopyFileAtomic(const fs::path& source, const fs::path& dest,
                    uint64_t& size, std::string& error)
{
    RemoveStaleTemps(dest);

    fs::path temp = dest;
    temp += "." + std::to_string(GetCurrentProcessId()) + ".tmp";

    bool ok = false;
    {
        std::ifstream in(source, std::ios::binary);
        if (!in) {
            error = "package: cannot open " + DescribePath(source);
        } else {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                error = "package: cannot create " + DescribePath(temp);
            } else {
                char    buffer[kBufferBytes];
                uint64_t written = 0;

                for (;;) {
                    in.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
                    const std::streamsize got = in.gcount();
                    if (got > 0) {
                        out.write(buffer, got);
                        if (!out) {
                            error = "package: cannot write " + DescribePath(temp);
                            break;
                        }
                        written += static_cast<uint64_t>(got);
                    }
                    if (in.eof()) {
                        ok = true; // the whole file, including an empty one
                        break;
                    }
                    if (!in) {
                        error = "package: cannot read " + DescribePath(source);
                        break;
                    }
                }

                if (ok) {
                    out.flush();
                    out.close(); // a buffered/failed close invalidates the copy
                    if (!out) {
                        ok    = false;
                        error = "package: cannot finish " + DescribePath(temp);
                    } else {
                        size = written;
                    }
                }
            }
        }
    }

    std::error_code ec;
    if (!ok) {
        fs::remove(temp, ec); // a half-written copy, if any
        return false;
    }

    // Overwriting is the intent, so a read-only destination is cleared first:
    // MoveFileExW(REPLACE_EXISTING) refuses it with ERROR_ACCESS_DENIED.
    const DWORD attributes = GetFileAttributesW(dest.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        const DWORD writable =
            attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        if (!SetFileAttributesW(dest.c_str(), writable)) {
            error = "package: cannot clear the read-only attribute of " +
                    DescribePath(dest) + " (error " +
                    std::to_string(GetLastError()) + ")";
            fs::remove(temp, ec);
            return false;
        }
    }

    if (!MoveFileExW(temp.c_str(), dest.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "package: cannot replace " + DescribePath(dest) +
                " (error " + std::to_string(GetLastError()) + ")";
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

// Builds the result in |result| so a late exception cannot lose the records of
// files already copied.
void ApplyImpl(const fs::path& sourceDir, const fs::path& gameDir,
               Result& result)
{
    if (gameDir.empty()) {
        result.error = "package: empty game directory";
        return;
    }
    if (sourceDir.empty()) {
        result.error = "package: empty source directory";
        return;
    }

    const Probe game = ProbePath(gameDir);
    if (game.error != 0 || !game.exists || !game.directory) {
        // Without a real directory every relative path would resolve against
        // the wrong place (or against the process CWD).
        result.error = "package: game directory is not available: " +
                       DescribePath(gameDir);
        return;
    }

    const Probe sourceRoot = ProbePath(sourceDir);
    if (sourceRoot.error != 0 || !sourceRoot.exists || !sourceRoot.directory) {
        result.error = "package: source directory is not available: " +
                       DescribePath(sourceDir);
        return;
    }

    std::error_code ec;
    fs::recursive_directory_iterator it(sourceDir, ec);
    if (ec) {
        result.error = "package: cannot enumerate " + DescribePath(sourceDir);
        return;
    }

    const fs::recursive_directory_iterator end;
    // An increment error empties the iterator, so |ec| is checked in the
    // condition and after the loop: a walk that stopped early must fail the
    // whole apply, or the caller would record the files that were reached as a
    // complete install.
    for (; it != end && !ec; it.increment(ec)) {
        const fs::path source = it->path();

        const Probe probe = ProbePath(source);
        if (probe.error != 0) {
            result.error = "package: cannot inspect " + DescribePath(source);
            return;
        }
        if (probe.reparse) {
            // Not something Zip::ExtractGame produces, and following it would
            // copy whatever it points at.
            result.error = "package: unsafe source entry (link): " +
                           DescribePath(source);
            return;
        }
        if (probe.directory)
            continue; // the payload root's subfolders: their files are visited
        if (!probe.exists) {
            result.error = "package: source entry disappeared: " +
                           DescribePath(source);
            return;
        }

        std::error_code relEc;
        const fs::path rel = fs::relative(source, sourceDir, relEc);
        if (relEc || !IsSafeRelative(rel)) {
            result.error = "package: unsafe source path: " + DescribePath(source);
            return;
        }

        // Built before the copy: a path the narrow/UTF-8 conversion refuses must
        // fail the file with nothing written, not leave a copied file that has
        // no record (the caller would not be able to account for it).
        const std::string recordPath = PortablePath(rel);

        fs::path    dest;
        bool        existed = false;
        std::string error;
        if (!PrepareDestination(gameDir, rel, dest, existed, error)) {
            result.error = std::move(error);
            return;
        }

        uint64_t size = 0;
        if (!CopyFileAtomic(source, dest, size, error)) {
            result.error = std::move(error);
            return;
        }

        TrainerState::FileRecord record;
        record.path    = recordPath;
        record.size    = size;
        record.existed = existed;
        result.files.push_back(std::move(record));
    }

    if (ec) {
        result.error = "package: cannot enumerate " + DescribePath(sourceDir);
        return;
    }

    // A payload with no files is not an install: TrainerState::NeedsApply
    // treats a zero-file entry as pending forever, so reporting success here
    // would re-download and re-announce the same trainer on every start.
    if (result.files.empty()) {
        result.error = "package: source contains no files";
        return;
    }

    result.ok = true;
}

} // namespace

Result Apply(const std::filesystem::path& sourceDir,
             const std::filesystem::path& gameDir)
{
    Result result;

    // No exception may escape: the walk and every path construction allocate,
    // and std::filesystem throws on malformed names. Unsafe paths, locked
    // destinations and I/O errors are already reported through Result; this is
    // the backstop, and it keeps the records of files already copied.
    try {
        ApplyImpl(sourceDir, gameDir, result);
    } catch (const std::bad_alloc&) {
        result.ok    = false;
        result.error = "package: out of memory";
    } catch (const std::exception&) {
        result.ok    = false;
        result.error = "package: cannot build destination path";
    } catch (...) {
        result.ok    = false;
        result.error = "package: unexpected failure";
    }
    return result;
}

} // namespace Trainers::TrainerPackage
