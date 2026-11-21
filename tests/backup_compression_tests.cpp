#include "biblioled/backup_compression.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

bool WriteFile(const std::string& path, const std::vector<unsigned char>& data)
{
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(&data[0]),
                     static_cast<std::streamsize>(data.size()));
    }
    return output.good();
}

bool ReadFile(const std::string& path, std::vector<unsigned char>* data)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return false;
    data->assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool CopyWithTrailingByte(const std::string& source, const std::string& destination)
{
    std::ifstream input(source.c_str(), std::ios::binary);
    std::ofstream output(destination.c_str(), std::ios::binary | std::ios::trunc);
    if (!input || !output) return false;
    output << input.rdbuf();
    output.put('X');
    return output.good();
}

bool CopyWithoutLastByte(const std::string& source, const std::string& destination)
{
    std::vector<unsigned char> bytes;
    if (!ReadFile(source, &bytes) || bytes.size() < 2) return false;
    bytes.pop_back();
    return WriteFile(destination, bytes);
}

} // namespace

int main()
{
    const std::string base =
        "backup-compression-test-" + std::to_string(std::rand());
    const std::string input_path = base + ".sqlite";
    const std::string corrupt_path = base + ".corrupt.gz";
    const std::string trailing_path = base + ".trailing.gz";

    std::vector<unsigned char> original(512 * 1024);
    for (size_t i = 0; i < original.size(); ++i) {
        original[i] = static_cast<unsigned char>((i / 37 + i / 4096) % 251);
    }
    Check(WriteFile(input_path, original), "test SQLite-like input should be written");

    const biblioled::BackupCompressionFormat formats[] = {
        biblioled::BackupCompressionFormat::Gzip,
        biblioled::BackupCompressionFormat::Zlib
    };
    const int levels[] = {0, 1, 6, 9};

    for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
        for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); ++l) {
            const std::string extension =
                biblioled::BackupCompressionExtension(formats[f]);
            const std::string compressed = base + "_" +
                (formats[f] == biblioled::BackupCompressionFormat::Gzip ? "gzip" : "zlib") +
                "_" + std::to_string(levels[l]) + extension;
            const std::string decoded = compressed + ".decoded.sqlite";
            std::string error;

            Check(biblioled::CompressBackupFile(
                      input_path, compressed, formats[f], levels[l], &error),
                  "backup should compress in both formats at every supported test level");
            if (!error.empty()) std::cerr << error << std::endl;
            Check(biblioled::DecompressBackupFile(
                      compressed, decoded, formats[f], &error),
                  "compressed backup should decompress successfully");
            if (!error.empty()) std::cerr << error << std::endl;

            std::vector<unsigned char> restored;
            Check(ReadFile(decoded, &restored),
                  "decompressed output should be readable");
            Check(restored == original,
                  "decompression should reproduce every original byte");

            biblioled::RemoveBackupFile(compressed);
            biblioled::RemoveBackupFile(decoded);
        }
    }

    const std::string valid_gzip = base + "_valid.sqlite.gz";
    const std::string decoded_valid = base + "_valid.decoded.sqlite";
    std::string error;
    Check(biblioled::CompressBackupFile(
              input_path, valid_gzip,
              biblioled::BackupCompressionFormat::Gzip, 6, &error),
          "a valid gzip archive should be generated for corruption tests");
    Check(CopyWithoutLastByte(valid_gzip, corrupt_path),
          "truncated compressed data should be generated");
    Check(!biblioled::DecompressBackupFile(
              corrupt_path, decoded_valid,
              biblioled::BackupCompressionFormat::Gzip, &error),
          "truncated gzip data must be rejected");
    Check(!ReadFile(decoded_valid, &original),
          "failed decompression must remove its partial output");

    Check(CopyWithTrailingByte(valid_gzip, trailing_path),
          "trailing-data compressed file should be generated");
    Check(!biblioled::DecompressBackupFile(
              trailing_path, decoded_valid,
              biblioled::BackupCompressionFormat::Gzip, &error),
          "unexpected trailing data must be rejected");

    Check(!biblioled::CompressBackupFile(
              input_path, base + ".bad-level.gz",
              biblioled::BackupCompressionFormat::Gzip, 10, &error),
          "compression levels outside 0..9 must be rejected");

    biblioled::RemoveBackupFile(input_path);
    biblioled::RemoveBackupFile(valid_gzip);
    biblioled::RemoveBackupFile(corrupt_path);
    biblioled::RemoveBackupFile(trailing_path);
    biblioled::RemoveBackupFile(decoded_valid);
    biblioled::RemoveBackupFile(base + ".bad-level.gz");

    if (failures != 0) {
        std::cerr << failures << " backup-compression assertion(s) failed." << std::endl;
        return 1;
    }
    std::cout << "Backup compression tests passed." << std::endl;
    return 0;
}
