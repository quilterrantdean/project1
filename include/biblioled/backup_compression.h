#ifndef BIBLIOLED_BACKUP_COMPRESSION_H
#define BIBLIOLED_BACKUP_COMPRESSION_H

#include <string>

namespace biblioled {

enum class BackupCompressionFormat {
    Gzip,
    Zlib
};

// Stream a file through zlib. Paths are UTF-8 on Windows.
bool CompressBackupFile(const std::string& source_path,
                        const std::string& destination_path,
                        BackupCompressionFormat format,
                        int level,
                        std::string* error);

bool DecompressBackupFile(const std::string& source_path,
                          const std::string& destination_path,
                          BackupCompressionFormat format,
                          std::string* error);

bool RemoveBackupFile(const std::string& path);
bool MoveBackupFile(const std::string& source_path,
                    const std::string& destination_path);
std::string BackupCompressionExtension(BackupCompressionFormat format);

} // namespace biblioled

#endif
