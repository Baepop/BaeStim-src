#include "PatternLoader.h"
#include "OSTPlatform/include/Memory.h"
#include "OSTPlatform/include/Numbers.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/PatternFallback.h"
#include "Utils/SteamMetadata/RemoteToml.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Support/FnvHash.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <toml++/toml.hpp>

// ---- compile-time sanity checks for FNV-1a table keys ----
// If the steam-monitor bot uses the same algorithm these must hold.
static_assert(Fnv1aHash("BBuildAndAsyncSendFrame") == 0x82428E37u,
              "FNV-1a mismatch for BBuildAndAsyncSendFrame");
static_assert(Fnv1aHash("BuildDepotDependency") == 0xC37F2D8Eu,
              "FNV-1a mismatch for BuildDepotDependency");

namespace {

// ---- per-function pattern record ----
struct PatternEntry {
    std::string name;
    uintptr_t   rva = 0;   // 0 = not present in file
    std::string sig;        // empty = not present in file
};

// key = Fnv1aHash(funcName)
using PatternMap = std::unordered_map<uint32_t, PatternEntry>;

// module → its pattern map
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, PatternMap> g_moduleMaps;

// Modules whose Load() call failed (popup already shown). FindPattern
// silently returns nullptr for these — without re-logging or adding the
// function to g_missingByModule — so we don't follow one "TOML missing"
// popup with a second popup listing every dependent hook.
static std::unordered_set<OSTPlatform::DynamicLibrary::ModuleHandle> g_failedModules;

// Registration info captured by Load() for every module that got that far,
// on EVERY return path of Load (success, fallback, total failure).
// ReportMissingFunctions needs the component + sha + steamRoot to build the
// popup-once marker key ("component_sha_missing") and to place the marker
// flag under the right Steam root.
struct ModuleContext {
    std::string component;
    std::string sha256;
    std::string steamRoot;
};
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, ModuleContext> g_moduleContexts;

// Per-module record of the functions FindPattern could not locate. Keyed by
// module (not a flat list) so ReportMissingFunctions can show one popup per
// module, each with its own component/sha marker key.
static std::unordered_map<OSTPlatform::DynamicLibrary::ModuleHandle, std::vector<std::string>> g_missingByModule;

// ---- byte-pattern scanner ----

static bool ParseSig(const std::string& str,
                     std::vector<uint8_t>& bytes,
                     std::vector<uint8_t>& mask)
{
    bytes.clear();
    mask.clear();
    for (const char* p = str.c_str(); *p; ) {
        if (*p == ' ' || *p == '\t' || *p == ',') { ++p; continue; }
        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0); mask.push_back(0); p += 2; continue;
        }
        char hi = p[0], lo = p[1];
        if (!hi || !lo) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(hi), l = nib(lo);
        if (h < 0 || l < 0) return false;
        bytes.push_back(static_cast<uint8_t>((h << 4) | l));
        mask.push_back(1);
        p += 2;
    }
    return !bytes.empty();
}

static void* ScanModule(OSTPlatform::DynamicLibrary::ModuleHandle module,
                        const std::vector<uint8_t>& bytes,
                        const std::vector<uint8_t>& mask)
{
    const auto image = OSTPlatform::Memory::GetModuleImage(module);
    if (!image) return nullptr;

    auto* base = image->base;
    size_t size = image->size;
    size_t patLen = bytes.size();
    if (size < patLen) return nullptr;

    for (size_t i = 0; i <= size - patLen; ++i) {
        bool found = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (mask[j] && base[i + j] != bytes[j]) { found = false; break; }
        }
        if (found) return base + i;
    }
    return nullptr;
}

// ---- TOML pattern parser ----

// Section keys are hex literals like "0x82428E37"; each section is a table
// with optional `name`, `rva` (hex string), and `sig` (IDA-style bytes).
static PatternMap TableToPatternMap(const toml::table& tbl)
{
    PatternMap map;
    map.reserve(tbl.size());
    for (auto& [rawKey, val] : tbl) {
        if (!val.is_table()) continue;
        auto& sub = *val.as_table();

        const auto parsedKey = OSTPlatform::Numbers::ParseHexUInt32(std::string(rawKey));
        if (!parsedKey) continue;
        const uint32_t hashKey = *parsedKey;

        PatternEntry entry;
        if (auto v = sub["name"].value<std::string>()) entry.name = *v;
        if (auto v = sub["rva"].value<std::string>()) {
            if (const auto rva = OSTPlatform::Numbers::ParseHexUInt64(*v)) {
                entry.rva = static_cast<uintptr_t>(*rva);
            }
        }
        if (auto v = sub["sig"].value<std::string>()) entry.sig = *v;

        map[hashKey] = std::move(entry);
    }
    return map;
}

static PatternMap ParsePatternString(std::string_view body,
                                     std::string* outError = nullptr)
{
    try {
        return TableToPatternMap(toml::parse(body));
    } catch (const toml::parse_error& e) {
        if (outError) *outError = e.description();
        return {};
    }
}

// Parse a neighbor TOML for sig-only use. Entries without a non-empty `sig`
// are DROPPED and every kept entry gets rva = 0: the neighbor's RVAs belong
// to a different build and must never reach FindPattern's priority-1 path.
// Returns how many of the kept sigs actually scan-hit in `module`.
static size_t EvaluateNeighbor(OSTPlatform::DynamicLibrary::ModuleHandle module,
                               const std::string& body,
                               PatternMap& outMap)
{
    outMap.clear();
    const PatternMap parsed = ParsePatternString(body);
    if (parsed.empty()) return 0;

    size_t hits = 0;
    for (const auto& [key, entry] : parsed) {
        if (entry.sig.empty()) continue; // no sig → useless for a foreign build

        PatternEntry kept = entry;
        kept.rva = 0; // forced: neighbor RVAs are meaningless in this build

        std::vector<uint8_t> bytes, mask;
        if (ParseSig(kept.sig, bytes, mask) && ScanModule(module, bytes, mask))
            ++hits;

        outMap[key] = std::move(kept);
    }
    return hits;
}

// ---- popup helpers (detached threads so we never block Steam) ----

// Build the "signature file not found" message, with enough detail to either
// (a) drop a file in manually, (b) check the upstream repo, or (c) file
// an actionable bug report.  Shared by the every-start popup (hash failed,
// no stable dedupe key) and the popup-once path in Load().
static std::string BuildDownloadFailedMessage(const std::string& dllName,
                                              const std::string& sha256,
                                              const std::string& component)
{
    return
        "BaeStim: signature file not found for " + dllName + ".\n\n"
        "Hooks that depend on " + dllName + " are disabled for this session; "
        "other modules are unaffected.\n\n"
        "You can:\n"
        "  1. Wait for the next signature update, then restart Steam.\n"
        "  2. Drop a matching TOML at:\n"
        "       <Steam>\\baestim\\pattern\\" + component + "\\" + sha256 + ".toml\n"
        "  3. Check upstream:\n"
        "       https://github.com/madoiscool/steam-monitor/tree/pattern/" + component + "\n"
        "  4. Report the diagnostics below:\n"
        "       https://github.com/madoiscool/BetterSteamTools/issues";
}

// Surface a missing pattern file to the user on every start. Only used when
// the DLL hash itself failed: without a sha there is no stable marker key to
// dedupe the popup on.
static void ShowDownloadFailedPopup(const std::string& dllName,
                                    const std::string& sha256,
                                    const std::string& component)
{
    SteamDiagnostics::ShowWarning("BaeStim - Unsupported Steam Version",
                                  BuildDownloadFailedMessage(dllName, sha256, component));
}

} // namespace

// ---- public API ----

namespace PatternLoader {

    constexpr const char* kPatternChannel = "pattern";

bool Load(OSTPlatform::DynamicLibrary::ModuleHandle module, const std::string& dllPath, const std::string& component)
{
    namespace fs = std::filesystem;

    // Delegate fetch + cache + mirror fallback to RemoteToml.
    RemoteToml::Result r = RemoteToml::Fetch({
        kPatternChannel,
        component,
        dllPath,
    });

    // The directory holding the Steam DLL is the Steam root every
    // fallback-cache and marker path hangs off of (PatternFallback and
    // SteamDiagnostics both build "<steamRoot>\baestim\...").
    const std::string steamRoot = fs::path(dllPath).parent_path().string();

    // Register the module context only when a stable sha256 exists: with an
    // empty sha there is no key to dedupe a popup-once marker on, and the
    // missing-popup key would degenerate (component__missing). Context-less
    // modules fall back to a direct ShowWarning in ReportMissingFunctions —
    // same rationale as the hash-failed popup below.
    if (!r.sha256.empty()) {
        g_moduleContexts[module] = ModuleContext{component, r.sha256, steamRoot};
    }

    if (r.ok) {
        std::string parseErr;
        PatternMap map = ParsePatternString(r.body, &parseErr);
        if (!map.empty()) {
            LOG_INFO("PatternLoader: loaded {} patterns for {} ({})",
                     map.size(), component, r.fromCache ? "cache fallback" : "remote");
            g_moduleMaps[module] = std::move(map);
            return true;
        }
        LOG_WARN("PatternLoader: TOML for {} parsed empty ({})",
                 component, parseErr.empty() ? "no entries" : parseErr);
    }

    // ---- neighbor-TOML fallback ----
    // The exact (sha-matched) fetch failed or its TOML parsed empty. Before
    // declaring the DLL unsupported, try the newest published neighbor
    // TOMLs: one may still carry sigs that hit in this build. Their RVAs
    // never do (different build) — EvaluateNeighbor zeroes them, so only
    // sig scans ever run against neighbor entries.
    if (!component.empty() && !r.sha256.empty()) {
        bool retriedListing = false;
        for (;;) {
            const std::vector<std::string> bodies = PatternFallback::GetNeighborBodies(
                steamRoot, kPatternChannel, component, r.sha256);

            size_t bestHits = 0;
            PatternMap bestMap;
            const std::string* bestBody = nullptr;

            // The primary listing orders bodies newest first (the jsDelivr
            // fallback listing has no recency ordering): a tie keeps the
            // earliest body because `best` is only replaced on a strictly
            // higher count.
            for (const std::string& body : bodies) {
                PatternMap candidate;
                const size_t hits = EvaluateNeighbor(module, body, candidate);
                if (hits > bestHits) {
                    bestHits = hits;
                    bestMap = std::move(candidate);
                    bestBody = &body;
                }
            }

            if (bestBody) {
                // Remember the winner as <sha>.fallback.toml so the next
                // start skips the listing + downloads entirely. Saving a
                // body that just came back from that same cache is
                // harmless: it rewrites the identical file.
                PatternFallback::SaveNeighborBody(steamRoot, kPatternChannel,
                                                  component, r.sha256,
                                                  *bestBody);
                const size_t kept = bestMap.size();
                g_moduleMaps[module] = std::move(bestMap);
                LOG_INFO("PatternLoader: fallback sig-scan resolved {}/{} functions for {} (neighbor TOML)",
                         bestHits, kept, component);
                return true;
            }

            // A single body can only be the previously saved fallback cache
            // (or a lone listed neighbor) that no longer hits this build:
            // drop the stale cache and let GetNeighborBodies list fresh
            // candidates exactly once. The re-call skips the cache (it was
            // just cleared) and lists — unless the unsupported marker
            // blocks it.
            if (!retriedListing && bodies.size() == 1) {
                retriedListing = true;
                PatternFallback::ClearNeighborCache(steamRoot, kPatternChannel,
                                                    component, r.sha256);
                continue;
            }
            break;
        }
    }

    // Total failure — popup + disable module's hooks. We deliberately only
    // disable hooks for the failing module — the rest of BaeStim keeps
    // working.
    std::string dllName = fs::path(dllPath).filename().string();
    std::string sha     = r.sha256.empty() ? "(hash failed)" : r.sha256;
    if (!r.sha256.empty()) {
        // Popup-once per (component, sha): the marker flag keeps later Steam
        // starts quiet while this exact build stays unsupported.
        SteamDiagnostics::ShowWarningOnce(
            steamRoot,
            PatternFallback::UnsupportedMarkerKey(component, r.sha256),
            "BaeStim - Unsupported Steam Version",
            BuildDownloadFailedMessage(dllName, sha, component));
    } else {
        // Hash failed → no stable key to dedupe on; pop up every start.
        ShowDownloadFailedPopup(dllName, sha, component);
    }
    g_failedModules.insert(module);
    return false;
}

void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle module, const char* funcName)
{
    // If the whole module's pattern file failed to load, stay quiet — the
    // user already saw one popup and the main.log already has the warning.
    // No point amplifying that into one log line per hook plus a second
    // "missing functions" popup later.
    if (g_failedModules.count(module)) {
        return nullptr;
    }

    uint32_t key = Fnv1aHash(funcName);

    auto mapIt = g_moduleMaps.find(module);
    if (mapIt == g_moduleMaps.end()) {
        // Load() was never called for this module.
        LOG_WARN("PatternLoader: FindPattern called for module that was never loaded "
                 "('{}')", funcName);
        g_missingByModule[module].emplace_back(funcName);
        return nullptr;
    }

    auto& map = mapIt->second;
    auto entryIt = map.find(key);
    if (entryIt == map.end()) {
        LOG_WARN("PatternLoader: no entry for '{}' (key=0x{:08X})", funcName, key);
        g_missingByModule[module].emplace_back(funcName);
        return nullptr;
    }

    const PatternEntry& entry = entryIt->second;

    // Priority 1: RVA direct offset
    if (entry.rva != 0) {
        void* addr = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(module) + entry.rva);
        LOG_DEBUG("PatternLoader: {} resolved via RVA 0x{:X}", funcName, entry.rva);
        return addr;
    }

    // Priority 2: byte-signature scan
    if (!entry.sig.empty()) {
        std::vector<uint8_t> bytes, mask;
        if (ParseSig(entry.sig, bytes, mask)) {
            void* addr = ScanModule(module, bytes, mask);
            if (addr) {
                uintptr_t rva = reinterpret_cast<uintptr_t>(addr) -
                                reinterpret_cast<uintptr_t>(module);
                LOG_DEBUG("PatternLoader: {} resolved via sig @ RVA 0x{:X}",
                          funcName, rva);
                return addr;
            }
            LOG_WARN("PatternLoader: sig scan miss for '{}' (pattern parsed OK, "
                     "no match in module image)", funcName);
        } else {
            LOG_WARN("PatternLoader: malformed sig for '{}': '{}'",
                     funcName, entry.sig);
        }
    } else {
        LOG_WARN("PatternLoader: entry for '{}' has neither rva nor sig", funcName);
    }

    g_missingByModule[module].emplace_back(funcName);
    return nullptr;
}

void ReportMissingFunctions()
{
    if (g_missingByModule.empty()) return;

    // Walk per module so each popup carries its own (component, sha) marker
    // key: a given (component, sha) missing-signatures popup shows once, and
    // later Steam starts stay quiet until the signatures actually change.
    for (auto& [module, names] : g_missingByModule) {
        if (names.empty()) continue;

        // Build the list
        std::string list;
        for (const auto& name : names)
            list += "  - " + name + "\n";
        names.clear();

        const std::string message =
            "BaeStim: some functions could not be located.\n\n"
            "The following functions were not found in the signature file:\n" +
            list +
            "\nHooks for these functions are disabled for this session.\n\n"
            "Please report this at:\n"
            "https://github.com/madoiscool/BetterSteamTools/issues";

        auto ctxIt = g_moduleContexts.find(module);
        if (ctxIt != g_moduleContexts.end()) {
            const ModuleContext& ctx = ctxIt->second;
            SteamDiagnostics::ShowWarningOnce(
                ctx.steamRoot,
                ctx.component + "_" + ctx.sha256 + "_missing",
                "BaeStim - Missing Signatures",
                message);
        } else {
            // Load() never registered a context for this module (e.g. the
            // "never loaded" path in FindPattern): pop up directly, no
            // marker — same behavior as before the fallback existed.
            SteamDiagnostics::ShowWarning("BaeStim - Missing Signatures",
                                          message);
        }
    }
    g_missingByModule.clear();
}

} // namespace PatternLoader
