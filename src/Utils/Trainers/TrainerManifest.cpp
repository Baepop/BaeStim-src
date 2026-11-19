#include "Utils/Trainers/TrainerManifest.h"

#include "Utils/Json/MiniJson.h"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// See TrainerManifest.h for the contract. The scan below is intentionally the
// same shape as RemoteConfig's "appids" reader: locate the array, split it into
// complete top-level objects, then validate each field with MiniJson. Nothing
// here parses the document as a whole — a row the backend cannot describe must
// cost only that row.
namespace Trainers::TrainerManifest {

namespace {

    constexpr std::string_view::size_type kNpos = std::string_view::npos;

    // Path of a signed object inside the private "trainers" bucket. Combined
    // with the configured base, this is the only origin the downloader may see.
    constexpr const char* kSignedUrlPath = "/storage/v1/object/sign/trainers/";

    // A version is a short display/compare token; anything longer is not a
    // version the backend writes.
    constexpr size_t kMaxVersionChars = 64;

    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    // Index of the quote closing the literal opening at |open| (which points at
    // '"'), or kNpos when the input ends inside it. Escape-aware, so \" never
    // terminates a literal early.
    std::string_view::size_type FindClosingQuote(std::string_view json,
                                                 std::string_view::size_type open)
    {
        for (auto i = open + 1; i < json.size(); ++i) {
            if (json[i] == '\\') {
                ++i; // skip the escaped character
                continue;
            }
            if (json[i] == '"')
                return i;
        }
        return kNpos;
    }

    std::string_view::size_type SkipWhitespace(std::string_view json,
                                               std::string_view::size_type i)
    {
        while (i < json.size() && IsSpace(json[i]))
            ++i;
        return i;
    }

    bool IsHexChar(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool IsHex64(std::string_view text)
    {
        if (text.size() != 64)
            return false;
        for (const char c : text) {
            if (!IsHexChar(c))
                return false;
        }
        return true;
    }

    char ToLowerAscii(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    bool StartsWith(std::string_view text, std::string_view prefix)
    {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    // The url is handed straight to WinHTTP. A literal space or control
    // character is not something a signed Supabase URL contains, and letting it
    // reach the request layer would at best fail the item: refuse the entry
    // here instead of forwarding a malformed request target.
    bool IsRequestSafeUrl(std::string_view url)
    {
        for (const char c : url) {
            const unsigned char byte = static_cast<unsigned char>(c);
            if (byte <= 0x20u || byte == 0x7Fu)
                return false;
        }
        return true;
    }

    // Location of the "trainers" array inside the response body.
    struct ArrayLocation {
        bool   arrayOpened = false; // a "trainers" key's value starts with '['
        size_t begin       = 0;     // index just past '[' when arrayOpened
    };

    // Walks the body consuming string literals whole, so the word "trainers"
    // inside a string value (the steamid, a name) is never taken for the key.
    ArrayLocation LocateTrainersArray(std::string_view json)
    {
        ArrayLocation out;

        size_t i = 0;
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const std::string_view::size_type close = FindClosingQuote(json, i);
            if (close == kNpos)
                return out; // truncated literal: nothing more to find

            if (json.substr(i + 1, close - i - 1) == "trainers") {
                size_t after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    after = SkipWhitespace(json, after + 1);
                    if (after < json.size() && json[after] == '[') {
                        out.arrayOpened = true;
                        out.begin       = after + 1;
                    }
                    return out;
                }
                // A string VALUE that reads "trainers": not a key, keep scanning.
            }

            i = close + 1; // unwanted key or a value: skip it whole
        }
        return out;
    }

    // Every complete top-level object of the array, in document order. A slice
    // only exists once its closing '}' has been seen, so an entry cut off by a
    // truncated body is never parsed: a half-row could otherwise look valid
    // (its fields are complete long before the object is).
    std::vector<std::string_view> SplitObjects(std::string_view json, size_t begin)
    {
        std::vector<std::string_view> objects;

        size_t depth     = 0;
        size_t objStart  = kNpos;
        size_t i         = begin;

        while (i < json.size()) {
            const char c = json[i];

            if (c == '"') {
                const std::string_view::size_type quote = FindClosingQuote(json, i);
                if (quote == kNpos)
                    return objects; // truncated literal: keep complete objects only
                i = quote + 1;
                continue;
            }

            if (c == '{') {
                if (depth == 0)
                    objStart = i;
                ++depth;
                ++i;
                continue;
            }

            if (c == '[') {
                ++depth;
                ++i;
                continue;
            }

            if (c == '}') {
                if (depth == 0)
                    return objects; // stray '}': malformed body, stop trusting it
                --depth;
                if (depth == 0 && objStart != kNpos) {
                    objects.push_back(json.substr(objStart, i - objStart + 1));
                    objStart = kNpos;
                }
                ++i;
                continue;
            }

            if (c == ']') {
                if (depth == 0)
                    return objects; // end of the array
                --depth;           // a nested array closes
                ++i;
                continue;
            }

            ++i;
        }

        // Ran out of input before the closing ']': the complete objects found
        // so far are still usable.
        return objects;
    }

    // The signed-URL prefix every entry must live under. False when the base is
    // empty: without a configured origin no URL can be vouched for.
    bool BuildSignedUrlPrefix(std::string_view supabaseUrl, std::string& out)
    {
        std::string base(supabaseUrl);
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        if (base.empty())
            return false;

        base += kSignedUrlPath;
        out = std::move(base);
        return true;
    }

    // Validates one entry object. Every output is written only after all fields
    // passed, so a rejected entry leaves |out| untouched.
    bool ParseItem(std::string_view object, std::string_view urlPrefix, Item& out)
    {
        const std::vector<int64_t> appids = MiniJson::ExtractNumbers(object, "appid");
        if (appids.empty())
            return false;
        const int64_t appid = appids.front();
        if (appid <= 0 || appid > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
            return false;

        const std::string version = MiniJson::ExtractJsonString(object, "version");
        if (version.empty() || version.size() > kMaxVersionChars)
            return false;

        std::string sha256 = MiniJson::ExtractJsonString(object, "sha256");
        if (!IsHex64(sha256))
            return false;
        for (char& c : sha256)
            c = ToLowerAscii(c);

        // ExtractNumbers already dropped floats, strings and values beyond
        // int64; what is left must be a positive byte count.
        const std::vector<int64_t> sizes = MiniJson::ExtractNumbers(object, "size");
        if (sizes.empty())
            return false;
        const int64_t size = sizes.front();
        if (size <= 0)
            return false;

        const std::string url = MiniJson::ExtractJsonString(object, "url");
        if (!StartsWith(url, urlPrefix) || !IsRequestSafeUrl(url))
            return false;

        // The bucket layout is fixed: packages/<appid>/<version>.zip. Binding
        // the url to this entry's appid keeps a validly signed link for another
        // trainer from being installed under this appid (and vice versa). The
        // filename/version is NOT compared: encoding the version into a URL is
        // the backend's business (spaces, "+", dots) and a mismatch there would
        // reject legitimate packages.
        std::string appidPrefix;
        appidPrefix.reserve(urlPrefix.size() + 16);
        appidPrefix.append(urlPrefix);
        appidPrefix += "packages/";
        appidPrefix += std::to_string(appid);
        appidPrefix += '/';
        if (!StartsWith(url, appidPrefix))
            return false;

        out.appid   = static_cast<uint32_t>(appid);
        out.version = version;
        out.sha256  = std::move(sha256);
        out.size    = static_cast<uint64_t>(size);
        out.name    = MiniJson::ExtractJsonString(object, "name"); // may be empty
        out.url     = url;
        return true;
    }

} // namespace

std::vector<Item> Parse(std::string_view body, std::string_view supabaseUrl)
{
    std::vector<Item> items;

    // The body is remote input and every step below allocates, so the whole
    // routine is wrapped: on any escape the caller gets the entries collected
    // so far instead of an exception crossing the worker.
    try {
        std::string urlPrefix;
        if (!BuildSignedUrlPrefix(supabaseUrl, urlPrefix))
            return items;

        const ArrayLocation array = LocateTrainersArray(body);
        if (!array.arrayOpened)
            return items;

        const std::vector<std::string_view> objects = SplitObjects(body, array.begin);
        items.reserve(objects.size());

        for (const std::string_view object : objects) {
            Item item;
            if (ParseItem(object, urlPrefix, item))
                items.push_back(std::move(item));
        }
    } catch (...) {
        // Out of memory: keep what is already validated.
    }

    return items;
}

std::string BuildRequestUrl(std::string_view supabaseUrl, uint64_t steamId)
{
    try {
        std::string url(supabaseUrl);
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        if (url.empty())
            return {};

        url += "/functions/v1/trainers?steamid=";
        url += std::to_string(steamId);
        return url;
    } catch (...) {
        return {};
    }
}

} // namespace Trainers::TrainerManifest
