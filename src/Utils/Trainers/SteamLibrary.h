#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Steam install discovery for trainer bundles.
//
// Steam describes its libraries and installed apps in plain-text KeyValues
// ("VDF"/"ACF") files:
//
//   <steamRoot>\steamapps\libraryfolders.vdf   -> every library folder
//   <library>\steamapps\appmanifest_<appid>.acf -> one installed app
//
// The helpers below read those formats with the standard library only: no
// registry, no Steam client API, no exceptions escaping. Resolve() is what a
// trainer worker calls to turn an appid into the folder the game lives in.

namespace Trainers::SteamLibrary {

struct GameInstall {
    uint32_t              appid = 0;
    std::string           name;
    std::filesystem::path installDir;
};

// Every value whose key is "path" in a libraryfolders.vdf, in the order
// encountered. Only that key name matters: the block above a path may be
// numeric ("0", "1", ...) or named, and unrelated keys such as "contentid" or
// the nested "apps" list are ignored. The pre-2021 layout, which listed
// libraries as "<index>" "<path>" pairs with no "path" key at all, is not
// supported (Steam has not written it for years). A relative path is resolved
// against steamRoot. steamRoot itself is not added here: Resolve() always
// considers it.
std::vector<std::filesystem::path>
ParseLibraryFolders(std::string_view vdf,
                    const std::filesystem::path& steamRoot);

// Extracts the fields of an appmanifest_<appid>.acf.
//
// Only the direct children of the root "AppState" block are read, so a "name"
// or "installdir" nested inside a block (InstalledDepots, UserConfig, ...)
// cannot shadow the top-level value. A document with no AppState block has
// nothing to extract.
//
// Returns false — and clears all three outputs — when the manifest has no
// usable "installdir" (missing or empty): without it there is no folder to
// return. outFullyInstalled reports the fully-installed bit of StateFlags
// ((flags & 4) != 0); a missing, malformed or out-of-range StateFlags counts
// as 0.
bool ParseAppManifest(std::string_view acf,
                      std::string& outInstallDir,
                      std::string& outName,
                      bool& outFullyInstalled);

// Finds appid's install folder:
//
//   1. steamRoot itself, then every library in
//      <steamRoot>\steamapps\libraryfolders.vdf (when that file exists);
//   2. the first candidate with <candidate>\steamapps\appmanifest_<appid>.acf
//      whose StateFlags has the fully-installed bit and whose installdir is
//      non-empty wins.
//
// installdir must be a plain relative sub-path: rooted or drive-qualified
// values and any value containing ".." are refused (nullopt), because Task 7
// writes trainer files below common/. A safe value is normalised before it is
// joined.
//
// Returns <library>\steamapps\common\<installdir>, or nullopt when no candidate
// qualifies (not installed, partially installed, a different library, or an
// unsafe installdir).
std::optional<GameInstall> Resolve(uint32_t appid,
                                   const std::filesystem::path& steamRoot);

} // namespace Trainers::SteamLibrary
