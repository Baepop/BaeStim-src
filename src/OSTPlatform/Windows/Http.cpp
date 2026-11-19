#include "include/Http.h"

#include "include/Encoding.h"
#include "include/Log.h"
#include "include/Numbers.h"

#include "Windows/Handles.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Present since the Windows 8.1 SDK; define defensively so an older SDK header
// still compiles (the option is simply a no-op on pre-8.1 runtimes).
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#endif
#ifndef WINHTTP_DECOMPRESSION_FLAG_ALL
#define WINHTTP_DECOMPRESSION_FLAG_GZIP    0x00000001
#define WINHTTP_DECOMPRESSION_FLAG_DEFLATE 0x00000002
#define WINHTTP_DECOMPRESSION_FLAG_ALL \
    (WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE)
#endif

namespace OSTPlatform::Http {
namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool tls = false;
    bool valid = false;
};

ParsedUrl ParseUrl(const char* rawUrl) {
    ParsedUrl out;
    if (!rawUrl) return out;

    std::string url(rawUrl);
    if (url.starts_with("https://")) {
        out.tls = true;
        out.port = INTERNET_DEFAULT_HTTPS_PORT;
        url = url.substr(8);
    } else if (url.starts_with("http://")) {
        url = url.substr(7);
    } else {
        return out;
    }

    const size_t slash = url.find('/');
    const std::string hostPart = url.substr(0, slash);
    out.path = (slash != std::string::npos)
        ? L"/" + std::wstring(url.begin() + slash + 1, url.end())
        : L"/";

    const size_t colon = hostPart.find(':');
    if (colon != std::string::npos) {
        out.host = std::wstring(hostPart.begin(), hostPart.begin() + colon);
        const auto port = Numbers::ParseUInt32(hostPart, {colon + 1});
        if (!port || *port == 0 || *port > 65535) return out;
        out.port = static_cast<INTERNET_PORT>(*port);
    } else {
        out.host = std::wstring(hostPart.begin(), hostPart.end());
    }

    out.valid = !out.host.empty();
    return out;
}

// WinHTTP handles are released with WinHttpCloseHandle, not CloseHandle, so the
// shared move-only RAII wrapper gets its own close policy. Every error path in
// DownloadToFile can then return without a close ladder like Execute's.
struct WinHttpHandlePolicy {
    static bool IsValid(HINTERNET handle) noexcept { return handle != nullptr; }
    static void Close(HINTERNET handle) noexcept { ::WinHttpCloseHandle(handle); }
};

using UniqueWinHttpHandle = Windows::UniqueHandleT<WinHttpHandlePolicy>;

// The whole point of this unit is that a multi-gigabyte package never has to
// fit in memory, so the body is streamed in fixed 64 KiB pieces.
constexpr DWORD kDownloadChunkBytes = 64 * 1024;

// A path for a log line. WideToUtf8 allocates and can throw; a log statement
// must never turn a failed download into an escaping exception (or a crash).
std::string DescribePath(const std::filesystem::path& path) noexcept {
    try {
        return Encoding::WideToUtf8(path.native());
    } catch (...) {
        return "<unprintable path>";
    }
}

// Scheme + host + path, with the query string dropped. Signed URLs carry a
// time-limited token as query parameters, so DownloadToFile must never log the
// raw URL. noexcept for the same reason as DescribePath: a log line may not
// become an escaping exception.
std::string RedactUrl(const char* rawUrl) noexcept {
    if (!rawUrl) return {};
    try {
        const std::string_view url(rawUrl);
        return std::string(url.substr(0, url.find('?')));
    } catch (...) {
        return "<unprintable url>";
    }
}

// Removes a partial file left behind by an earlier run. Returns an empty code
// when the path is gone (including when it never existed), otherwise the error
// that stopped the deletion. A read-only leftover is the one failure this unit
// can undo itself; a file another process holds open cannot be removed at all,
// and saying so is better than letting the ofstream fail for the same reason
// with no diagnosis. This is cleanup, not a download: callers decide whether a
// residual file aborts the operation.
std::error_code RemoveLeftoverPartial(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // remove() reports "not found" the same way it reports some failures, so
    // the filesystem is asked directly whether anything actually remains.
    std::error_code existsEc;
    if (!std::filesystem::exists(path, existsEc)) return existsEc;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        // SetFileAttributesW rejects bits it cannot set (COMPRESSED, ENCRYPTED,
        // REPARSE_POINT, DIRECTORY, ...) with ERROR_INVALID_PARAMETER, so pass
        // only the documented settable bits instead of the value read back.
        constexpr DWORD kSettable =
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
            FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        DWORD writable = attributes & kSettable & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        // A zero attribute mask is invalid; NORMAL is the documented stand-in.
        if (writable == 0) writable = FILE_ATTRIBUTE_NORMAL;
        if (SetFileAttributesW(path.c_str(), writable)) {
            std::error_code retryEc;
            std::filesystem::remove(path, retryEc);
            std::error_code retryExistsEc;
            if (!std::filesystem::exists(path, retryExistsEc)) return retryExistsEc;
            ec = retryEc;
        }
    }

    // The path may have vanished since the checks above (external cleaner);
    // report success rather than an error for a file that is no longer there.
    std::error_code finalEc;
    if (!std::filesystem::exists(path, finalEc)) return finalEc;

    // A remove() that reported no error yet left the path in place is rare;
    // permission_denied is the closest description for the caller's log.
    if (!ec) ec = std::make_error_code(std::errc::permission_denied);
    return ec;
}

} // namespace

Result Execute(const wchar_t* method,
               const char* url,
               const void* reqBody,
               uint32_t reqBodyLen,
               const wchar_t* headers,
               uint32_t timeoutResolve,
               uint32_t timeoutConnect,
               uint32_t timeoutSend,
               uint32_t timeoutRecv,
               uint32_t maxBodyBytes) {
    Result r;

    ParsedUrl pu = ParseUrl(url);
    if (!pu.valid) {
        OSTP_LOG_WARN("Invalid URL: {}", url ? url : "");
        return r;
    }

    auto t0 = std::chrono::steady_clock::now();

    // The default User-Agent is part of the contract with the frozen third-party
    // services we talk to (manifest.opensteamtool.com, stats, donate). Do NOT
    // rebrand it — explicit per-request headers (e.g. the GitHub API listing
    // calls) remain scoped to their own endpoint.
    HINTERNET hSession = WinHttpOpen(L"OpenSteamTool/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        OSTP_LOG_WARN("{} - WinHttpOpen failed (error={})", url ? url : "", GetLastError());
        return r;
    }

    WinHttpSetTimeouts(hSession, timeoutResolve, timeoutConnect, timeoutSend, timeoutRecv);

    // Ask for and transparently decode gzip/deflate. The wanted list is ~4.9 MB
    // raw but ~2 MB gzipped, and every donor pulls it on a schedule, so this is
    // a >50% cut on the largest recurring transfer. Best-effort: pre-Win8.1
    // WinHTTP does not support this option and simply serves uncompressed.
    {
        DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(hSession, WINHTTP_OPTION_DECOMPRESSION,
                         &decompress, sizeof(decompress));
    }

    HINTERNET hConnect = WinHttpConnect(hSession, pu.host.c_str(), pu.port, 0);
    if (!hConnect) {
        OSTP_LOG_WARN("{} - WinHttpConnect(host='{}', port={}) failed (error={})",
                      url ? url : "",
                      Encoding::WideToUtf8(pu.host),
                      pu.port,
                      GetLastError());
        WinHttpCloseHandle(hSession);
        return r;
    }

    const DWORD flags = pu.tls ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        method,
        pu.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!hRequest) {
        OSTP_LOG_WARN("{} - WinHttpOpenRequest failed (error={})", url ? url : "", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return r;
    }

    if (headers && headers[0]) {
        if (!WinHttpAddRequestHeaders(
            hRequest,
            headers,
            static_cast<DWORD>(wcslen(headers)),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
            OSTP_LOG_WARN("{} - WinHttpAddRequestHeaders failed (error={})", url ? url : "", GetLastError());
        }
    }

    const DWORD totalLen = reqBodyLen;
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            const_cast<void*>(reqBody), reqBodyLen, totalLen, 0)) {
        OSTP_LOG_WARN("{} - WinHttpSendRequest failed (error={})", url, GetLastError());
    } else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        OSTP_LOG_WARN("{} - WinHttpReceiveResponse failed (error={})", url, GetLastError());
    } else {
        DWORD sz = sizeof(r.status);
        if (!WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &r.status,
            &sz,
            WINHTTP_NO_HEADER_INDEX)) {
            OSTP_LOG_WARN("{} - WinHttpQueryHeaders(status) failed (error={})", url ? url : "", GetLastError());
        }

        DWORD avail = 0;
        while (true) {
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                OSTP_LOG_WARN("{} - WinHttpQueryDataAvailable failed (error={})", url ? url : "", GetLastError());
                break;
            }
            if (!avail) break;

            const size_t off = r.body.size();
            r.body.resize(off + avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, r.body.data() + off, avail, &read)) {
                OSTP_LOG_WARN("{} - WinHttpReadData(size={}) failed (error={})",
                              url ? url : "", avail, GetLastError());
                r.body.resize(off);
                break;
            }
            r.body.resize(off + read);
            if (r.body.size() >= maxBodyBytes) break;
        }

        if (r.status < 200 || r.status >= 300) {
            OSTP_LOG_WARN("{} - unexpected HTTP {}  body={}",
                             url,
                             r.status,
                             r.body.size() > 512 ? r.body.substr(0, 512) + "..." : r.body);
        } else {
            OSTP_LOG_TRACE("{} - response body={} ({}bytes)",
                           url ? url : "", r.body, r.body.size());
        }
        r.ok = true;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    OSTP_LOG_DEBUG("{} - elapsed: {}ms status={} body_bytes={}",
                   url ? url : "", elapsed, r.status, r.body.size());

    return r;
}

DownloadResult DownloadToFile(const char* url,
                              const std::wstring& headers,
                              const std::filesystem::path& outputPath,
                              uint64_t maxBytes,
                              uint32_t timeoutResolve,
                              uint32_t timeoutConnect,
                              uint32_t timeoutSend,
                              uint32_t timeoutRecv) {
    DownloadResult r;
    // Redacted once, before anything can fail: scheme + host + path without the
    // signed query. Every log line in this function uses this form.
    const std::string safeUrl = RedactUrl(url);

    // Everything below reports through DownloadResult and the filesystem error
    // codes; this guard is the backstop for allocation failure or a path the
    // standard library refuses to build, so no exception escapes this API.
    try {
        if (outputPath.empty()) {
            OSTP_LOG_WARN("{} - download failed: empty output path", safeUrl);
            return r;
        }

        std::filesystem::path partPath = outputPath;
        partPath += L".part";

        // A .part left by an interrupted earlier run must not be appended to.
        // If it cannot be removed (read-only after clearing the attribute, or
        // still held open by another process), fail now with a diagnosis: the
        // ofstream would fail for the same reason and every retry would look
        // identical with no clue why.
        if (const std::error_code ec = RemoveLeftoverPartial(partPath)) {
            OSTP_LOG_DEBUG("{} - download failed: cannot remove leftover partial {} ({})",
                           safeUrl, DescribePath(partPath), ec.message());
            return r;
        }

        // Every exit after this point that does not reach a successful rename
        // removes the partial: bad input, a missing directory, an HTTP failure,
        // a write error, a cap violation, a truncated body or a failed rename.
        // Running on unwind also covers the catch-all below.
        bool committed = false;
        Windows::ScopeExit removePartial([&] {
            if (committed) return;
            // This runs from a destructor, so it must not throw: the log line
            // allocates, and an allocation failure here would terminate.
            try {
                // Best effort on a path that is already failing; a read-only
                // leftover still gets the attribute cleared and one retry.
                if (const std::error_code ec = RemoveLeftoverPartial(partPath)) {
                    OSTP_LOG_DEBUG("{} - cannot remove partial {} ({})",
                                   safeUrl, DescribePath(partPath), ec.message());
                }
            } catch (...) {
            }
        });

        ParsedUrl pu = ParseUrl(url);
        if (!pu.valid) {
            OSTP_LOG_WARN("Invalid URL: {}", safeUrl);
            return r;
        }

        // The caller owns the cache directory; creating it here would hide a
        // configuration bug behind an unexpected write location. A missing
        // parent is reported as a clean failure instead.
        const std::filesystem::path parent = outputPath.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            if (!std::filesystem::is_directory(parent, ec)) {
                OSTP_LOG_WARN("{} - download failed: parent directory is not available: {}",
                              safeUrl, DescribePath(parent));
                return r;
            }
        }

        auto t0 = std::chrono::steady_clock::now();

        // Same User-Agent as Execute: it is part of the contract with the
        // frozen services this project talks to.
        UniqueWinHttpHandle session(WinHttpOpen(L"OpenSteamTool/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        if (!session) {
            OSTP_LOG_WARN("{} - WinHttpOpen failed (error={})", safeUrl, GetLastError());
            return r;
        }

        if (!WinHttpSetTimeouts(session.get(), timeoutResolve, timeoutConnect, timeoutSend, timeoutRecv)) {
            // The timeouts are part of the contract, but this failure is not
            // fatal: WinHTTP keeps its defaults and the GET can still succeed.
            OSTP_LOG_WARN("{} - WinHttpSetTimeouts failed (error={})", safeUrl, GetLastError());
        }

        // Deliberately no WINHTTP_OPTION_DECOMPRESSION here. The payload is an
        // already-compressed archive, and transparent decoding would make the
        // declared Content-Length describe the encoded body rather than the
        // bytes stored on disk, which is exactly the number the cap check and
        // the caller's digest comparison expect.

        UniqueWinHttpHandle connect(WinHttpConnect(session.get(), pu.host.c_str(), pu.port, 0));
        if (!connect) {
            OSTP_LOG_WARN("{} - WinHttpConnect(host='{}', port={}) failed (error={})",
                          safeUrl,
                          Encoding::WideToUtf8(pu.host),
                          pu.port,
                          GetLastError());
            return r;
        }

        const DWORD flags = pu.tls ? WINHTTP_FLAG_SECURE : 0;
        UniqueWinHttpHandle request(WinHttpOpenRequest(
            connect.get(),
            L"GET",
            pu.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags));
        if (!request) {
            OSTP_LOG_WARN("{} - WinHttpOpenRequest failed (error={})", safeUrl, GetLastError());
            return r;
        }

        if (!headers.empty()) {
            if (!WinHttpAddRequestHeaders(
                request.get(),
                headers.c_str(),
                static_cast<DWORD>(headers.size()),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
                // Matches Execute: a rejected header is not fatal by itself; the
                // request goes out and the status code decides the outcome.
                OSTP_LOG_WARN("{} - WinHttpAddRequestHeaders failed (error={})", safeUrl, GetLastError());
            }
        }

        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                nullptr, 0, 0, 0)) {
            OSTP_LOG_WARN("{} - WinHttpSendRequest failed (error={})", safeUrl, GetLastError());
            return r;
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            OSTP_LOG_WARN("{} - WinHttpReceiveResponse failed (error={})", safeUrl, GetLastError());
            return r;
        }

        DWORD statusSize = sizeof(r.status);
        if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &r.status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
            OSTP_LOG_WARN("{} - WinHttpQueryHeaders(status) failed (error={})", safeUrl, GetLastError());
            return r;
        }

        if (r.status < 200 || r.status >= 300) {
            // Nothing is drained: an error page can be arbitrarily large, and
            // closing the request is all the cleanup a failed GET needs.
            OSTP_LOG_WARN("{} - download failed: HTTP {}", safeUrl, r.status);
            return r;
        }

        // The declared length is read as text, not with WINHTTP_QUERY_FLAG_NUMBER:
        // the numeric form is a DWORD, so a >4 GiB claim would wrap and pass a
        // cap it should have failed. A missing header (chunked response) or an
        // unparsable value leaves enforcement to the read loop below.
        bool hasContentLength = false;
        uint64_t contentLength = 0;
        {
            wchar_t text[32] = {};
            DWORD textSize = sizeof(text);
            if (WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_CONTENT_LENGTH,
                WINHTTP_HEADER_NAME_BY_INDEX,
                text,
                &textSize,
                WINHTTP_NO_HEADER_INDEX)) {
                if (const auto parsed = Numbers::ParseUInt64(std::wstring_view(text))) {
                    hasContentLength = true;
                    contentLength = *parsed;
                }
            }
        }

        if (hasContentLength && contentLength > maxBytes) {
            // Refused before the body is touched: no partial file is created for
            // a payload that cannot be accepted.
            OSTP_LOG_WARN("{} - download failed: Content-Length={} exceeds cap={}",
                          safeUrl, contentLength, maxBytes);
            return r;
        }

        std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            OSTP_LOG_WARN("{} - cannot create partial file {}", safeUrl, DescribePath(partPath));
            return r;
        }

        std::vector<char> buffer(kDownloadChunkBytes);

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                OSTP_LOG_WARN("{} - WinHttpQueryDataAvailable failed (error={})", safeUrl, GetLastError());
                return r;
            }
            // WinHTTP reports the end of the body as "nothing available"; there
            // is no separate EOF flag, so this is the loop's only clean exit.
            if (available == 0) break;

            const DWORD wanted = available < kDownloadChunkBytes ? available : kDownloadChunkBytes;
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), wanted, &read)) {
                OSTP_LOG_WARN("{} - WinHttpReadData(size={}) failed (error={})",
                              safeUrl, wanted, GetLastError());
                return r;
            }
            if (read == 0) {
                // A zero-byte read while bytes were just announced would spin
                // forever. Treat it as a broken transfer rather than accepting a
                // silently truncated body.
                OSTP_LOG_WARN("{} - WinHttpReadData returned 0 with {} bytes available",
                              safeUrl, available);
                return r;
            }

            // The cap is enforced on the bytes actually received, because a
            // chunked response declares no length and a Content-Length can lie.
            if (read > maxBytes - r.bytesWritten) {
                OSTP_LOG_WARN("{} - download failed: body exceeds cap={} ({} bytes + {})",
                              safeUrl, maxBytes, r.bytesWritten, read);
                return r;
            }

            out.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!out) {
                OSTP_LOG_WARN("{} - cannot write {}", safeUrl, DescribePath(partPath));
                return r;
            }
            r.bytesWritten += read;
        }

        out.flush();
        out.close(); // a buffered or failed close invalidates the download
        if (!out) {
            OSTP_LOG_WARN("{} - cannot finish writing {}", safeUrl, DescribePath(partPath));
            return r;
        }

        // A declared length that does not match what arrived means the body was
        // cut short (or padded), so the file is not the complete payload.
        if (hasContentLength && r.bytesWritten != contentLength) {
            OSTP_LOG_WARN("{} - download failed: wrote {} bytes, Content-Length={}",
                          safeUrl, r.bytesWritten, contentLength);
            return r;
        }

        // MoveFileExW with replace semantics, mirroring TrainerPackage's atomic
        // copy: .part is a sibling of the destination, so this is a same-volume
        // rename — an atomic directory-entry swap that never copies, and a
        // reader sees either the previous file or the complete new one.
        // MOVEFILE_WRITE_THROUGH makes the call return only once the file is
        // flushed. (std::filesystem::rename would map to the same Win32 call
        // under MSVC, but the explicit flags and GetLastError stay visible.)
        if (!MoveFileExW(partPath.c_str(), outputPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            OSTP_LOG_WARN("{} - cannot replace {} (error={})",
                          safeUrl, DescribePath(outputPath), GetLastError());
            return r;
        }
        committed = true;
        r.ok = true;

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        OSTP_LOG_DEBUG("{} - downloaded {} bytes to {} in {}ms",
                       safeUrl, r.bytesWritten, DescribePath(outputPath), elapsed);
    } catch (const std::bad_alloc&) {
        OSTP_LOG_ERROR("{} - download failed: out of memory", safeUrl);
        r = DownloadResult{};
    } catch (const std::exception&) {
        OSTP_LOG_ERROR("{} - download failed: unexpected exception", safeUrl);
        r = DownloadResult{};
    } catch (...) {
        OSTP_LOG_ERROR("{} - download failed: unknown exception", safeUrl);
        r = DownloadResult{};
    }

    return r;
}

} // namespace OSTPlatform::Http
