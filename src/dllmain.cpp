#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Utils/Config/Config.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Remote/RemoteConfig.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/ManifestDonor.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "Utils/Tokeer/TokeerBridge.h"
#include "Utils/Trainers/Trainers.h"
#ifdef OST_ENABLE_UPDATER
#include "Utils/Update/AppUpdater.h"
#endif
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <string>
#include <windows.h>

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion.dll", SteamInstallPath);
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s\\config\\baestim",    SteamInstallPath);
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s\\baestim.toml",       SteamInstallPath);
    
    client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
    if (!client_hModule) {
        LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                  SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    LOG_INFO("Loaded steamclient64.dll from {}", SteamclientPath);
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("BaeStim init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs =
        LuaConfig::MergeWatchDirs(Config::GetLuaPaths(), std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    // Per-user appid list from the Supabase Edge Function. Started after the Lua
    // watcher on purpose: the worker rewrites config/baestim/baestim-remote.lua,
    // which only takes effect once something is watching that directory. Returns
    // immediately (own thread) and idles when [supabase] is not configured.
    RemoteConfig::Start();

    // Trainer bundles assigned to this account. Always compiled in and gated
    // by [trainers] enabled; the worker downloads, verifies, extracts and
    // copies pending trainers on its own thread, then shows one summary.
    Trainers::Start();

    SteamUI::CoreHook();
    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    // Contributes manifest request codes for depots this account owns, on
    // request. Started after the hooks are in place because it needs the
    // netpacket send path; it idles until the license list resolves anyway.
    ManifestDonor::Start();

    // Register the bst:// URI scheme so the website can drive code redemption via this
    // DLL (rundll32 handler). HKCU, no admin; idempotent.
    TokeerBridge::RegisterUriScheme(std::string(SteamInstallPath) + "\\BaeStim.dll");

    // Self-update check. Runs on its own detached thread so the network round-trip
    // never delays hook installation; a staged DLL applies on the next launch.
    //
    // The only source is this project's own public Supabase "updates" bucket
    // (latest.toml + the DLL it names, SHA-256 verified), so a build here cannot
    // be replaced by an upstream release. [update] enabled = false in baestim.toml
    // turns the check off at runtime while the updater stays compiled in.
    // -DOST_ENABLE_UPDATER=OFF is the build-time cut: the unit is not in the
    // binary and the DLL makes no update request at all.
#ifdef OST_ENABLE_UPDATER
    if (Config::GetUpdateEnabled()) {
        LOG_INFO("Self-updater enabled (source: public updates bucket)");
        OSTPlatform::Thread::StartDetached([] () -> uint32_t {
            const std::string self = std::string(SteamInstallPath) + "\\BaeStim.dll";
            AppUpdater::CleanupStagedBackup(self);

            const AppUpdater::CheckResult upd = AppUpdater::Check();
            if (!upd.updateAvailable) return 0;
            if (!AppUpdater::DownloadAndStage(upd, self)) return 0;

            const bool restart = OSTPlatform::Dialog::ShowConfirm(
                "BaeStim Updated!",
                upd.oldVersion + " -> " + upd.newVersion +
                "\n\nRestart Steam now to apply?");
            if (restart) AppUpdater::RestartSteam();
            return 0;
        });
    } else {
        LOG_INFO("Self-updater disabled by [update] enabled=false");
    }
#else
    LOG_INFO("Self-updater not compiled in (OST_ENABLE_UPDATER=OFF)");
#endif

    LOG_INFO("BaeStim init complete");
    return 0;
}

// True only when the host process is steam.exe. The proxy DLLs already gate injection to
// Steam, but rundll32 loads this DLL directly to service a bst:// link — there we must NOT
// run the Steam-injection machinery (steamclient load, hooks, watchers); the TokeerUri
// export does its work standalone.
static bool IsSteamHost()
{
    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) return false;
    const char* name = strrchr(exePath, '\\');
    name = name ? name + 1 : exePath;
    return _stricmp(name, "steam.exe") == 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        if (!IsSteamHost())
            return TRUE;   // e.g. rundll32 bst:// handler — no injection here
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH && IsSteamHost())
    {
        // Producer before consumers: the remote worker writes into the watched
        // Lua directory, so it is joined before the watchers that consume that
        // file go away. Stop() always joins, bounded by the in-flight fetch's
        // HTTP phase timeouts (~15 s worst case).
        RemoteConfig::Stop();
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
