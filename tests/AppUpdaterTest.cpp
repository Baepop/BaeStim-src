// AppUpdater pointer fixtures — plain stdlib, no framework, no network.
//
// Covers the pure surface exposed by AppUpdater::Test:
//   - ParseLatestToml: the exact field contract of the bucket's latest.toml
//     (version/path/sha256 present and non-empty) plus the hardening added when
//     the pointer moved from the upstream GitHub mirrors to the project's own
//     Supabase bucket: "path" must be a plain file name (it is joined to the
//     object URL and later written next to Steam) and "sha256" must be exactly
//     64 hex chars;
//   - BuildObjectUrl: <base>/storage/v1/object/public/updates/<name> with the
//     base's trailing slashes trimmed and invalid input refused;
//   - IsUpdateAvailable: the "latest.toml is authoritative, any difference from
//     the running build means an update" rule, shared with Check().
//
// The fixtures are the permanent form of the implementation probe that drove
// the change: they were written first and failed to compile before the API
// existed.

#include "Utils/Update/AppUpdater.h"

#include <cstdio>
#include <string>

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

// 64 lowercase hex chars: the only sha256 shape the updater accepts.
static const char kSha256[] =
    "3f2a1b0c"
    "9d8e7f6a"
    "5b4c3d2e"
    "1f0a9b8c"
    "7d6e5f4a"
    "3b2c1d0e"
    "9f8a7b6c"
    "5d4e3f2a";

// A well-formed pointer, the shape the bucket actually publishes.
static const char kValidToml[] =
    "version = \"2.0.2\"\n"
    "path    = \"BaeStim.dll\"\n"
    "sha256  = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n";

static AppUpdater::CheckResult Parse(const std::string& body)
{
    AppUpdater::CheckResult r;
    r.oldVersion = "2.0.1";
    if (!AppUpdater::Test::ParseLatestToml(body, r))
        r.updateAvailable = false; // caller's contract on a failed parse
    return r;
}

// ── parse: valid pointer ───────────────────────────────────────────────────

static void Parse_ValidPointer()
{
    AppUpdater::CheckResult r;
    r.oldVersion = "2.0.1";
    CHECK(AppUpdater::Test::ParseLatestToml(kValidToml, r));

    CHECK(r.newVersion == "2.0.2");
    CHECK(r.dllRelPath == "BaeStim.dll");
    CHECK(r.sha256 == kSha256);

    // The parser owns only the three pointer fields; the caller keeps the rest.
    CHECK(r.oldVersion == "2.0.1");
    CHECK(!r.updateAvailable);
}

static void Parse_AcceptsUppercaseSha()
{
    // The hash compare downstream is case-insensitive (EqualsIgnoreCase), so an
    // uppercased pointer must still parse; only the length/alphabet is checked.
    const std::string body =
        "version = \"2.0.2\"\n"
        "path = \"BaeStim.dll\"\n"
        "sha256 = \"3F2A1B0C9D8E7F6A5B4C3D2E1F0A9B8C7D6E5F4A3B2C1D0E9F8A7B6C5D4E3F2A\"\n";

    AppUpdater::CheckResult r;
    CHECK(AppUpdater::Test::ParseLatestToml(body, r));
    CHECK(r.sha256 == "3F2A1B0C9D8E7F6A5B4C3D2E1F0A9B8C7D6E5F4A3B2C1D0E9F8A7B6C5D4E3F2A");
}

// ── parse: malformed pointers ──────────────────────────────────────────────

static void Parse_MissingVersion()
{
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "path = \"BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
    CHECK(r.newVersion.empty());
    CHECK(r.dllRelPath.empty());
    CHECK(r.sha256.empty());
}

static void Parse_MissingSha()
{
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"BaeStim.dll\"\n", r));
    CHECK(r.newVersion.empty());
}

static void Parse_EmptyVersion()
{
    // Present but empty is as useless as missing: never a "new version".
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"\"\n"
        "path = \"BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
}

static void Parse_ShaWrongLength()
{
    AppUpdater::CheckResult r;
    // 63 hex chars: one short of a SHA-256 digest.
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2\"\n",
        r));
    CHECK(r.sha256.empty());
}

static void Parse_ShaNonHex()
{
    AppUpdater::CheckResult r;
    // 64 chars, but 'g' is not a hex digit.
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2g\"\n",
        r));
    CHECK(r.sha256.empty());
}

static void Parse_PathTraversal()
{
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"../evil.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
    CHECK(r.dllRelPath.empty());
}

static void Parse_PathSubdirectory()
{
    // The bucket publishes flat object names; a slash is never legal here.
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"sub/BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
    CHECK(r.dllRelPath.empty());
}

static void Parse_PathAbsoluteWindows()
{
    // A drive-qualified path would let a hostile pointer write anywhere.
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"C:\\\\evil.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
    CHECK(r.dllRelPath.empty());
}

static void Parse_PathBackslash()
{
    AppUpdater::CheckResult r;
    CHECK(!AppUpdater::Test::ParseLatestToml(
        "version = \"2.0.2\"\n"
        "path = \"sub\\\\BaeStim.dll\"\n"
        "sha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n",
        r));
    CHECK(r.dllRelPath.empty());
}

static void Parse_PathTooLong()
{
    // 129 chars of plain name: refused, the bound is 128.
    AppUpdater::CheckResult r;
    const std::string longName = std::string(129, 'a') + ".dll";
    const std::string body =
        "version = \"2.0.2\"\npath = \"" + longName +
        "\"\nsha256 = \"3f2a1b0c9d8e7f6a5b4c3d2e1f0a9b8c7d6e5f4a3b2c1d0e9f8a7b6c5d4e3f2a\"\n";
    CHECK(!AppUpdater::Test::ParseLatestToml(body, r));
    CHECK(r.dllRelPath.empty());
}

static void Parse_InvalidToml()
{
    AppUpdater::CheckResult r;
    // Unterminated array: toml++ throws, the updater must not.
    CHECK(!AppUpdater::Test::ParseLatestToml("version = [unclosed\n", r));
    CHECK(r.newVersion.empty());
    CHECK(r.dllRelPath.empty());
    CHECK(r.sha256.empty());
}

static void Parse_EmptyBody()
{
    AppUpdater::CheckResult r;
    r.oldVersion = "2.0.1";
    CHECK(!AppUpdater::Test::ParseLatestToml("", r));
    CHECK(r.newVersion.empty());
    // A failed parse never claims an update, whoever consumes the result.
    CHECK(!AppUpdater::Test::IsUpdateAvailable(r));
}

static void Parse_FailedParseClearsPreviousFields()
{
    // The caller may reuse one CheckResult across attempts; stale pointer data
    // from an earlier read must never survive into a failed parse.
    AppUpdater::CheckResult r;
    CHECK(AppUpdater::Test::ParseLatestToml(kValidToml, r));
    CHECK(r.newVersion == "2.0.2");

    CHECK(!AppUpdater::Test::ParseLatestToml("version = \"9.9.9\"\n", r));
    CHECK(r.newVersion.empty());
    CHECK(r.dllRelPath.empty());
    CHECK(r.sha256.empty());
}

// ── update decision ────────────────────────────────────────────────────────

static void UpdateAvailable_DifferentVersion()
{
    const AppUpdater::CheckResult r = Parse(kValidToml);
    CHECK(r.newVersion == "2.0.2");
    CHECK(r.oldVersion == "2.0.1");
    CHECK(AppUpdater::Test::IsUpdateAvailable(r));
}

static void UpdateAvailable_SameVersion()
{
    AppUpdater::CheckResult r;
    r.oldVersion = "2.0.2";
    CHECK(AppUpdater::Test::ParseLatestToml(kValidToml, r));
    CHECK(!AppUpdater::Test::IsUpdateAvailable(r));
}

static void UpdateAvailable_FailedParse()
{
    AppUpdater::CheckResult r = Parse("not toml at all");
    CHECK(!AppUpdater::Test::IsUpdateAvailable(r));
}

// ── bucket URL ─────────────────────────────────────────────────────────────

static const char kBase[] = "https://x.supabase.co";

static const char kExpectedUrl[] =
    "https://x.supabase.co/storage/v1/object/public/updates/latest.toml";

static void Url_TrimsTrailingSlash()
{
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "latest.toml") == kExpectedUrl);
    CHECK(AppUpdater::Test::BuildObjectUrl("https://x.supabase.co/", "latest.toml") == kExpectedUrl);
    CHECK(AppUpdater::Test::BuildObjectUrl("https://x.supabase.co//", "latest.toml") == kExpectedUrl);
    CHECK(AppUpdater::Test::BuildObjectUrl("https://x.supabase.co///", "latest.toml") ==
          kExpectedUrl);
}

static void Url_DllObject()
{
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "BaeStim.dll") ==
          "https://x.supabase.co/storage/v1/object/public/updates/BaeStim.dll");
}

static void Url_RejectsEmptyBase()
{
    CHECK(AppUpdater::Test::BuildObjectUrl("", "latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl("/", "latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl("///", "latest.toml").empty());
}

static void Url_RejectsInvalidObject()
{
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "../latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "opensteamtool/latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "sub\\latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, "C:latest.toml").empty());
    CHECK(AppUpdater::Test::BuildObjectUrl(kBase, std::string(129, 'a')).empty());
}

int main()
{
    Parse_ValidPointer();
    Parse_AcceptsUppercaseSha();
    Parse_MissingVersion();
    Parse_MissingSha();
    Parse_EmptyVersion();
    Parse_ShaWrongLength();
    Parse_ShaNonHex();
    Parse_PathTraversal();
    Parse_PathSubdirectory();
    Parse_PathAbsoluteWindows();
    Parse_PathBackslash();
    Parse_PathTooLong();
    Parse_InvalidToml();
    Parse_EmptyBody();
    Parse_FailedParseClearsPreviousFields();

    UpdateAvailable_DifferentVersion();
    UpdateAvailable_SameVersion();
    UpdateAvailable_FailedParse();

    Url_TrimsTrailingSlash();
    Url_DllObject();
    Url_RejectsEmptyBase();
    Url_RejectsInvalidObject();

    if (g_checksFailed != 0) {
        printf("FAILED (%d of %d checks)\n", g_checksFailed, g_checksRun);
        return 1;
    }
    printf("ALL OK (%d checks)\n", g_checksRun);
    return 0;
}
