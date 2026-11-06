#pragma once

#include <cstddef>

// Returns 2 for enabled, 1 for disabled, 0 for missing, and -1 when FSKit
// cannot answer. The URL is optional and only populated for an installed module.
extern "C" int vexfs_fskit_extension_state(const char *bundle_identifier,
                                             char *url,
                                             std::size_t url_size);
