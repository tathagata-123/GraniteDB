#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace {

int NormalizeExitStatus(int status) {
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return status;
#endif
}

int RunCommand(const std::string &command) {
    int rc = std::system(command.c_str());
    return NormalizeExitStatus(rc);
}

std::string Quote(const std::string &s) {
    return "'" + s + "'";
}

void ExpectExit(const std::string &command, int expected_exit, const std::string &label) {
    int rc = RunCommand(command);
    if (rc != expected_exit) {
        throw std::runtime_error(label + " failed: expected exit " + std::to_string(expected_exit) +
                                 ", got " + std::to_string(rc));
    }
}


std::filesystem::path WalPath(const std::filesystem::path &run_dir) {
    return run_dir / "wal.log";
}

std::uint64_t ReadMasterCheckpointLSN(const std::filesystem::path &wal_path) {
    std::ifstream in(wal_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("failed to open WAL file for master read");
    }
    std::uint64_t master = 0;
    in.read(reinterpret_cast<char *>(&master), sizeof(master));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(master))) {
        throw std::runtime_error("failed to read WAL master checkpoint record");
    }
    return master;
}

void AppendInvalidWalTail(const std::filesystem::path &wal_path) {
    std::ofstream out(wal_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        throw std::runtime_error("failed to open WAL file for tail corruption");
    }
    const char junk[] = {'B', 'A', 'D', 'T', 'A', 'I', 'L'};
    out.write(junk, static_cast<std::streamsize>(sizeof(junk)));
    out.flush();
}

void TruncateWal(const std::filesystem::path &wal_path, std::uint64_t size) {
    std::filesystem::resize_file(wal_path, static_cast<std::uintmax_t>(size));
}


}  // namespace

int main(int argc, char **argv) {
    try {
        std::filesystem::path exe_dir = std::filesystem::absolute(argv[0]).parent_path();
        std::filesystem::path demo = exe_dir / "db_crash_demo";
        if (!std::filesystem::exists(demo)) {
            throw std::runtime_error("db_crash_demo executable not found next to db_recovery_regression");
        }

        std::string base_dir = (argc >= 2) ? argv[1] : "./recovery_regression_runs";
        std::filesystem::create_directories(base_dir);

        const std::vector<std::string> writer_crash_points = {
            "BEFORE_WAL_APPEND",
            "AFTER_WAL_APPEND",
            "BEFORE_WAL_FLUSH",
            "AFTER_WAL_FLUSH",
            "BEFORE_PAGE_FLUSH",
            "AFTER_PAGE_FLUSH"
        };

        for (const std::string &point : writer_crash_points) {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / point;
            std::filesystem::remove_all(run_dir);

            std::string fresh = "SIMPLEDB_CRASH_POINT=" + point + " " + Quote(demo.string()) +
                                " fresh " + Quote(run_dir.string()) + " 160";
            ExpectExit(fresh, 88, "writer crash run " + point);

            std::string recover = Quote(demo.string()) + " recover " + Quote(run_dir.string());
            ExpectExit(recover, 0, "recovery after writer crash " + point);
            std::cout << "PASS writer crash point: " << point << "\n";
        }

        {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "redo_restart";
            std::filesystem::remove_all(run_dir);
            std::string fresh = "SIMPLEDB_CRASH_POINT=BEFORE_PAGE_FLUSH " + Quote(demo.string()) +
                                " fresh " + Quote(run_dir.string()) + " 160";
            ExpectExit(fresh, 88, "setup for redo restart regression");

            std::string recover_crash = "SIMPLEDB_CRASH_POINT=BEFORE_REDO_APPLY " + Quote(demo.string()) +
                                        " recover " + Quote(run_dir.string());
            ExpectExit(recover_crash, 88, "crash during redo regression");

            std::string recover_again = Quote(demo.string()) + " recover " + Quote(run_dir.string());
            ExpectExit(recover_again, 0, "second recovery after redo crash");
            std::cout << "PASS restart during redo\n";
        }

        {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "undo_restart";
            std::filesystem::remove_all(run_dir);
            std::string fresh = "SIMPLEDB_CRASH_POINT=BEFORE_WAL_FLUSH " + Quote(demo.string()) +
                                " fresh " + Quote(run_dir.string()) + " 160";
            ExpectExit(fresh, 88, "setup for undo restart regression");

            std::string recover_crash = "SIMPLEDB_CRASH_POINT=BEFORE_UNDO_APPLY " + Quote(demo.string()) +
                                        " recover " + Quote(run_dir.string());
            ExpectExit(recover_crash, 88, "crash during undo regression");

            std::string recover_again = Quote(demo.string()) + " recover " + Quote(run_dir.string());
            ExpectExit(recover_again, 0, "second recovery after undo crash");
            std::cout << "PASS restart during undo\n";
        }

        {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_new_page_checkpoint";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_new_page_setup " + Quote(run_dir.string());
            ExpectExit(setup, 88, "setup exact new-page checkpoint regression");

            std::string recover = Quote(demo.string()) + " exact_new_page_recover " + Quote(run_dir.string());
            ExpectExit(recover, 0, "recover exact new-page checkpoint regression");
            std::cout << "PASS exact new-page checkpoint recovery\n";
        }

        {
            constexpr int kSplitRows = 260;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_split_checkpoint";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_split_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSplitRows);
            ExpectExit(setup, 88, "setup exact split checkpoint regression");

            std::string recover = Quote(demo.string()) + " exact_split_recover " + Quote(run_dir.string()) +
                                  " " + std::to_string(kSplitRows);
            ExpectExit(recover, 0, "recover exact split checkpoint regression");
            std::cout << "PASS exact split recovery\n";
        }

        {
            constexpr int kSplitRows = 260;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_split_redo_restart";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_split_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSplitRows);
            ExpectExit(setup, 88, "setup exact split redo restart regression");

            std::string recover_crash = "SIMPLEDB_CRASH_POINT=BEFORE_REDO_APPLY " + Quote(demo.string()) +
                                        " exact_split_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSplitRows);
            ExpectExit(recover_crash, 88, "crash during exact split redo recovery");

            std::string recover_again = Quote(demo.string()) + " exact_split_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSplitRows);
            ExpectExit(recover_again, 0, "second exact split recovery after redo crash");
            std::cout << "PASS exact restart during redo\n";
        }

        {
            constexpr int kSeedRows = 120;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_loser_recovery";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_loser_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSeedRows);
            ExpectExit(setup, 88, "setup exact loser regression");

            std::string recover = Quote(demo.string()) + " exact_loser_recover " + Quote(run_dir.string()) +
                                  " " + std::to_string(kSeedRows);
            ExpectExit(recover, 0, "recover exact loser regression");
            std::cout << "PASS exact loser recovery\n";
        }

        {
            constexpr int kSeedRows = 120;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_loser_undo_restart";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_loser_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSeedRows);
            ExpectExit(setup, 88, "setup exact loser undo restart regression");

            std::string recover_crash = "SIMPLEDB_CRASH_POINT=BEFORE_UNDO_APPLY " + Quote(demo.string()) +
                                        " exact_loser_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSeedRows);
            ExpectExit(recover_crash, 88, "crash during exact loser undo recovery");

            std::string recover_again = Quote(demo.string()) + " exact_loser_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSeedRows);
            ExpectExit(recover_again, 0, "second exact loser recovery after undo crash");
            std::cout << "PASS exact restart during undo\n";
        }

        {
            constexpr int kSeedRows = 1200;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_structural_loser_recovery";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_structural_loser_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSeedRows);
            ExpectExit(setup, 88, "setup exact structural loser regression");

            std::string recover = Quote(demo.string()) + " exact_structural_loser_recover " + Quote(run_dir.string()) +
                                  " " + std::to_string(kSeedRows);
            ExpectExit(recover, 0, "recover exact structural loser regression");
            std::cout << "PASS exact structural loser recovery\n";
        }

        {
            constexpr int kSeedRows = 1200;
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "exact_structural_loser_undo_restart";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_structural_loser_setup " + Quote(run_dir.string()) +
                                " " + std::to_string(kSeedRows);
            ExpectExit(setup, 88, "setup exact structural loser undo restart regression");

            std::string recover_crash = "SIMPLEDB_CRASH_POINT=BEFORE_UNDO_APPLY " + Quote(demo.string()) +
                                        " exact_structural_loser_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSeedRows);
            ExpectExit(recover_crash, 88, "crash during exact structural loser undo recovery");

            std::string recover_again = Quote(demo.string()) + " exact_structural_loser_recover " + Quote(run_dir.string()) +
                                        " " + std::to_string(kSeedRows);
            ExpectExit(recover_again, 0, "second exact structural loser recovery after undo crash");
            std::cout << "PASS exact structural restart during undo\n";
        }


        {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "wal_tail_truncation";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_new_page_setup " + Quote(run_dir.string());
            ExpectExit(setup, 88, "setup WAL tail truncation regression");

            std::filesystem::path wal = WalPath(run_dir);
            std::uintmax_t size_before = std::filesystem::file_size(wal);
            AppendInvalidWalTail(wal);
            std::uintmax_t size_after_corrupt = std::filesystem::file_size(wal);
            if (size_after_corrupt <= size_before) {
                throw std::runtime_error("failed to append invalid WAL tail in regression");
            }

            std::string recover = Quote(demo.string()) + " exact_new_page_recover " + Quote(run_dir.string());
            ExpectExit(recover, 0, "recover after invalid WAL tail regression");

            std::uintmax_t size_after_recover = std::filesystem::file_size(wal);
            if (size_after_recover >= size_after_corrupt) {
                throw std::runtime_error("WAL tail was not truncated on startup");
            }
            std::cout << "PASS invalid WAL tail truncation\n";
        }

        {
            std::filesystem::path run_dir = std::filesystem::path(base_dir) / "wal_master_repair_after_truncate";
            std::filesystem::remove_all(run_dir);
            std::string setup = Quote(demo.string()) + " exact_new_page_setup " + Quote(run_dir.string());
            ExpectExit(setup, 88, "setup WAL master repair regression");

            std::filesystem::path wal = WalPath(run_dir);
            std::uint64_t master = ReadMasterCheckpointLSN(wal);
            if (master == 0) {
                throw std::runtime_error("expected non-zero master checkpoint LSN before truncation");
            }
            TruncateWal(wal, master);

            std::string recover = Quote(demo.string()) + " exact_new_page_recover " + Quote(run_dir.string());
            ExpectExit(recover, 0, "recover after checkpoint truncation regression");

            if (ReadMasterCheckpointLSN(wal) != 0) {
                throw std::runtime_error("WAL master checkpoint record was not repaired after truncation");
            }
            std::cout << "PASS WAL master checkpoint repair after truncation\n";
        }

        std::cout << "All recovery regressions passed.\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "db_recovery_regression failed: " << ex.what() << "\n";
        return 1;
    }
}
