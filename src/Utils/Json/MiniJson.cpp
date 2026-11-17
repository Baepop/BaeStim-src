#include "Utils/Json/MiniJson.h"

#include <cstdint>

namespace MiniJson {

    namespace {

        constexpr std::string_view::size_type kNpos = std::string_view::npos;

        // Index of the quote that closes the string literal opening at |open|
        // (which points at '"'), or kNpos when the input ends mid-literal
        // (truncated JSON). Escape-aware: \" never terminates the literal.
        std::string_view::size_type FindClosingQuote(std::string_view json, std::string_view::size_type open)
        {
            for (auto i = open + 1; i < json.size(); ++i) {
                if (json[i] == '\\') {
                    ++i; // skip the escaped character (so \" is not a quote)
                    continue;
                }
                if (json[i] == '"')
                    return i;
            }
            return kNpos;
        }

        std::string_view::size_type SkipWhitespace(std::string_view json, std::string_view::size_type i)
        {
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n'))
                ++i;
            return i;
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        bool HexValue(char c, uint32_t& value)
        {
            if (c >= '0' && c <= '9')
                value = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value = static_cast<uint32_t>(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F')
                value = static_cast<uint32_t>(c - 'A') + 10u;
            else
                return false;
            return true;
        }

        // Reads exactly four hex digits at |at| and advances past them.
        bool ReadHex4(std::string_view json, std::string_view::size_type& at, uint32_t& out)
        {
            out = 0;
            for (int i = 0; i < 4; ++i) {
                uint32_t nibble = 0;
                if (at >= json.size() || !HexValue(json[at], nibble))
                    return false;
                out = (out << 4) | nibble;
                ++at;
            }
            return true;
        }

        // Appends |cp| as UTF-8. |cp| is a scalar value by construction (lone
        // surrogates are rejected before this point), so no replacement
        // character is ever emitted.
        void AppendUtf8(uint32_t cp, std::string& out)
        {
            if (cp <= 0x7Fu) {
                out.push_back(static_cast<char>(cp));
            } else if (cp <= 0x7FFu) {
                out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else if (cp <= 0xFFFFu) {
                out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            }
        }

        // Decodes the escapes of one JSON string literal body (the bytes between
        // the quotes). Returns false on any sequence JSON does not define, so a
        // caller can drop the whole value rather than hand back a corrupted one.
        bool UnescapeString(std::string_view raw, std::string& out)
        {
            out.clear();
            out.reserve(raw.size());

            std::string_view::size_type i = 0;
            while (i < raw.size()) {
                const char c = raw[i++];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (i >= raw.size())
                    return false; // trailing backslash

                const char escape = raw[i++];
                switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ReadHex4(raw, i, cp))
                        return false;

                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate: JSON represents the pair as two \u
                        // escapes. A lone high surrogate is not a character.
                        if (i + 1 >= raw.size() || raw[i] != '\\' || raw[i + 1] != 'u')
                            return false;
                        i += 2;
                        uint32_t low = 0;
                        if (!ReadHex4(raw, i, low) || low < 0xDC00u || low > 0xDFFFu)
                            return false;
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        return false; // lone low surrogate
                    }

                    AppendUtf8(cp, out);
                    break;
                }
                default:
                    return false; // unknown escape: not JSON
                }
            }
            return true;
        }

        // Digits signalled after the number token that mean "not an integer"
        // (JSON fraction or exponent).
        bool IsFloatTail(char c)
        {
            return c == '.' || c == 'e' || c == 'E';
        }

        // Parses the value that follows "key": starting at |at| (already past
        // the ':'). Appends to |values| only when the value is a whole integer
        // within int64_t range; string, float, boolean and null values are
        // skipped whole so "key" text inside them is never matched. Returns the
        // index just past the consumed token, or kNpos when the input ends
        // before the value can be trusted (truncation): the caller then just
        // resumes scanning after the key literal.
        std::string_view::size_type ParseIntegerValue(
            std::string_view json, std::string_view::size_type at, std::vector<int64_t>& values)
        {
            if (at >= json.size())
                return kNpos; // truncated right after ':'

            if (json[at] == '"') {
                // String value (e.g. "token"): not a number. Skip the literal
                // whole so keys inside it can never match.
                const std::string_view::size_type close = FindClosingQuote(json, at);
                return close == kNpos ? kNpos : close + 1;
            }

            std::string_view::size_type i = at;
            bool negative = false;
            if (json[i] == '-') {
                negative = true;
                ++i;
            }

            if (i >= json.size())
                return kNpos; // truncated after the sign
            if (!IsDigit(json[i]))
                return kNpos; // null / true / false / object / array / bad input

            // Saturating magnitude accumulation: no signed-overflow UB on huge
            // digit strings, the value is simply dropped.
            const uint64_t limit = negative ? (static_cast<uint64_t>(INT64_MAX) + 1u)
                                            : static_cast<uint64_t>(INT64_MAX);
            uint64_t magnitude = 0;
            bool overflow = false;
            while (i < json.size() && IsDigit(json[i])) {
                const unsigned digit = static_cast<unsigned>(json[i] - '0');
                if (!overflow) {
                    if (magnitude > (limit - digit) / 10u)
                        overflow = true;
                    else
                        magnitude = magnitude * 10u + digit;
                }
                ++i;
            }

            if (i >= json.size())
                return kNpos; // number cut off by the end of the input: unusable

            if (IsFloatTail(json[i])) {
                // Fraction or exponent: a float, never an integer. Consume the
                // rest of the token so scanning resumes cleanly after it.
                while (i < json.size() && (IsDigit(json[i]) || IsFloatTail(json[i]) || json[i] == '+' ||
                                           json[i] == '-'))
                    ++i;
                return i;
            }

            const char terminator = json[i];
            const bool wellFormed = terminator == ',' || terminator == '}' || terminator == ']' ||
                                    terminator == ' ' || terminator == '\t' || terminator == '\r' ||
                                    terminator == '\n';
            if (!wellFormed)
                return kNpos; // not a number we understand (e.g. `12abc`)

            if (!overflow) {
                if (negative) {
                    values.push_back(magnitude == static_cast<uint64_t>(INT64_MAX) + 1u
                                         ? INT64_MIN
                                         : -static_cast<int64_t>(magnitude));
                } else {
                    values.push_back(static_cast<int64_t>(magnitude));
                }
            }
            return i;
        }

    } // namespace

    std::vector<std::string> ExtractStrings(std::string_view json, std::string_view key)
    {
        std::vector<std::string> values;

        // Single-pass scan, no recursion. Every string literal is consumed
        // whole, so the word "key" inside a string VALUE is never mistaken for
        // a key. Numbers, booleans, null and nesting need no special handling:
        // non-quote characters are simply stepped over.
        std::string_view::size_type i = 0;
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const std::string_view::size_type close = FindClosingQuote(json, i);
            if (close == kNpos)
                break; // truncated literal: return what we have

            const std::string_view content = json.substr(i + 1, close - i - 1);
            if (content == key) {
                auto after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    after = SkipWhitespace(json, after + 1);
                    if (after < json.size() && json[after] == '"') {
                        const std::string_view::size_type valueClose = FindClosingQuote(json, after);
                        if (valueClose == kNpos)
                            break; // truncated value
                        values.emplace_back(json.substr(after + 1, valueClose - after - 1));
                        i = valueClose + 1;
                        continue;
                    }
                }
            }

            // Ordinary string (an unwanted key, or a value): skip past it as a
            // whole so keys inside string values are never matched.
            i = close + 1;
        }

        return values;
    }

    std::vector<int64_t> ExtractNumbers(std::string_view json, std::string_view key)
    {
        std::vector<int64_t> values;

        // Same single-pass scan as ExtractStrings: every string literal is
        // consumed whole, so `"key"` text inside a string VALUE is never
        // mistaken for a key, and a string value under a matching key is
        // skipped as a unit (so it is never read as a number).
        std::string_view::size_type i = 0;
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const std::string_view::size_type close = FindClosingQuote(json, i);
            if (close == kNpos)
                break; // truncated literal: return what we have

            const std::string_view content = json.substr(i + 1, close - i - 1);
            if (content == key) {
                auto after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    after = SkipWhitespace(json, after + 1);
                    const std::string_view::size_type end = ParseIntegerValue(json, after, values);
                    if (end != kNpos) {
                        i = end;
                        continue;
                    }
                }
            }

            // Ordinary string (an unwanted key, or a string value): skip past
            // it as a whole.
            i = close + 1;
        }

        return values;
    }

    std::string ExtractJsonString(std::string_view json, std::string_view key)
    {
        // Same single-pass scan as ExtractStrings: every string literal is
        // consumed whole, so the word "lua" inside a string VALUE is never
        // mistaken for the key.
        std::string_view::size_type i = 0;
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const std::string_view::size_type close = FindClosingQuote(json, i);
            if (close == kNpos)
                return {}; // truncated literal: nothing trustworthy left

            if (json.substr(i + 1, close - i - 1) == key) {
                auto after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    after = SkipWhitespace(json, after + 1);
                    if (after < json.size() && json[after] == '"') {
                        const std::string_view::size_type valueClose =
                            FindClosingQuote(json, after);
                        if (valueClose == kNpos)
                            return {}; // truncated value

                        std::string out;
                        if (!UnescapeString(json.substr(after + 1, valueClose - after - 1), out))
                            return {}; // malformed escape: drop the whole value
                        return out;
                    }
                }
                // The key is bound to a non-string value: not this contract,
                // so keep scanning rather than answer for a different shape.
            }

            i = close + 1;
        }

        return {};
    }

} // namespace MiniJson
