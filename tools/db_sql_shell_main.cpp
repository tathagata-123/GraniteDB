#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "../sql/planner_frontend.h"

using namespace simpledb;

namespace {

std::string Repeat(char c, std::size_t count) {
    return std::string(count, c);
}

std::vector<std::size_t> ComputeColumnWidths(const SqlQueryResult &result) {
    std::vector<std::size_t> widths(result.schema.GetColumnCount(), 3);
    for (std::size_t i = 0; i < result.schema.GetColumnCount(); i++) {
        widths[i] = std::max<std::size_t>(widths[i], result.schema.GetColumn(i).GetName().size());
    }
    for (const Tuple &tuple : result.rows) {
        for (std::size_t i = 0; i < tuple.Size(); i++) {
            widths[i] = std::max<std::size_t>(widths[i], tuple.GetValue(i).ToString().size());
        }
    }

    for (std::size_t i = 0; i < widths.size(); i++) {
        if (result.schema.GetColumn(i).GetName() == "plan") {
            widths[i] = std::min<std::size_t>(std::max<std::size_t>(widths[i], 60), 120);
        } else {
            widths[i] = std::min<std::size_t>(std::max<std::size_t>(widths[i], 12), 24);
        }
    }
    return widths;
}

std::string Clip(const std::string &text, std::size_t width) {
    if (text.size() <= width) return text;
    if (width <= 3) return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

void PrintHorizontalLine(const std::vector<std::size_t> &widths) {
    for (std::size_t width : widths) {
        std::cout << '+' << Repeat('-', width + 2);
    }
    std::cout << "+\n";
}

void PrintResultTable(const SqlQueryResult &result) {
    if (!result.has_rows) return;
    std::vector<std::size_t> widths = ComputeColumnWidths(result);

    PrintHorizontalLine(widths);
    for (std::size_t i = 0; i < result.schema.GetColumnCount(); i++) {
        std::string name = Clip(result.schema.GetColumn(i).GetName(), widths[i]);
        std::cout << "| " << name << Repeat(' ', widths[i] - name.size() + 1);
    }
    std::cout << "|\n";
    PrintHorizontalLine(widths);

    for (const Tuple &tuple : result.rows) {
        for (std::size_t i = 0; i < tuple.Size(); i++) {
            std::string text = Clip(tuple.GetValue(i).ToString(), widths[i]);
            std::cout << "| " << text << Repeat(' ', widths[i] - text.size() + 1);
        }
        std::cout << "|\n";
    }
    PrintHorizontalLine(widths);
}

}  // namespace

int main(int argc, char **argv) {
    std::string db_dir = (argc >= 2) ? argv[1] : "sql_shell_db";
    try {
        SqlPlannerFrontend frontend(db_dir);
        std::cout << "SimpleDB SQL shell\n";
        std::cout << "Supported subset: CREATE TABLE, CREATE INDEX, INSERT, SELECT, UPDATE, DELETE\n";
        std::cout << "Type quit; or exit; to leave.\n";

        std::string buffer;
        while (true) {
            std::cout << (buffer.empty() ? "simpledb> " : "......> ");
            std::string line;
            if (!std::getline(std::cin, line)) break;
            buffer += line + "\n";
            if (buffer.find(';') == std::string::npos) continue;

            std::string sql = buffer;
            buffer.clear();
            std::string trimmed = sql;
            for (char &c : trimmed) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (trimmed == "quit;\n" || trimmed == "exit;\n") break;

            try {
                SqlQueryResult result = frontend.ExecuteSql(sql);
                if (result.has_rows) PrintResultTable(result);
                std::cout << result.message << "\n";
            } catch (const std::exception &ex) {
                std::cout << "ERROR: " << ex.what() << "\n";
            }
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "db_sql_shell failed: " << ex.what() << "\n";
        return 1;
    }
}
