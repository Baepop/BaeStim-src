#include "Utils/Trainers/Zip.h"

#include <miniz.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <system_error>

// Implementation notes
//
// - Only regular file entries under "game/" are extracted; that prefix is
//   stripped so <destDir>/a.dll is the payload root the caller expects.
// - Entry names come from the central directory and are read in full with
//   mz_zip_reader_get_filename (the file_stat copy is capped at 512 bytes, and
//   a name that long must not be silently truncated into a different path).
// - Each entry is validated immediately before its write and the limits are
//   checked the same way, so an oversized entry is never written; files written
//   from earlier entries stay in place when a later entry fails.
// - Entry names are UTF-8 bytes and are rebuilt as char8_t (see ToU8PathString)
//   so std::filesystem reads them as UTF-8 instead of reinterpreting them in
//   the process ANSI code page. miniz itself is char-based and opens files with
//   fopen, so the UTF-8 bytes are also what reaches it; whether fopen can open
//   a non-ASCII name still depends on the active code page. Bundles are ASCII
//   in practice.
// - Names that are not well-formed UTF-8 are refused before a path is built
//   (std::filesystem throws on them), and a catch-all around the body turns any
//   remaining exception — allocation failure included — into an error result.

namespace Trainers::Zip {
namespace {

constexpr std::string_view kPayloadPrefix = "game/";
constexpr size_t kMaxNameBytes            = 1024;
constexpr size_t kMaxComponentBytes       = 255;
constexpr size_t kMaxErrorNameBytes       = 120;

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

// Windows opens these as devices no matter which directory they are resolved
// against, and it also treats "CON.txt" as CON. Windows ignores trailing spaces
// and dots in the name before the extension, so "CON .txt" is CON too.
bool IsReservedDeviceName(std::string_view component)
{
    std::string_view base = component.substr(0, component.find('.'));
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.remove_suffix(1);

    static constexpr std::array<std::string_view, 22> kReserved = {
        "CON",  "PRN",  "AUX",  "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const std::string_view r : kReserved) {
        if (EqualsIgnoreCaseAscii(base, r))
            return true;
    }
    return false;
}

bool IsForbiddenChar(unsigned char c)
{
    if (c < 0x20 || c == 0x7F) // control characters
        return true;
    switch (c) {
    case '<': case '>': case ':': case '"': case '|': case '?': case '*':
        return true;
    default:
        return false;
    }
}

// Strict UTF-8 (RFC 3629): no overlongs, no surrogates, nothing above
// U+10FFFF. std::filesystem::path is built from these bytes and throws on
// malformed input, so they must be checked before any path is constructed.
bool IsWellFormedUtf8(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }

        size_t len        = 0;
        unsigned char lo  = 0x80; // allowed range for the second byte
        unsigned char hi  = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c == 0xE0) {
            len = 3;
            lo  = 0xA0; // no overlong 3-byte forms
        } else if (c >= 0xE1 && c <= 0xEC) {
            len = 3;
        } else if (c == 0xED) {
            len = 3;
            hi  = 0x9F; // no UTF-16 surrogates
        } else if (c >= 0xEE && c <= 0xEF) {
            len = 3;
        } else if (c == 0xF0) {
            len = 4;
            lo  = 0x90; // no overlong 4-byte forms
        } else if (c >= 0xF1 && c <= 0xF3) {
            len = 4;
        } else if (c == 0xF4) {
            len = 4;
            hi  = 0x8F; // nothing above U+10FFFF
        } else {
            return false; // 0x80-0xC1 lead bytes, 0xF5-0xFF
        }

        if (s.size() - i < len)
            return false;
        const auto second = static_cast<unsigned char>(s[i + 1]);
        if (second < lo || second > hi)
            return false;
        for (size_t k = 2; k < len; ++k) {
            const auto cont = static_cast<unsigned char>(s[i + k]);
            if (cont < 0x80 || cont > 0xBF)
                return false;
        }
        i += len;
    }
    return true;
}

// Error text must stay printable even when the name it quotes is not valid
// UTF-8: non-ASCII and control bytes are escaped as \xNN.
std::string EscapeForError(std::string_view name)
{
    std::string out;
    const size_t shown = name.size() < kMaxErrorNameBytes ? name.size()
                                                          : kMaxErrorNameBytes;
    for (size_t i = 0; i < shown; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c >= 0x7F) {
            char esc[5];
            std::snprintf(esc, sizeof(esc), "\\x%02X", c);
            out += esc;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    if (name.size() > shown)
        out += "...";
    return out;
}

// "game\\a.txt" -> "game/a.txt", "./game/a.txt" -> "game/a.txt".
std::string NormalizeEntryName(std::string_view raw)
{
    std::string out(raw);
    for (char& c : out) {
        if (c == '\\')
            c = '/';
    }
    while (out.size() >= 2 && out[0] == '.' && out[1] == '/')
        out.erase(0, 2);
    return out;
}

std::string TruncateForError(std::string_view name)
{
    if (name.size() <= kMaxErrorNameBytes)
        return std::string(name);
    std::string out(name.substr(0, kMaxErrorNameBytes));
    out += "...";
    return out;
}

std::string PathToUtf8(const std::filesystem::path& p)
{
    const std::u8string u8 = p.u8string();
    std::string out;
    out.reserve(u8.size());
    for (const char8_t c : u8)
        out.push_back(static_cast<char>(c));
    return out;
}

// Entry names arrive as UTF-8 bytes. path(std::string) would reinterpret those
// bytes in the ANSI code page; the char8_t constructor keeps them UTF-8. The
// bytes are copied explicitly so no conversion can fail.
std::u8string ToU8PathString(std::string_view utf8)
{
    std::u8string out;
    out.reserve(utf8.size());
    for (const char c : utf8)
        out.push_back(static_cast<char8_t>(c));
    return out;
}

// Closes the reader on every exit path (early returns included).
struct ReaderGuard {
    mz_zip_archive* zip;
    ~ReaderGuard() { mz_zip_reader_end(zip); }
};

bool ReadEntryName(mz_zip_archive& zip, mz_uint index, std::string& out)
{
    const mz_uint len = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
    if (len < 2) // 1 == an empty name (just the terminator)
        return false;

    out.assign(len, '\0');
    if (mz_zip_reader_get_filename(&zip, index, out.data(), len) != len)
        return false;
    out.resize(len - 1); // drop the terminator
    return true;
}

} // namespace

bool IsSafeRelativePath(std::string_view rel)
{
    if (rel.empty() || rel.size() > kMaxNameBytes)
        return false;
    // Malformed UTF-8 becomes a malformed path: std::filesystem throws on it,
    // so it is never a safe name.
    if (!IsWellFormedUtf8(rel))
        return false;
    if (rel.front() == '/' || rel.front() == '\\')
        return false;

    size_t start = 0;
    for (;;) {
        const size_t slash = rel.find('/', start);
        const size_t end   = (slash == std::string_view::npos) ? rel.size() : slash;
        const std::string_view comp = rel.substr(start, end - start);

        if (comp.empty()) // "a//b", "a/", leading "/" (already handled)
            return false;
        if (comp == "." || comp == "..")
            return false;
        if (comp.size() > kMaxComponentBytes)
            return false;
        if (comp.back() == ' ' || comp.back() == '.')
            return false;
        if (IsReservedDeviceName(comp))
            return false;
        for (const unsigned char c : comp) {
            if (IsForbiddenChar(c) || c == '\\')
                return false;
        }

        if (slash == std::string_view::npos)
            return true;
        start = slash + 1;
    }
}

namespace {

// Builds the result in `result` so the public wrapper can catch a late
// exception without losing the count of files already written.
void ExtractGameImpl(const std::filesystem::path& zipPath,
                     const std::filesystem::path& destDir,
                     const Limits& limits,
                     ExtractResult& result)
{
    if (destDir.empty()) {
        result.error = "zip: empty destination";
        return;
    }

    mz_zip_archive zip{};
    const std::string zipName = PathToUtf8(zipPath);
    if (zipName.empty() || !mz_zip_reader_init_file(&zip, zipName.c_str(), 0)) {
        result.error = "zip: cannot open archive";
        return;
    }
    const ReaderGuard guard{&zip};

    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (ec) {
        result.error = "zip: cannot create destination";
        return;
    }

    const mz_uint totalEntries = mz_zip_reader_get_num_files(&zip);
    uint64_t totalBytes = 0;

    for (mz_uint i = 0; i < totalEntries; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            result.error = "zip: corrupt central directory";
            return;
        }

        std::string rawName;
        if (!ReadEntryName(zip, i, rawName)) {
            result.error = "zip: corrupt entry name";
            return;
        }

        const std::string name = NormalizeEntryName(rawName);
        if (stat.m_is_directory || name.empty() || name.back() == '/') // directory entry
            continue;
        if (!name.starts_with(kPayloadPrefix)) // outside.txt, __MACOSX/, ...
            continue;

        const std::string rel = name.substr(kPayloadPrefix.size());
        // Checked before IsSafeRelativePath so malformed bytes get a precise
        // error; the predicate refuses them too.
        if (!IsWellFormedUtf8(rel)) {
            result.error = "zip: entry name is not valid UTF-8: " + EscapeForError(rel);
            return;
        }
        if (!IsSafeRelativePath(rel)) {
            result.error = "zip: unsafe entry name: " + TruncateForError(rel);
            return;
        }

        if (stat.m_is_encrypted) {
            result.error = "zip: encrypted entry: " + TruncateForError(rel);
            return;
        }
        if (!stat.m_is_supported) {
            result.error = "zip: unsupported entry: " + TruncateForError(rel);
            return;
        }
        if (result.files >= limits.maxEntries) {
            result.error = "zip: too many entries";
            return;
        }
        if (stat.m_uncomp_size > limits.maxEntryBytes) {
            result.error = "zip: entry too large: " + TruncateForError(rel);
            return;
        }
        // Subtraction form: totalBytes <= maxTotalBytes always holds here, so
        // this cannot underflow (a sum could overflow with a huge custom cap).
        if (stat.m_uncomp_size > limits.maxTotalBytes - totalBytes) {
            result.error = "zip: archive too large";
            return;
        }

        const std::filesystem::path target =
            destDir / std::filesystem::path(ToU8PathString(rel));
        const std::filesystem::path parent = target.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                result.error = "zip: cannot create directory for " + TruncateForError(rel);
                return;
            }
        }

        const std::string targetName = PathToUtf8(target);
        if (!mz_zip_reader_extract_to_file(&zip, i, targetName.c_str(), 0)) {
            result.error = "zip: extracting " + TruncateForError(rel) + " failed";
            return;
        }

        totalBytes += stat.m_uncomp_size;
        ++result.files;
    }

    result.ok = true;
}

} // namespace

ExtractResult ExtractGame(const std::filesystem::path& zipPath,
                          const std::filesystem::path& destDir,
                          const Limits& limits)
{
    ExtractResult result;

    // No exception may escape: invalid entry names are refused before any path
    // is built, and this is the backstop for the rest (a caller path that
    // cannot be converted, allocation failure, ...).
    try {
        ExtractGameImpl(zipPath, destDir, limits, result);
    } catch (const std::bad_alloc&) {
        result.ok    = false;
        result.error = "zip: out of memory";
    } catch (const std::exception&) {
        result.ok    = false;
        result.error = "zip: cannot build destination path";
    } catch (...) {
        result.ok    = false;
        result.error = "zip: unexpected failure";
    }
    return result;
}

} // namespace Trainers::Zip
