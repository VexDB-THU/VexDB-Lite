#ifndef VEXFS_ARCHIVE_H
#define VEXFS_ARCHIVE_H

#include <string>

namespace vexfs_cli {

// The .vexfs file is a backend-neutral logical package stored in an SQLite
// container. It is not a copy of the source database.
std::string ExportArchive(const std::string &database, const std::string &workspace,
                          const std::string &snapshot, const std::string &output);
std::string ExportPostgresArchive(const std::string &dsn, const std::string &workspace,
                                  const std::string &snapshot, const std::string &output);
std::string ImportArchive(const std::string &database, const std::string &workspace,
                          const std::string &input);
std::string ImportPostgresArchive(const std::string &dsn, const std::string &workspace,
                                  const std::string &input);
std::string VerifyArchive(const std::string &input);

}  // namespace vexfs_cli

#endif  // VEXFS_ARCHIVE_H
