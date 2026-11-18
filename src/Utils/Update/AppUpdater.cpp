#include "AppUpdater.h"

#include "BaeStimBuildInfo.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Process.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"

#include <toml++/toml.hpp>

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

// Self-updater source: the project's OWN public Supabase Storage bucket
// ("updates" of the [supabase] project URL compiled into this build), read
// through the storage object API:
//
//   GET <supabaseUrl>/storage/v1/object/public/updates/<object>
//
// Two flat objects live there: "latest.toml" (the version authority: version,
// path, sha256) and the DLL it names. The bucket is public, so no Authorization
// header is sent. There is NO fallback to the upstream GitHub mirrors: one
// source, one attempt, and a failure leaves the running DLL untouched.
namespace AppUpdater {

namespace {
    // Bounds for a sane framework DLL payload (8 MB is also the HTTP body cap
    // for the download) — guards against a truncated or hostile response being
    // written to disk. BaeStim.dll is ~1 MB.
    constexpr size_t kMinDllBytes = 200 * 1024;
    constexpr size_t kMaxDllBytes = 8 * 1024 * 1024;

    // latest.toml is a handful of lines; 64 KB is a hard ceiling, not a target.
    constexpr size_t kMaxPointerBytes = 64 * 1024;

    // Object name of the pointer inside the bucket. Flat name: the bucket has no
    // per-fork subdirectories, and every object this unit fetches must be a
    // plain file name (see IsPlainFileName).
    constexpr const char* kPointerPath = "latest.toml";

    // Public object path under the project URL (Config::GetSupabaseUrl()).
    constexpr const char* kUpdatesBucketPath = "/storage/v1/object/public/updates/";

    // Startup-friendly: the update check runs inline during DLL init, so a slow
    // bucket must not stall Steam for long.
    constexpr uint32_t kTimeoutResolveMs = 2500;
    constexpr uint32_t kTimeoutConnectMs = 2500;
    constexpr uint32_t kTimeoutSendMs    = 5000;
    constexpr uint32_t kTimeoutRecvMs    = 5000;

    // Longest object name accepted. Generous for "BaeStim.dll", tight enough
    // that no path-like value slips through.
    constexpr size_t kMaxObjectNameChars = 128;

    // A bucket object name is a FLAT file name: no directories, no drive or
    // stream separator, no traversal. The value reaches both the URL and a
    // download whose bytes are written next to Steam, so it is validated before
    // either use.
    bool IsPlainFileName(std::string_view name)
    {
        if (name.empty() || name.size() > kMaxObjectNameChars) return false;
        if (name.find("..") != std::string_view::npos) return false;
        for (const char c : name) {
            if (c == '/' || c == '\\' || c == ':') return false;
        }
        return true;
    }

    // SHA-256 as published by the build: exactly 64 hex chars. Case is not
    // checked here because the comparison against the downloaded bytes is
    // case-insensitive (EqualsIgnoreCase).
    bool IsSha256Hex(std::string_view sha)
    {
        if (sha.size() != 64) return false;
        for (const char c : sha) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    std::string BuildObjectUrlImpl(std::string_view baseUrl, std::string_view objectPath)
    {
        std::string base(baseUrl);
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        if (base.empty() || !IsPlainFileName(objectPath)) return {};
        base += kUpdatesBucketPath;
        base.append(objectPath);
        return base;
    }

    // Single GET against the public updates bucket. No headers (public object),
    // no retry loop (the update check is once per process).
    bool FetchFromUpdatesBucket(const std::string& objectPath, size_t maxBodyBytes,
                                std::string& out)
    {
        const std::string supabaseUrl = Config::GetSupabaseUrl();
        if (supabaseUrl.empty()) {
            // Warn once per process: the source is a compile-time default, so a
            // missing URL is a build/config problem, not a transient failure.
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARN("AppUpdater: [supabase] url is not configured; "
                         "update source unavailable");
            }
            return false;
        }

        const std::string url = BuildObjectUrlImpl(supabaseUrl, objectPath);
        if (url.empty()) {
            LOG_WARN("AppUpdater: refusing invalid updates object name \"{}\"", objectPath);
            return false;
        }

        OSTPlatform::Http::Result http = OSTPlatform::Http::Execute(
            L"GET", url.c_str(), nullptr, 0, nullptr,
            kTimeoutResolveMs, kTimeoutConnectMs, kTimeoutSendMs, kTimeoutRecvMs,
            static_cast<uint32_t>(maxBodyBytes));

        if (!http.ok || http.status != 200 || http.body.empty()) {
            LOG_WARN("AppUpdater: GET {} failed (ok={} HTTP={} body={} bytes)",
                     url, http.ok, http.status, http.body.size());
            return false;
        }

        out = std::move(http.body);
        return true;
    }

    bool IsUpdateAvailableImpl(const CheckResult& result)
    {
        // latest.toml is authoritative for "newest"; build tags are not semver,
        // so any difference from the running version means there is something
        // new to stage. An unread pointer (empty newVersion) is never an update.
        return !result.newVersion.empty() && result.newVersion != result.oldVersion;
    }

    // Pure parse/validate step of latest.toml. Only the three pointer fields are
    // written; oldVersion and updateAvailable belong to the caller.
    bool ParseLatestTomlImpl(std::string_view body, CheckResult& out)
    {
        // Never leave a half-filled pointer behind: callers reuse the result and
        // stale fields would be indistinguishable from a fresh read.
        out.newVersion.clear();
        out.dllRelPath.clear();
        out.sha256.clear();

        if (body.empty()) {
            LOG_WARN("AppUpdater: latest.toml body is empty");
            return false;
        }

        toml::table tbl;
        try {
            tbl = toml::parse(body);
        } catch (const toml::parse_error& e) {
            LOG_WARN("AppUpdater: latest.toml parse error: {}", e.description());
            return false;
        }

        const auto version = tbl["version"].value<std::string>();
        const auto path    = tbl["path"].value<std::string>();
        const auto sha     = tbl["sha256"].value<std::string>();
        if (!version || !path || !sha || version->empty() || path->empty() || sha->empty()) {
            LOG_WARN("AppUpdater: latest.toml missing version/path/sha256");
            return false;
        }

        // Hardening: the pointer is a remote object and "path" is fetched from
        // the same bucket and eventually written next to Steam, so only a plain
        // file name may pass.
        if (!IsPlainFileName(*path)) {
            LOG_WARN("AppUpdater: latest.toml path is not a plain file name: \"{}\"", *path);
            return false;
        }
        if (!IsSha256Hex(*sha)) {
            LOG_WARN("AppUpdater: latest.toml sha256 is not 64 hex chars ({} chars)",
                     sha->size());
            return false;
        }

        out.newVersion = *version;
        out.dllRelPath = *path;
        out.sha256     = *sha;
        return true;
    }

    bool EqualsIgnoreCase(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    std::string EscapeForPowerShellSingleQuoted(std::string s)
    {
        // Inside a PowerShell single-quoted string, ' is escaped by doubling it.
        for (size_t p = s.find('\''); p != std::string::npos; p = s.find('\'', p + 2))
            s.insert(p, 1, '\'');
        return s;
    }
} // namespace

CheckResult Check()
{
    CheckResult r;
    r.oldVersion = BAESTIM_VERSION;

    std::string body;
    if (!FetchFromUpdatesBucket(kPointerPath, kMaxPointerBytes, body)) {
        LOG_WARN("AppUpdater: {} unavailable from the updates bucket", kPointerPath);
        return r;
    }

    if (!ParseLatestTomlImpl(body, r))
        return r;

    if (!IsUpdateAvailableImpl(r)) {
        LOG_INFO("AppUpdater: up to date (version {})", r.oldVersion);
        return r;
    }

    LOG_INFO("AppUpdater: update available {} -> {}", r.oldVersion, r.newVersion);
    r.updateAvailable = true;
    return r;
}

bool DownloadAndStage(const CheckResult& result, const std::string& selfDllPath)
{
    std::string body;
    if (!FetchFromUpdatesBucket(result.dllRelPath, kMaxDllBytes, body)) {
        LOG_WARN("AppUpdater: DLL download failed for {}", result.dllRelPath);
        return false;
    }

    if (body.size() < kMinDllBytes || body.size() > kMaxDllBytes) {
        LOG_WARN("AppUpdater: rejected DLL (suspicious size {} bytes)", body.size());
        return false;
    }
    if (body.size() < 2 || body[0] != 'M' || body[1] != 'Z') {
        LOG_WARN("AppUpdater: rejected DLL (missing MZ header)");
        return false;
    }

    // Integrity check happens BEFORE anything is renamed or written: a body that
    // fails here never touches the DLL on disk.
    const std::string actual = OSTPlatform::Hash::Sha256OfBuffer(body.data(), body.size());
    if (actual.empty() || !EqualsIgnoreCase(actual, result.sha256)) {
        LOG_WARN("AppUpdater: SHA-256 mismatch (expected {}, got {})", result.sha256, actual);
        return false;
    }

    // The running DLL is write-locked while mapped, but NTFS allows renaming a loaded
    // image, so move it aside and write the new bytes at the canonical path.
    const std::string backup = selfDllPath + ".old";
    if (!MoveFileExA(selfDllPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_WARN("AppUpdater: could not rename current DLL to backup (error={})", GetLastError());
        return false;
    }

    {
        std::ofstream ofs(selfDllPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            LOG_WARN("AppUpdater: could not open {} for writing; restoring backup", selfDllPath);
            MoveFileExA(backup.c_str(), selfDllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            return false;
        }
        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
        ofs.flush();
        if (!ofs) {
            LOG_WARN("AppUpdater: write failed for {}; restoring backup", selfDllPath);
            ofs.close();
            MoveFileExA(backup.c_str(), selfDllPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            return false;
        }
    }

    LOG_INFO("AppUpdater: staged {} ({} bytes); applies on next Steam start",
             selfDllPath, body.size());
    return true;
}

void CleanupStagedBackup(const std::string& selfDllPath)
{
    const std::string backup = selfDllPath + ".old";
    std::error_code ec;
    if (std::filesystem::exists(backup, ec) && DeleteFileA(backup.c_str()))
        LOG_INFO("AppUpdater: removed stale backup {}", backup);
}

void RestartSteam()
{
    const std::filesystem::path steamExe = OSTPlatform::DynamicLibrary::GetMainExecutablePath();
    if (steamExe.empty()) {
        LOG_WARN("AppUpdater: steam.exe path unknown; cannot auto-restart");
        return;
    }

    const std::string exe = EscapeForPowerShellSingleQuoted(steamExe.string());
    // Detached, hidden PowerShell: graceful -shutdown, wait for every steam process to
    // exit (covers steamwebhelper), then relaunch.
    const std::string command =
        "powershell -NoProfile -WindowStyle Hidden -Command "
        "\"& '" + exe + "' -shutdown; "
        "Wait-Process -Name steam -Timeout 30 -ErrorAction SilentlyContinue; "
        "Start-Process '" + exe + "'\"";

    if (OSTPlatform::Process::LaunchDetachedHidden(command))
        LOG_INFO("AppUpdater: restart helper launched");
    else
        LOG_WARN("AppUpdater: restart helper failed to launch; "
                 "update applies on next manual Steam start");
}

// ── test surface ────────────────────────────────────────────────────────────

namespace Test {

    bool ParseLatestToml(std::string_view body, CheckResult& out)
    {
        return ParseLatestTomlImpl(body, out);
    }

    std::string BuildObjectUrl(const std::string& baseUrl, const std::string& objectPath)
    {
        return BuildObjectUrlImpl(baseUrl, objectPath);
    }

    bool IsUpdateAvailable(const CheckResult& result)
    {
        return IsUpdateAvailableImpl(result);
    }

} // namespace Test

} // namespace AppUpdater
