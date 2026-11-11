#include "receiver-diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
  }
}

static void cleanup(const char *path)
{
  char backup[256];
  remove(path);
  snprintf(backup, sizeof(backup), "%s.1", path);
  remove(backup);
  snprintf(backup, sizeof(backup), "%s.2", path);
  remove(backup);
}

static int file_equals(const char *path, const char *expected)
{
  char actual[128];
  FILE *file = fopen(path, "rb");
  if (!file)
    return 0;
  size_t size = fread(actual, 1, sizeof(actual) - 1, file);
  int ok = !ferror(file) && feof(file);
  fclose(file);
  actual[size] = '\0';
  return ok && strcmp(actual, expected) == 0;
}

int main(void)
{
  const char *path = "sei-diagnostics-rotation-test.jsonl";
  cleanup(path);
  check(!receiver_diagnostics_append(NULL, "x\n", 8, 2),
        "null path is rejected");
  check(!receiver_diagnostics_append(path, "123456789\n", 8, 2),
        "oversized record is rejected without writing");
  check(receiver_diagnostics_append(path, "AAAA\n", 8, 2),
        "first record is appended");
  check(receiver_diagnostics_append(path, "BBBB\n", 8, 2),
        "second record is appended within the cap");
  check(receiver_diagnostics_append(path, "CCCC\n", 8, 2),
        "rotation preserves the newest record");
  check(file_equals(path, "CCCC\n"), "active log contains newest record");
  char backup1[256];
  char backup2[256];
  snprintf(backup1, sizeof(backup1), "%s.1", path);
  snprintf(backup2, sizeof(backup2), "%s.2", path);
  check(file_equals(backup1, "BBBB\n"), "first backup contains prior record");
  check(file_equals(backup2, "AAAA\n"), "second backup contains oldest record");
  cleanup(path);
  if (failures) {
    fprintf(stderr, "%d diagnostic test(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  puts("Bounded diagnostic log rotation tests passed.");
  return EXIT_SUCCESS;
}
