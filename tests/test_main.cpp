#include "test_harness.h"

int main() {
  TestSuite suite;
  register_resp_parser_tests(suite);
  register_storage_tests(suite);
  register_command_tests(suite);
  register_persistence_tests(suite);
  return suite.run();
}
