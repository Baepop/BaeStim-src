#include "Utils/Trainers/SteamLibrary.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

// Implementation notes
//
// - Steam's text KeyValues format is small enough for a tokenizer: quoted
//   strings (with \\, \" and the usual escapes), braces and unquoted tokens.
//   "//" starts a comment that runs to the end of the line; a UTF-8 BOM and
//   CRLF are accepted because that is what Steam writes on Windows.
// - Values are read as key/value pairs rather than by walking a fixed schema:
//   no keys are hard-coded except the handful of names looked up, so an
//   unknown block ("apps", "InstalledDepots", "UserConfig", ...) is skipped
//   naturally and a future Steam field cannot break the parse.
// - Only the first occurrence of a field among the direct children of AppState
//   wins, matching KeyValues semantics; nested blocks are not consulted.
// - ParseLibraryFolders deliberately does not add steamRoot: the fixtures pin
//   it to "one library in the file, one path out". Resolve() always considers
//   steamRoot first, because the root install is a library that
//   libraryfolders.vdf does not list.
// - Resolve() never touches the registry or the Steam client, only files under
//   steamRoot <library>; a missing or unreadable file is just a candidate that
//   did not qualify. The manifest's installdir is validated as a relative
//   sub-path (no root, no "..") before it is joined under steamapps/common,
//   since the caller writes trainer files there. No exception escapes the
//   public functions.

namespace Trainers::SteamLibrary {
namespace {

constexpr std::string_view kLibraryFoldersFile = "steamapps/libraryfolders.vdf";
constexpr std::string_view kCommonPrefix       = "steamapps/common";
constexpr uint32_t         kFullyInstalledBit  = 4;

bool EqualsIgnoreCaseAscii(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

// One parsed element of a text VDF/ACF document.
struct Token {
    enum class Kind { String, OpenBrace, CloseBrace };

    Kind        kind = Kind::String;
    std::string text;
};

class Tokenizer {
public:
    explicit Tokenizer(std::string_view text) : m_text(text)
    {
        // UTF-8 BOM: editors on Windows add it and it is not a token character.
        if (m_text.size() >= 3 && static_cast<unsigned char>(m_text[0]) == 0xEF &&
            static_cast<unsigned char>(m_text[1]) == 0xBB &&
            static_cast<unsigned char>(m_text[2]) == 0xBF) {
            m_pos = 3;
        }
    }

    bool Next(Token& out)
    {
        SkipSpaceAndComments();
        if (m_pos >= m_text.size())
            return false;

        out.text.clear();
        const char c = m_text[m_pos];
        if (c == '{') {
            ++m_pos;
            out.kind = Token::Kind::OpenBrace;
            return true;
        }
        if (c == '}') {
            ++m_pos;
            out.kind = Token::Kind::CloseBrace;
            return true;
        }
        if (c == '"') {
            out.kind = Token::Kind::String;
            return ReadQuoted(out.text);
        }
        out.kind = Token::Kind::String;
        return ReadUnquoted(out.text);
    }

private:
    void SkipSpaceAndComments()
    {
        while (m_pos < m_text.size()) {
            if (IsSpace(m_text[m_pos])) {
                ++m_pos;
                continue;
            }
            if (m_text[m_pos] == '/' && m_pos + 1 < m_text.size() &&
                m_text[m_pos + 1] == '/') {
                m_pos += 2;
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
                continue;
            }
            break;
        }
    }

    // Opens on the quote; an unterminated string consumes the rest of the text
    // and is still returned (the parse is best-effort, the caller decides).
    bool ReadQuoted(std::string& out)
    {
        ++m_pos; // opening quote
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"')
                return true;
            if (c == '\\' && m_pos < m_text.size()) {
                const char escaped = m_text[m_pos++];
                switch (escaped) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                default: out.push_back(escaped); break;
                }
                continue;
            }
            out.push_back(c);
        }
        return true;
    }

    // Unquoted tokens are rare in Steam's files but are part of the format
    // (conditionals, "#base"), so they are accepted and compared like strings.
    bool ReadUnquoted(std::string& out)
    {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (IsSpace(c) || c == '{' || c == '}' || c == '"')
                break;
            out.push_back(c);
            ++m_pos;
        }
        return true;
    }

    std::string_view m_text;
    size_t           m_pos = 0;
};

// Visits every "key value" pair in the document, including pairs nested inside
// any number of blocks. A key immediately followed by "{" introduces a block
// and has no value, so it is not reported.
template <typename Fn>
void ForEachPair(std::string_view text, Fn&& fn)
{
    Tokenizer  tokenizer(text);
    Token      token;
    std::string key;
    bool       haveKey = false;

    while (tokenizer.Next(token)) {
        switch (token.kind) {
        case Token::Kind::OpenBrace:
        case Token::Kind::CloseBrace:
            // A block name (or a stray brace) ends the pending key.
            haveKey = false;
            break;
        case Token::Kind::String:
            if (!haveKey) {
                key     = std::move(token.text);
                haveKey = true;
            } else {
                fn(key, token.text);
                haveKey = false;
            }
            break;
        }
    }
}

// Visits the direct key/value pairs of the document's root block, provided
// that block is "AppState" (the only root Steam writes for an appmanifest).
// Pairs nested inside a block are not reported, so a nested "name" or
// "installdir" cannot shadow the top-level one.
template <typename Fn>
void ForEachAppStatePair(std::string_view text, Fn&& fn)
{
    Tokenizer tokenizer(text);
    Token     token;

    // Root name, then its opening brace. Anything between the two is not part
    // of the manifest shape and is ignored.
    std::string rootName;
    bool        haveRoot = false;
    bool        opened   = false;
    while (tokenizer.Next(token)) {
        if (token.kind == Token::Kind::String) {
            if (!haveRoot) {
                rootName = std::move(token.text);
                haveRoot = true;
            }
            continue;
        }
        if (token.kind == Token::Kind::OpenBrace) {
            opened = true;
            break;
        }
        return; // '}' before any block: nothing to report
    }

    if (!opened || !haveRoot || !EqualsIgnoreCaseAscii(rootName, "AppState"))
        return;

    int         depth = 1; // inside AppState
    std::string key;
    bool        haveKey = false;

    while (tokenizer.Next(token)) {
        switch (token.kind) {
        case Token::Kind::OpenBrace:
            ++depth;
            haveKey = false;
            break;
        case Token::Kind::CloseBrace:
            if (--depth == 0)
                return;
            haveKey = false;
            break;
        case Token::Kind::String:
            if (!haveKey) {
                key     = std::move(token.text);
                haveKey = true;
            } else {
                if (depth == 1)
                    fn(key, token.text);
                haveKey = false;
            }
            break;
        }
    }
}

std::filesystem::path PathFromUtf8(std::string_view bytes)
{
    std::u8string utf8;
    utf8.reserve(bytes.size());
    for (const char c : bytes)
        utf8.push_back(static_cast<char8_t>(c));
    return std::filesystem::path(utf8);
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& out)
{
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

bool ParseStateFlags(std::string_view text, uint32_t& out)
{
    // Steam writes decimal; tolerate a 0x prefix for hand-edited files.
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty())
        return false;

    uint64_t value = 0;
    const char* begin = text.data();
    const char* end   = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc() || result.ptr != end)
        return false;
    // StateFlags is a 32-bit mask. A larger value is not something Steam
    // writes, and truncating it could turn a bogus number into bit 4.
    if (value > 0xFFFFFFFFull)
        return false;

    out = static_cast<uint32_t>(value);
    return true;
}

// Validates a manifest's installdir before it is joined under
// <library>\steamapps\common. Only a plain relative sub-path is acceptable:
// a rooted, drive-qualified or traversal value would let the manifest decide
// where trainer files get written.
bool IsSafeInstallDir(std::string_view value, std::filesystem::path& out)
{
    const std::filesystem::path path = PathFromUtf8(value);
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory())
        return false;

    for (const std::filesystem::path& part : path) {
        if (part == std::filesystem::path(".."))
            return false;
    }

    const std::filesystem::path normalised = path.lexically_normal();
    if (normalised.empty() || normalised == std::filesystem::path("."))
        return false;
    if (normalised.is_absolute() || normalised.has_root_name() ||
        normalised.has_root_directory())
        return false;
    for (const std::filesystem::path& part : normalised) {
        if (part == std::filesystem::path(".."))
            return false;
    }

    out = normalised;
    return true;
}

// Appends candidate when it is not already there. Steam may list the root
// library in libraryfolders.vdf as well; comparing the normalised native form
// case-insensitively keeps that from causing a second read of the manifest.
void AddCandidate(std::vector<std::filesystem::path>& candidates,
                  const std::filesystem::path& path)
{
    const std::filesystem::path normalised = path.lexically_normal();
    const std::wstring candidate = normalised.native();
    for (const std::filesystem::path& existing : candidates) {
        const std::wstring other = existing.lexically_normal().native();
        if (candidate.size() != other.size())
            continue;
        bool same = true;
        for (size_t i = 0; i < candidate.size(); ++i) {
            if (std::towlower(candidate[i]) != std::towlower(other[i])) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }
    candidates.push_back(normalised);
}

} // namespace

std::vector<std::filesystem::path>
ParseLibraryFolders(std::string_view vdf,
                    const std::filesystem::path& steamRoot)
{
    std::vector<std::filesystem::path> paths;
    try {
        ForEachPair(vdf, [&](const std::string& key, const std::string& value) {
            if (!EqualsIgnoreCaseAscii(key, "path"))
                return;

            const std::filesystem::path path = PathFromUtf8(value);
            if (path.empty())
                return;
            // Steam normally writes absolute paths; a relative one is only
            // meaningful next to the Steam install.
            paths.push_back(path.is_absolute() ? path : steamRoot / path);
        });
    } catch (...) {
        // Allocation failure: report what was collected so far.
    }
    return paths;
}

bool ParseAppManifest(std::string_view acf,
                      std::string& outInstallDir,
                      std::string& outName,
                      bool& outFullyInstalled)
{
    outInstallDir.clear();
    outName.clear();
    outFullyInstalled = false;

    try {
        bool         haveInstallDir = false;
        bool         haveFlags      = false;
        std::string  installDir;
        std::string  name;
        uint32_t     flags = 0;

        ForEachAppStatePair(acf, [&](const std::string& key,
                                      const std::string& value) {
            if (!haveInstallDir && EqualsIgnoreCaseAscii(key, "installdir")) {
                installDir     = value;
                haveInstallDir = true;
            } else if (!haveFlags && EqualsIgnoreCaseAscii(key, "stateflags")) {
                if (ParseStateFlags(value, flags))
                    haveFlags = true;
            } else if (name.empty() && EqualsIgnoreCaseAscii(key, "name")) {
                name = value;
            }
        });

        if (!haveInstallDir || installDir.empty())
            return false;

        outInstallDir     = std::move(installDir);
        outName           = std::move(name);
        outFullyInstalled = (flags & kFullyInstalledBit) != 0;
        return true;
    } catch (...) {
        outInstallDir.clear();
        outName.clear();
        outFullyInstalled = false;
        return false;
    }
}

std::optional<GameInstall> Resolve(uint32_t appid,
                                   const std::filesystem::path& steamRoot)
{
    if (steamRoot.empty())
        return std::nullopt;

    try {
        // The root install is always a library; the file only lists the extra
        // ones.
        std::vector<std::filesystem::path> candidates;
        AddCandidate(candidates, steamRoot);

        std::string libraryFolders;
        if (ReadWholeFile(steamRoot / std::string(kLibraryFoldersFile),
                          libraryFolders)) {
            for (const std::filesystem::path& library :
                 ParseLibraryFolders(libraryFolders, steamRoot)) {
                AddCandidate(candidates, library);
            }
        }

        const std::string manifestName =
            "appmanifest_" + std::to_string(appid) + ".acf";

        for (const std::filesystem::path& library : candidates) {
            std::string acf;
            if (!ReadWholeFile(library / "steamapps" / manifestName, acf))
                continue;

            std::string installdir;
            std::string name;
            bool        fullyInstalled = false;
            if (!ParseAppManifest(acf, installdir, name, fullyInstalled))
                continue;

            // A manifest that tries to point outside its library fails the
            // whole resolve: nothing is written below a path it chose.
            std::filesystem::path installSubdir;
            if (!IsSafeInstallDir(installdir, installSubdir))
                return std::nullopt;
            if (!fullyInstalled)
                continue;

            GameInstall install;
            install.appid      = appid;
            install.name       = std::move(name);
            install.installDir = library / std::string(kCommonPrefix) /
                                 installSubdir;
            return install;
        }
    } catch (...) {
        // A malformed manifest or a filesystem error means "not found".
    }
    return std::nullopt;
}

} // namespace Trainers::SteamLibrary
