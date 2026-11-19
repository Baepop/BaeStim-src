#include "SteamDiagnostics.h"
#include "BaeStimBuildInfo.h"
#include "OSTPlatform/include/Dialog.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Hash.h"
#include "Utils/Logging/Log.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace SteamDiagnostics {

namespace {

    struct Snapshot {
        std::string baestimVersion = BAESTIM_VERSION;
        std::string buildID = "(unavailable)";
        std::string steamclientPath;
        std::string steamclientSha256 = "(unavailable)";
        std::string steamUIPath;
        std::string steamUISha256 = "(unavailable)";
    };

    Snapshot g_snapshot;

    static std::string DetectSteamBuildID()
    {
        using GetBootstrapperVersion_t = int64_t (*)();

        const auto steam = OSTPlatform::DynamicLibrary::GetLoaded("steam.exe");
        if (!steam) {
            LOG_WARN("SteamDiagnostics: steam.exe module not loaded; build id unavailable");
            return "(unavailable)";
        }

        const auto getBootstrapperVersion =
            reinterpret_cast<GetBootstrapperVersion_t>(
                OSTPlatform::DynamicLibrary::GetSymbol(steam, "GetBootstrapperVersion"));
        if (!getBootstrapperVersion) {
            LOG_WARN("SteamDiagnostics: steam.exe!GetBootstrapperVersion not exported");
            return "(unavailable)";
        }

        return std::to_string(getBootstrapperVersion());
    }

    static std::string HashOrUnavailable(const std::string& path)
    {
        std::string sha256 = OSTPlatform::Hash::Sha256OfFile(path);
        return sha256.empty() ? "(unavailable)" : std::move(sha256);
    }

    static std::string AppendSnapshot(std::string message)
    {
        message +=
            "\n\nSteam diagnostics:\n"
            "  BaeStim version: " + g_snapshot.baestimVersion + "\n"
            "  Build ID:              " + g_snapshot.buildID + "\n"
            "  steamclient64.dll SHA: " + g_snapshot.steamclientSha256 + "\n"
            "  steamui.dll SHA:       " + g_snapshot.steamUISha256;
        return message;
    }

} // namespace

namespace {

    std::string IsoTimestampNow()
    {
        const std::time_t now =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        std::tm tmUtc {};
#ifdef _WIN32
        gmtime_s(&tmUtc, &now);
#else
        gmtime_r(&now, &tmUtc);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

} // namespace

void Initialize(const std::string& steamclientPath,
                const std::string& steamUIPath)
{
    g_snapshot.buildID = DetectSteamBuildID();
    g_snapshot.steamclientPath = steamclientPath;
    g_snapshot.steamclientSha256 = HashOrUnavailable(steamclientPath);
    g_snapshot.steamUIPath = steamUIPath;
    g_snapshot.steamUISha256 = HashOrUnavailable(steamUIPath);

    LOG_INFO("SteamDiagnostics: ost.version={} build={} steamclient64.sha256={} steamui.sha256={}",
             g_snapshot.baestimVersion,
             g_snapshot.buildID,
             g_snapshot.steamclientSha256,
             g_snapshot.steamUISha256);
}

std::string Sha256Of(const std::string& path)
{
    if (path == g_snapshot.steamclientPath)
        return g_snapshot.steamclientSha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamclientSha256;

    if (path == g_snapshot.steamUIPath)
        return g_snapshot.steamUISha256 == "(unavailable)"
            ? std::string{}
            : g_snapshot.steamUISha256;

    return OSTPlatform::Hash::Sha256OfFile(path);
}

void ShowWarning(std::string title, std::string message)
{
    std::thread([title = std::move(title),
                 message = AppendSnapshot(std::move(message))]() {
        OSTPlatform::Dialog::ShowWarning(title, message);
    }).detach();
}

void ShowWarningOnce(const std::string& steamRoot,
                     const std::string& markerKey,
                     std::string title,
                     std::string message)
{
    // No marker key → no once-semantics; behave exactly like ShowWarning.
    if (markerKey.empty()) {
        ShowWarning(std::move(title), std::move(message));
        return;
    }

    namespace fs = std::filesystem;
    const fs::path markersDir = fs::path(steamRoot) / "baestim" / "markers";
    const fs::path flagPath = markersDir / (markerKey + ".flag");

    std::error_code mkdirEc;
    fs::create_directories(markersDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("SteamDiagnostics: could not create markers dir {} ({})",
                 markersDir.string(), mkdirEc.message());
    }

    {
        std::error_code ec;
        if (fs::exists(flagPath, ec)) {
            LOG_INFO("SteamDiagnostics: marker {} already present, suppressing warning",
                     flagPath.string());
            return;
        }
    }

    // Write the flag synchronously BEFORE showing: if Steam dies right after the
    // popup appears, the warning must still count as shown. The popup itself is
    // the async part (its own thread, via ShowWarning).
    {
        std::ofstream ofs(flagPath, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs << IsoTimestampNow();
        }
        if (!ofs) {
            // Degraded: no marker on disk, so this popup may repeat next launch.
            // Warn instead of dropping the message entirely.
            LOG_WARN("SteamDiagnostics: could not write marker {}", flagPath.string());
        }
    }

    ShowWarning(std::move(title), std::move(message));
}

} // namespace SteamDiagnostics
