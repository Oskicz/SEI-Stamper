#ifndef SEI_RECEIVER_DIAGNOSTICS_H
#define SEI_RECEIVER_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller serializes writers; keeps active log and numbered backups bounded. */
bool receiver_diagnostics_append(const char *path, const char *line,
                                 size_t max_file_bytes, unsigned backup_count);

#ifdef __cplusplus
}
#endif
#endif
