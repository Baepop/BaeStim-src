// TrainerManifest fixtures — plain stdlib, no framework, no network.
//
// The trainers Edge Function answers GET /functions/v1/trainers?steamid=<id>
// with
//
//   {"steamid":"<id>","trainers":[{appid,version,sha256,size,name,url}, ...]}
//
// where every url is a time-limited signed link into the private "trainers"
// bucket. These fixtures pin the client half of that contract:
//   - the URL the worker requests (BuildRequestUrl);
//   - the entries Parse keeps: appid 1..UINT32_MAX, non-empty version (<= 64
//     chars), 64 hex sha256 (lower-cased), size > 0, and a url under
//     <supabaseUrl>/storage/v1/object/sign/trainers/packages/<that same appid>/;
//   - the entries Parse drops: anything missing or out of range above, a
//     foreign host, a public (unsigned) object URL, a signed URL for a
//     different appid, an entry cut off by a truncated body;
//   - the widened fixtures: the appid == UINT32_MAX boundary, an entry with no
//     name key at all, a pretty-printed body, and duplicate appid rows (kept:
//     de-duplication is the worker's job, not this parser's);
//   - a body that is not JSON at all, which yields no entries instead of
//     throwing.

#include "Utils/Trainers/TrainerManifest.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

namespace {

constexpr const char* kBase = "https://x.supabase.co";

// A valid signed URL for |appid| under the configured base.
std::string SignedUrl(uint32_t appid, std::string_view version)
{
    return std::string(kBase) + "/storage/v1/object/sign/trainers/packages/" +
           std::to_string(appid) + "/" + std::string(version) + ".zip?token=eyJhbGci";
}

std::vector<uint32_t> Appids(const std::vector<Trainers::TrainerManifest::Item>& items)
{
    std::vector<uint32_t> appids;
    appids.reserve(items.size());
    for (const auto& item : items)
        appids.push_back(item.appid);
    return appids;
}

void TestValidBody()
{
    const std::string body =
        R"({"steamid":"76561198604542817","trainers":[)"
        R"({"appid":1361510,"version":"1.0","sha256":")"
        R"(aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa)"
        R"(","size":1048576,"name":"Example Trainer","url":")" +
        SignedUrl(1361510, "1.0") +
        R"("},)"
        // Upper-case sha256: accepted and normalised to lower case. Empty name
        // is allowed (the worker falls back to the appid when displaying).
        R"({"appid":730,"version":"2.1.3","sha256":")"
        R"(ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789)"
        R"(","size":42,"name":"","url":")" +
        SignedUrl(730, "2.1.3") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    CHECK(items.size() == 2);
    if (items.size() != 2)
        return;

    const auto& first = items[0];
    CHECK(first.appid == 1361510);
    CHECK(first.version == "1.0");
    CHECK(first.sha256 ==
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(first.size == 1048576);
    CHECK(first.name == "Example Trainer");
    CHECK(first.url == SignedUrl(1361510, "1.0"));

    const auto& second = items[1];
    CHECK(second.appid == 730);
    CHECK(second.version == "2.1.3");
    CHECK(second.sha256 ==
          "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
    CHECK(second.size == 42);
    CHECK(second.name.empty());
    CHECK(second.url == SignedUrl(730, "2.1.3"));

    // A trailing slash on the configured base is trimmed before the signed-URL
    // prefix is built, so the same body parses either way.
    const std::vector<Trainers::TrainerManifest::Item> withSlash =
        Trainers::TrainerManifest::Parse(body, "https://x.supabase.co/");
    CHECK(withSlash.size() == 2);
}

void TestInvalidEntriesAreSkipped()
{
    const char* kSha =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const char* kSha63 =
        "ccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    const char* kSha65 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" "d";

    // One valid entry first and one last, with every rejected shape between
    // them: one bad row must never cost the good ones.
    std::string body = std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        R"({"appid":10,"version":"1.0","sha256":")" + kSha + R"(","size":10,"name":"ok","url":")" +
        SignedUrl(10, "1.0") + R"("},)"

        // missing sha256
        R"({"appid":1111,"version":"1.0","size":10,"name":"no sha","url":")" +
        SignedUrl(1111, "1.0") + R"("},)"

        // 63-char sha256
        R"({"appid":2222,"version":"1.0","sha256":")" + kSha63 +
        R"(","size":10,"name":"short sha","url":")" + SignedUrl(2222, "1.0") + R"("},)"

        // 65-char sha256
        R"({"appid":2223,"version":"1.0","sha256":")" + kSha65 +
        R"(","size":10,"name":"long sha","url":")" + SignedUrl(2223, "1.0") + R"("},)"

        // sha256 that is 64 chars but not hex
        R"({"appid":2224,"version":"1.0","sha256":")"
        R"(zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz)"
        R"(","size":10,"name":"non hex","url":")" + SignedUrl(2224, "1.0") + R"("},)"

        // size: 0
        R"({"appid":3333,"version":"1.0","sha256":")" + kSha +
        R"(","size":0,"name":"zero size","url":")" + SignedUrl(3333, "1.0") + R"("},)"

        // negative and fractional size
        R"({"appid":3334,"version":"1.0","sha256":")" + kSha +
        R"(","size":-5,"name":"negative","url":")" + SignedUrl(3334, "1.0") + R"("},)"
        R"({"appid":3335,"version":"1.0","sha256":")" + kSha +
        R"(","size":1.5,"name":"fractional","url":")" + SignedUrl(3335, "1.0") + R"("},)"

        // missing size
        R"({"appid":3336,"version":"1.0","sha256":")" + kSha +
        R"(","name":"no size","url":")" + SignedUrl(3336, "1.0") + R"("},)"

        // appid: 0 and appid past UINT32_MAX
        R"({"appid":0,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"zero appid","url":")" + SignedUrl(0, "1.0") + R"("},)"
        R"({"appid":4294967296,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"big appid","url":")" + SignedUrl(1, "1.0") + R"("},)"

        // appid as a string (the contract says numeric)
        R"({"appid":"4444","version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"string appid","url":")" + SignedUrl(4444, "1.0") + R"("},)"

        // empty and over-long version
        R"({"appid":6666,"version":"","sha256":")" + kSha +
        R"(","size":10,"name":"no version","url":")" + SignedUrl(6666, "1.0") + R"("},)"
        R"({"appid":6667,"version":")"
        R"(0123456789012345678901234567890123456789012345678901234567890123456)"
        R"(","sha256":")" + kSha + R"(","size":10,"name":"long version","url":")" +
        SignedUrl(6667, "1.0") + R"("},)"

        // http:// url (the manifest is signed https under the configured base)
        R"({"appid":5555,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"http","url":"http://x.supabase.co/storage/v1/object/sign/trainers/packages/5555/1.0.zip"},)"

        // missing url
        R"({"appid":5556,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"no url"},)"

        // trailing valid entry
        R"({"appid":20,"version":"2.0","sha256":")" + kSha +
        R"(","size":20,"name":"also ok","url":")" + SignedUrl(20, "2.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    const std::vector<uint32_t> appids = Appids(items);
    CHECK(appids.size() == 2);
    CHECK(appids == std::vector<uint32_t>({10, 20}));
}

void TestUrlMustBeSignedUnderBase()
{
    const char* kSha =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

    const std::string body =
        std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        // foreign host but the right path
        R"({"appid":1,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"foreign","url":"https://evil.example/storage/v1/object/sign/trainers/packages/1/1.0.zip"},)"
        // public (unsigned) object URL on the right host
        R"({"appid":2,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"public","url":"https://x.supabase.co/storage/v1/object/public/trainers/packages/2/1.0.zip"},)"
        // right host as a prefix of a longer host
        R"({"appid":3,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"lookalike","url":"https://x.supabase.co.evil.example/storage/v1/object/sign/trainers/packages/3/1.0.zip"},)"
        // another bucket under the same host
        R"({"appid":4,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"other bucket","url":"https://x.supabase.co/storage/v1/object/sign/updates/packages/4/1.0.zip"},)"
        // the empty-download URL
        R"({"appid":5,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"empty","url":""},)"
        // a prefix-valid URL carrying a space: never handed to the request layer
        R"({"appid":7,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"space","url":")" + SignedUrl(7, "1.0") + R"( with space"},)"
        // a body-level signed URL: the entry survives
        R"({"appid":6,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"ok","url":")" + SignedUrl(6, "1.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    const std::vector<uint32_t> appids = Appids(items);
    CHECK(appids.size() == 1);
    CHECK(appids == std::vector<uint32_t>({6}));

    // An unconfigured base cannot vouch for any URL, so nothing is kept.
    CHECK(Trainers::TrainerManifest::Parse(body, "").empty());
}

void TestNotJson()
{
    CHECK(Trainers::TrainerManifest::Parse("not json", kBase).empty());
    CHECK(Trainers::TrainerManifest::Parse("", kBase).empty());
    CHECK(Trainers::TrainerManifest::Parse("{}", kBase).empty());
    CHECK(Trainers::TrainerManifest::Parse(R"({"trainers":[]})", kBase).empty());
    // "trainers" bound to a non-array value
    CHECK(Trainers::TrainerManifest::Parse(R"({"steamid":"1","trainers":"nope"})", kBase).empty());
    // an array of scalars instead of objects
    CHECK(Trainers::TrainerManifest::Parse(R"({"trainers":[1,2,3]})", kBase).empty());
}

void TestKeyLookAlikes()
{
    const char* kSha =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

    // "trainers" and every field name also appear as string VALUES; only the
    // real keys may be read, and escaped characters must be decoded.
    const std::string body =
        std::string(R"({"note":"trainers","label":"appid","trainers":[)") +
        R"({"appid":7,"name":"Caf\u00e9 \"Edition\"","version":"3.0","sha256":")" + kSha +
        R"(","size":7,"url":")" + SignedUrl(7, "3.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    CHECK(items.size() == 1);
    if (items.size() != 1)
        return;
    CHECK(items[0].appid == 7);
    CHECK(items[0].name == "Caf\xC3\xA9 \"Edition\"");
}

void TestTruncatedBody()
{
    const char* kSha =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

    // Complete first entry, then a second one cut off before its closing brace:
    // only the complete entry may be returned.
    const std::string body =
        std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        R"({"appid":100,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"complete","url":")" + SignedUrl(100, "1.0") + R"("},)"
        R"({"appid":200,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"truncated","url":")" + SignedUrl(200, "1.0") + R"(")";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    const std::vector<uint32_t> appids = Appids(items);
    CHECK(appids.size() == 1);
    CHECK(appids == std::vector<uint32_t>({100}));

    // Cut inside a string literal: no partial value is guessed at.
    const std::string cutInsideString =
        std::string(R"({"trainers":[{"appid":300,"version":"1.0","sha256":")") + kSha +
        R"(","size":10,"url":")" + SignedUrl(300, "1.0").substr(0, 20);
    CHECK(Trainers::TrainerManifest::Parse(cutInsideString, kBase).empty());
}

void TestUrlBoundToAppid()
{
    const char* kSha =
        "1111111111111111111111111111111111111111111111111111111111111111";

    // Every row below carries appid 10 and differs only in its url. Only the
    // url naming packages/10/ may be kept: a validly signed link for another
    // trainer must not be installed under this appid.
    const std::string body =
        std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        // the only acceptable one
        R"({"appid":10,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"own","url":")" + SignedUrl(10, "1.0") + R"("},)"
        // another trainer's package
        R"({"appid":10,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"swapped","url":")" + SignedUrl(730, "1.0") + R"("},)"
        // no packages/<appid>/ segment at all
        R"({"appid":10,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"bare","url":")" + kBase +
        R"(/storage/v1/object/sign/trainers/1.0.zip"},)"
        // packages/<appid>/ is matched with its slash, so 100 is not appid 10
        R"({"appid":10,"version":"1.0","sha256":")" + kSha +
        R"(","size":10,"name":"wrong dir","url":")" + kBase +
        R"(/storage/v1/object/sign/trainers/packages/100/1.0.zip"}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    const std::vector<uint32_t> appids = Appids(items);
    CHECK(appids.size() == 1);
    CHECK(appids == std::vector<uint32_t>({10}));
    if (items.size() != 1)
        return;
    CHECK(items[0].url == SignedUrl(10, "1.0"));
}

void TestAppidUpperBound()
{
    const char* kSha =
        "2222222222222222222222222222222222222222222222222222222222222222";
    const uint32_t kMaxAppid = 4294967295u;

    // The boundary itself is a valid appid (only UINT32_MAX + 1 is rejected,
    // which TestInvalidEntriesAreSkipped covers).
    const std::string body =
        std::string(R"({"trainers":[{"appid":4294967295,"version":"1.0","sha256":")") +
        kSha + R"(","size":10,"name":"max","url":")" + SignedUrl(kMaxAppid, "1.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    CHECK(items.size() == 1);
    if (items.size() != 1)
        return;
    CHECK(items[0].appid == kMaxAppid);
    CHECK(items[0].url == SignedUrl(kMaxAppid, "1.0"));
}

void TestSparseFields()
{
    const char* kSha =
        "3333333333333333333333333333333333333333333333333333333333333333";

    // No "name" key at all: the entry is still usable, and the worker falls
    // back to the appid when it displays it.
    const std::string body =
        std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        R"({"appid":42,"version":"1.0","sha256":")" + kSha + R"(","size":10,"url":")" +
        SignedUrl(42, "1.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    CHECK(items.size() == 1);
    if (items.size() != 1)
        return;
    CHECK(items[0].appid == 42);
    CHECK(items[0].name.empty());
    CHECK(items[0].size == 10);
}

void TestPrettyPrintedBody()
{
    const char* kSha =
        "4444444444444444444444444444444444444444444444444444444444444444";
    const char* kSha2 =
        "5555555555555555555555555555555555555555555555555555555555555555";

    // Whitespace around the array, the objects, the commas and the colons is
    // the backend's formatting choice, not a contract change.
    const std::string body =
        std::string("{\n  \"steamid\" : \"76561198604542817\",\n  \"trainers\" : [\n") +
        "    {\n"
        "      \"appid\" : 9,\n"
        "      \"version\" : \"1.0\",\n"
        "      \"sha256\" : \"" + kSha + "\",\n"
        "      \"size\" : 9,\n"
        "      \"name\" : \"Pretty One\",\n"
        "      \"url\" : \"" + SignedUrl(9, "1.0") + "\"\n"
        "    },\n"
        "    {\n"
        "      \"appid\" : 11,\n"
        "      \"version\" : \"2.0\",\n"
        "      \"sha256\" : \"" + kSha2 + "\",\n"
        "      \"size\" : 11,\n"
        "      \"name\" : \"Pretty Two\",\n"
        "      \"url\" : \"" + SignedUrl(11, "2.0") + "\"\n"
        "    }\n"
        "  ]\n"
        "}\n";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    CHECK(items.size() == 2);
    if (items.size() != 2)
        return;

    CHECK(items[0].appid == 9);
    CHECK(items[0].version == "1.0");
    CHECK(items[0].sha256 == kSha);
    CHECK(items[0].size == 9);
    CHECK(items[0].name == "Pretty One");
    CHECK(items[0].url == SignedUrl(9, "1.0"));

    CHECK(items[1].appid == 11);
    CHECK(items[1].version == "2.0");
    CHECK(items[1].sha256 == kSha2);
    CHECK(items[1].size == 11);
    CHECK(items[1].name == "Pretty Two");
    CHECK(items[1].url == SignedUrl(11, "2.0"));
}

void TestDuplicateAppids()
{
    const char* kSha =
        "6666666666666666666666666666666666666666666666666666666666666666";

    // Both rows are returned in document order. De-duplicating (or deciding
    // which version wins) belongs to the worker that applies them, not here:
    // dropping a row silently would hide a backend mistake.
    const std::string body =
        std::string(R"({"steamid":"76561198604542817","trainers":[)") +
        R"({"appid":5,"version":"1.0","sha256":")" + kSha +
        R"(","size":5,"name":"dup one","url":")" + SignedUrl(5, "1.0") + R"("},)"
        R"({"appid":5,"version":"2.0","sha256":")" + kSha +
        R"(","size":6,"name":"dup two","url":")" + SignedUrl(5, "2.0") + R"("}]})";

    const std::vector<Trainers::TrainerManifest::Item> items =
        Trainers::TrainerManifest::Parse(body, kBase);

    const std::vector<uint32_t> appids = Appids(items);
    CHECK(appids.size() == 2);
    CHECK(appids == std::vector<uint32_t>({5, 5}));
    if (items.size() != 2)
        return;
    CHECK(items[0].version == "1.0");
    CHECK(items[1].version == "2.0");
    CHECK(items[1].size == 6);
}

void TestBuildRequestUrl()
{
    CHECK(Trainers::TrainerManifest::BuildRequestUrl("https://x.supabase.co/",
                                                     76561198604542817ull) ==
          "https://x.supabase.co/functions/v1/trainers?steamid=76561198604542817");
    CHECK(Trainers::TrainerManifest::BuildRequestUrl("https://x.supabase.co",
                                                     76561198604542817ull) ==
          "https://x.supabase.co/functions/v1/trainers?steamid=76561198604542817");
    // Every trailing slash is trimmed, never doubled.
    CHECK(Trainers::TrainerManifest::BuildRequestUrl("https://x.supabase.co///",
                                                     1ull) ==
          "https://x.supabase.co/functions/v1/trainers?steamid=1");
    // No configured base: no URL (the worker must not request anything).
    CHECK(Trainers::TrainerManifest::BuildRequestUrl("", 76561198604542817ull).empty());
}

} // namespace

int main()
{
    TestValidBody();
    TestInvalidEntriesAreSkipped();
    TestUrlMustBeSignedUnderBase();
    TestNotJson();
    TestKeyLookAlikes();
    TestTruncatedBody();
    TestUrlBoundToAppid();
    TestAppidUpperBound();
    TestSparseFields();
    TestPrettyPrintedBody();
    TestDuplicateAppids();
    TestBuildRequestUrl();

    printf("%d/%d checks passed\n", g_checksRun - g_checksFailed, g_checksRun);
    return g_checksFailed == 0 ? 0 : 1;
}
