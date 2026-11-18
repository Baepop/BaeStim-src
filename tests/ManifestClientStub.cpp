// Inert ManifestClient definitions for the baestim_remoteconfig_test executable.
//
// RemoteConfig.cpp calls Config::GetSupabaseUrl()/GetSupabaseAnonKey(), so the
// test target compiles the real Config.cpp. Config.cpp in turn names two
// ManifestClient entry points while loading [manifest]. Linking the real
// ManifestClient would drag in LuaConfig, AppTicket and the Steam client for a
// single provider-name lookup no test exercises, so this TU supplies the two
// symbols the config unit needs and nothing else.
//
// Nothing asserts on providers here: the test binary never calls Config::Load.
#include "Utils/SteamMetadata/ManifestClient.h"

namespace ManifestClient {

    bool SetProvider(std::string_view) { return true; }

    const char* ActiveProviderName() { return "baestim"; }

} // namespace ManifestClient
