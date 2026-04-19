#include <iostream>

int main() {
    std::cout << "SimpleDB built successfully.\n"
              << "Available executables:\n"
              << "  simpledb\n"
              << "  db_bench <db_dir> [row_count] [iterations]\n"
              << "  db_crash_demo fresh <db_dir> [row_count]\n"
              << "  db_crash_demo recover <db_dir>\n"
              << "  db_sql_shell <db_dir>\n";
    return 0;
}
