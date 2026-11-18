#pragma once

#include <string>
#include <string_view>

namespace AppUpdater {

    struct CheckResult {
        bool updateAvailable = false;
        std::string oldVersion;   // version baked into the running DLL
        std::string newVersion;   // version named by the bucket's latest.toml
        std::string dllRelPath;   // flat object name inside the updates bucket
        std::string sha256;       // expected SHA-256 of the new DLL
    };

    // Read "latest.toml" from this build's public Supabase updates bucket and
    // decide whether the published build differs from the running one. The
    // bucket is the only source (no mirror fallback). Never throws; returns
    // updateAvailable = false on any failure.
    CheckResult Check();

    // Download the new DLL from the same bucket, validate it (size, MZ header,
    // SHA-256 — all before touching disk), and stage it next to Steam via
    // rename-then-write. The current (loaded) DLL is renamed to "<name>.old";
    // the new bytes are written at the canonical path and load on the next Steam
    // start. Returns false (leaving the live DLL untouched) on any failure.
    bool DownloadAndStage(const CheckResult& result, const std::string& selfDllPath);

    // Best-effort removal of a "<name>.old" left by a previous staged update. Succeeds
    // only once the old image is no longer loaded (i.e. after a restart), so it is safe
    // to call unconditionally on startup.
    void CleanupStagedBackup(const std::string& selfDllPath);

    // Launch a detached, hidden helper that gracefully restarts Steam so a staged
    // update takes effect immediately. No-op-safe: if the helper can't launch, the
    // update still applies on the user's next manual start.
    void RestartSteam();

    // ── test surface ────────────────────────────────────────────────────────
    //
    // The pure core of the unit, exposed so tests/AppUpdaterTest.cpp can assert
    // the pointer contract without network. Production code must keep using
    // Check / DownloadAndStage; this namespace exists only for tests.
    namespace Test {

        // Parse and validate a latest.toml body. On success writes newVersion,
        // dllRelPath and sha256 into |out| and returns true; oldVersion and
        // updateAvailable are never touched. On failure the three pointer fields
        // are cleared and false is returned (never throws).
        bool ParseLatestToml(std::string_view body, CheckResult& out);

        // <baseUrl>/storage/v1/object/public/updates/<objectPath>, with the base's
        // trailing '/' trimmed. Empty when the base is empty (or only slashes) or
        // the object name is not a plain file name.
        std::string BuildObjectUrl(const std::string& baseUrl, const std::string& objectPath);

        // The update decision Check() applies to a parsed result: latest.toml is
        // authoritative, so any version difference is an update.
        bool IsUpdateAvailable(const CheckResult& result);

    } // namespace Test

} // namespace AppUpdater
