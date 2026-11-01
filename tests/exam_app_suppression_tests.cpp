#include "nstu/exam_app_suppression.hpp"

#include <cassert>
#include <string>
#include <vector>

int main() {
    using namespace nstu::client;

    // Disarmed: nothing is ever suppressed.
    {
        ExamAppSuppression suppression({"game.exe"});
        assert(!suppression.active());
        assert(!suppression.should_suppress("game.exe"));
        assert(!suppression.should_suppress("notepad.exe"));
    }

    // Armed: non-allowlisted, non-protected apps are suppressed; an empty image
    // name never is; disarming stops suppression at once.
    {
        ExamAppSuppression suppression({});
        suppression.arm();
        assert(suppression.active());
        assert(suppression.should_suppress("game.exe"));
        assert(suppression.should_suppress("Discord.exe"));
        assert(!suppression.should_suppress(""));
        suppression.disarm();
        assert(!suppression.should_suppress("game.exe"));
    }

    // Protected processes are NEVER suppressed, even armed with an empty exam
    // allowlist -- killing any of these would crash the box, lock out the
    // desktop, or take down NSTU / the exam kiosk itself.
    {
        ExamAppSuppression suppression({});
        suppression.arm();
        for (const char* keep :
             {"csrss.exe", "winlogon.exe", "svchost.exe", "lsass.exe",
              "explorer.exe", "dwm.exe", "nstu-agent.exe", "nstu-service.exe",
              "nstu-server.exe", "msedgewebview2.exe"}) {
            assert(ExamAppSuppression::is_protected(keep));
            assert(!suppression.should_suppress(keep));
        }
    }

    // Case-insensitive: protected and allowlisted names match in any case.
    {
        ExamAppSuppression suppression({"Calc.EXE"});
        suppression.arm();
        assert(!suppression.should_suppress("CSRSS.EXE"));  // protected
        assert(!suppression.should_suppress("calc.exe"));   // allowlisted
        assert(suppression.should_suppress("Cheat.exe"));   // neither
    }

    // The exam's own allowlist spares exactly the named app, nothing else.
    {
        ExamAppSuppression suppression({"safeexam.exe"});
        suppression.arm();
        assert(!suppression.should_suppress("safeexam.exe"));
        assert(suppression.should_suppress("chrome.exe"));
    }

    // is_protected is a static floor, independent of arm state and allowlist.
    assert(ExamAppSuppression::is_protected("winlogon.exe"));
    assert(!ExamAppSuppression::is_protected("game.exe"));

    // --- ExamAppSuppressionExecutor Tests with Injected Seams ---

    struct AuditRecord {
        std::uint32_t pid = 0;
        std::string image;
        std::uint32_t session = 0;
    };

    std::vector<ProcessSnapshotEntry> fake_processes{
        // Session 0 processes (system services, drivers) - MUST NEVER BE TOUCHED
        ProcessSnapshotEntry{.pid = 4, .session_id = 0, .image_name = "System"},
        ProcessSnapshotEntry{.pid = 600, .session_id = 0, .image_name = "services.exe"},
        ProcessSnapshotEntry{.pid = 601, .session_id = 0, .image_name = "svchost.exe"},
        ProcessSnapshotEntry{.pid = 602, .session_id = 0, .image_name = "cheat_daemon.exe"},

        // Session 1 processes (active student session)
        // Core/protected floor - MUST NEVER BE TERMINATED
        ProcessSnapshotEntry{.pid = 700, .session_id = 1, .image_name = "csrss.exe"},
        ProcessSnapshotEntry{.pid = 701, .session_id = 1, .image_name = "explorer.exe"},
        ProcessSnapshotEntry{.pid = 702, .session_id = 1, .image_name = "msedgewebview2.exe"},
        ProcessSnapshotEntry{.pid = 703, .session_id = 1, .image_name = "nstu-agent.exe"},
        ProcessSnapshotEntry{.pid = 704, .session_id = 1, .image_name = "nstu-service.exe"},
        ProcessSnapshotEntry{.pid = 705, .session_id = 1, .image_name = "dwm.exe"},
        // Manifest-allowlisted application
        ProcessSnapshotEntry{.pid = 710, .session_id = 1, .image_name = "allowed_calc.exe"},
        // Prohibited applications - MUST BE TERMINATED
        ProcessSnapshotEntry{.pid = 1001, .session_id = 1, .image_name = "game.exe"},
        ProcessSnapshotEntry{.pid = 1002, .session_id = 1, .image_name = "Discord.exe"},
        ProcessSnapshotEntry{.pid = 1003, .session_id = 1, .image_name = "CheatEngine.exe"},

        // Session 2 processes (different user session) - MUST NOT BE TOUCHED WHEN SCANNING SESSION 1
        ProcessSnapshotEntry{.pid = 2001, .session_id = 2, .image_name = "game.exe"},
    };

    // 1. Disarmed executor: scans terminate nothing
    {
        std::vector<std::uint32_t> terminated_pids;
        std::vector<AuditRecord> audit_records;

        ExamAppSuppressionExecutor executor(
            [&fake_processes]() { return fake_processes; },
            [&terminated_pids](std::uint32_t pid, std::string_view) {
                terminated_pids.push_back(pid);
                return true;
            },
            [&audit_records](std::uint32_t pid, std::string_view image, std::uint32_t session) {
                audit_records.push_back({pid, std::string(image), session});
            });

        assert(!executor.active());
        const auto suppressed = executor.scan(1);
        assert(suppressed == 0);
        assert(terminated_pids.empty());
        assert(audit_records.empty());
        assert(executor.total_suppressions() == 0);
    }

    // 2. Armed executor with target session 0: refuses unconditionally
    {
        std::vector<std::uint32_t> terminated_pids;
        ExamAppSuppressionExecutor executor(
            [&fake_processes]() { return fake_processes; },
            [&terminated_pids](std::uint32_t pid, std::string_view) {
                terminated_pids.push_back(pid);
                return true;
            });

        executor.arm();
        assert(executor.active());
        assert(executor.scan(0) == 0);
        assert(terminated_pids.empty());
    }

    // 3. Armed executor with student session 1: terminates prohibited apps, spares protected
    {
        std::vector<std::uint32_t> terminated_pids;
        std::vector<AuditRecord> audit_records;

        ExamAppSuppressionExecutor executor(
            [&fake_processes]() { return fake_processes; },
            [&terminated_pids](std::uint32_t pid, std::string_view) {
                terminated_pids.push_back(pid);
                return true;
            },
            [&audit_records](std::uint32_t pid, std::string_view image, std::uint32_t session) {
                audit_records.push_back({pid, std::string(image), session});
            });

        executor.arm({"allowed_calc.exe"});
        assert(executor.active());

        const auto suppressed = executor.scan(1);
        assert(suppressed == 3);
        assert(executor.total_suppressions() == 3);

        // Terminated: game.exe (1001), Discord.exe (1002), CheatEngine.exe (1003)
        assert(terminated_pids.size() == 3);
        assert(terminated_pids[0] == 1001);
        assert(terminated_pids[1] == 1002);
        assert(terminated_pids[2] == 1003);

        // Audited
        assert(audit_records.size() == 3);
        assert(audit_records[0].pid == 1001);
        assert(audit_records[0].image == "game.exe");
        assert(audit_records[0].session == 1);
        assert(audit_records[1].pid == 1002);
        assert(audit_records[1].image == "Discord.exe");
        assert(audit_records[2].pid == 1003);
        assert(audit_records[2].image == "CheatEngine.exe");

        // Disarm stops immediately
        executor.disarm();
        assert(!executor.active());
        assert(executor.scan(1) == 0);
    }

    // 4. Default native process enumerator: callable safely without crash
    {
        const auto procs = default_process_enumerator();
        (void)procs; // on Windows returns non-empty list of running processes
    }

    return 0;
}
