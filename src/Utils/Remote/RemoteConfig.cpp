#include "Utils/Remote/RemoteConfig.h"

#include "Utils/Config/Config.h"
#include "Utils/Json/MiniJson.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamIdentity/SteamIdentity.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Encoding.h"
#include "OSTPlatform/include/Http.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

// [supabase] endpoints live in Config ([supabase] url/anon_key, with compiled
// placeholder defaults): the worker treats an empty url/key as "feature not
// configured": one LOG_INFO, no request at all.
namespace RemoteConfig {

namespace {

    namespace fs = std::filesystem;

    // Same short timeouts as RemoteToml/PatternFallback: a dead endpoint must
    // not hold the worker — and with it Stop()'s join on shutdown — for the
    // long WinHTTP defaults.
    constexpr uint32_t kTimeoutResolve = 2500;
    constexpr uint32_t kTimeoutConnect = 2500;
    constexpr uint32_t kTimeoutSend    = 5000;
    constexpr uint32_t kTimeoutRecv    = 5000;

    // The whole per-user list arrives in one response. The Http default (256 KB)
    // is below what a few thousand entries need, so raise it the same way
    // PatternFallback does for tracker listings.
    constexpr uint32_t kMaxResponseBytes = 4u * 1024u * 1024u;

    // One attempt per cycle and one cycle every 30 minutes: the next cycle IS
    // the retry, so a failing backend sees at most one request per 30 min from
    // each Steam install (no retry storm).
    constexpr auto kRefreshInterval = std::chrono::minutes(30);

    // The session SteamID is captured by the IPC GetSteamID / netpacket hooks
    // on the player's first Steam action. Give Steam startup 10 minutes of
    // polling before going idle for the rest of the session: without an
    // identity there is nothing to fetch for.
    constexpr auto kIdentityTimeout = std::chrono::minutes(10);
    constexpr auto kIdentityPoll    = std::chrono::seconds(2);

    // The only Lua filename this unit ever writes. Fixed by contract: the
    // watcher picks the file up, and a full rewrite is how an account switch
    // drops the previous user's appid set.
    constexpr const char* kLuaFileName = "baestim-remote.lua";

    constexpr size_t kNpos = std::string_view::npos;

    std::atomic<bool>       g_running{false};
    std::thread             g_worker;
    std::mutex              g_wakeMutex;
    std::condition_variable g_wake;

    // ── small helpers ───────────────────────────────────────────────────────

    // Sleeps for |duration| or until Stop() asks the worker to quit. Returns
    // true when the worker should keep going.
    template <typename Rep, typename Period>
    bool WaitWhileRunning(std::chrono::duration<Rep, Period> duration)
    {
        std::unique_lock<std::mutex> lock(g_wakeMutex);
        return !g_wake.wait_for(lock, duration, [] { return !g_running.load(); });
    }

    bool IsHexChar(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    // The Lua addappid parser only keeps a key argument of exactly 64 chars
    // read from argument 3 (LuaConfig.cpp:212-222); anything else would be
    // silently dropped there, so refuse to emit it here. Hex is checked too: a
    // 64-char non-hex key is corrupt data, not a valid depot key.
    bool IsDepotKey(std::string_view key)
    {
        if (key.size() != 64)
            return false;
        for (const char c : key) {
            if (!IsHexChar(c))
                return false;
        }
        return true;
    }

    // addtoken() goes through ParseUInt64Decimal in the Lua parser, which
    // rejects anything but digits. strtoull with errno + end-pointer catches
    // overflow at the uint64 boundary; the digits-only precondition also keeps
    // strtoull's leading sign/whitespace handling out of the picture.
    bool IsUint64Decimal(std::string_view token)
    {
        if (token.empty())
            return false;
        for (const char c : token) {
            if (c < '0' || c > '9')
                return false;
        }

        const std::string text(token);
        errno = 0;
        char* end = nullptr;
        std::strtoull(text.c_str(), &end, 10);
        // ERANGE rejects the uint64 overflow and the end pointer proves every
        // digit was consumed; the return value cannot exceed uint64_t.
        return errno != ERANGE && end == text.c_str() + text.size();
    }

    fs::path LuaPath(const std::string& steamRoot)
    {
        return fs::path(steamRoot) / "config" / "baestim" / kLuaFileName;
    }

    // <Steam> root = the directory holding this DLL, which is also where
    // steamclient64.dll lives (RemoteToml derives its root from the steamclient
    // path the same way). Resolved from the module itself instead of the process
    // CWD, which is not guaranteed to be the install dir — same approach as
    // TokeerBridge::SteamDirFromSelf.
    fs::path SteamRootFromModule()
    {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&SteamRootFromModule), &self) ||
            !self) {
            return {};
        }
        return OSTPlatform::DynamicLibrary::GetModuleDirectory(
            reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(self));
    }

    // Per-user cache of the last authoritative response body. Keyed by steamid
    // so switching accounts never serves one user's list to another.
    fs::path CachePath(const std::string& steamRoot, uint64_t steamId)
    {
        return fs::path(steamRoot) / "baestim" / "remote" /
               (std::to_string(steamId) + ".json");
    }

    // ── file I/O ────────────────────────────────────────────────────────────

    bool ReadWholeFile(const fs::path& path, std::string& out)
    {
        out.clear();
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs)
            return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return ifs && !out.empty();
    }

    // Write-then-rename, so a reader (the Lua watcher or the next session
    // reading the cache) sees either the old file or the complete new one,
    // never a half-written one. On Windows std::filesystem::rename maps to
    // MoveFileExW(..., MOVEFILE_REPLACE_EXISTING), which replaces the
    // destination in one step; the temp file is a sibling, so it never spans
    // volumes.
    bool WriteFileAtomic(const fs::path& path, std::string_view contents, const char* what)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            LOG_WARN("RemoteConfig: could not create {} dir {} ({})",
                     what, path.parent_path().string(), ec.message());
            return false;
        }

        // PID-suffixed sibling: Steam loads the hooks into more than one child
        // process (dwmapi and xinput1_4 both host the DLL), so a fixed
        // ".lua.tmp" would let two writers truncate each other's file mid-write.
        // The watcher only picks ".lua" files, so the temp stays invisible.
        fs::path tmpPath = path;
        tmpPath += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                LOG_WARN("RemoteConfig: could not open {} for writing", tmpPath.string());
                return false;
            }
            ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            ofs.flush();
            if (!ofs) {
                LOG_WARN("RemoteConfig: short write to {}", tmpPath.string());
                ofs.close();
                std::error_code rmEc;
                fs::remove(tmpPath, rmEc);
                return false;
            }
        }

        fs::rename(tmpPath, path, ec);
        if (ec) {
            LOG_WARN("RemoteConfig: rename {} -> {} failed ({})",
                     tmpPath.string(), path.string(), ec.message());
            std::error_code rmEc;
            fs::remove(tmpPath, rmEc);
            return false;
        }
        return true;
    }

    // ── tolerant parsing ────────────────────────────────────────────────────
    //
    // The response is NOT validated as a whole document: only
    // the "appids" array matters, and everything inside it is parsed per
    // entry so one bad row cannot poison the rest.

    // Index of the quote that closes the string literal opening at |open|
    // (which points at '"'), or kNpos when the input ends inside the literal.
    // Escape-aware like MiniJson's own scanner, so \" never terminates a
    // literal early.
    size_t FindClosingQuote(std::string_view json, size_t open)
    {
        for (size_t i = open + 1; i < json.size(); ++i) {
            if (json[i] == '\\') {
                ++i; // skip the escaped character
                continue;
            }
            if (json[i] == '"')
                return i;
        }
        return kNpos;
    }

    size_t SkipWhitespace(std::string_view json, size_t i)
    {
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
            ++i;
        return i;
    }

    // Whitespace-trimmed view of one entry segment.
    std::string_view TrimSpaces(std::string_view text)
    {
        size_t begin = 0;
        size_t end   = text.size();
        while (begin < end &&
               (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                text[begin] == '\n'))
            ++begin;
        while (end > begin &&
               (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                text[end - 1] == '\n'))
            --end;
        return text.substr(begin, end - begin);
    }

    // Location of the "appids" array. keyPresent separates "the backend said
    // nothing about appids" (suspicious: never cached) from "appids: []"
    // (authoritative empty list: cached), and closed separates a complete
    // array from one cut off by a truncated body.
    struct AppidsArray {
        bool   keyPresent   = false;
        bool   arrayOpened  = false;
        bool   closed       = false;
        size_t begin        = 0; // index just past '[' when arrayOpened
    };

    AppidsArray LocateAppidsArray(std::string_view json)
    {
        AppidsArray out;

        size_t i = 0;
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const size_t close = FindClosingQuote(json, i);
            if (close == kNpos)
                return out; // truncated literal: what we have is all there is

            if (json.substr(i + 1, close - i - 1) == "appids") {
                size_t after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    out.keyPresent = true;
                    after          = SkipWhitespace(json, after + 1);
                    if (after < json.size() && json[after] == '[') {
                        out.arrayOpened = true;
                        out.begin       = after + 1;
                    }
                    return out;
                }
                // A string VALUE that happens to read "appids" (e.g.
                // {"note":"appids", ...}): not a key, keep scanning.
            }

            // Ordinary string (unwanted key or a value): skip it whole so the
            // word "appids" inside a string value is never taken for the key.
            i = close + 1;
        }
        return out;
    }

    // Splits the array body into per-entry slices at depth-0 commas. Strings
    // are consumed whole, so brackets or commas inside a key/token/steamid
    // string cannot shift the nesting.
    std::vector<std::string_view> SplitEntries(std::string_view json,
                                               const AppidsArray& array,
                                               bool& closed)
    {
        std::vector<std::string_view> entries;
        closed = false;
        if (!array.arrayOpened)
            return entries;

        size_t depth       = 0;
        size_t entryStart  = array.begin;
        size_t i           = array.begin;

        while (i < json.size()) {
            const char c = json[i];

            if (c == '"') {
                const size_t quote = FindClosingQuote(json, i);
                if (quote == kNpos)
                    return entries; // truncated: keep the complete entries only
                i = quote + 1;
                continue;
            }

            if (c == '{' || c == '[') {
                ++depth;
                ++i;
                continue;
            }

            if (c == '}' || c == ']') {
                if (depth == 0) {
                    if (c != ']')
                        return entries; // stray '}' at array level: malformed body
                    const std::string_view last =
                        TrimSpaces(json.substr(entryStart, i - entryStart));
                    if (!last.empty())
                        entries.push_back(last);
                    closed = true;
                    return entries;
                }
                --depth;
                ++i;
                continue;
            }

            if (c == ',' && depth == 0) {
                const std::string_view entry =
                    TrimSpaces(json.substr(entryStart, i - entryStart));
                if (!entry.empty())
                    entries.push_back(entry);
                entryStart = i + 1;
                ++i;
                continue;
            }

            ++i;
        }

        // Ran out of input before the closing ']': the array is truncated, so
        // it is not authoritative (closed stays false) and the complete
        // entries found so far are still usable in memory.
        return entries;
    }

    // Parses one entry object. key/token are optional; an entry whose appid is
    // missing or out of range is dropped whole, while a malformed key or token
    // only costs that argument (the appid unlock itself stays valid).
    bool ParseEntry(std::string_view object, Entry& out)
    {
        const std::vector<int64_t> appids = MiniJson::ExtractNumbers(object, "appid");
        if (appids.empty()) {
            // Length only: the object may hold a valid depot key or token, and
            // this log line fires precisely when its appid is unreadable.
            LOG_WARN("RemoteConfig: skipping entry without an integer appid ({} bytes)",
                     object.size());
            return false;
        }

        const int64_t appid = appids.front();
        if (appid <= 0 || appid > static_cast<int64_t>(UINT32_MAX)) {
            LOG_WARN("RemoteConfig: skipping entry with out-of-range appid {}", appid);
            return false;
        }
        out.appid = static_cast<uint32_t>(appid);

        // Both fields travel as JSON strings or null (never as numbers), and
        // ExtractStrings only returns actual string values, so a null field
        // simply yields nothing.
        const std::vector<std::string> keys = MiniJson::ExtractStrings(object, "key");
        if (!keys.empty()) {
            if (IsDepotKey(keys.front()))
                out.key = keys.front();
            else
                LOG_WARN("RemoteConfig: appid {}: ignoring key that is not 64 hex chars",
                         out.appid);
        }

        const std::vector<std::string> tokens = MiniJson::ExtractStrings(object, "token");
        if (!tokens.empty()) {
            if (IsUint64Decimal(tokens.front()))
                out.token = tokens.front();
            else
                LOG_WARN("RemoteConfig: appid {}: ignoring token that is not a decimal uint64",
                         out.appid);
        }

        return true;
    }

    struct ParsedConfig {
        bool               luaPresent = false;
        std::string        lua; // decoded verbatim body when luaPresent
        AppidsArray        array;
        std::vector<Entry> entries;
    };

    ParsedConfig ParseConfigBody(std::string_view body)
    {
        ParsedConfig parsed;

        // Full-Lua mode first: the response then has NO "appids" key at all
        // ({"steamid":"...","lua":"..."}), so it must be recognised before the
        // missing-array check would declare the body suspicious. The value is
        // the owner's file; the structured list is ignored in that cycle.
        parsed.lua = MiniJson::ExtractJsonString(body, "lua");
        if (!parsed.lua.empty()) {
            parsed.luaPresent = true;
            return parsed;
        }

        parsed.array = LocateAppidsArray(body);

        bool closed = false;
        const std::vector<std::string_view> slices =
            SplitEntries(body, parsed.array, closed);
        parsed.array.closed = closed;

        parsed.entries.reserve(slices.size());
        for (const std::string_view slice : slices) {
            Entry entry;
            if (ParseEntry(slice, entry))
                parsed.entries.push_back(std::move(entry));
        }
        return parsed;
    }

    // ── Lua rendering ───────────────────────────────────────────────────────

    std::string RenderLuaText(uint64_t steamId, const std::vector<Entry>& entries,
                              size_t& emitted)
    {
        emitted = 0;

        // Header fixed by the contract. The em dash is written as explicit UTF-8
        // bytes so the output does not depend on the source file's encoding.
        std::string out = "-- BaeStim remote config for " + std::to_string(steamId) +
                          " \xE2\x80\x94 generated; do not edit\n";

        for (const Entry& entry : entries) {
            if (entry.appid == 0) {
                LOG_WARN("RemoteConfig: refusing to emit an appid of 0");
                continue;
            }

            // Re-validated here, not just at parse time, so no caller can get
            // an argument into the file that the Lua parser would reject.
            std::string key;
            if (!entry.key.empty()) {
                if (IsDepotKey(entry.key))
                    key = entry.key;
                else
                    LOG_WARN("RemoteConfig: appid {}: dropping malformed key from Lua output",
                             entry.appid);
            }

            std::string token;
            if (!entry.token.empty()) {
                if (IsUint64Decimal(entry.token))
                    token = entry.token;
                else
                    LOG_WARN("RemoteConfig: appid {}: dropping malformed token from Lua output",
                             entry.appid);
            }

            // 3-argument form on purpose: the Lua reader takes the depot key as
            // argument 3 (LuaConfig.cpp: addappid(integer, integer, string)) and
            // ignores a key passed as argument 2. The middle 0 is the parser's
            // "no manifest" placeholder.
            if (key.empty())
                out += "addappid(" + std::to_string(entry.appid) + ")\n";
            else
                out += "addappid(" + std::to_string(entry.appid) + ", 0, \"" + key +
                       "\")\n";

            if (!token.empty())
                out += "addtoken(" + std::to_string(entry.appid) + ", \"" + token + "\")\n";

            ++emitted;
        }

        return out;
    }

    // ── cache ───────────────────────────────────────────────────────────────

    void SaveCache(const std::string& steamRoot, uint64_t steamId, const std::string& body)
    {
        const fs::path path = CachePath(steamRoot, steamId);
        if (WriteFileAtomic(path, body, "cache"))
            LOG_INFO("RemoteConfig: cached response to {}", path.string());
    }

    // Only bodies cached by this unit are read back, and those always carry
    // either a complete "appids" array or a non-empty "lua" string; anything
    // else on that path is foreign or corrupt and must not silence a live
    // fetch. |lua| is set (and entries left empty) for a full-Lua cache entry.
    bool LoadCache(const std::string& steamRoot, uint64_t steamId,
                   std::vector<Entry>& entries, std::string& lua)
    {
        entries.clear();
        lua.clear();

        const fs::path path = CachePath(steamRoot, steamId);
        std::string body;
        if (!ReadWholeFile(path, body))
            return false;

        // The filename is already per-user, but a copied or renamed file must
        // never serve one account's list to another: the body always names the
        // steamid it was generated for (supabase index.ts).
        const std::string expected =
            "\"steamid\":\"" + std::to_string(steamId) + "\"";
        if (body.find(expected) == kNpos) {
            LOG_WARN("RemoteConfig: cache {} does not belong to steamid {}, ignoring",
                     path.string(), steamId);
            return false;
        }

        ParsedConfig parsed = ParseConfigBody(body);

        if (parsed.luaPresent) {
            lua = std::move(parsed.lua);
            LOG_INFO("RemoteConfig: loaded verbatim lua from cache {} ({} bytes)",
                     path.string(), lua.size());
            return true;
        }

        if (!parsed.array.keyPresent || !parsed.array.closed) {
            LOG_WARN("RemoteConfig: cache {} is not a usable response, ignoring",
                     path.string());
            return false;
        }

        entries = std::move(parsed.entries);
        LOG_INFO("RemoteConfig: loaded {} appids from cache {}", entries.size(),
                 path.string());
        return true;
    }

    // ── worker ──────────────────────────────────────────────────────────────

    // Returns the session SteamID, or 0 when none was captured within
    // kIdentityTimeout or Stop() was requested meanwhile.
    uint64_t WaitForSteamId()
    {
        const auto giveUpAt = std::chrono::steady_clock::now() + kIdentityTimeout;
        while (g_running) {
            if (const uint64_t steamId = SteamIdentity::GetLocalSteamId(); steamId != 0)
                return steamId;
            if (std::chrono::steady_clock::now() >= giveUpAt)
                return 0;
            if (!WaitWhileRunning(kIdentityPoll))
                return 0;
        }
        return 0;
    }

    // One fetch → cache/lua cycle. Never retries: a failed fetch is handled by
    // the cache, and the next cycle is 30 minutes away.
    void RunCycle(const std::string& steamRoot, const std::string& url,
                  const std::string& anonKey, uint64_t steamId)
    {
        std::string body;
        const FetchResult fetched = FetchConfig(url, anonKey, steamId, body);

        if (fetched.ok) {
            if (fetched.luaPresent) {
                LOG_INFO("RemoteConfig: fetched verbatim lua for steamid {} ({} bytes)",
                         steamId, fetched.lua.size());
            } else {
                LOG_INFO("RemoteConfig: fetched {} appids for steamid {}",
                         fetched.entries.size(), steamId);
            }
            SaveCache(steamRoot, steamId, body);
            if (fetched.luaPresent) {
                WriteRemoteLuaVerbatim(steamRoot, fetched.lua);
                return;
            }
            WriteRemoteLua(steamRoot, steamId, fetched.entries);
            return;
        }

        LOG_WARN("RemoteConfig: fetch failed for steamid {}, falling back to cache",
                 steamId);
        std::vector<Entry> cached;
        std::string cachedLua;
        if (LoadCache(steamRoot, steamId, cached, cachedLua)) {
            if (!cachedLua.empty())
                WriteRemoteLuaVerbatim(steamRoot, cachedLua);
            else
                WriteRemoteLua(steamRoot, steamId, cached);
            return;
        }

        // No usable cache. Clearing a file left by an earlier session keeps a
        // disabled/unknown user from keeping stale unlocks, but creating an
        // empty file when none exists would claim an authoritative empty list
        // we do not have: do nothing in that case.
        const fs::path luaPath = LuaPath(steamRoot);
        std::error_code ec;
        if (fs::exists(luaPath, ec)) {
            LOG_INFO("RemoteConfig: no cache, clearing stale {}", luaPath.string());
            WriteRemoteLua(steamRoot, steamId, {});
        } else {
            LOG_INFO("RemoteConfig: no cache and no previous Lua file, leaving as is");
        }
    }

    void WorkerThread()
    {
        // The worker is not awaited by anyone, so an exception escaping it asks
        // std::terminate for the whole host process. The guard covers the ENTIRE
        // body: the prologue already does filesystem and string work (a
        // filesystem_error from the Steam path → std::string narrowing on a
        // non-ANSI path, or bad_alloc) before the first cycle starts.
        try {
            const fs::path steamRootPath = SteamRootFromModule();
            if (steamRootPath.empty()) {
                LOG_WARN("RemoteConfig: could not resolve the Steam root, worker idle");
                return;
            }
            const std::string steamRoot = steamRootPath.string();

            // Empty endpoint/key = feature not configured (the compiled defaults
            // are placeholders). Log once and stay idle: never guess an endpoint.
            const std::string url     = Config::GetSupabaseUrl();
            const std::string anonKey = Config::GetSupabaseAnonKey();
            if (url.empty() || anonKey.empty()) {
                LOG_INFO("RemoteConfig: supabase url/anon key not configured, worker idle");
                return;
            }

            uint64_t steamId = WaitForSteamId();
            if (steamId == 0) {
                if (g_running)
                    LOG_WARN("RemoteConfig: no session steamid within 10 minutes, worker idle");
                return;
            }

            while (g_running) {
                // Per-cycle guard inside the outer one: one bad cycle must not
                // kill the loop, which is the only retry mechanism there is.
                try {
                    RunCycle(steamRoot, url, anonKey, steamId);
                } catch (const std::exception& e) {
                    LOG_ERROR("RemoteConfig: cycle failed: {}", e.what());
                    (void)e; // LOG_ERROR is compiled out in Release
                } catch (...) {
                    LOG_ERROR("RemoteConfig: cycle failed with a non-std exception");
                }

                if (!WaitWhileRunning(kRefreshInterval))
                    return;

                // Re-read every cycle: SteamIdentity is first-wins for the process
                // (a session never switches account in place), so this is the same
                // id today — but the cycle must not keep a stale copy if that
                // invariant ever changes.
                if (const uint64_t current = SteamIdentity::GetLocalSteamId(); current != 0)
                    steamId = current;
            }
        } catch (const std::exception& e) {
            LOG_ERROR("RemoteConfig: worker failed: {}", e.what());
            (void)e; // LOG_ERROR is compiled out in Release
        } catch (...) {
            LOG_ERROR("RemoteConfig: worker failed with a non-std exception");
        }
    }

} // namespace

// ── test surface ────────────────────────────────────────────────────────────
//
// Thin wrappers over the file-local parser/renderer. The public entry points
// above add HTTP, disk and thread state around them; tests/RemoteConfigTest.cpp
// exercises these directly. Production code must keep using FetchConfig,
// WriteRemoteLua and WriteRemoteLuaVerbatim.
namespace Test {

    ParseResult ParseResponse(std::string_view body)
    {
        ParsedConfig parsed = ParseConfigBody(body);

        ParseResult out;
        out.luaPresent = parsed.luaPresent;
        out.lua        = std::move(parsed.lua);
        out.keyPresent = parsed.array.keyPresent;
        out.complete   = parsed.array.closed;
        out.entries    = std::move(parsed.entries);
        return out;
    }

    std::string RenderLua(uint64_t steamId, const std::vector<Entry>& entries,
                          size_t& emitted)
    {
        return RenderLuaText(steamId, entries, emitted);
    }

} // namespace Test

// ── public API ──────────────────────────────────────────────────────────────

FetchResult FetchConfig(const std::string& url, const std::string& anonKey,
                        uint64_t steamId, std::string& httpBody)
{
    FetchResult result;
    httpBody.clear();

    if (url.empty() || anonKey.empty() || steamId == 0) {
        // The worker guards all three; reaching this means a caller bypassed it.
        LOG_WARN("RemoteConfig: refusing to fetch with an empty endpoint/key or steamid");
        return result;
    }

    std::string endpoint = url;
    while (!endpoint.empty() && endpoint.back() == '/')
        endpoint.pop_back();
    endpoint += "/functions/v1/config?steamid=" + std::to_string(steamId);

    const std::wstring headers =
        L"Authorization: Bearer " + OSTPlatform::Encoding::Utf8ToWide(anonKey);

    // Single attempt with the startup-friendly timeouts above; no retry loop
    // here on purpose (the 30-minute cycle is the retry).
    const OSTPlatform::Http::Result http =
        OSTPlatform::Http::Execute(L"GET", endpoint.c_str(), nullptr, 0, headers.c_str(),
                                   kTimeoutResolve, kTimeoutConnect,
                                   kTimeoutSend, kTimeoutRecv, kMaxResponseBytes);
    httpBody = http.body;

    if (!http.ok || http.status != 200) {
        LOG_WARN("RemoteConfig: GET {} failed (ok={} HTTP={})",
                 endpoint, http.ok, http.status);
        return result;
    }

    ParsedConfig parsed = ParseConfigBody(http.body);
    result.appidsPresent = parsed.array.keyPresent;
    result.entries       = std::move(parsed.entries);
    result.luaPresent    = parsed.luaPresent;
    result.lua           = std::move(parsed.lua);

    // Only a 200 whose "appids" array is present AND complete (or whose "lua"
    // string is non-empty and fully decoded) is authoritative (cacheable).
    // "appids": [] is valid and caches; a missing key or a truncated array is
    // suspicious and must not overwrite the offline copy.
    result.ok = result.luaPresent || (parsed.array.keyPresent && parsed.array.closed);
    if (!result.ok) {
        LOG_WARN("RemoteConfig: 200 response without a complete \"appids\" array "
                 "(present={}) and without a usable \"lua\" body, treating as failure",
                 parsed.array.keyPresent);
    }
    return result;
}

bool WriteRemoteLua(const std::string& steamRoot, uint64_t steamId,
                    const std::vector<Entry>& entries)
{
    const fs::path path = LuaPath(steamRoot);

    size_t emitted = 0;
    const std::string contents = RenderLuaText(steamId, entries, emitted);

    // Rewriting identical bytes would only churn the watcher (reparse + license
    // notify) every cycle; the file on disk already says what this cycle wants.
    std::string existing;
    if (ReadWholeFile(path, existing) && existing == contents) {
        LOG_DEBUG("RemoteConfig: {} already up to date", path.string());
        return true;
    }

    if (!WriteFileAtomic(path, contents, "Lua file"))
        return false;

    LOG_INFO("RemoteConfig: wrote {} appid entries to {}", emitted, path.string());
    return true;
}

bool WriteRemoteLuaVerbatim(const std::string& steamRoot, std::string_view lua)
{
    // The owner's file is written as-is, so there is no header and no
    // validation beyond refusing an empty body: writing "" would truncate the
    // file and silently drop every unlock.
    if (lua.empty()) {
        LOG_WARN("RemoteConfig: refusing to write an empty verbatim lua body");
        return false;
    }

    const fs::path path = LuaPath(steamRoot);

    // Same no-churn rule as the structured writer: identical bytes would only
    // make the watcher reparse and re-notify every cycle.
    std::string existing;
    if (ReadWholeFile(path, existing) && existing == lua) {
        LOG_DEBUG("RemoteConfig: {} already up to date", path.string());
        return true;
    }

    if (!WriteFileAtomic(path, lua, "Lua file"))
        return false;

    LOG_INFO("RemoteConfig: wrote verbatim lua ({} bytes) to {}", lua.size(),
             path.string());
    return true;
}

void Start()
{
    if (g_running.exchange(true)) {
        LOG_WARN("RemoteConfig: worker already running");
        return;
    }
    g_worker = std::thread(WorkerThread);
}

void Stop()
{
    // Always join, even if the flag says "not running": a worker launched by
    // Start() that exited on its own (not configured, no steamid) is still a
    // joinable std::thread, and destroying one terminates the process. The
    // join is bounded by the 4 WinHTTP phase timeouts of an in-flight fetch.
    g_running.store(false);
    g_wake.notify_all();
    if (g_worker.joinable())
        g_worker.join();
}

} // namespace RemoteConfig
