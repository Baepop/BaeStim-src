#include "Utils/SteamMetadata/PatternFallback.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Json/MiniJson.h"
#include "Utils/Logging/Log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string_view>
#include <utility>

namespace PatternFallback {

namespace {

    namespace fs = std::filesystem;

    // Tracker repo both the listing APIs and the download mirrors read from.
    // Hardcoded on purpose: the [remote] url_template override must not be
    // able to break (or redirect) the fallback path.
    constexpr const char* kTrackerOwner = "madoiscool";
    constexpr const char* kTrackerRepo  = "steam-monitor";

    // Fallback only ever deals with the pattern channel.
    constexpr const char* kPatternChannel = "pattern";

    // GitHub API rejects requests without a User-Agent, and the Accept header
    // pins the JSON response shape. WinHTTP takes a wide header block.
    constexpr wchar_t kGithubApiHeaders[] =
        L"User-Agent: BaeStim/1.0\r\nAccept: application/vnd.github+json";

    // Same short timeouts as RemoteToml: a dead endpoint must not stall the
    // startup path for seconds.
    constexpr uint32_t kTimeoutResolve = 2500;
    constexpr uint32_t kTimeoutConnect = 2500;
    constexpr uint32_t kTimeoutSend    = 5000;
    constexpr uint32_t kTimeoutRecv    = 5000;

    // Wall-clock cap on the fallback's own network work (listing + neighbor
    // downloads). The 8s budget keeps the fallback from eating Steam startup:
    // each HTTP call has its own short timeout, but worst case the mirror
    // chain is 2 listings + 3 downloads x 2 mirrors, so a total cap is the
    // only guarantee the fallback's share of startup stays small. The budget
    // guards only the listing+download path; the saved-fallback cache read
    // is local disk and runs unbudgeted.
    constexpr int kTotalBudgetMs = 8000;

    // Local HTTP wrapper over OSTPlatform::Http::Execute with the fallback's
    // short timeouts. Listing callers pass a raised maxBodyBytes (see below);
    // the default 256 KB cap stays for neighbor downloads (single TOMLs).
    static OSTPlatform::Http::Result HttpGet(const char* url,
                                             const wchar_t* headers = nullptr,
                                             uint32_t maxBodyBytes = 256 * 1024)
    {
        return OSTPlatform::Http::Execute(L"GET", url, nullptr, 0, headers,
                                          kTimeoutResolve, kTimeoutConnect,
                                          kTimeoutSend, kTimeoutRecv, maxBodyBytes);
    }

    bool HttpOk(const OSTPlatform::Http::Result& http)
    {
        return http.ok && http.status == 200 && !http.body.empty();
    }

    bool IsHexChar(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // Git commit SHAs are 64 hex chars. Used to pick the commit sha out of the
    // "sha" string values: in the commits-list response the commit's own sha
    // appears in document order before the tree/parents shas.
    bool IsHexSha256(const std::string& s)
    {
        if (s.size() != 64)
            return false;
        for (const char c : s) {
            if (!IsHexChar(c))
                return false;
        }
        return true;
    }

    std::string UrlEncodePath(const std::string& text)
    {
        // Component names are internal identifiers (steamclient, steamui, ...)
        // with no reserved characters; pass through untouched. Guarded by the
        // caller's contract, kept as a named step so any future change to the
        // component set has a single place to sanitize at.
        return text;
    }

    // Extracts the neighbor sha from a tracker filename. Both listing sources
    // name the same file differently: the commits API reports
    // "steamclient/<sha>.toml" while the jsDelivr flat structure reports
    // "/steamclient/<sha>.toml", so both roots are accepted. Only a plain
    // "<sha>.toml" directly under the component dir qualifies — the download
    // URLs are built flat, so a nested path could never be fetched.
    std::string NeighborShaFromFilename(std::string_view name,
                                        const std::string& component)
    {
        const std::string prefix = component + "/";
        const std::string slashPrefix = "/" + component + "/";
        std::string_view base;
        if (name.starts_with(prefix))
            base = name.substr(prefix.size());
        else if (name.starts_with(slashPrefix))
            base = name.substr(slashPrefix.size());
        else
            return {};

        if (base.find('/') != std::string_view::npos)
            return {};

        constexpr std::string_view kTomlSuffix = ".toml";
        if (!base.ends_with(kTomlSuffix) || base.size() <= kTomlSuffix.size())
            return {};

        base.remove_suffix(kTomlSuffix.size());

        const std::string sha(base);
        if (!IsHexSha256(sha))
            return {};
        return sha;
    }

    // Primary listing source: the newest commit that touched the component
    // dir, then that commit's file list. The commit detail response contains
    // every filename of the commit; only "<component>/<sha>.toml" entries are
    // neighbors. Non-200/empty at any step → signal the caller to try the
    // secondary source. Bounded by kMaxNeighbors, newest first (per_page=1 on
    // the list request already gives the most recent commit).
    std::vector<std::string> ListViaGithubCommits(const std::string& component)
    {
        std::vector<std::string> out;

        const std::string componentPath = UrlEncodePath(component);
        const std::string listUrl =
            "https://api.github.com/repos/" + std::string(kTrackerOwner) + "/" +
            kTrackerRepo + "/commits?sha=" + kPatternChannel +
            "&path=" + componentPath + "&per_page=1";

        // The three listing responses below (GitHub commits list, commit
        // detail, jsDelivr flat) can exceed the 256 KB default body cap; a
        // truncated listing parses to zero neighbors, which would pin a
        // permanent _unsupported marker on the DLL. 4 MB of headroom keeps
        // the full listing readable. Neighbor downloads stay on the default.
        const OSTPlatform::Http::Result list =
            HttpGet(listUrl.c_str(), kGithubApiHeaders, 4u * 1024u * 1024u);
        if (!HttpOk(list)) {
            LOG_WARN("PatternFallback: commit listing failed (HTTP {}), "
                     "falling back to jsDelivr", list.status);
            return out;
        }

        std::string commitSha;
        for (const std::string& sha : MiniJson::ExtractStrings(list.body, "sha")) {
            if (IsHexSha256(sha)) {
                commitSha = sha;
                break;
            }
        }
        if (commitSha.empty()) {
            LOG_WARN("PatternFallback: no 64-hex commit sha in listing response, "
                     "falling back to jsDelivr");
            return out;
        }

        const std::string detailUrl =
            "https://api.github.com/repos/" + std::string(kTrackerOwner) + "/" +
            kTrackerRepo + "/commits/" + commitSha;
        const OSTPlatform::Http::Result detail =
            HttpGet(detailUrl.c_str(), kGithubApiHeaders, 4u * 1024u * 1024u);
        if (!HttpOk(detail)) {
            LOG_WARN("PatternFallback: commit detail failed (HTTP {}), "
                     "falling back to jsDelivr", detail.status);
            return out;
        }

        for (const std::string& name :
             MiniJson::ExtractStrings(detail.body, "filename")) {
            std::string sha = NeighborShaFromFilename(name, component);
            if (sha.empty())
                continue;
            out.push_back(std::move(sha));
            if (out.size() >= kMaxNeighbors)
                break;
        }
        return out;
    }

    // Secondary listing source: jsDelivr's flat package listing. Verified live
    // shape: files[].name = "/steamclient/<sha64>.toml". DEGRADED PATH: this
    // listing has NO date ordering, so the returned shas are in an
    // indeterminate order — recency is not guaranteed here (the primary
    // source is the one that provides it).
    std::vector<std::string> ListViaJsDelivr(const std::string& component)
    {
        std::vector<std::string> out;

        const std::string url =
            "https://data.jsdelivr.com/v1/packages/gh/" + std::string(kTrackerOwner) +
            "/" + kTrackerRepo + "@" + kPatternChannel + "?structure=flat";

        const OSTPlatform::Http::Result http =
            HttpGet(url.c_str(), nullptr, 4u * 1024u * 1024u);
        if (!HttpOk(http)) {
            LOG_WARN("PatternFallback: jsDelivr listing failed (HTTP {})", http.status);
            return out;
        }

        for (const std::string& name : MiniJson::ExtractStrings(http.body, "name")) {
            std::string sha = NeighborShaFromFilename(name, component);
            if (sha.empty())
                continue;
            out.push_back(std::move(sha));
            if (out.size() >= kMaxNeighbors)
                break;
        }
        return out;
    }

    // Download one neighbor body from the hardcoded mirror pair, independent
    // of any [remote] url_template override. First non-empty body wins.
    // jsDelivr first for the same reason RemoteToml leads with it: GitHub raw
    // is unreachable on a lot of networks and only fails after a timeout.
    // overBudget is the caller's total wall-clock predicate: the first mirror
    // is always attempted, but later mirrors are skipped once the fallback's
    // total time budget is spent.
    std::string DownloadNeighborBody(const std::string& component,
                                     const std::string& neighborSha,
                                     const std::function<bool()>& overBudget)
    {
        const std::string componentPath = UrlEncodePath(component);
        // Composed from the tracker constants so the download URLs cannot
        // silently drift from the listing URLs. Still hardcoded at this
        // callsite: never through [remote] url_template.
        const std::string mirrors[] = {
            std::string("https://cdn.jsdelivr.net/gh/") + kTrackerOwner + "/" +
                kTrackerRepo + "@" + kPatternChannel + "/",
            std::string("https://raw.githubusercontent.com/") + kTrackerOwner +
                "/" + kTrackerRepo + "/" + kPatternChannel + "/",
        };
        const size_t mirrorCount = sizeof(mirrors) / sizeof(mirrors[0]);

        for (size_t i = 0; i < mirrorCount; ++i) {
            if (i > 0 && overBudget()) {
                LOG_WARN("PatternFallback: fallback time budget spent, "
                         "skipping remaining mirrors");
                break;
            }
            const std::string url =
                mirrors[i] + componentPath + "/" + neighborSha + ".toml";
            const OSTPlatform::Http::Result http = HttpGet(url.c_str());
            if (HttpOk(http)) {
                LOG_INFO("PatternFallback: downloaded neighbor {} from {}",
                         neighborSha, url);
                return std::move(http.body);
            }
            if (i + 1 < mirrorCount) {
                LOG_WARN("PatternFallback: neighbor download failed "
                         "({} ok={} HTTP={}), trying next mirror",
                         url, http.ok, http.status);
            } else {
                LOG_WARN("PatternFallback: neighbor download failed "
                         "({} ok={} HTTP={}), no more mirrors",
                         url, http.ok, http.status);
            }
        }
        return {};
    }

    fs::path CachePath(const std::string& steamRoot, const std::string& channel,
                       const std::string& component, const std::string& sha256)
    {
        return fs::path(steamRoot) / "baestim" / channel / component /
               (sha256 + ".fallback.toml");
    }

    fs::path MarkerPath(const std::string& steamRoot, const std::string& component,
                        const std::string& sha256)
    {
        // Must match SteamDiagnostics::ShowWarningOnce exactly: same directory,
        // same key spelling, same ".flag" suffix, or the popup's once-marker
        // and this check would disagree.
        return fs::path(steamRoot) / "baestim" / "markers" /
               (UnsupportedMarkerKey(component, sha256) + ".flag");
    }

} // namespace

std::vector<std::string> GetNeighborBodies(const std::string& steamRoot,
                                           const std::string& channel,
                                           const std::string& component,
                                           const std::string& sha256)
{
    namespace fs = std::filesystem;

    // 1. Fallback only exists for pattern TOMLs; other channels have no
    //    neighbor concept (and no listing branch "pattern" to query).
    if (channel != kPatternChannel)
        return {};

    // 2. Cached fallback body short-circuits everything, network included.
    //    Only a non-empty, readable cache counts; a hollow/empty file falls
    //    through so the listing runs and a fresh body can be saved over it.
    {
        std::error_code ec;
        const fs::path cachePath = CachePath(steamRoot, channel, component, sha256);
        const auto cachedSize = fs::file_size(cachePath, ec);
        if (!ec && cachedSize > 0) {
            std::ifstream ifs(cachePath, std::ios::binary);
            std::string buf((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
            if (ifs && !buf.empty()) {
                LOG_INFO("PatternFallback: serving cached fallback body {}",
                         cachePath.string());
                return { std::move(buf) };
            }
            LOG_WARN("PatternFallback: fallback cache unreadable/empty, listing: {}",
                     cachePath.string());
        }
    }

    // 3. The "unsupported" popup already fired for this exact DLL: don't burn
    //    the listing + up to three downloads on every launch.
    {
        std::error_code ec;
        if (fs::exists(MarkerPath(steamRoot, component, sha256), ec)) {
            LOG_INFO("PatternFallback: unsupported marker present, skipping listing");
            return {};
        }
    }

    // 4. List newest neighbors (GitHub commits primary, jsDelivr data API
    //    secondary) and download each until kMaxNeighbors bodies are in.
    //    Everything below is bounded by kTotalBudgetMs: the 8s budget caps
    //    the fallback's share of Steam startup, whatever the endpoints do.
    const auto start = std::chrono::steady_clock::now();
    const std::function<bool()> overBudget = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start).count() >= kTotalBudgetMs;
    };

    std::vector<std::string> neighborShas = ListViaGithubCommits(component);
    if (neighborShas.empty() && !overBudget())
        neighborShas = ListViaJsDelivr(component);

    std::vector<std::string> bodies;
    for (const std::string& neighborSha : neighborShas) {
        if (overBudget())
            break;
        std::string body = DownloadNeighborBody(component, neighborSha, overBudget);
        if (!body.empty()) {
            bodies.push_back(std::move(body));
            if (bodies.size() >= kMaxNeighbors)
                break;
        }
    }

    if (bodies.empty())
        LOG_WARN("PatternFallback: no neighbor body available for {}/{}",
                 component, sha256);
    return bodies;
}

void SaveNeighborBody(const std::string& steamRoot, const std::string& channel,
                      const std::string& component, const std::string& sha256,
                      const std::string& body)
{
    namespace fs = std::filesystem;

    const fs::path cachePath = CachePath(steamRoot, channel, component, sha256);

    std::error_code mkdirEc;
    fs::create_directories(cachePath.parent_path(), mkdirEc);
    if (mkdirEc) {
        LOG_WARN("PatternFallback: could not create cache dir {} ({})",
                 cachePath.parent_path().string(), mkdirEc.message());
        return;
    }

    std::ofstream ofs(cachePath, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        LOG_WARN("PatternFallback: could not open {} for writing",
                 cachePath.string());
        return;
    }
    ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!ofs) {
        LOG_WARN("PatternFallback: short write to {}", cachePath.string());
        return;
    }
    LOG_INFO("PatternFallback: saved fallback body to {} ({} bytes)",
             cachePath.string(), body.size());
}

void ClearNeighborCache(const std::string& steamRoot, const std::string& channel,
                        const std::string& component, const std::string& sha256)
{
    namespace fs = std::filesystem;

    const fs::path cachePath = CachePath(steamRoot, channel, component, sha256);
    std::error_code ec;
    if (!fs::exists(cachePath, ec))
        return;
    if (fs::remove(cachePath, ec))
        LOG_INFO("PatternFallback: removed fallback cache {}", cachePath.string());
    else
        LOG_WARN("PatternFallback: could not remove {} ({})",
                 cachePath.string(), ec.message());
}

} // namespace PatternFallback
