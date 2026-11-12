#include "receiver-diagnostics.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECEIVER_DIAGNOSTICS_MAX_BACKUPS 8U

static char *backup_path(const char *path, unsigned index)
{
  size_t length = strlen(path) + 16;
  char *result = (char *)malloc(length);
  if (!result)
    return NULL;
  snprintf(result, length, "%s.%u", path, index);
  return result;
}

static bool remove_if_present(const char *path)
{
  if (remove(path) == 0)
    return true;
  return errno == ENOENT;
}

static bool rotate_files(const char *path, unsigned backups)
{
  if (!backups)
    return remove_if_present(path);

  for (unsigned index = backups; index > 1; --index) {
    char *from = backup_path(path, index - 1);
    char *to = backup_path(path, index);
    if (!from || !to) {
      free(from);
      free(to);
      return false;
    }
    bool ok = remove_if_present(to);
    if (ok && rename(from, to) != 0 && errno != ENOENT)
      ok = false;
    free(from);
    free(to);
    if (!ok)
      return false;
  }

  char *first_backup = backup_path(path, 1);
  if (!first_backup)
    return false;
  bool ok = remove_if_present(first_backup);
  if (ok && rename(path, first_backup) != 0 && errno != ENOENT)
    ok = false;
  free(first_backup);
  return ok;
}

static bool current_size(const char *path, size_t *size)
{
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (errno == ENOENT) {
      *size = 0;
      return true;
    }
    return false;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return false;
  }
  long end = ftell(file);
  fclose(file);
  if (end < 0)
    return false;
  *size = (size_t)end;
  return true;
}

bool receiver_diagnostics_append(const char *path, const char *line,
                                 size_t max_file_bytes, unsigned backup_count)
{
  if (!path || !*path || !line || !max_file_bytes)
    return false;
  size_t line_size = strlen(line);
  if (!line_size || line_size > max_file_bytes)
    return false;
  if (backup_count > RECEIVER_DIAGNOSTICS_MAX_BACKUPS)
    backup_count = RECEIVER_DIAGNOSTICS_MAX_BACKUPS;

  size_t existing_size = 0;
  if (!current_size(path, &existing_size))
    return false;
  if (existing_size > max_file_bytes ||
      line_size > max_file_bytes - existing_size) {
    if (!rotate_files(path, backup_count))
      return false;
  }

  FILE *file = fopen(path, "ab");
  if (!file)
    return false;
  bool ok = fwrite(line, 1, line_size, file) == line_size;
  if (fclose(file) != 0)
    ok = false;
  return ok;
}
