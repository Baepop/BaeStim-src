#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace PatternFallback {

    // Upper bound on how many neighbor TOMLs are listed and downloaded per
    // call. Three is enough to find a body whose patterns match the running
    // DLL without hammering the tracker (each miss costs one HTTP GET).
    inline constexpr size_t kMaxNeighbors = 3;

    // Marker key used by the "unsupported" popup (see SteamDiagnostics::
    // ShowWarningOnce). Defined here as the one shared
    // spelling of the key so the popup flag this module checks for is the
    // exact same file the popup writes.
    inline std::string UnsupportedMarkerKey(const std::string& component,
                                            const std::string& sha256)
    {
        return component + "_" + sha256 + "_unsupported";
    }

    // TOML bodies of neighbor components for channel/component, newest first.
    // Used when Steam has no published TOML for the running DLL's exact sha:
    // the caller scans these neighbors against the loaded module and saves
    // the best match via SaveNeighborBody.
    //
    // Flow:
    //   1. channel != "pattern" → {} (fallback only exists for pattern TOMLs).
    //   2. If <steamRoot>\baestim\<channel>\<component>\<sha256>.fallback.toml
    //      exists and is non-empty, return it as the single candidate (no
    //      network). The non-canonical ".fallback.toml" suffix is safe: the
    //      base cache-first lookup keys "<sha>.toml" EXACTLY, so a fallback
    //      body saved here is never picked up as a canonical cached TOML.
    //   3. If the "unsupported" marker for (component, sha256) exists, return
    //      {} without listing — the user was already told this DLL is
    //      unsupported; repeating the network listing every launch is waste.
    //   4. Otherwise list the newest published TOMLs (GitHub commits API,
    //      jsDelivr data API as secondary) and download up to kMaxNeighbors
    //      bodies. {} when nothing worked.
    //
    // No-throwing (error_code filesystem I/O); every HTTP call uses short
    // timeouts (resolve/connect 2.5s, send/recv 5s).
    std::vector<std::string> GetNeighborBodies(const std::string& steamRoot,
                                               const std::string& channel,
                                               const std::string& component,
                                               const std::string& sha256);

    // Persist a neighbor body under the NON-canonical cache name
    // <sha>.fallback.toml (see header note above about the safe suffix).
    void SaveNeighborBody(const std::string& steamRoot, const std::string& channel,
                          const std::string& component, const std::string& sha256,
                          const std::string& body);

    // Remove the fallback cache file (e.g. when a cached body parses empty).
    void ClearNeighborCache(const std::string& steamRoot, const std::string& channel,
                            const std::string& component, const std::string& sha256);

} // namespace PatternFallback
