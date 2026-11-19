#pragma once
#include <cstdint>

namespace SteamIdentity {

    // Real SteamID of the signed-in Steam session.
    //
    // Captured from the two transport-level points that expose it unspoofed:
    // the ORIGINAL IClientUser::GetSteamID response (before the ticket-driven
    // spoof rewrites it) and CMsgProtoBufHeader.steamid on outbound
    // CMsgClientGamesPlayed. Values produced by the spoofing path are never
    // fed here.
    //
    // A process never switches Steam account in place, so the first non-zero
    // value wins: later different values are ignored and logged as a warning.
    // Zero is ignored (it means "not observed yet").
    void SetLocalSteamId(uint64_t steamId);

    // 0 until a capture point has observed the session SteamID.
    // Thread-safe.
    uint64_t GetLocalSteamId();

} // namespace SteamIdentity
