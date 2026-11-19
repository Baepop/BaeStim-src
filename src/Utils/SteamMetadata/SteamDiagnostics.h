#pragma once

#include <string>

namespace SteamDiagnostics {

    // Capture the Steam build id and DLL hashes used in support popups.
    void Initialize(const std::string& steamclientPath,
                    const std::string& steamUIPath);

    // Reuse captured hashes for known Steam DLLs and hash other files on demand.
    std::string Sha256Of(const std::string& path);

    // Display a warning asynchronously with the captured Steam diagnostics.
    void ShowWarning(std::string title, std::string message);

    // Same as ShowWarning, but suppressed after the first call per marker key:
    // shows only if <steamRoot>\baestim\markers\<markerKey>.flag does not
    // exist yet; the flag is written before the popup is queued.
    void ShowWarningOnce(const std::string& steamRoot,
                         const std::string& markerKey,
                         std::string title,
                         std::string message);

} // namespace SteamDiagnostics
