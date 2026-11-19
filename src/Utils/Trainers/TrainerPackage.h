#pragma once
#include "Utils/Trainers/TrainerState.h"

#include <filesystem>
#include <string>
#include <vector>

// Copies an extracted trainer payload into a game install folder.
//
// Task 1 extracts a bundle into a staging directory whose root is exactly the
// payload ("TMNT.exe", "Binaries/Win64/x.dll", ...); this unit is what makes the
// install invisible to the user: every staged file is written over the game's
// copy (or added when it is not there yet), then the staging directory is
// deleted by the caller.
//
// Two properties matter to the worker:
//
//   - the copy is atomic per file. Each file is streamed into a PID-suffixed
//     sibling (<dest>.<pid>.tmp) and moved into place with MoveFileExW and
//     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, so a running game
//     either sees the previous file or the complete new one, never a truncated
//     one. No backup is taken: overwriting is the intent.
//   - a destination outside the game folder is never written. Only paths that
//     resolve below the game directory are accepted, and a subdirectory that is
//     a reparse point (junction/symlink) is refused, because a write through it
//     would land wherever the link points.
//
// Apply reports what it did through TrainerState::FileRecord: the relative path
// with forward slashes, the number of bytes copied and `existed`, which is true
// when a file was already at that path before this run overwrote it (a future
// remove feature uses it to delete only what the trainer added).
//
// No exception escapes Apply: an unreadable source, an unsafe path, a locked
// destination and allocation failure are all reported through Result.

namespace Trainers::TrainerPackage {

struct Result {
    bool                                  ok = false;
    std::vector<TrainerState::FileRecord> files;
    std::string                           error;
};

// Copies every regular file below |sourceDir| into |gameDir|, preserving the
// relative sub-path, never following links:
//
//   <sourceDir>/a.dll          -> <gameDir>/a.dll
//   <sourceDir>/sub/b.txt      -> <gameDir>/sub/b.txt   (parent created)
//
// Directories are created as needed. An existing destination file is replaced
// (a read-only attribute on it is cleared first: overwriting it is the intent);
// no backup is kept. `<dest>.<pid>.tmp` siblings left by a crashed run are
// removed before the copy (best effort, regular files only; a file that merely
// ends in ".tmp" but has a non-numeric middle segment is left alone). Files are
// copied in enumeration order.
//
// |sourceDir| must be an existing directory with at least one regular file: an
// empty payload is a failure, because a zero-file install cannot be verified
// (TrainerState::NeedsApply would treat it as pending forever). A source entry
// is refused (ok == false) when it is not a regular file (a directory is
// skipped, a link or other special file is an error), when its relative path is
// absolute, has a drive or root, or contains "." or "..", and when the matching
// destination is a directory or a link, or one of its parent directories inside
// |gameDir| is a link. |gameDir| must be an existing directory. A source
// directory that cannot be enumerated completely is a failure: a partial walk
// must never be recorded as a complete install.
//
// On any failure ok == false, error carries a short reason, and files holds the
// records of the files that were already copied (their bytes stay on disk; a
// retry simply overwrites them). The caller must not advance the recorded
// state in that case. On success ok == true and files holds one record per
// copied file (never an empty vector).
Result Apply(const std::filesystem::path& sourceDir,
             const std::filesystem::path& gameDir);

} // namespace Trainers::TrainerPackage
