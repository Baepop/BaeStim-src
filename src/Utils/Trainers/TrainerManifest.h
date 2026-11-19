#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Client half of the trainers manifest contract.
//
// GET <supabaseUrl>/functions/v1/trainers?steamid=<id> answers with the
// trainers assigned to that Steam account:
//
//   {"steamid":"76561198604542817","trainers":[
//      {"appid":1361510,"version":"1.0",
//       "sha256":"<64 hex>","size":1048576,"name":"Example",
//       "url":"<supabaseUrl>/storage/v1/object/sign/trainers/..."}]}
//
// Packages live in a private bucket, so every url is a time-limited signed
// link: the host, the "/storage/v1/object/sign/trainers/" path and the entry's
// own "packages/<appid>/" segment are the only ones this unit will hand to the
// downloader. The url is never rebuilt from appid/version here — a signed url
// cannot be guessed, and a manifest entry that points somewhere else is dropped,
// not repaired.
//
// Parsing is deliberately tolerant and fail-closed: the useful entries are
// returned even when the body is malformed or truncated, and an entry is
// dropped (never partially filled) when any field fails validation. No
// exception escapes these functions.

namespace Trainers::TrainerManifest {

// One trainer assigned to the current user. sha256 is lower-case hex (64
// chars), the form Http::DownloadToFile's hash check compares against. name may
// be empty: the worker falls back to the appid when it displays the entry.
struct Item {
    uint32_t    appid = 0;
    std::string version;
    std::string sha256;
    uint64_t    size = 0;
    std::string name;
    std::string url;
};

// Parses the response body, returning only the entries that pass every check:
//
//   - appid  in 1..UINT32_MAX, as a JSON integer;
//   - version non-empty and at most 64 characters;
//   - sha256 exactly 64 hex characters (case-insensitive, stored lower-case);
//   - size   > 0, as a JSON integer (a value beyond int64 is therefore
//            dropped, as are floats, strings and negatives);
//   - url    starts with the project's signed-URL prefix:
//            <supabaseUrl with trailing '/' trimmed>/storage/v1/object/sign/trainers/,
//            contains no space or control character (it goes straight to
//            WinHTTP), and is bound to this entry's appid: the bucket layout is
//            fixed at packages/<appid>/<version>.zip, so after the prefix the url
//            must continue with "packages/<appid>/" for the appid just validated.
//            A validly signed link for another trainer is dropped. The filename
//            and version are deliberately not compared (URL encoding makes that
//            brittle); only the appid segment is.
//
// The first value of each key inside one entry object wins. A missing or
// non-object "trainers" value, a body that is not JSON, and an empty
// supabaseUrl (nothing can be verified without the configured base) all yield
// an empty vector. Entries are returned in document order; a body cut off
// mid-entry returns the complete entries before the cut. Never throws.
std::vector<Item> Parse(std::string_view body, std::string_view supabaseUrl);

// <supabaseUrl with trailing '/' trimmed>/functions/v1/trainers?steamid=<id>.
// Returns "" when supabaseUrl is empty (feature not configured): the worker
// must then make no request.
std::string BuildRequestUrl(std::string_view supabaseUrl, uint64_t steamId);

} // namespace Trainers::TrainerManifest
