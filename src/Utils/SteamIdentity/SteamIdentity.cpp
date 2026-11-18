#include "Utils/SteamIdentity/SteamIdentity.h"
#include "Utils/Logging/Log.h"

#include <atomic>

namespace SteamIdentity {

    namespace {
        std::atomic<uint64_t> g_localSteamId{0};
    }

    void SetLocalSteamId(uint64_t steamId)
    {
        if (steamId == 0)
            return;

        // First non-zero value wins. Exchange only from the empty state, so
        // concurrent capture points (IPC and netpacket run on different
        // threads) converge on whichever value arrived first.
        uint64_t current = 0;
        if (g_localSteamId.compare_exchange_strong(current, steamId,
                std::memory_order_release, std::memory_order_relaxed)) {
            return;
        }

        // A different id after a successful capture would mean the session
        // identity changed mid-process, which Steam does not do. Keep the
        // first value and leave a trail for diagnosis.
        if (current != steamId) {
            LOG_WARN("SteamIdentity: ignoring session SteamID change 0x{:X} -> 0x{:X}",
                     current, steamId);
        }
    }

    uint64_t GetLocalSteamId()
    {
        return g_localSteamId.load(std::memory_order_acquire);
    }

} // namespace SteamIdentity
