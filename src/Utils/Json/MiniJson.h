#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MiniJson {

    // Extracts the string values of every occurrence of "key": "string" in a
    // JSON document, in document order. Values are returned raw (no
    // unescaping). Never throws: malformed or truncated JSON yields whatever
    // was accumulated so far. Used on GitHub commits-list / commit-detail
    // responses with keys "sha", "filename" and "name".
    std::vector<std::string> ExtractStrings(std::string_view json, std::string_view key);

    // Extracts the integer values of every occurrence of "key": <integer> in a
    // JSON document, in document order. Used on the config Edge Function
    // response with key "appid".
    //
    // Only whole integers are returned: string values ("token", "key" travel
    // as strings), floats (13.7, 1e3), booleans and null are skipped, as are
    // integers that do not fit in int64_t. Never throws: malformed or
    // truncated JSON yields whatever was accumulated so far, and a number cut
    // off by the end of the input is discarded rather than guessed at (a
    // silently wrong appid would be worse than a missing one).
    std::vector<int64_t> ExtractNumbers(std::string_view json, std::string_view key);

    // Extracts the FIRST string value bound to "key" ("key": "..."), with the
    // standard JSON escapes decoded (\" \\ \/ \b \f \n \r \t and \uXXXX,
    // surrogate pairs included). Used on the config Edge Function response with
    // key "lua", where the value is a whole .lua file and the DLL must write it
    // back byte-for-byte.
    //
    // An absent key, a value that is not a string, an empty string, a truncated
    // literal and any malformed escape (unknown sequence, incomplete \uXXXX,
    // lone surrogate) all yield "". Never throws: a half-decoded body would be
    // worse than none, so the whole value is dropped instead of repaired.
    std::string ExtractJsonString(std::string_view json, std::string_view key);

} // namespace MiniJson
