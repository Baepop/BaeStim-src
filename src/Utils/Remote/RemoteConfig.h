#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Per-user appid list fetched from the Supabase Edge Function and materialised
// as a Lua file the existing LuaFileWatcher loads. The unit owns its worker
// thread: start it after the Lua watcher (it writes into a watched directory)
// and stop it from the DLL_PROCESS_DETACH cleanup.
//
// Behaviour:
//   - one fetch attempt per cycle, 30 minutes apart, short HTTP timeouts;
//   - a successful (authoritative 200) response is cached offline under
//     <Steam>\baestim\remote\<steamid>.json and rendered to Lua;
//   - a 200 whose body carries a non-empty "lua" string is FULL-LUA mode: that
//     string is the whole file, written verbatim (the owner pastes a client
//     .lua into users.lua_text), and the structured "appids" list is ignored;
//   - when the fetch fails the previous cache for the same steamid is used;
//   - with no cache, the Lua file is only cleared when one exists from an
//     earlier session (never created just to say "empty").
namespace RemoteConfig {

    // One normalised entry of the per-user config: appid is always non-zero and
    // in uint32 range, key is empty or 64 hex chars, token is empty or a
    // decimal uint64. Anything the Lua parser would reject never gets here.
    struct Entry {
        uint32_t    appid = 0;
        std::string key;   // depot decryption key, or empty
        std::string token; // access token, or empty
    };

    // Outcome of one fetch. ok means "authoritative 200": HTTP 200 plus either
    // a present and fully delimited "appids" array (structured mode) or a
    // non-empty "lua" string (full-Lua mode). Only an ok result may be written
    // to the offline cache; a 200 whose body has neither (or a truncated
    // array/value) is a suspicious response, so it yields an empty list but is
    // never cached.
    struct FetchResult {
        bool               ok            = false;
        bool               appidsPresent = false;
        std::vector<Entry> entries;
        bool               luaPresent    = false;
        std::string        lua; // verbatim .lua body when luaPresent
    };

    // Pure network + parse step: no filesystem, no thread state, so the whole
    // request/parse contract is verifiable by inspection.
    //
    //   GET <url>/functions/v1/config?steamid=<steamId>
    //   Authorization: Bearer <anonKey>
    //
    // httpBody receives the raw response body (empty when no response arrived)
    // so the caller can cache exactly what the server sent.
    FetchResult FetchConfig(const std::string& url,
                            const std::string& anonKey,
                            uint64_t steamId,
                            std::string& httpBody);

    // Pure render + atomic write of the synthetic Lua file. The filename is
    // FIXED on purpose (<Steam>\config\baestim\baestim-remote.lua): the file is
    // rewritten as a whole, so an account switch replaces the entire set and
    // the watcher's per-file refcount unloads the previous user's appids.
    // steamId only feeds the "generated for <steamid>" header comment.
    bool WriteRemoteLua(const std::string& steamRoot,
                        uint64_t steamId,
                        const std::vector<Entry>& entries);

    // Full-Lua mode counterpart of WriteRemoteLua: writes |lua| EXACTLY as
    // received (no generated header, no re-render, no trailing newline added),
    // through the same atomic write-then-rename and identical-bytes skip. The
    // owner controls the content, so nothing is validated here beyond the
    // empty-body refusal (never truncate the file to nothing). Returns false
    // when the body is empty or the write failed.
    bool WriteRemoteLuaVerbatim(const std::string& steamRoot, std::string_view lua);

    // Start() is idempotent. Stop() flips the flag, wakes the worker and always
    // joins a joinable worker (a thread that exited on its own is not leaked).
    void Start();
    void Stop();

    // ── test surface ────────────────────────────────────────────────────────
    //
    // The pure core of the unit, exposed so tests/RemoteConfigTest.cpp can
    // assert the parse and render contracts without network, disk or threads.
    // Production code must keep using FetchConfig / WriteRemoteLua /
    // WriteRemoteLuaVerbatim; this namespace exists only for tests.
    namespace Test {

        struct ParseResult {
            // Full-Lua mode: the body carried a non-empty "lua" string. When
            // true, lua holds the decoded file and the fields below are
            // deliberately empty (the structured list is ignored).
            bool        luaPresent = false;
            std::string lua;

            bool               keyPresent = false; // "appids" key found
            bool               complete   = false; // array closed by ']'
            std::vector<Entry> entries;
        };

        ParseResult ParseResponse(std::string_view body);

        // emitted receives the number of addappid() lines written. The key, when
        // valid, is emitted in argument 3: addappid(<appid>, 0, "<key>").
        std::string RenderLua(uint64_t steamId,
                              const std::vector<Entry>& entries,
                              size_t& emitted);

    } // namespace Test

} // namespace RemoteConfig
