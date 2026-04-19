#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../sql/planner_frontend.h"

using namespace simpledb;

namespace {

std::vector<std::string> ColumnStrings(const SqlQueryResult &result, std::size_t column_idx) {
    std::vector<std::string> out;
    out.reserve(result.rows.size());
    for (const Tuple &row : result.rows) {
        out.push_back(row.GetValue(column_idx).ToString());
    }
    return out;
}

std::string ExplainText(const SqlQueryResult &result) {
    std::string text;
    for (const Tuple &row : result.rows) {
        if (!text.empty()) text += "\n";
        text += row.GetValue(0).ToString();
    }
    return text;
}

void Expect(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void ExpectVectorEq(const std::vector<std::string> &actual,
                    const std::vector<std::string> &expected,
                    const std::string &message) {
    if (actual == expected) return;
    std::string detail = message + "\nexpected:";
    for (const auto &v : expected) detail += " [" + v + "]";
    detail += "\nactual:";
    for (const auto &v : actual) detail += " [" + v + "]";
    throw std::runtime_error(detail);
}

SqlQueryResult Run(SqlPlannerFrontend &frontend, const std::string &sql) {
    return frontend.ExecuteSql(sql);
}

}  // namespace

int main(int argc, char **argv) {
    std::string db_dir = (argc >= 2) ? argv[1] : "sql_regression_db";
    std::filesystem::remove_all(db_dir);
    std::filesystem::create_directories(db_dir);

    try {
        SqlPlannerFrontend frontend(db_dir);

        Run(frontend, "CREATE TABLE emp (id INT, dept INT, salary INT, name VARCHAR(20));");
        Run(frontend, "CREATE TABLE dept (id INT, dname VARCHAR(20));");
        Run(frontend, "CREATE INDEX emp_id_idx ON emp(id);");
        Run(frontend, "CREATE INDEX emp_dept_idx ON emp(dept);");
        Run(frontend, "CREATE INDEX emp_salary_idx ON emp(salary);");
        Run(frontend, "CREATE INDEX emp_dept_salary_idx ON emp(dept, salary);");

        Run(frontend, "INSERT INTO emp VALUES (1, 10, 100, 'a');");
        Run(frontend, "INSERT INTO emp VALUES (2, 10, 200, 'b');");
        Run(frontend, "INSERT INTO emp VALUES (3, 20, 150, 'c');");
        Run(frontend, "INSERT INTO emp VALUES (4, 20, 300, 'd');");
        Run(frontend, "INSERT INTO dept VALUES (10, 'eng');");
        Run(frontend, "INSERT INTO dept VALUES (20, 'sales');");

        {
            SqlQueryResult result = Run(frontend, "SELECT name FROM emp ORDER BY salary;");
            ExpectVectorEq(ColumnStrings(result, 0), {"a", "c", "b", "d"},
                           "ORDER BY on non-projected source column failed");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT salary AS s FROM emp ORDER BY s DESC LIMIT 2;");
            ExpectVectorEq(ColumnStrings(result, 0), {"300", "200"},
                           "ORDER BY alias with LIMIT failed");
        }

        {
            SqlQueryResult result = Run(frontend,
                                        "SELECT dept, COUNT(*), SUM(salary) FROM emp GROUP BY dept ORDER BY dept;");
            Expect(result.rows.size() == 2, "Grouped COUNT/SUM returned wrong row count");
            Expect(result.rows[0].GetValue(0).ToString() == "10" &&
                       result.rows[0].GetValue(1).ToString() == "2" &&
                       result.rows[0].GetValue(2).ToString() == "300",
                   "Grouped COUNT/SUM first row mismatch");
            Expect(result.rows[1].GetValue(0).ToString() == "20" &&
                       result.rows[1].GetValue(1).ToString() == "2" &&
                       result.rows[1].GetValue(2).ToString() == "450",
                   "Grouped COUNT/SUM second row mismatch");
        }

        {
            SqlQueryResult result = Run(frontend,
                                        "SELECT dept, MIN(name), MAX(name) FROM emp GROUP BY dept ORDER BY dept;");
            Expect(result.rows.size() == 2, "Grouped MIN/MAX returned wrong row count");
            Expect(result.rows[0].GetValue(1).ToString() == "a" && result.rows[0].GetValue(2).ToString() == "b",
                   "Grouped MIN/MAX first row mismatch");
            Expect(result.rows[1].GetValue(1).ToString() == "c" && result.rows[1].GetValue(2).ToString() == "d",
                   "Grouped MIN/MAX second row mismatch");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT COUNT(*) FROM emp WHERE dept = 999;");
            Expect(result.rows.size() == 1, "COUNT(*) on empty input should return one row");
            Expect(result.rows[0].GetValue(0).ToString() == "0", "COUNT(*) on empty input should return 0");
        }

        for (int i = 5; i <= 500; i++) {
            int dept = i % 20;
            int salary = 1000 + i;
            Run(frontend, "INSERT INTO emp VALUES (" + std::to_string(i) + ", " + std::to_string(dept) + ", " +
                              std::to_string(salary) + ", 'x');");
        }

        {
            SqlQueryResult result = Run(frontend, "EXPLAIN SELECT * FROM emp WHERE id = 250;");
            std::string plan = ExplainText(result);
            Expect(plan.find("IndexScan(EQ)") != std::string::npos,
                   "Point lookup EXPLAIN did not choose equality index scan");
            Expect(plan.find("emp_id_idx") != std::string::npos,
                   "Point lookup EXPLAIN did not mention emp_id_idx");
        }

        {
            SqlQueryResult result = Run(frontend,
                                        "EXPLAIN SELECT * FROM emp WHERE dept = 5 AND salary >= 1200 ORDER BY dept, salary;");
            std::string plan = ExplainText(result);
            Expect(plan.find("IndexScan(RANGE)") != std::string::npos,
                   "Composite range EXPLAIN did not choose range index scan");
            Expect(plan.find("emp_dept_salary_idx") != std::string::npos,
                   "Composite range EXPLAIN did not mention emp_dept_salary_idx");
            Expect(plan.find("OrderBy: sort avoided") != std::string::npos,
                   "Composite ordered query should avoid explicit sort");
        }

        {
            SqlQueryResult result = Run(frontend,
                                        "EXPLAIN SELECT dept, COUNT(*) FROM emp GROUP BY dept ORDER BY dept;");
            std::string plan = ExplainText(result);
            Expect(plan.find("StreamAggregate") != std::string::npos,
                   "Grouped ordered query did not choose stream aggregate");
            Expect(plan.find("sort avoided") != std::string::npos,
                   "Grouped ordered query should avoid extra sort");
        }

        {
            SqlQueryResult result = Run(frontend,
                                        "SELECT e.name, d.dname FROM emp e JOIN dept d ON e.dept = d.id WHERE e.id = 1 ORDER BY e.name;");
            Expect(result.rows.size() == 1, "Join query returned wrong row count");
            Expect(result.rows[0].GetValue(0).ToString() == "a" && result.rows[0].GetValue(1).ToString() == "eng",
                   "Join query returned wrong values");
        }

        {
            SqlQueryResult result = Run(frontend, "EXPLAIN SELECT name FROM emp WHERE dept = 10 AND salary >= 150 AND salary <= 250;");
            std::string plan = ExplainText(result);
            Expect(plan.find("BitmapIndexScan + BitmapHeapScan") != std::string::npos,
                   "Bitmap access EXPLAIN did not mention bitmap scan");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT name FROM emp WHERE dept = 10 AND salary >= 150 AND salary <= 250 ORDER BY name;");
            ExpectVectorEq(ColumnStrings(result, 0), {"b"},
                           "Bitmap heap scan query returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "EXPLAIN SELECT dept, salary FROM emp WHERE dept = 10 AND salary <= 200 ORDER BY dept, salary;");
            std::string plan = ExplainText(result);
            Expect(plan.find("IndexOnlyScan") != std::string::npos,
                   "Index-only EXPLAIN did not mention index-only scan");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT dept, salary FROM emp WHERE dept = 10 AND salary <= 200 ORDER BY dept, salary;");
            Expect(result.rows.size() == 2, "Index-only query returned wrong row count");
            Expect(result.rows[0].GetValue(0).ToString() == "10" && result.rows[0].GetValue(1).ToString() == "100",
                   "Index-only query first row mismatch");
            Expect(result.rows[1].GetValue(0).ToString() == "10" && result.rows[1].GetValue(1).ToString() == "200",
                   "Index-only query second row mismatch");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT id FROM emp WHERE id <= 2 UNION SELECT id FROM emp WHERE id = 3 ORDER BY id;");
            ExpectVectorEq(ColumnStrings(result, 0), {"1", "2", "3"},
                           "UNION DISTINCT returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT dept FROM emp WHERE id <= 3 INTERSECT ALL SELECT dept FROM emp WHERE id >= 2 AND id <= 4 ORDER BY dept;");
            ExpectVectorEq(ColumnStrings(result, 0), {"10", "20"},
                           "INTERSECT ALL returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT dept FROM emp WHERE id <= 4 EXCEPT ALL SELECT dept FROM emp WHERE id <= 2 ORDER BY dept;");
            ExpectVectorEq(ColumnStrings(result, 0), {"20", "20"},
                           "EXCEPT ALL returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT id AS k FROM emp WHERE id <= 3 UNION ALL SELECT id FROM emp WHERE id = 4 ORDER BY k DESC LIMIT 2;");
            ExpectVectorEq(ColumnStrings(result, 0), {"4", "3"},
                           "Compound ORDER BY / LIMIT failed");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT DISTINCT dept FROM emp ORDER BY dept;");
            ExpectVectorEq(ColumnStrings(result, 0),
                           {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20"},
                           "DISTINCT returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT dept FROM emp GROUP BY dept ORDER BY dept;");
            ExpectVectorEq(ColumnStrings(result, 0),
                           {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20"},
                           "GROUP BY without aggregates returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "SELECT DISTINCT dept AS d FROM emp ORDER BY d DESC LIMIT 2;");
            ExpectVectorEq(ColumnStrings(result, 0), {"20", "19"},
                           "DISTINCT with ORDER BY/LIMIT returned wrong rows");
        }

        {
            SqlQueryResult result = Run(frontend, "EXPLAIN SELECT DISTINCT dept FROM emp ORDER BY dept;");
            std::string plan = ExplainText(result);
            Expect(plan.find("Dedup:") != std::string::npos,
                   "EXPLAIN DISTINCT did not mention dedup");
        }

        {
            SqlQueryResult result = Run(frontend, "EXPLAIN SELECT id FROM emp WHERE id <= 2 UNION SELECT id FROM emp WHERE id = 3;");
            std::string plan = ExplainText(result);
            Expect(plan.find("CompoundSetOp") != std::string::npos,
                   "Compound EXPLAIN did not identify compound set op");
            Expect(plan.find("Append + Sort + Unique") != std::string::npos,
                   "Compound EXPLAIN did not show UNION DISTINCT implementation");
        }

        std::cout << "SQL regression suite passed.\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "db_sql_regression failed: " << ex.what() << "\n";
        return 1;
    }
}
