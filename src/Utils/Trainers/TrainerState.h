#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Local install record for trainer bundles.
//
// One JSON file, owned by the trainers worker and passed in by the caller (the
// data directory is not this unit's business):
//
//   {
//     "version": 1,
//     "trainers": {
//       "1361510": {
//         "version": "1.0",
//         "status": "installed",
//         "sha256": "<64 hex>",
//         "updated_at": "2026-11-18T12:00:00Z",
//         "files": [{ "path": "TMNT.exe", "size": 1234, "existed": true }]
//       }
//     }
//   }
//
// Keys are decimal appids. File paths are relative to the game directory, and
// `existed` records whether the destination was already there before the
// package overwrote it: no backup is taken, but a future remove feature can
// use the flag to delete only what the trainer added. "installed" is the only
// persisted status (a failed apply is not written at all and is retried on the
// next Steam start), so it is emitted by Render and ignored by Parse.
//
// Parsing is fail-closed and tolerant in the same way as the rest of the
// trainers code: a document that cannot be read as a whole yields nullopt (the
// caller then treats the state as empty, i.e. everything is pending), while an
// entry that fails validation is skipped and never costs its siblings. No
// exception escapes any function in this header.

namespace Trainers::TrainerState {

// One destination the package wrote, relative to the game directory.
struct FileRecord {
    std::string path;
    uint64_t    size    = 0;
    bool        existed = false;
};

// What was installed for one appid.
struct Entry {
    std::string             version;   // trainer version compared on startup
    std::string             sha256;    // package hash, lower-case hex
    std::string             updatedAt; // ISO-8601 UTC, informational
    std::vector<FileRecord> files;     // insertion order preserved
};

struct State {
    std::map<uint32_t, Entry> trainers; // one entry per appid
};

// Reads a state document. Returns nullopt when |json| is empty, truncated, not
// a single complete JSON object, or has no usable "trainers" object to read.
// An entry is kept when its key is a decimal appid in 1..UINT32_MAX (no
// leading zero), its version is non-empty, it has a "files" array (it may be
// empty: NeedsApply then treats the entry as pending, since an install that
// cannot be verified must be redone), and every file has a non-empty path and a
// non-negative integer size. `existed` absent or non-boolean is read as true
// (unknown = keep), the safe direction for the future remove feature. Unknown
// keys (including "version" and "status") are ignored. A duplicate appid keeps
// the last entry in document order.
std::optional<State> Parse(std::string_view json);

// Serialises |state| as the document above: map order (ascending appid), files
// in insertion order, `"` and `\` (and control characters) escaped. Entries
// whose appid is outside 1..UINT32_MAX (only 0 fits the key type) are omitted,
// because the format cannot represent them and Parse would drop them anyway.
// Parsing the result yields the representable subset of |state|: everything
// except such entries (appid 0) is preserved.
std::string Render(const State& state);

// Writes |state| to |path| through a PID-suffixed sibling
// (<path>.<pid>.tmp) and std::filesystem::rename, so a reader sees either the
// previous file or the complete new one. Creates the parent directories and
// sweeps stale <path>.*.tmp siblings left by an earlier crash (best effort).
// Returns false (and leaves no temp file behind) on any failure, including an
// empty |path|. Never throws.
bool WriteAtomic(const std::filesystem::path& path, const State& state);

// True when the trainer described by |version| must be applied to |appid|:
//   - no entry for the appid;
//   - the recorded version differs from |version| (byte comparison);
//   - the entry records no files, so nothing can be verified as applied;
//   - any recorded file is missing, is not a regular file, or has a different
//     size (a Steam repair/update that reverted a file);
//   - |gameDir| is empty, so nothing can be verified.
// False only when the entry exists, the version matches and every recorded
// file is intact. Never throws; filesystem errors count as "not intact".
bool NeedsApply(const State& state, uint32_t appid, std::string_view version,
                const std::filesystem::path& gameDir);

} // namespace Trainers::TrainerState
