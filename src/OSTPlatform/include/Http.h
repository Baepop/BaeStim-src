#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace OSTPlatform::Http {

    struct Result {
        std::string body;
        uint32_t status = 0;
        // True once WinHTTP produced a response; callers must still check status for 2xx.
        bool ok = false;
    };

    Result Execute(const wchar_t* method,
                   const char* url,
                   const void* reqBody = nullptr,
                   uint32_t reqBodyLen = 0,
                   const wchar_t* headers = nullptr,
                   uint32_t timeoutResolve = 5000,
                   uint32_t timeoutConnect = 5000,
                   uint32_t timeoutSend = 10000,
                   uint32_t timeoutRecv = 10000,
                   // Upper bound on the response body read into memory. The default
                   // suits small metadata (TOML/JSON); callers fetching larger payloads
                   // (e.g. a DLL) must raise it. Reading stops once the cap is reached.
                   uint32_t maxBodyBytes = 256 * 1024);

    struct DownloadResult {
        uint32_t status = 0;
        bool ok = false;
        uint64_t bytesWritten = 0;
    };

    // Streams a GET response body straight to disk. The body is never held in
    // memory: it is read in 64 KiB chunks with WinHttpReadData and written to
    // <outputPath>.part with a binary std::ofstream.
    //
    // - <outputPath>.part is a sibling of the destination, so moving it over
    //   <outputPath> on success is a same-volume rename (MoveFileExW with
    //   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH): a reader sees the
    //   previous file or the complete new one, never a truncated one.
    // - The partial file is deleted on ANY failure, including a non-2xx status,
    //   a short/failed write, a body over the cap or a failed rename. An
    //   existing <outputPath> is left untouched in that case. The one exception
    //   is a leftover partial from an earlier run that cannot be removed (held
    //   open, or read-only in a folder whose attributes cannot be cleared): the
    //   download then fails before any network I/O and says so.
    // - ok is true only for a 2xx status whose whole body was written and stayed
    //   within maxBytes. `status` still carries the response code for callers
    //   that want to distinguish 404 from 401 (0 when no response was read).
    // - Content-Length is consulted first and an oversized declared body is
    //   refused before any byte is read. The same cap is enforced while reading,
    //   because a chunked response has no Content-Length and a declared length
    //   cannot be trusted.
    // - A status >= 300 (or any other non-2xx) fails immediately: the body is not
    //   drained, the connection is just closed.
    // - bytesWritten is the number of bytes written to the partial file before
    //   the function returned; on failure the partial file is always removed, so
    //   the value does not describe a file that exists on disk (except for the
    //   unremovable-leftover case above, where it is 0).
    // - The parent directory of outputPath must already exist; this unit does not
    //   create cache directories (the caller owns them) and fails cleanly if it
    //   is missing.
    // - `headers` carries raw additional request headers in WinHTTP syntax, e.g.
    //   L"apikey: ...\r\n" (same convention as Execute). It may be empty.
    // - No exception escapes: invalid input, HTTP failures and I/O errors are all
    //   reported through DownloadResult.
    DownloadResult DownloadToFile(const char* url,
                                  const std::wstring& headers,
                                  const std::filesystem::path& outputPath,
                                  uint64_t maxBytes,
                                  uint32_t timeoutResolve = 5000,
                                  uint32_t timeoutConnect = 5000,
                                  uint32_t timeoutSend = 10000,
                                  uint32_t timeoutRecv = 30000);

} // namespace OSTPlatform::Http
