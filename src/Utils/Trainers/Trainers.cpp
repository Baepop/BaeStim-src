#include "Utils/Trainers/Trainers.h"

#include "dllmain.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Hash.h"
#include "OSTPlatform/include/Http.h"
#include "OSTPlatform/include/Thread.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamIdentity/SteamIdentity.h"
#include "Utils/Trainers/SteamLibrary.h"
#include "Utils/Trainers/TrainerManifest.h"
#include "Utils/Trainers/TrainerPackage.h"
#include "Utils/Trainers/TrainerState.h"
#include "Utils/Trainers/Zip.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Trainers {

namespace {

    namespace fs = std::filesystem;

    // Same startup-friendly phase timeouts as RemoteConfig/AppUpdater: a dead
    // endpoint must not hold the worker on the long WinHTTP defaults.
    constexpr uint32_t kTimeoutResolveMs = 2500;
    constexpr uint32_t kTimeoutConnectMs = 2500;
    constexpr uint32_t kTimeoutSendMs    = 5000;
    constexpr uint32_t kTimeoutRecvMs    = 5000;

    // A package is one trainer bundle: a few DLLs and config files. The download
    // cap matches Zip::Limits' per-entry worst case and the 2 GiB the brief
    // allows; the manifest's own `size` is what the file is verified against.
    constexpr uint64_t kDownloadMaxBytes = 2ull * 1024 * 1024 * 1024;

    // The receive phase of a package download is not a startup budget: 30 s per
    // read (the DownloadToFile default) keeps a slow link from failing a large
    // but healthy body.
    constexpr uint32_t kDownloadRecvTimeoutMs = 30000;

    // The manifest is a small JSON document (one entry per assigned trainer).
    // The Http default (256 KB) is below what a few thousand entries need.
    constexpr uint32_t kMaxManifestBytes = 1024 * 1024;

    // Without a session SteamID there is nothing to fetch for. Steam startup can
    // be slow (sign-in, updates), so poll for 10 minutes before going idle.
    constexpr auto kIdentityPoll    = std::chrono::seconds(1);
    constexpr auto kIdentityTimeout = std::chrono::seconds(600);

    // One failing manifest request is retried every 30 s, and the whole fetch
    // gives up after 10 minutes: a dead backend must not keep a worker (and its
    // single-instance mutex) alive for the session.
    constexpr auto kManifestRetry  = std::chrono::seconds(30);
    constexpr auto kManifestGiveUp = std::chrono::minutes(10);

    // Advisory free-space pre-check: the package plus room for the extracted
    // copy and the copied-over files. Extraction itself is bounded by
    // Zip::Limits, so a failing check only defers the item to the next start.
    constexpr uint64_t kFreeSpaceMarginBytes = 512ull * 1024 * 1024;

    // At most this many lines in the summary dialog, then "... and N more".
    constexpr size_t kMaxNoticeLines = 8;

    // ── paths ───────────────────────────────────────────────────────────────

    struct Paths {
        fs::path stateFile;  // <Steam>\baestim\trainers\state.json
        fs::path cacheDir;   // <Steam>\baestim\trainers\cache
        fs::path stagingDir; // <Steam>\baestim\trainers\staging
    };

    Paths MakePaths(const fs::path& steamRoot)
    {
        const fs::path dataDir = steamRoot / "baestim" / "trainers";
        return {
            dataDir / "state.json",
            dataDir / "cache",
            dataDir / "staging",
        };
    }

    // ── small helpers ───────────────────────────────────────────────────────

    std::string IsoTimestampNow()
    {
        const std::time_t now =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        std::tm tmUtc {};
        gmtime_s(&tmUtc, &now);

        std::ostringstream oss;
        oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    bool ReadWholeFile(const fs::path& path, std::string& out)
    {
        out.clear();
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return ifs && !out.empty();
    }

    void RemoveAllQuietly(const fs::path& path, const char* what)
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        if (ec)
            LOG_WARN("Trainers: could not remove {} {} ({})", what, path.string(), ec.message());
    }

    void RemoveFileQuietly(const fs::path& path, const char* what)
    {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec)
            LOG_WARN("Trainers: could not remove {} {} ({})", what, path.string(), ec.message());
    }

    // ── identity ────────────────────────────────────────────────────────────

    // Returns the session SteamID, or 0 when none was captured within
    // kIdentityTimeout.
    uint64_t WaitForSteamId()
    {
        const auto giveUpAt = std::chrono::steady_clock::now() + kIdentityTimeout;
        for (;;) {
            if (const uint64_t steamId = SteamIdentity::GetLocalSteamId(); steamId != 0)
                return steamId;
            if (std::chrono::steady_clock::now() >= giveUpAt)
                return 0;
            std::this_thread::sleep_for(kIdentityPoll);
        }
    }

    // ── manifest ────────────────────────────────────────────────────────────

    // One GET of the per-user manifest. A non-200, an unreadable response and an
    // empty body are all the same failure: the caller retries.
    bool FetchManifestBody(const std::string& endpoint, const std::string& anonKey,
                           std::string& body)
    {
        const std::wstring headers =
            L"Authorization: Bearer " + OSTPlatform::Encoding::Utf8ToWide(anonKey) + L"\r\n";

        const OSTPlatform::Http::Result http = OSTPlatform::Http::Execute(
            L"GET", endpoint.c_str(), nullptr, 0, headers.c_str(),
            kTimeoutResolveMs, kTimeoutConnectMs, kTimeoutSendMs, kTimeoutRecvMs,
            kMaxManifestBytes);

        if (!http.ok || http.status != 200) {
            LOG_WARN("Trainers: manifest request failed (ok={}, HTTP={})", http.ok, http.status);
            return false;
        }
        if (http.body.empty()) {
            LOG_WARN("Trainers: manifest response was empty");
            return false;
        }

        body = http.body;
        return true;
    }

    // Fetches and parses the manifest, retrying every 30 s for at most
    // 10 minutes. There is no manifest cache: a failed fetch simply means
    // nothing is applied this session.
    bool FetchManifest(const std::string& endpoint, const std::string& anonKey,
                       const std::string& supabaseUrl,
                       std::vector<TrainerManifest::Item>& items)
    {
        const auto giveUpAt = std::chrono::steady_clock::now() + kManifestGiveUp;
        uint32_t attempts = 0;

        for (;;) {
            ++attempts;
            std::string body;
            if (FetchManifestBody(endpoint, anonKey, body)) {
                // A 200 from a captive portal or proxy is HTML, and a truncated
                // body can lose the "trainers" field altogether. Both parse to
                // an empty list, which must not be mistaken for "nothing
                // assigned": only a body carrying the field is authoritative,
                // and an empty array then legitimately ends the run.
                if (body.find("\"trainers\"") == std::string::npos) {
                    LOG_WARN("Trainers: 200 response has no \"trainers\" field, retrying");
                } else {
                    items = TrainerManifest::Parse(body, supabaseUrl);
                    LOG_INFO("Trainers: manifest answered {} usable entries (attempt {})",
                             items.size(), attempts);
                    return true;
                }
            }

            if (std::chrono::steady_clock::now() >= giveUpAt) {
                LOG_WARN("Trainers: no usable manifest after {} attempts in 10 minutes, "
                         "giving up",
                         attempts);
                return false;
            }

            std::this_thread::sleep_for(kManifestRetry);
        }
    }

    // ── state ───────────────────────────────────────────────────────────────

    // A missing or unusable state document means "nothing is known to be
    // installed", so every assigned trainer is pending.
    TrainerState::State LoadState(const fs::path& stateFile)
    {
        std::string body;
        if (!ReadWholeFile(stateFile, body)) {
            LOG_INFO("Trainers: no usable state file at {}, every trainer is pending",
                     stateFile.string());
            return {};
        }

        std::optional<TrainerState::State> parsed = TrainerState::Parse(body);
        if (!parsed) {
            LOG_WARN("Trainers: state file {} is not usable, treating it as empty",
                     stateFile.string());
            return {};
        }

        LOG_INFO("Trainers: loaded state for {} appids", parsed->trainers.size());
        return std::move(*parsed);
    }

    // ── item selection ──────────────────────────────────────────────────────

    struct PendingItem {
        TrainerManifest::Item     manifest;
        SteamLibrary::GameInstall install;
    };

    // One pass over the manifest in document order. The first entry for an appid
    // wins; later duplicates are dropped. An entry is pending only when its game
    // is fully installed, the recorded install is stale (or absent) and the
    // target volume can hold the package plus the extraction margin.
    std::vector<PendingItem> CollectPending(const std::vector<TrainerManifest::Item>& items,
                                            const fs::path& steamRoot,
                                            const TrainerState::State& state)
    {
        std::vector<PendingItem> pending;
        std::unordered_set<uint32_t> seen;

        for (const TrainerManifest::Item& item : items) {
            if (!seen.insert(item.appid).second) {
                LOG_INFO("Trainers: duplicate appid {} in manifest, keeping the first",
                         item.appid);
                continue;
            }

            const std::optional<SteamLibrary::GameInstall> install =
                SteamLibrary::Resolve(item.appid, steamRoot);
            if (!install) {
                LOG_INFO("Trainers: appid {} is not fully installed, skipping", item.appid);
                continue;
            }

            if (!TrainerState::NeedsApply(state, item.appid, item.version,
                                          install->installDir)) {
                LOG_DEBUG("Trainers: appid {} v{} already applied, skipping",
                          item.appid, item.version);
                continue;
            }

            // Reject an impossible package before any bytes are fetched: the
            // downloader is capped at kDownloadMaxBytes, so a larger declared
            // size could only be downloaded and then fail verification.
            if (item.size > kDownloadMaxBytes) {
                LOG_WARN("Trainers: appid {} package is {} bytes, above the {} byte "
                         "download cap, skipping",
                         item.appid, item.size, kDownloadMaxBytes);
                continue;
            }

            // Saturating add: a hostile or corrupt size must not wrap into a
            // small requirement and skip the check.
            const uint64_t required =
                item.size > UINT64_MAX - kFreeSpaceMarginBytes
                    ? UINT64_MAX
                    : item.size + kFreeSpaceMarginBytes;

            std::error_code ec;
            const fs::space_info info = fs::space(install->installDir, ec);
            if (ec) {
                LOG_WARN("Trainers: appid {}: could not read free space for {} ({})",
                         item.appid, install->installDir.string(), ec.message());
                continue;
            }
            if (info.available < required) {
                LOG_WARN("Trainers: appid {} needs {} bytes free ({} available), "
                         "skipping it this session",
                         item.appid, required, static_cast<uint64_t>(info.available));
                continue;
            }

            pending.push_back(PendingItem{ item, *install });
        }

        return pending;
    }

    // ── one item ────────────────────────────────────────────────────────────

    // Download → verify → extract → copy. Returns true only when |files| holds
    // the records of a complete payload copied into |gameDir|. The caller owns
    // the cache ZIP and the staging directory and deletes both whatever happens
    // here.
    bool StageItem(const TrainerManifest::Item& item, const fs::path& cacheZip,
                   const fs::path& stagingItem, const fs::path& gameDir,
                   std::vector<TrainerState::FileRecord>& files)
    {
        const OSTPlatform::Http::DownloadResult download =
            OSTPlatform::Http::DownloadToFile(item.url.c_str(), L"", cacheZip,
                                              kDownloadMaxBytes,
                                              kTimeoutResolveMs, kTimeoutConnectMs,
                                              kTimeoutSendMs, kDownloadRecvTimeoutMs);
        if (!download.ok) {
            LOG_WARN("Trainers: appid {} v{} download failed (HTTP {})",
                     item.appid, item.version, download.status);
            return false;
        }

        // Integrity is checked BEFORE the archive is opened: a body that fails
        // here is deleted by the caller and never reaches Zip.
        std::error_code ec;
        const uint64_t size = fs::file_size(cacheZip, ec);
        if (ec) {
            LOG_WARN("Trainers: appid {} package is unreadable after download ({})",
                     item.appid, ec.message());
            return false;
        }
        if (size != item.size) {
            LOG_WARN("Trainers: appid {} package size mismatch (expected {} bytes, got {})",
                     item.appid, item.size, size);
            return false;
        }

        const std::string digest = OSTPlatform::Hash::Sha256OfFile(cacheZip);
        if (digest.empty() || digest != item.sha256) {
            LOG_WARN("Trainers: appid {} package SHA-256 mismatch", item.appid);
            return false;
        }

        // A leftover staging directory from a crashed run would mix payloads
        // from two versions, so extraction starts only from an empty directory.
        // A locked file can make remove_all fail without removing everything:
        // carrying on would merge the old and the new payload into something
        // that must never be recorded as an install.
        {
            std::error_code removeEc;
            fs::remove_all(stagingItem, removeEc);
            std::error_code probeEc;
            const bool stillThere = fs::exists(stagingItem, probeEc);
            if (stillThere || probeEc) {
                LOG_WARN("Trainers: appid {}: could not clear staging directory {} "
                         "before extracting ({})",
                         item.appid, stagingItem.string(),
                         removeEc ? removeEc.message() : std::string("still present"));
                return false;
            }
        }

        const Zip::ExtractResult extract =
            Zip::ExtractGame(cacheZip, stagingItem, Zip::Limits{});
        if (!extract.ok) {
            LOG_WARN("Trainers: appid {} extraction failed: {}",
                     item.appid, extract.error);
            return false;
        }
        if (extract.files == 0) {
            LOG_WARN("Trainers: appid {} package carries no game/ payload", item.appid);
            return false;
        }

        const TrainerPackage::Result applied = TrainerPackage::Apply(stagingItem, gameDir);
        if (!applied.ok) {
            LOG_WARN("Trainers: appid {} apply failed: {}", item.appid, applied.error);
            return false;
        }
        if (applied.files.empty()) {
            // Apply never reports success without records; recording an empty set
            // would make NeedsApply treat this install as pending forever.
            LOG_WARN("Trainers: appid {} reported no copied files, not recording it",
                     item.appid);
            return false;
        }

        files = applied.files;
        return true;
    }

    struct Applied {
        uint32_t    appid = 0;
        std::string name;    // empty when the manifest carried none
        std::string version;
    };

    // Removes one item's staging directory and cache ZIP on scope exit, so the
    // cleanup also runs when an exception unwinds out of the item pipeline. A
    // destructor must not throw: a failure is only logged (best effort anyway).
    struct ItemCleanup {
        fs::path stagingItem;
        fs::path cacheZip;

        ~ItemCleanup() noexcept
        {
            try {
                RemoveAllQuietly(stagingItem, "staging directory");
                RemoveFileQuietly(cacheZip, "package");
            } catch (...) {
                // Logging itself failed; there is nothing left to report to.
            }
        }
    };

    // Full per-item pipeline, including state bookkeeping and the unconditional
    // cleanup. Returns true only when the trainer is applied AND recorded: a
    // failed state write leaves the previous state untouched and is reported as
    // a failure so the next start retries.
    bool ProcessItem(const PendingItem& pending, const Paths& paths,
                     TrainerState::State& state)
    {
        const TrainerManifest::Item& item = pending.manifest;

        // Cache and staging names are stable per (appid, package): the first
        // 16 hex chars of the verified hash are plenty to tell versions apart.
        const std::string key = std::to_string(item.appid) + "-" + item.sha256.substr(0, 16);
        const fs::path cacheZip    = paths.cacheDir / (key + ".zip");
        const fs::path stagingItem = paths.stagingDir / key;
        const ItemCleanup cleanup{ stagingItem, cacheZip };

        std::vector<TrainerState::FileRecord> files;
        bool ok = StageItem(item, cacheZip, stagingItem, pending.install.installDir, files);

        if (ok) {
            TrainerState::Entry entry;
            entry.version   = item.version;
            entry.sha256    = item.sha256;
            entry.updatedAt = IsoTimestampNow();
            entry.files     = std::move(files);

            // Commit to the live state only once the document is on disk: a
            // failed write must not leave this item recorded in memory, where a
            // later item's write would persist an install this run reported as
            // failed.
            TrainerState::State candidate = state;
            candidate.trainers[item.appid] = std::move(entry);

            if (TrainerState::WriteAtomic(paths.stateFile, candidate)) {
                state = std::move(candidate);
                LOG_INFO("Trainers: appid {} v{} installed", item.appid, item.version);
            } else {
                LOG_WARN("Trainers: appid {} v{} applied but the state write failed; "
                         "it will be retried on the next start",
                         item.appid, item.version);
                ok = false;
            }
        }

        return ok;
    }

    // One summary dialog for everything that was installed, never one per item.
    void ShowNotice(const std::vector<Applied>& applied)
    {
        if (applied.empty())
            return;

        std::string body;
        const size_t shown = applied.size() < kMaxNoticeLines ? applied.size()
                                                              : kMaxNoticeLines;
        for (size_t i = 0; i < shown; ++i) {
            const Applied& entry = applied[i];
            body += "- ";
            body += entry.name.empty() ? std::to_string(entry.appid) : entry.name;
            body += " (" + std::to_string(entry.appid) + ") v" + entry.version + "\n";
        }
        if (applied.size() > shown) {
            body += "... and " + std::to_string(applied.size() - shown) + " more\n";
        }
        body += "\nYou can start the game now.";

        OSTPlatform::Dialog::ShowInfo("BaeStim Trainers", body);
    }

    // ── worker ──────────────────────────────────────────────────────────────

    void Run()
    {
        const std::string steamRootUtf8 = SteamInstallPath;
        if (steamRootUtf8.empty()) {
            LOG_WARN("Trainers: Steam install path unknown, worker idle");
            return;
        }
        const fs::path steamRoot = OSTPlatform::Encoding::Utf8ToWide(steamRootUtf8);

        // Empty endpoint/key = feature not configured: log once and never make a
        // request. Checked before the identity poll so a misconfigured build
        // does not hold a worker for 10 minutes.
        const std::string supabaseUrl = Config::GetSupabaseUrl();
        const std::string anonKey     = Config::GetSupabaseAnonKey();
        if (supabaseUrl.empty() || anonKey.empty()) {
            LOG_INFO("Trainers: [supabase] url/anon key not configured, worker idle");
            return;
        }

        const uint64_t steamId = WaitForSteamId();
        if (steamId == 0) {
            LOG_WARN("Trainers: no session steamid within 10 minutes, worker idle");
            return;
        }

        const std::string endpoint = TrainerManifest::BuildRequestUrl(supabaseUrl, steamId);
        if (endpoint.empty()) {
            LOG_WARN("Trainers: could not build the manifest request url, worker idle");
            return;
        }

        std::vector<TrainerManifest::Item> items;
        if (!FetchManifest(endpoint, anonKey, supabaseUrl, items)) {
            LOG_WARN("Trainers: manifest unavailable, nothing applied this session");
            return;
        }
        if (items.empty()) {
            LOG_INFO("Trainers: no trainers assigned, nothing to do");
            return;
        }

        const Paths paths = MakePaths(steamRoot);

        // DownloadToFile does not create the cache directory, and both working
        // directories are shared by every item.
        std::error_code ec;
        fs::create_directories(paths.cacheDir, ec);
        if (ec) {
            LOG_WARN("Trainers: could not create cache dir {} ({})",
                     paths.cacheDir.string(), ec.message());
            return;
        }
        fs::create_directories(paths.stagingDir, ec);
        if (ec) {
            LOG_WARN("Trainers: could not create staging dir {} ({})",
                     paths.stagingDir.string(), ec.message());
            return;
        }

        TrainerState::State state = LoadState(paths.stateFile);
        const std::vector<PendingItem> pending = CollectPending(items, steamRoot, state);
        if (pending.empty()) {
            LOG_INFO("Trainers: nothing to install ({} manifest entries)", items.size());
            return;
        }

        LOG_INFO("Trainers: installing {} of {} assigned trainers",
                 pending.size(), items.size());

        std::vector<Applied> applied;
        for (const PendingItem& item : pending) {
            if (ProcessItem(item, paths, state)) {
                applied.push_back(Applied{ item.manifest.appid, item.manifest.name,
                                           item.manifest.version });
            }
        }

        LOG_INFO("Trainers: {} of {} trainer(s) installed",
                 applied.size(), pending.size());
        ShowNotice(applied);
    }

    // The worker is not awaited by anyone, so an exception escaping it would ask
    // std::terminate for the whole Steam process. The guard covers the entire
    // body; the single-instance mutex is released as the last act either way.
    uint32_t Worker(HANDLE mutex)
    {
        try {
            Run();
        } catch (const std::exception& e) {
            LOG_ERROR("Trainers: worker failed: {}", e.what());
            (void)e; // LOG_ERROR is compiled out in Release
        } catch (...) {
            LOG_ERROR("Trainers: worker failed with a non-std exception");
        }

        if (mutex != nullptr)
            CloseHandle(mutex);
        return 0;
    }

} // namespace

void Start()
{
    if (!Config::GetTrainersEnabled()) {
        LOG_INFO("Trainers: disabled by [trainers] enabled=false");
        return;
    }

    // Steam loads BaeStim.dll into more than one process (dwmapi and xinput1_4
    // both host it), and each copy runs its own init thread. Holding this mutex
    // for the whole worker lifetime makes exactly one of them install trainers.
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\BaeStim.Trainers");
    if (mutex == nullptr) {
        LOG_WARN("Trainers: could not create the single-instance mutex (error={})",
                 GetLastError());
        return;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_INFO("Trainers: another process already runs the worker, not starting");
        CloseHandle(mutex);
        return;
    }

    if (!OSTPlatform::Thread::StartDetached([mutex]() -> uint32_t {
            return Worker(mutex);
        })) {
        LOG_WARN("Trainers: could not start the worker thread");
        CloseHandle(mutex);
    }
}

} // namespace Trainers
