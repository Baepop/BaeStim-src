#include "Utils/Trainers/TrainerState.h"

#include "Utils/Json/MiniJson.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

// GetCurrentProcessId for the temp-file suffix. NOMINMAX keeps windows.h's
// "max" macro from eating std::numeric_limits<...>::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// See TrainerState.h for the contract. The scanner below follows the same
// defensive shape as TrainerManifest's: locate the top-level object, walk its
// members while consuming strings whole, then reuse MiniJson for the scalar
// keys of each slice. Nothing parses the document into a tree — one corrupt
// entry costs only that entry, and a document that cannot be walked at all
// yields nullopt (the caller treats it as "no state").
namespace Trainers::TrainerState {

namespace {

    constexpr std::string_view::size_type kNpos = std::string_view::npos;

    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }

    bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    std::string_view::size_type SkipWhitespace(std::string_view json,
                                               std::string_view::size_type i)
    {
        while (i < json.size() && IsSpace(json[i]))
            ++i;
        return i;
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

    std::string_view TrimSpaces(std::string_view text)
    {
        std::string_view::size_type begin = 0;
        std::string_view::size_type end   = text.size();
        while (begin < end && IsSpace(text[begin]))
            ++begin;
        while (end > begin && IsSpace(text[end - 1]))
            --end;
        return text.substr(begin, end - begin);
    }

    // Index just past the complete value starting at |at| (which is already
    // past the colon and any whitespace), or kNpos when the value is truncated
    // or unbalanced. Strings and nested objects/arrays are consumed whole;
    // scalars (numbers, true/false/null) end at a comma, a closing bracket or
    // whitespace.
    std::string_view::size_type SkipValue(std::string_view json,
                                          std::string_view::size_type at)
    {
        if (at >= json.size())
            return kNpos;

        if (json[at] == '"') {
            const std::string_view::size_type close = FindClosingQuote(json, at);
            return close == kNpos ? kNpos : close + 1;
        }

        if (json[at] == '{' || json[at] == '[') {
            const char open  = json[at];
            const char close = open == '{' ? '}' : ']';

            size_t depth = 1;
            for (auto i = at + 1; i < json.size(); ++i) {
                const char c = json[i];
                if (c == '"') {
                    const std::string_view::size_type quote = FindClosingQuote(json, i);
                    if (quote == kNpos)
                        return kNpos;
                    i = quote; // for-loop ++i steps past the closing quote
                    continue;
                }
                if (c == '{' || c == '[') {
                    ++depth;
                    continue;
                }
                if (c == '}' || c == ']') {
                    --depth;
                    if (depth == 0)
                        return c == close ? i + 1 : kNpos;
                }
            }
            return kNpos; // ran out of input: the container never closed
        }

        auto i = at;
        while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ']' &&
               !IsSpace(json[i]))
            ++i;
        return i == at ? kNpos : i; // an empty value is not a value
    }

    // One key/value pair of an object body, with both sides trimmed and the
    // value complete.
    struct Member {
        std::string_view key;
        std::string_view value;
    };

    // Every member of the object whose interior is |body| (the bytes between
    // its braces), in document order. False when |body| is not a well-formed
    // object body, so a caller can refuse the whole document instead of
    // guessing at a half-read one.
    bool SplitMembers(std::string_view body, std::vector<Member>& out)
    {
        out.clear();

        auto i = SkipWhitespace(body, 0);
        if (i >= body.size())
            return true; // "{}"

        while (i < body.size()) {
            if (body[i] != '"')
                return false;
            const std::string_view::size_type keyClose = FindClosingQuote(body, i);
            if (keyClose == kNpos)
                return false;

            const std::string_view key = body.substr(i + 1, keyClose - i - 1);

            auto colon = SkipWhitespace(body, keyClose + 1);
            if (colon >= body.size() || body[colon] != ':')
                return false;

            const auto valueBegin = SkipWhitespace(body, colon + 1);
            if (valueBegin >= body.size())
                return false;

            const auto valueEnd = SkipValue(body, valueBegin);
            if (valueEnd == kNpos)
                return false;

            out.push_back(Member{key, TrimSpaces(body.substr(valueBegin, valueEnd - valueBegin))});

            auto next = SkipWhitespace(body, valueEnd);
            if (next >= body.size())
                break;
            if (body[next] != ',')
                return false;
            i = SkipWhitespace(body, next + 1);
            if (i >= body.size())
                return false; // trailing comma
        }
        return true;
    }

    // The single complete top-level object of |json| (braces included), or an
    // empty view when the document is not exactly one object.
    std::string_view TopLevelObject(std::string_view json)
    {
        const auto begin = SkipWhitespace(json, 0);
        if (begin >= json.size() || json[begin] != '{')
            return {};

        const auto end = SkipValue(json, begin);
        if (end == kNpos)
            return {};

        // Anything but whitespace after the object is not a document this unit
        // wrote; refuse it instead of silently ignoring the tail.
        if (SkipWhitespace(json, end) != json.size())
            return {};

        return json.substr(begin, end - begin);
    }

    // Every element of the array |array| (brackets included), in document
    // order. False when the array is not well formed.
    bool SplitArray(std::string_view array, std::vector<std::string_view>& out)
    {
        out.clear();
        if (array.size() < 2 || array.front() != '[' || array.back() != ']')
            return false;

        auto i = SkipWhitespace(array, 1);
        const auto close = array.size() - 1;
        if (i == close)
            return true; // "[]"

        while (i < close) {
            const auto end = SkipValue(array, i);
            if (end == kNpos || end > close)
                return false;
            out.push_back(TrimSpaces(array.substr(i, end - i)));

            auto next = SkipWhitespace(array, end);
            if (next == close)
                break;
            if (array[next] != ',')
                return false;
            i = SkipWhitespace(array, next + 1);
            if (i >= close)
                return false; // trailing comma
        }
        return true;
    }

    // Value of a boolean member ("key": true|false). Leaves |out| untouched and
    // returns false when the key is absent or bound to something else; MiniJson
    // has no boolean extractor, so this is the one scalar read done by hand.
    bool ExtractBool(std::string_view json, std::string_view key, bool& out)
    {
        auto i = std::string_view::size_type{0};
        while (i < json.size()) {
            if (json[i] != '"') {
                ++i;
                continue;
            }

            const std::string_view::size_type close = FindClosingQuote(json, i);
            if (close == kNpos)
                return false;

            if (json.substr(i + 1, close - i - 1) == key) {
                auto after = SkipWhitespace(json, close + 1);
                if (after < json.size() && json[after] == ':') {
                    after = SkipWhitespace(json, after + 1);
                    if (json.compare(after, 4, "true") == 0) {
                        out = true;
                        return true;
                    }
                    if (json.compare(after, 5, "false") == 0) {
                        out = false;
                        return true;
                    }
                    return false; // the key is bound to a non-boolean value
                }
            }

            i = close + 1;
        }
        return false;
    }

    // decimal appid in 1..UINT32_MAX, no sign, no leading zero.
    bool ParseAppidKey(std::string_view key, uint32_t& out)
    {
        if (key.empty() || key.size() > 10)
            return false;
        if (key[0] == '0' && key.size() > 1)
            return false; // "042" would collide with "42"

        uint64_t value = 0;
        for (const char c : key) {
            if (!IsDigit(c))
                return false;
            value = value * 10u + static_cast<uint64_t>(c - '0');
            if (value > std::numeric_limits<uint32_t>::max())
                return false;
        }
        if (value == 0)
            return false;

        out = static_cast<uint32_t>(value);
        return true;
    }

    // One {"path": "...", "size": N, "existed": bool} object. |object| is a
    // complete, balanced object slice.
    bool ParseFileRecord(std::string_view object, FileRecord& out)
    {
        if (object.size() < 2 || object.front() != '{' || object.back() != '}')
            return false;

        std::string path = MiniJson::ExtractJsonString(object, "path");
        if (path.empty())
            return false;

        // ExtractNumbers already dropped floats, strings and values beyond
        // int64; a negative byte count is corrupt.
        const std::vector<int64_t> sizes = MiniJson::ExtractNumbers(object, "size");
        if (sizes.empty() || sizes.front() < 0)
            return false;

        // A missing (or non-boolean) flag is "unknown", and the destructive
        // action is deleting a destination: default to "existed", i.e. keep it.
        // Only a future remove feature reads this; NeedsApply never does.
        bool existed = true;
        (void)ExtractBool(object, "existed", existed);

        out.path    = std::move(path);
        out.size    = static_cast<uint64_t>(sizes.front());
        out.existed = existed;
        return true;
    }

    // One appid entry. |object| is a complete, balanced object slice. Every
    // output is written only after all fields passed, so a rejected entry
    // leaves |out| untouched.
    bool ParseEntry(std::string_view object, Entry& out)
    {
        if (object.size() < 2 || object.front() != '{' || object.back() != '}')
            return false;

        std::vector<Member> members;
        if (!SplitMembers(object.substr(1, object.size() - 2), members))
            return false;

        Entry entry;
        entry.version   = MiniJson::ExtractJsonString(object, "version");
        entry.sha256    = MiniJson::ExtractJsonString(object, "sha256");
        entry.updatedAt = MiniJson::ExtractJsonString(object, "updated_at");

        if (entry.version.empty())
            return false;

        // The files array is the only structured value in an entry, so it is
        // taken from the member walk instead of a MiniJson scalar helper.
        std::string_view filesArray;
        bool haveFiles = false;
        for (const Member& member : members) {
            if (member.key == "files") {
                filesArray = member.value;
                haveFiles  = true;
                break; // first value wins, like the rest of the format
            }
        }
        if (!haveFiles)
            return false;

        std::vector<std::string_view> fileSlices;
        if (!SplitArray(filesArray, fileSlices))
            return false;

        entry.files.reserve(fileSlices.size());
        for (const std::string_view slice : fileSlices) {
            FileRecord record;
            if (!ParseFileRecord(slice, record))
                return false;
            entry.files.push_back(std::move(record));
        }

        out = std::move(entry);
        return true;
    }

    char HexDigit(uint32_t nibble)
    {
        return static_cast<char>(nibble < 10u ? '0' + nibble : 'a' + (nibble - 10u));
    }

    // JSON string body escaping. |"| and |\| are the two the brief requires;
    // control characters are encoded too so a corrupt path can never break the
    // document (Parse decodes \uXXXX).
    void AppendEscaped(std::string& out, std::string_view text)
    {
        for (const char c : text) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    const auto byte = static_cast<uint32_t>(static_cast<unsigned char>(c));
                    out += "\\u00";
                    out += HexDigit((byte >> 4) & 0xFu);
                    out += HexDigit(byte & 0xFu);
                } else {
                    out.push_back(c);
                }
                break;
            }
        }
    }

    // Removes <path>.<pid>.tmp siblings left by an earlier crash between write
    // and rename. The next run has a different PID, so those files would
    // otherwise stay forever. Only regular files are removed (a directory with
    // such a name is not ours). Best effort: a directory that cannot be listed
    // or an entry that cannot be removed is simply ignored. A live writer's
    // temp may be swept too; its rename then fails and it returns false without
    // touching the published file, which is the same outcome as any other
    // failed write.
    void RemoveStaleTempSiblings(const std::filesystem::path& path)
    {
        namespace fs = std::filesystem;

        const fs::path parent = path.parent_path();
        const fs::path dir    = parent.empty() ? fs::path(".") : parent;
        // Native (wide) comparison: a sibling name the narrow encoding cannot
        // represent must not make the sweep throw.
        const std::wstring prefix = path.filename().native() + L".";

        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec)
            return;

        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec)
                return;

            const std::wstring name = it->path().filename().native();
            if (name.size() <= prefix.size() + 4)
                continue;
            if (name.compare(0, prefix.size(), prefix) != 0)
                continue;
            if (name.compare(name.size() - 4, 4, L".tmp") != 0)
                continue;

            // Only regular files are swept: a directory named like a temp file
            // is not ours to delete.
            std::error_code typeEc;
            if (!it->is_regular_file(typeEc) || typeEc)
                continue;

            std::error_code removeEc;
            fs::remove(it->path(), removeEc);
        }
    }

} // namespace

std::optional<State> Parse(std::string_view json)
{
    // The file is local input but may still be corrupt or hand-edited, and
    // every step below allocates: no exception may reach the worker.
    try {
        const std::string_view object = TopLevelObject(json);
        if (object.empty())
            return std::nullopt;

        std::vector<Member> topMembers;
        if (!SplitMembers(object.substr(1, object.size() - 2), topMembers))
            return std::nullopt;

        std::string_view trainersObject;
        bool haveTrainers = false;
        for (const Member& member : topMembers) {
            if (member.key == "trainers") {
                trainersObject = member.value;
                haveTrainers  = true;
                break; // first value wins
            }
        }
        if (!haveTrainers)
            return std::nullopt;

        // "trainers" bound to a non-object is not a state file this unit wrote;
        // treating it as empty would be indistinguishable from "nothing
        // installed" anyway, but nullopt keeps the signal honest.
        if (trainersObject.size() < 2 || trainersObject.front() != '{' ||
            trainersObject.back() != '}')
            return std::nullopt;

        std::vector<Member> entryMembers;
        if (!SplitMembers(trainersObject.substr(1, trainersObject.size() - 2),
                          entryMembers))
            return std::nullopt;

        State state;
        for (const Member& member : entryMembers) {
            uint32_t appid = 0;
            if (!ParseAppidKey(member.key, appid))
                continue; // not an appid: skip the entry, never the document

            Entry entry;
            if (!ParseEntry(member.value, entry))
                continue;

            state.trainers[appid] = std::move(entry); // duplicates: last wins
        }
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

std::string Render(const State& state)
{
    std::string out;
    try {
        out += "{\n  \"version\": 1,\n  \"trainers\": ";

        // An appid outside 1..UINT32_MAX (only 0 fits the map key type) has no
        // representation in the format: Parse would drop it, so emitting it
        // would only produce a document that does not roundtrip.
        size_t total = 0;
        for (const auto& [appid, entry] : state.trainers) {
            (void)entry;
            if (appid != 0)
                ++total;
        }

        if (total == 0) {
            out += "{}\n}\n";
            return out;
        }

        out += "{\n";
        size_t index = 0;
        for (const auto& [appid, entry] : state.trainers) {
            if (appid == 0)
                continue;

            const bool lastEntry = ++index == total;

            out += "    \"";
            out += std::to_string(appid);
            out += "\": {\n      \"version\": \"";
            AppendEscaped(out, entry.version);
            // The only persisted status. Emitted as a constant so the file
            // matches the spec even though Entry does not carry it.
            out += "\",\n      \"status\": \"installed\",\n      \"sha256\": \"";
            AppendEscaped(out, entry.sha256);
            out += "\",\n      \"updated_at\": \"";
            AppendEscaped(out, entry.updatedAt);
            out += "\",\n";

            if (entry.files.empty()) {
                out += "      \"files\": []\n";
            } else {
                out += "      \"files\": [\n";
                size_t fileIndex = 0;
                for (const FileRecord& file : entry.files) {
                    const bool lastFile = ++fileIndex == entry.files.size();

                    out += "        { \"path\": \"";
                    AppendEscaped(out, file.path);
                    out += "\", \"size\": ";
                    out += std::to_string(file.size);
                    out += ", \"existed\": ";
                    out += file.existed ? "true" : "false";
                    out += " }";
                    out += lastFile ? "\n" : ",\n";
                }
                out += "      ]\n";
            }

            out += lastEntry ? "    }\n" : "    },\n";
        }
        out += "  }\n}\n";
        return out;
    } catch (...) {
        return {};
    }
}

bool WriteAtomic(const std::filesystem::path& path, const State& state)
{
    namespace fs = std::filesystem;

    if (path.empty())
        return false;

    try {
        const std::string contents = Render(state);
        if (contents.empty())
            return false; // Render only fails on allocation

        std::error_code ec;
        const fs::path parent = path.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec)
                return false;
        }

        RemoveStaleTempSiblings(path);

        // PID-suffixed sibling, same reasoning as RemoteConfig's: Steam hosts
        // the DLL in more than one process, so a fixed ".tmp" could be
        // truncated by a second writer mid-write. The rename is a sibling
        // move, never across volumes.
        fs::path tmpPath = path;
        tmpPath += "." + std::to_string(GetCurrentProcessId()) + ".tmp";

        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                std::error_code removeEc;
                fs::remove(tmpPath, removeEc); // a half-created file, if any
                return false;
            }

            ofs.write(contents.data(), static_cast<std::streamsize>(contents.size()));
            ofs.flush();
            ofs.close(); // a buffered/failed close invalidates the file too
            if (!ofs) {
                std::error_code removeEc;
                fs::remove(tmpPath, removeEc);
                return false;
            }
        }

        fs::rename(tmpPath, path, ec);
        if (ec) {
            std::error_code removeEc;
            fs::remove(tmpPath, removeEc);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool NeedsApply(const State& state, uint32_t appid, std::string_view version,
                const std::filesystem::path& gameDir)
{
    namespace fs = std::filesystem;

    try {
        // Without a game directory every relative path would resolve against
        // the process CWD and answer for the wrong files.
        if (gameDir.empty())
            return true;

        const auto it = state.trainers.find(appid);
        if (it == state.trainers.end())
            return true;

        const Entry& entry = it->second;
        if (entry.version != version)
            return true;

        // A fileless entry is accepted by Parse but cannot vouch for an
        // install: pending, same as a missing entry (NeedsApply may also be
        // called with a hand-built State).
        if (entry.files.empty())
            return true;

        for (const FileRecord& file : entry.files) {
            const fs::path path = gameDir / fs::path(file.path);

            std::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec)
                return true; // missing, a directory, or unreadable

            const std::uintmax_t size = fs::file_size(path, ec);
            if (ec || static_cast<uint64_t>(size) != file.size)
                return true;
        }
        return false;
    } catch (...) {
        return true;
    }
}

} // namespace Trainers::TrainerState
