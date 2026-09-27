/* Run one regional-session responsibility, or all suites without arguments. */
#include "regional_session/regional_session_test.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  static const struct {
    const char *name;
    int (*run)(void);
  } suites[] = {
      {"activation", RegionalSessionTest_RunActivation},
      {"persistence", RegionalSessionTest_RunPersistence},
      {"codec", RegionalSessionTest_RunCodec},
      {"profiles", RegionalSessionTest_RunProfiles},
      {"randomizer", RegionalSessionTest_RunRandomizer},
  };
  int failures = 0;
  bool matched = false;
  if (argc <= 2) {
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); ++i) {
      if (argc == 2 && strcmp(argv[1], suites[i].name)) continue;
      matched = true;
      int count = suites[i].run();
      printf("regional session %s: %d failures\n", suites[i].name, count);
      failures += count;
    }
  }
  if (!matched) {
    fprintf(stderr, "Usage: %s [suite]\nAvailable suites:", argv[0]);
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); ++i)
      fprintf(stderr, " %s", suites[i].name);
    fputc('\n', stderr);
    return 2;
  }
  return failures ? 1 : 0;
}
