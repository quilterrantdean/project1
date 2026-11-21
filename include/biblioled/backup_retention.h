#ifndef BIBLIOLED_BACKUP_RETENTION_H
#define BIBLIOLED_BACKUP_RETENTION_H

#include <string>

namespace biblioled {

// Prunes only BiblioLED_auto_* backup files. Manual, safety, and legacy
// uncompressed backups are deliberately outside this retention policy.
bool PruneAutomaticBackups(const std::string& directory,
                           std::string* error);

} // namespace biblioled

#endif
