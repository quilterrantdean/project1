#include "biblioled/backup_compression.h"

#include <zlib.h>

#include <cstdio>
#include <cerrno>
#include <stdint.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace biblioled {
namespace {

const size_t kBufferSize = 64 * 1024;

void SetError(std::string* error, const std::string& message)
{
    if (error) *error = message;
}

#ifdef _WIN32
std::wstring Utf8ToWidePath(const std::string& path)
{
    if (path.empty()) return std::wstring();
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, path.c_str(),
        static_cast<int>(path.size()), NULL, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0,
                             path.c_str(), static_cast<int>(path.size()),
                             &result[0], length)) {
        return std::wstring();
    }
    return result;
}
#endif

FILE* OpenBinaryFile(const std::string& path, bool write)
{
#ifdef _WIN32
    const std::wstring wide_path = Utf8ToWidePath(path);
    if (wide_path.empty()) return NULL;
    return _wfopen(wide_path.c_str(), write ? L"wb" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "wb" : "rb");
#endif
}

bool WriteOutput(FILE* output, const unsigned char* bytes, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const size_t count = std::fwrite(bytes + written, 1, size - written, output);
        if (count == 0) return false;
        written += count;
    }
    return true;
}

bool FlushOutputFile(FILE* output)
{
    if (!output || std::fflush(output) != 0) return false;

#ifdef _WIN32
    const int descriptor = _fileno(output);
    if (descriptor < 0) return false;
    const intptr_t native_handle = _get_osfhandle(descriptor);
    if (native_handle == -1) return false;
    return FlushFileBuffers(reinterpret_cast<HANDLE>(native_handle)) != FALSE;
#else
    const int descriptor = fileno(output);
    return descriptor >= 0 && fsync(descriptor) == 0;
#endif
}

bool FinishFiles(FILE* input, FILE* output)
{
    bool ok = true;
    // Flush the C runtime buffer and then ask the operating system to commit
    // file data before a validated backup is installed under its final name.
    if (output && !FlushOutputFile(output)) ok = false;
    if (input && std::fclose(input) != 0) ok = false;
    if (output && std::fclose(output) != 0) ok = false;
    return ok;
}

} // namespace

std::string BackupCompressionExtension(BackupCompressionFormat format)
{
    return format == BackupCompressionFormat::Gzip ? ".sqlite.gz" : ".sqlite.zlib";
}

bool RemoveBackupFile(const std::string& path)
{
#ifdef _WIN32
    const std::wstring wide_path = Utf8ToWidePath(path);
    if (wide_path.empty()) return false;
    if (DeleteFileW(wide_path.c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND ||
           GetLastError() == ERROR_PATH_NOT_FOUND;
#else
    return std::remove(path.c_str()) == 0 || errno == ENOENT;
#endif
}

bool MoveBackupFile(const std::string& source_path,
                    const std::string& destination_path)
{
#ifdef _WIN32
    const std::wstring source = Utf8ToWidePath(source_path);
    const std::wstring destination = Utf8ToWidePath(destination_path);
    if (source.empty() || destination.empty()) return false;
    return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    return std::rename(source_path.c_str(), destination_path.c_str()) == 0;
#endif
}

bool CompressBackupFile(const std::string& source_path,
                        const std::string& destination_path,
                        BackupCompressionFormat format,
                        int level,
                        std::string* error)
{
    if (level < 0 || level > 9) {
        SetError(error, "The zlib compression level must be between 0 and 9.");
        return false;
    }

    FILE* input = OpenBinaryFile(source_path, false);
    if (!input) {
        SetError(error, "Could not open the SQLite snapshot for compression.");
        return false;
    }
    FILE* output = OpenBinaryFile(destination_path, true);
    if (!output) {
        std::fclose(input);
        SetError(error, "Could not create the compressed backup file.");
        return false;
    }

    z_stream stream = {};
    const int window_bits = format == BackupCompressionFormat::Gzip
        ? MAX_WBITS + 16 : MAX_WBITS;
    int result = deflateInit2(&stream, level, Z_DEFLATED,
                              window_bits, 8, Z_DEFAULT_STRATEGY);
    if (result != Z_OK) {
        FinishFiles(input, output);
        RemoveBackupFile(destination_path);
        SetError(error, "zlib could not initialise backup compression.");
        return false;
    }

    unsigned char in_buffer[kBufferSize];
    unsigned char out_buffer[kBufferSize];
    bool ok = true;
    int flush = Z_NO_FLUSH;

    do {
        const size_t count = std::fread(in_buffer, 1, sizeof(in_buffer), input);
        if (std::ferror(input)) {
            ok = false;
            SetError(error, "Could not read the SQLite snapshot during compression.");
            break;
        }

        stream.next_in = in_buffer;
        stream.avail_in = static_cast<uInt>(count);
        flush = std::feof(input) ? Z_FINISH : Z_NO_FLUSH;

        do {
            stream.next_out = out_buffer;
            stream.avail_out = static_cast<uInt>(sizeof(out_buffer));
            result = deflate(&stream, flush);
            if (result != Z_OK && result != Z_STREAM_END) {
                ok = false;
                SetError(error, "zlib reported an error while compressing the backup.");
                break;
            }
            const size_t produced = sizeof(out_buffer) - stream.avail_out;
            if (produced && !WriteOutput(output, out_buffer, produced)) {
                ok = false;
                SetError(error, "Could not write the compressed backup.");
                break;
            }
        } while (ok && result != Z_STREAM_END && stream.avail_out == 0);

        if (!ok) break;
    } while (result != Z_STREAM_END);

    const int end_result = deflateEnd(&stream);
    if (end_result != Z_OK && ok) {
        ok = false;
        SetError(error, "zlib could not finalise the compressed backup.");
    }
    if (!FinishFiles(input, output) && ok) {
        ok = false;
        SetError(error, "Could not flush the compressed backup to disk.");
    }
    if (!ok) RemoveBackupFile(destination_path);
    return ok;
}

bool DecompressBackupFile(const std::string& source_path,
                          const std::string& destination_path,
                          BackupCompressionFormat format,
                          std::string* error)
{
    FILE* input = OpenBinaryFile(source_path, false);
    if (!input) {
        SetError(error, "Could not open the compressed backup.");
        return false;
    }
    FILE* output = OpenBinaryFile(destination_path, true);
    if (!output) {
        std::fclose(input);
        SetError(error, "Could not create the temporary SQLite restore file.");
        return false;
    }

    z_stream stream = {};
    const int window_bits = format == BackupCompressionFormat::Gzip
        ? MAX_WBITS + 16 : MAX_WBITS;
    int result = inflateInit2(&stream, window_bits);
    if (result != Z_OK) {
        FinishFiles(input, output);
        RemoveBackupFile(destination_path);
        SetError(error, "zlib could not initialise backup decompression.");
        return false;
    }

    unsigned char in_buffer[kBufferSize];
    unsigned char out_buffer[kBufferSize];
    bool ok = true;
    bool reached_stream_end = false;

    while (ok && !reached_stream_end) {
        if (stream.avail_in == 0) {
            const size_t count = std::fread(in_buffer, 1, sizeof(in_buffer), input);
            if (count == 0) {
                if (std::ferror(input)) {
                    SetError(error, "Could not read the compressed backup.");
                } else {
                    SetError(error, "The compressed backup ended before its stream was complete.");
                }
                ok = false;
                break;
            }
            stream.next_in = in_buffer;
            stream.avail_in = static_cast<uInt>(count);
        }

        stream.next_out = out_buffer;
        stream.avail_out = static_cast<uInt>(sizeof(out_buffer));
        result = inflate(&stream, Z_NO_FLUSH);

        if (result != Z_OK && result != Z_STREAM_END) {
            SetError(error, "The compressed backup is corrupt or uses an unsupported format.");
            ok = false;
            break;
        }

        const size_t produced = sizeof(out_buffer) - stream.avail_out;
        if (produced && !WriteOutput(output, out_buffer, produced)) {
            SetError(error, "Could not write the decompressed SQLite backup.");
            ok = false;
            break;
        }

        if (result == Z_STREAM_END) reached_stream_end = true;
    }

    // A backup is exactly one complete gzip or zlib stream. Reject trailing
    // bytes and concatenated members rather than silently ignoring data.
    if (ok && reached_stream_end) {
        if (stream.avail_in != 0) {
            SetError(error, "The compressed backup contains unexpected trailing data.");
            ok = false;
        } else {
            unsigned char extra = 0;
            const size_t count = std::fread(&extra, 1, 1, input);
            if (count != 0 || std::ferror(input)) {
                SetError(error, "The compressed backup contains unexpected trailing data.");
                ok = false;
            }
        }
    }

    const int end_result = inflateEnd(&stream);
    if (end_result != Z_OK && ok) {
        ok = false;
        SetError(error, "zlib could not finalise backup decompression.");
    }
    if (!FinishFiles(input, output) && ok) {
        ok = false;
        SetError(error, "Could not flush the decompressed SQLite backup to disk.");
    }
    if (!ok) RemoveBackupFile(destination_path);
    return ok;
}

} // namespace biblioled
