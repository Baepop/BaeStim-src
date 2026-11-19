// SteamLibrary fixtures — plain stdlib, no framework, no network.
//
// Covers the resolver a trainer worker calls to find a game's install folder:
//   - ParseLibraryFolders: every "path" value in libraryfolders.vdf is collected
//     in order (the numeric key above it is not depended on), non-path keys
//     ("contentid", nested "apps") are ignored, a relative path is resolved
//     against steamRoot, and CRLF plus a UTF-8 BOM are accepted;
//   - ParseAppManifest: the install folder and display name are extracted and
//     the fully-installed bit is (StateFlags & 4); a manifest without a usable
//     installdir is rejected;
//   - Resolve: walks the root library plus every library found in
//     libraryfolders.vdf and returns <library>\steamapps\common\<installdir>
//     only when the manifest is present and fully installed.
//
// Parser fixtures are strings; only the Resolve cases touch disk, and those
// build their tree under temp_directory_path() and remove it afterwards.

#include "Utils/Trainers/SteamLibrary.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// Simple CHECK macro: counts failures, main() returns 1 if any failed.
static int g_checksFailed = 0;
static int g_checksRun    = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checksRun;                                                           \
        if (!(cond)) {                                                           \
            ++g_checksFailed;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

// ── helpers ────────────────────────────────────────────────────────────────

// Owns one temp directory; removes it when the case ends, pass or fail.
struct TempDir {
    fs::path path;

    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() /
               (std::string("baestim_trainers_lib_") + name))
    {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
        CHECK(!ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// UTF-8 bytes of a path, so fixtures do not depend on the code page.
static std::string U8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// Builds a path from UTF-8 bytes, the same way the resolver does.
static fs::path PathFromU8(std::string_view bytes)
{
    std::u8string utf8;
    utf8.reserve(bytes.size());
    for (const char c : bytes)
        utf8.push_back(static_cast<char8_t>(c));
    return fs::path(utf8);
}

// A path as it appears inside a quoted VDF value: backslashes escaped.
static std::string VdfValue(const fs::path& p)
{
    const std::string raw = U8(p);
    std::string out;
    out.reserve(raw.size() * 2);
    for (const char c : raw) {
        if (c == '\\' || c == '"')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static void WriteFile(const fs::path& p, std::string_view bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(static_cast<bool>(out));
}

// A real-shaped appmanifest_<appid>.acf. Steam writes tab-indented KeyValues;
// the nested blocks are included so the parser has to stay depth-aware.
static std::string MakeAcf(std::string_view stateFlags,
                           std::string_view installdir = "TMNT")
{
    std::string acf =
        "\"AppState\"\n"
        "{\n"
        "\t\"appid\"\t\t\"105600\"\n"
        "\t\"Universe\"\t\t\"1\"\n"
        "\t\"name\"\t\t\"Teenage Mutant Ninja Turtles\"\n"
        "\t\"StateFlags\"\t\t\"";
    acf += stateFlags;
    acf +=
        "\"\n"
        "\t\"installdir\"\t\t\"";
    acf += installdir;
    acf +=
        "\"\n"
        "\t\"LastUpdated\"\t\t\"1700000000\"\n"
        "\t\"SizeOnDisk\"\t\t\"123456789\"\n"
        "\t\"buildid\"\t\t\"9876543\"\n"
        "\t\"InstalledDepots\"\n"
        "\t{\n"
        "\t\t\"105601\"\n"
        "\t\t{\n"
        "\t\t\t\"manifest\"\t\t\"111\"\n"
        "\t\t}\n"
        "\t}\n"
        "\t\"UserConfig\"\n"
        "\t{\n"
        "\t\t\"language\"\t\t\"english\"\n"
        "\t}\n"
        "}\n";
    return acf;
}

// ── ParseLibraryFolders ────────────────────────────────────────────────────

static void Library_OneLibrary()
{
    const std::string vdf = R"("0" { "path" "D:\\SteamLibrary" })";

    const std::vector<fs::path> paths =
        Trainers::SteamLibrary::ParseLibraryFolders(vdf, "C:\\Steam");

    CHECK(paths.size() == 1);
    if (paths.size() == 1)
        CHECK(paths[0] == fs::path("D:\\SteamLibrary"));
}

static void Library_TwoLibrariesInOrder()
{
    // contentid and the nested apps block must not leak into the result, and a
    // // comment between entries must be skipped like any other whitespace.
    const std::string vdf =
        "\"libraryfolders\"\n"
        "{\n"
        "\t\"0\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n"
        "\t\t\"label\"\t\t\"\"\n"
        "\t\t\"contentid\"\t\t\"1234567890\"\n"
        "\t\t\"apps\"\n"
        "\t\t{\n"
        "\t\t\t\"105600\"\t\t\"111\"\n"
        "\t\t}\n"
        "\t}\n"
        "\t// the second library\n"
        "\t\"1\"\n"
        "\t{\n"
        "\t\t\"path\"\t\t\"E:\\\\Games\"\n"
        "\t\t\"contentid\"\t\t\"987654321\"\n"
        "\t}\n"
        "}\n";

    const std::vector<fs::path> paths =
        Trainers::SteamLibrary::ParseLibraryFolders(vdf, "C:\\Steam");

    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == fs::path("D:\\SteamLibrary"));
        CHECK(paths[1] == fs::path("E:\\Games"));
    }
}

static void Library_RelativePathIsRelativeToSteamRoot()
{
    const std::string vdf = R"("0" { "path" "SteamLibrary" })";

    const std::vector<fs::path> paths =
        Trainers::SteamLibrary::ParseLibraryFolders(vdf, fs::path("C:\\Steam"));

    CHECK(paths.size() == 1);
    if (paths.size() == 1)
        CHECK(paths[0] == fs::path("C:\\Steam") / "SteamLibrary");
}

static void Library_CrlfAndBom()
{
    // UTF-8 BOM, CRLF line endings and a tab-indented body: what a
    // Windows-authored libraryfolders.vdf actually looks like.
    std::string vdf = "\xEF\xBB\xBF";
    vdf += "\"libraryfolders\"\r\n";
    vdf += "{\r\n";
    vdf += "\t\"0\"\r\n";
    vdf += "\t{\r\n";
    vdf += "\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\r\n";
    vdf += "\t}\r\n";
    vdf += "}\r\n";

    const std::vector<fs::path> paths =
        Trainers::SteamLibrary::ParseLibraryFolders(vdf, "C:\\Steam");

    CHECK(paths.size() == 1);
    if (paths.size() == 1)
        CHECK(paths[0] == fs::path("D:\\SteamLibrary"));
}

static void Library_Utf8Path()
{
    // Steam stores paths as UTF-8, and a library can live under a non-ASCII
    // profile directory. "é" is spelled as bytes C3 A9 so this fixture does
    // not depend on the compiler's input encoding.
    const fs::path library = PathFromU8("C:\\Users\\Jos\xC3\xA9\\SteamLibrary");

    const std::string vdf = "\"0\" { \"path\" \"" + VdfValue(library) + "\" }";

    const std::vector<fs::path> paths =
        Trainers::SteamLibrary::ParseLibraryFolders(vdf, "C:\\Steam");

    CHECK(paths.size() == 1);
    if (paths.size() == 1)
        CHECK(paths[0] == library);
}

static void Library_EmptyInputYieldsNothing()
{
    CHECK(Trainers::SteamLibrary::ParseLibraryFolders("", "C:\\Steam").empty());
    CHECK(Trainers::SteamLibrary::ParseLibraryFolders("   \r\n\t ", "C:\\Steam")
              .empty());
}

// ── ParseAppManifest ───────────────────────────────────────────────────────

static void Manifest_FullyInstalledFlags()
{
    const char* const flags[] = { "4", "6" };
    for (const char* flag : flags) {
        std::string installdir;
        std::string name;
        bool fullyInstalled = false;

        CHECK(Trainers::SteamLibrary::ParseAppManifest(
            MakeAcf(flag), installdir, name, fullyInstalled));
        CHECK(installdir == "TMNT");
        CHECK(name == "Teenage Mutant Ninja Turtles");
        CHECK(fullyInstalled);
    }
}

static void Manifest_NotFullyInstalled()
{
    std::string installdir;
    std::string name;
    bool fullyInstalled = true;

    CHECK(Trainers::SteamLibrary::ParseAppManifest(MakeAcf("2"), installdir, name,
                                                   fullyInstalled));
    CHECK(installdir == "TMNT");
    CHECK(name == "Teenage Mutant Ninja Turtles");
    CHECK(!fullyInstalled);
}

static void Manifest_MissingOrMalformedStateFlags()
{
    // No StateFlags at all, or a value that is not a number: the manifest still
    // parses (installdir is there) but cannot be fully installed.
    const std::string base =
        "\"AppState\"\n"
        "{\n"
        "\t\"name\"\t\t\"Teenage Mutant Ninja Turtles\"\n"
        "\t\"installdir\"\t\t\"TMNT\"\n";

    const std::string noFlags        = base + "}\n";
    const std::string malformedFlags = base + "\t\"StateFlags\"\t\t\"abc\"\n}\n";

    const std::string* const cases[] = { &noFlags, &malformedFlags };
    for (const std::string* acf : cases) {
        std::string installdir;
        std::string name;
        bool        fullyInstalled = true;

        CHECK(Trainers::SteamLibrary::ParseAppManifest(*acf, installdir, name,
                                                       fullyInstalled));
        CHECK(installdir == "TMNT");
        CHECK(name == "Teenage Mutant Ninja Turtles");
        CHECK(!fullyInstalled);
    }
}

static void Manifest_OutOfRangeStateFlags()
{
    // 2^32 + 4 would leave bit 4 set if the value were truncated to 32 bits;
    // 2^64 does not parse into uint64 at all. Both count as 0, so neither is
    // fully installed.
    const char* const flags[] = { "4294967300", "18446744073709551616" };
    for (const char* flag : flags) {
        std::string installdir;
        std::string name;
        bool        fullyInstalled = true;

        CHECK(Trainers::SteamLibrary::ParseAppManifest(MakeAcf(flag), installdir,
                                                       name, fullyInstalled));
        CHECK(installdir == "TMNT");
        CHECK(!fullyInstalled);
    }
}

static void Manifest_IgnoresNestedFields()
{
    // Only the direct AppState children count: a nested block that also carries
    // "name"/"installdir" must not shadow the top-level values.
    const std::string acf =
        "\"AppState\"\n"
        "{\n"
        "\t\"name\"\t\t\"Top Level\"\n"
        "\t\"installdir\"\t\t\"TopFolder\"\n"
        "\t\"InstalledDepots\"\n"
        "\t{\n"
        "\t\t\"111\"\n"
        "\t\t{\n"
        "\t\t\t\"name\"\t\t\"Nested\"\n"
        "\t\t\t\"installdir\"\t\t\"NestedFolder\"\n"
        "\t\t}\n"
        "\t}\n"
        "\t\"StateFlags\"\t\t\"4\"\n"
        "}\n";

    std::string installdir;
    std::string name;
    bool        fullyInstalled = false;

    CHECK(Trainers::SteamLibrary::ParseAppManifest(acf, installdir, name,
                                                   fullyInstalled));
    CHECK(installdir == "TopFolder");
    CHECK(name == "Top Level");
    CHECK(fullyInstalled);
}

static void Manifest_RejectsMissingOrEmptyInstallDir()
{
    std::string installdir;
    std::string name;
    bool        fullyInstalled = false;

    // installdir key absent entirely.
    const std::string noInstallDir =
        "\"AppState\"\n{\n\t\"name\"\t\t\"No Folder\"\n"
        "\t\"StateFlags\"\t\t\"4\"\n}\n";
    fullyInstalled = true;
    CHECK(!Trainers::SteamLibrary::ParseAppManifest(noInstallDir, installdir,
                                                    name, fullyInstalled));
    CHECK(installdir.empty());
    CHECK(name.empty());
    CHECK(!fullyInstalled);

    // installdir present but empty.
    const std::string emptyInstallDir =
        "\"AppState\"\n{\n\t\"name\"\t\t\"Empty Folder\"\n"
        "\t\"StateFlags\"\t\t\"4\"\n\t\"installdir\"\t\t\"\"\n}\n";
    fullyInstalled = true;
    CHECK(!Trainers::SteamLibrary::ParseAppManifest(emptyInstallDir, installdir,
                                                    name, fullyInstalled));
    CHECK(installdir.empty());
    CHECK(name.empty());
    CHECK(!fullyInstalled);

    // Empty input has nothing to resolve either.
    fullyInstalled = true;
    CHECK(!Trainers::SteamLibrary::ParseAppManifest("", installdir, name,
                                                    fullyInstalled));
    CHECK(installdir.empty());
    CHECK(name.empty());
    CHECK(!fullyInstalled);
}

// ── Resolve ────────────────────────────────────────────────────────────────

static void Resolve_FromLibraryFolders()
{
    TempDir tmp("resolve");
    const fs::path library = tmp.path / "lib2";

    WriteFile(tmp.path / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                  VdfValue(library) + "\"\n\t}\n}\n");
    WriteFile(library / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "Terraria"));

    const std::optional<Trainers::SteamLibrary::GameInstall> found =
        Trainers::SteamLibrary::Resolve(105600, tmp.path);

    CHECK(found.has_value());
    if (found) {
        CHECK(found->appid == 105600);
        CHECK(found->installDir ==
              library / "steamapps" / "common" / "Terraria");
        CHECK(found->name == "Teenage Mutant Ninja Turtles");
    }
}

static void Resolve_SteamRootIsALibrary()
{
    // No libraryfolders.vdf at all: the root Steam install is the library.
    TempDir tmp("root_library");

    WriteFile(tmp.path / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "Terraria"));

    const std::optional<Trainers::SteamLibrary::GameInstall> found =
        Trainers::SteamLibrary::Resolve(105600, tmp.path);

    CHECK(found.has_value());
    if (found)
        CHECK(found->installDir ==
              tmp.path / "steamapps" / "common" / "Terraria");
}

static void Resolve_RefusesNotFullyInstalled()
{
    TempDir tmp("partial");
    const fs::path library = tmp.path / "lib2";

    WriteFile(tmp.path / "steamapps" / "libraryfolders.vdf",
              "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" +
                  VdfValue(library) + "\"\n\t}\n}\n");
    WriteFile(library / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("2", "Terraria"));

    CHECK(!Trainers::SteamLibrary::Resolve(105600, tmp.path).has_value());
}

static void Resolve_RefusesRootedInstallDir()
{
    TempDir tmp("rooted_installdir");

    // A rooted value would discard <library>\steamapps\common entirely.
    WriteFile(tmp.path / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "C:/Windows/System32"));

    CHECK(!Trainers::SteamLibrary::Resolve(105600, tmp.path).has_value());
}

static void Resolve_RefusesTraversalInstallDir()
{
    TempDir tmp("traversal_installdir");

    // ".." would let the manifest escape steamapps/common.
    WriteFile(tmp.path / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "../outside"));

    CHECK(!Trainers::SteamLibrary::Resolve(105600, tmp.path).has_value());
    CHECK(!fs::exists(tmp.path / "steamapps" / "common"));
}

static void Resolve_NormalisesInstallDir()
{
    TempDir tmp("normalise_installdir");

    // A harmless nested value is normalised, not rejected.
    WriteFile(tmp.path / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "Terraria/./bin"));

    const std::optional<Trainers::SteamLibrary::GameInstall> found =
        Trainers::SteamLibrary::Resolve(105600, tmp.path);

    CHECK(found.has_value());
    if (found) {
        CHECK(found->installDir == tmp.path / "steamapps" / "common" /
                                       "Terraria" / "bin");
    }
}

static void Resolve_UnknownAppid()
{
    TempDir tmp("unknown");

    WriteFile(tmp.path / "steamapps" / "appmanifest_105600.acf",
              MakeAcf("4", "Terraria"));

    // Another app's manifest is present; this appid has none.
    CHECK(!Trainers::SteamLibrary::Resolve(999999, tmp.path).has_value());
}

static void Resolve_EmptySteamRoot()
{
    CHECK(!Trainers::SteamLibrary::Resolve(105600, fs::path()).has_value());
}

int main()
{
    Library_OneLibrary();
    Library_TwoLibrariesInOrder();
    Library_RelativePathIsRelativeToSteamRoot();
    Library_CrlfAndBom();
    Library_Utf8Path();
    Library_EmptyInputYieldsNothing();

    Manifest_FullyInstalledFlags();
    Manifest_NotFullyInstalled();
    Manifest_MissingOrMalformedStateFlags();
    Manifest_OutOfRangeStateFlags();
    Manifest_IgnoresNestedFields();
    Manifest_RejectsMissingOrEmptyInstallDir();

    Resolve_FromLibraryFolders();
    Resolve_SteamRootIsALibrary();
    Resolve_RefusesNotFullyInstalled();
    Resolve_RefusesRootedInstallDir();
    Resolve_RefusesTraversalInstallDir();
    Resolve_NormalisesInstallDir();
    Resolve_UnknownAppid();
    Resolve_EmptySteamRoot();

    if (g_checksFailed != 0) {
        printf("FAILED (%d of %d checks)\n", g_checksFailed, g_checksRun);
        return 1;
    }
    printf("ALL OK (%d checks)\n", g_checksRun);
    return 0;
}
