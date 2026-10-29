#include "nstu/exam_telemetry_lifecycle.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

nstu::client::TelemetryContextIdentity make_sample_context() {
    nstu::client::TelemetryContextIdentity ctx;
    ctx.package_id = "exam-package-eng-101";
    ctx.package_digest = nstu::security::Sha256Digest{
        std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
    ctx.client_id = nstu::security::ClientId{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    ctx.session_id = nstu::exam::SessionId{
        std::byte{0x55}, std::byte{0x66}, std::byte{0x77}, std::byte{0x88}};
    ctx.candidate_id = "candidate-john-doe";
    return ctx;
}

nstu::exam::TelemetryEnable make_matching_enable(
    const nstu::client::TelemetryContextIdentity& ctx) {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = ctx.package_id;
    enable.package_digest = ctx.package_digest;
    enable.client_id = ctx.client_id;
    enable.session_id = ctx.session_id;
    enable.candidate_id = ctx.candidate_id;
    enable.server_time_unix_milliseconds = 1700000000000ULL;
    return enable;
}

void test_context_matching() {
    const auto ctx = make_sample_context();
    const auto matching = make_matching_enable(ctx);
    assert(ctx.matches(matching));

    // Package ID mismatch
    auto wrong_pkg = matching;
    wrong_pkg.package_id = "other-package";
    assert(!ctx.matches(wrong_pkg));

    // Package digest mismatch
    auto wrong_digest = matching;
    wrong_digest.package_digest[0] = std::byte{0x00};
    assert(!ctx.matches(wrong_digest));

    // Client ID mismatch
    auto wrong_client = matching;
    wrong_client.client_id[0] = std::byte{0x00};
    assert(!ctx.matches(wrong_client));

    // Session ID mismatch
    auto wrong_session = matching;
    wrong_session.session_id[0] = std::byte{0x00};
    assert(!ctx.matches(wrong_session));

    // Candidate ID mismatch
    auto wrong_cand = matching;
    wrong_cand.candidate_id = "other-candidate";
    assert(!ctx.matches(wrong_cand));
}

void test_initial_state() {
    nstu::client::ExamTelemetryLifecycle lifecycle;
    assert(lifecycle.is_disabled());
    assert(lifecycle.state() == nstu::client::TelemetryLifecycleState::disabled);
    assert(!lifecycle.can_collect());
    assert(!lifecycle.can_upload());
    assert(lifecycle.should_retain_outbox());
    assert(lifecycle.allows_replay());
    assert(lifecycle.context().empty());

    const auto ctx = make_sample_context();
    lifecycle.set_context(ctx);
    assert(lifecycle.is_disabled());
    assert(!lifecycle.can_collect());
    assert(!lifecycle.can_upload());
    assert(lifecycle.context() == ctx);
}

struct TableRow {
    nstu::client::TelemetryLifecycleState initial_state;
    nstu::client::TelemetryLifecycleEvent event;
    bool enable_matches;
    nstu::client::TelemetryLifecycleState restart_state;
    nstu::client::TelemetryLifecycleState expected_state;
    bool expected_accepted;
    bool expected_changed;
    bool expected_can_upload;
    bool expected_can_collect;
    bool expected_clear_inflight;
    bool expected_purge_outbox;
    bool expected_retain_outbox;
    bool expected_allows_replay;
    const char* description;
};

void test_table_driven_transitions() {
    using nstu::client::TelemetryLifecycleEvent;
    using nstu::client::TelemetryLifecycleState;

    const auto ctx = make_sample_context();
    const auto matching_enable = make_matching_enable(ctx);
    auto mismatch_enable = matching_enable;
    mismatch_enable.package_id = "wrong-package";

    const std::vector<TableRow> table = {
        // --- 1. ENABLE EVENT ---
        // From disabled:
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::enable, true, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::enabled, true, true, true, true, false, false, true, true,
         "disabled + matching enable -> enabled"},
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::enable, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::disabled, false, false, false, false, false, false, true, true,
         "disabled + mismatched enable -> disabled (ignored)"},

        // From enabled (duplicate/idempotent):
        {TelemetryLifecycleState::enabled, TelemetryLifecycleEvent::enable, true, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::enabled, true, false, true, true, false, false, true, true,
         "enabled + matching enable -> enabled (idempotent)"},
        {TelemetryLifecycleState::enabled, TelemetryLifecycleEvent::enable, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::enabled, false, false, true, true, false, false, true, true,
         "enabled + mismatched enable -> enabled (ignored)"},

        // From suspended (resume):
        {TelemetryLifecycleState::suspended, TelemetryLifecycleEvent::enable, true, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::enabled, true, true, true, true, false, false, true, true,
         "suspended + matching enable -> enabled (resumes)"},
        {TelemetryLifecycleState::suspended, TelemetryLifecycleEvent::enable, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::suspended, false, false, false, false, false, false, true, true,
         "suspended + mismatched enable -> suspended (ignored)"},

        // From revoked (terminal / cannot re-enable):
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::enable, true, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, false, false, false, false, false, false, false, false,
         "revoked + matching enable -> revoked (rejected, terminal)"},
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::enable, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, false, false, false, false, false, false, false, false,
         "revoked + mismatched enable -> revoked (rejected)"},

        // --- 2. DISCONNECT EVENT ---
        // From disabled:
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::disconnect, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::disabled, true, false, false, false, false, false, true, true,
         "disabled + disconnect -> disabled (idempotent)"},

        // From enabled:
        {TelemetryLifecycleState::enabled, TelemetryLifecycleEvent::disconnect, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::suspended, true, true, false, false, true, false, true, true,
         "enabled + disconnect -> suspended (clears inflight, retains outbox)"},

        // From suspended:
        {TelemetryLifecycleState::suspended, TelemetryLifecycleEvent::disconnect, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::suspended, true, false, false, false, false, false, true, true,
         "suspended + disconnect -> suspended (idempotent)"},

        // From revoked:
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::disconnect, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, false, false, false, false, false, false, false,
         "revoked + disconnect -> revoked (idempotent)"},

        // --- 3. EXPLICIT STOP EVENT ---
        // From disabled:
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::explicit_stop, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, true, false, false, false, true, false, false,
         "disabled + explicit_stop -> revoked (purges outbox)"},

        // From enabled:
        {TelemetryLifecycleState::enabled, TelemetryLifecycleEvent::explicit_stop, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, true, false, false, false, true, false, false,
         "enabled + explicit_stop -> revoked (purges outbox)"},

        // From suspended:
        {TelemetryLifecycleState::suspended, TelemetryLifecycleEvent::explicit_stop, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, true, false, false, false, true, false, false,
         "suspended + explicit_stop -> revoked (purges outbox)"},

        // From revoked:
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::explicit_stop, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, false, false, false, false, false, false, false,
         "revoked + explicit_stop -> revoked (idempotent, no second purge)"},

        // --- 4. SERVICE RESTART ORDERING ---
        // Restart after being enabled: cannot upload before matching enable; state becomes suspended
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::enabled,
         TelemetryLifecycleState::suspended, true, true, false, false, true, false, true, true,
         "restart with persisted enabled -> suspended (cannot upload before matching enable)"},

        // Restart after being suspended: stays suspended, batches retained, clear inflight
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::suspended,
         TelemetryLifecycleState::suspended, true, true, false, false, true, false, true, true,
         "restart with persisted suspended -> suspended"},

        // Restart after being revoked: stays revoked, cannot upload, forbids replay
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::revoked,
         TelemetryLifecycleState::revoked, true, true, false, false, false, false, false, false,
         "restart with persisted revoked -> revoked (terminal)"},

        // Restart after being disabled: stays disabled
        {TelemetryLifecycleState::disabled, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::disabled, true, false, false, false, false, false, true, true,
         "restart with persisted disabled -> disabled"},

        // Restart from revoked state: absorbs restart and stays revoked for all persisted values
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::enabled,
         TelemetryLifecycleState::revoked, true, false, false, false, false, false, false, false,
         "revoked + restart with persisted enabled -> revoked (absorbing)"},
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::suspended,
         TelemetryLifecycleState::revoked, true, false, false, false, false, false, false, false,
         "revoked + restart with persisted suspended -> revoked (absorbing)"},
        {TelemetryLifecycleState::revoked, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::disabled,
         TelemetryLifecycleState::revoked, true, false, false, false, false, false, false, false,
         "revoked + restart with persisted disabled -> revoked (absorbing)"},

        // Restart from enabled state: becomes suspended with clear_inflight
        {TelemetryLifecycleState::enabled, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::enabled,
         TelemetryLifecycleState::suspended, true, true, false, false, true, false, true, true,
         "enabled + restart with persisted enabled -> suspended (clears inflight)"},

        // Restart from suspended state: stays suspended, changed=false, clear_inflight=true
        {TelemetryLifecycleState::suspended, TelemetryLifecycleEvent::service_restart, false, TelemetryLifecycleState::suspended,
         TelemetryLifecycleState::suspended, true, false, false, false, true, false, true, true,
         "suspended + restart with persisted suspended -> suspended (clears inflight, unchanged)"},
    };

    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& row = table[i];
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx, row.initial_state);

        std::optional<nstu::exam::TelemetryEnable> enable_opt;
        if (row.event == TelemetryLifecycleEvent::enable) {
            enable_opt = row.enable_matches ? matching_enable : mismatch_enable;
        }

        const auto result = lifecycle.dispatch_event(
            row.event, enable_opt, row.restart_state);

        if (result.current_state != row.expected_state ||
            result.accepted != row.expected_accepted ||
            result.state_changed != row.expected_changed ||
            lifecycle.can_upload() != row.expected_can_upload ||
            lifecycle.can_collect() != row.expected_can_collect ||
            lifecycle.should_retain_outbox() != row.expected_retain_outbox ||
            lifecycle.allows_replay() != row.expected_allows_replay ||
            result.clear_inflight != row.expected_clear_inflight ||
            result.purge_outbox != row.expected_purge_outbox) {
            std::cerr << "FAIL on table row " << i << " [" << row.description << "]:\n"
                      << "  expected state: " << to_string(row.expected_state)
                      << ", got: " << to_string(result.current_state) << "\n"
                      << "  expected accepted: " << row.expected_accepted
                      << ", got: " << result.accepted << "\n"
                      << "  expected changed: " << row.expected_changed
                      << ", got: " << result.state_changed << "\n"
                      << "  expected can_upload: " << row.expected_can_upload
                      << ", got: " << lifecycle.can_upload() << "\n"
                      << "  expected can_collect: " << row.expected_can_collect
                      << ", got: " << lifecycle.can_collect() << "\n"
                      << "  expected retain_outbox: " << row.expected_retain_outbox
                      << ", got: " << lifecycle.should_retain_outbox() << "\n"
                      << "  expected allows_replay: " << row.expected_allows_replay
                      << ", got: " << lifecycle.allows_replay() << "\n"
                      << "  expected clear_inflight: " << row.expected_clear_inflight
                      << ", got: " << result.clear_inflight << "\n"
                      << "  expected purge_outbox: " << row.expected_purge_outbox
                      << ", got: " << result.purge_outbox << "\n";
            assert(false);
        }
    }
}

void test_full_lifecycle_sequence() {
    using nstu::client::TelemetryLifecycleState;

    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);

    nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
    assert(lifecycle.is_disabled());

    // 1. Initial matching enable -> enabled
    auto res = lifecycle.on_enable(enable);
    assert(res.accepted && res.state_changed);
    assert(lifecycle.is_enabled());
    assert(lifecycle.can_collect());
    assert(lifecycle.can_upload());
    assert(lifecycle.should_retain_outbox());
    assert(lifecycle.allows_replay());

    // 2. Disconnect -> suspended
    res = lifecycle.on_disconnect();
    assert(res.accepted && res.state_changed);
    assert(res.clear_inflight);
    assert(!res.purge_outbox);
    assert(lifecycle.is_suspended());
    assert(!lifecycle.can_collect());
    assert(!lifecycle.can_upload());
    assert(lifecycle.should_retain_outbox());
    assert(lifecycle.allows_replay());

    // 3. Matching re-enable -> enabled (resumes)
    res = lifecycle.on_enable(enable);
    assert(res.accepted && res.state_changed);
    assert(lifecycle.is_enabled());
    assert(lifecycle.can_collect());
    assert(lifecycle.can_upload());

    // 4. Duplicate matching enable -> idempotent
    res = lifecycle.on_enable(enable);
    assert(res.accepted && !res.state_changed);
    assert(lifecycle.is_enabled());

    // 5. Service shutdown -> suspended (retains outbox)
    res = lifecycle.on_service_shutdown();
    assert(res.accepted && res.state_changed);
    assert(!res.purge_outbox);
    assert(lifecycle.is_suspended());
    assert(lifecycle.should_retain_outbox());

    // 6. Service restart with persisted state enabled -> suspended (cannot upload before matching enable)
    res = lifecycle.on_service_restart(TelemetryLifecycleState::enabled);
    assert(res.accepted);
    assert(lifecycle.is_suspended());
    assert(!lifecycle.can_upload());

    // 7. Re-enable after restart -> resumes upload
    res = lifecycle.on_enable(enable);
    assert(res.accepted && res.state_changed);
    assert(lifecycle.is_enabled());
    assert(lifecycle.can_upload());

    // 8. Explicit stop -> permanently revoked
    res = lifecycle.on_explicit_stop();
    assert(res.accepted && res.state_changed);
    assert(res.purge_outbox);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_collect());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.should_retain_outbox());
    assert(!lifecycle.allows_replay());

    // 9. Replay or re-enable after explicit stop is permanently forbidden
    res = lifecycle.on_enable(enable);
    assert(!res.accepted);
    assert(!res.state_changed);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());
}

void test_revoked_restart_and_enable_rejected() {
    using nstu::client::TelemetryLifecycleState;
    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);

    const std::vector<TelemetryLifecycleState> persisted_states = {
        TelemetryLifecycleState::enabled,
        TelemetryLifecycleState::suspended,
        TelemetryLifecycleState::disabled,
    };

    for (const auto persisted : persisted_states) {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto stop_res = lifecycle.on_explicit_stop();
        assert(stop_res.accepted && lifecycle.is_revoked());

        const auto restart_res = lifecycle.on_service_restart(persisted);
        assert(restart_res.accepted);
        assert(!restart_res.state_changed);
        assert(!restart_res.clear_inflight);
        assert(!restart_res.purge_outbox);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.can_collect());
        assert(!lifecycle.should_retain_outbox());
        assert(!lifecycle.allows_replay());

        const auto enable_res = lifecycle.on_enable(enable);
        assert(!enable_res.accepted);
        assert(!enable_res.state_changed);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.allows_replay());
    }
}

void test_set_context_same_identity_absorbing() {
    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);
    nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
    const auto stop_res = lifecycle.on_explicit_stop();
    assert(stop_res.accepted && lifecycle.is_revoked());

    lifecycle.set_context(ctx);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());

    const auto enable_res = lifecycle.on_enable(enable);
    assert(!enable_res.accepted);
    assert(!enable_res.state_changed);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());
}

void test_different_identity_binding_and_rebound_revocation() {
    const auto ctx_a = make_sample_context();
    const auto enable_a = make_matching_enable(ctx_a);

    auto ctx_b = ctx_a;
    ctx_b.package_id = "exam-package-math-202";
    ctx_b.candidate_id = "candidate-jane-doe";
    const auto enable_b = make_matching_enable(ctx_b);

    nstu::client::ExamTelemetryLifecycle lifecycle(ctx_a);
    const auto stop_res = lifecycle.on_explicit_stop();
    assert(stop_res.accepted && lifecycle.is_revoked());

    // Bind different identity B
    lifecycle.set_context(ctx_b);
    assert(lifecycle.is_disabled());
    assert(lifecycle.context() == ctx_b);
    assert(!lifecycle.can_upload());
    assert(lifecycle.allows_replay());

    // Matching B enable accepted
    const auto enable_b_res = lifecycle.on_enable(enable_b);
    assert(enable_b_res.accepted && enable_b_res.state_changed);
    assert(lifecycle.is_enabled());
    assert(lifecycle.can_upload());

    // Switch back to revoked identity A -> state restored to revoked
    lifecycle.set_context(ctx_a);
    assert(lifecycle.is_revoked());
    assert(lifecycle.context() == ctx_a);
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());

    // Matching A enable rejected
    const auto enable_a_res = lifecycle.on_enable(enable_a);
    assert(!enable_a_res.accepted);
    assert(!enable_a_res.state_changed);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());
}

void test_out_of_range_persisted_state_fails_closed() {
    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);
    const auto bad_state = static_cast<nstu::client::TelemetryLifecycleState>(0x7f);

    // From disabled:
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        assert(lifecycle.is_disabled());
        const auto res = lifecycle.on_service_restart(bad_state);
        assert(!res.accepted);
        assert(res.state_changed);
        assert(res.clear_inflight);
        assert(!res.purge_outbox);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.allows_replay());

        const auto enable_res = lifecycle.on_enable(enable);
        assert(!enable_res.accepted);
        assert(lifecycle.is_revoked());
    }

    // From revoked:
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto stop_res = lifecycle.on_explicit_stop();
        assert(stop_res.accepted && lifecycle.is_revoked());

        const auto res = lifecycle.on_service_restart(bad_state);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(res.clear_inflight);
        assert(!res.purge_outbox);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.allows_replay());

        const auto enable_res = lifecycle.on_enable(enable);
        assert(!enable_res.accepted);
        assert(lifecycle.is_revoked());
    }
}

void test_repeated_restart() {
    using nstu::client::TelemetryLifecycleState;
    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);

    nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
    const auto enable_res = lifecycle.on_enable(enable);
    assert(enable_res.accepted && lifecycle.is_enabled());

    // First restart with persisted enabled: transitions enabled -> suspended
    const auto res1 = lifecycle.on_service_restart(TelemetryLifecycleState::enabled);
    assert(res1.accepted && res1.state_changed);
    assert(res1.clear_inflight);
    assert(lifecycle.is_suspended());

    // Second restart with persisted enabled: stays suspended with changed=false
    const auto res2 = lifecycle.on_service_restart(TelemetryLifecycleState::enabled);
    assert(res2.accepted);
    assert(!res2.state_changed);
    assert(res2.clear_inflight);
    assert(lifecycle.is_suspended());
}

void test_default_lifecycle_rejects_default_enable() {
    nstu::client::ExamTelemetryLifecycle lifecycle;
    assert(lifecycle.context().empty());
    assert(!lifecycle.context().bound());

    const nstu::exam::TelemetryEnable def_enable{};
    const auto res = lifecycle.on_enable(def_enable);
    assert(!res.accepted);
    assert(!res.state_changed);
    assert(lifecycle.is_disabled());
}

void test_partially_zero_identity_not_bound_and_rejected() {
    // Zero session_id
    {
        auto ctx = make_sample_context();
        ctx.session_id.fill(std::byte{0});
        assert(!ctx.empty());
        assert(!ctx.bound());

        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto enable = make_matching_enable(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_disabled());
    }

    // Zero client_id
    {
        auto ctx = make_sample_context();
        ctx.client_id.fill(std::byte{0});
        assert(!ctx.empty());
        assert(!ctx.bound());

        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto enable = make_matching_enable(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_disabled());
    }

    // Zero package_digest
    {
        auto ctx = make_sample_context();
        ctx.package_digest.fill(std::byte{0});
        assert(!ctx.empty());
        assert(!ctx.bound());

        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto enable = make_matching_enable(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_disabled());
    }

    // Empty package_id
    {
        auto ctx = make_sample_context();
        ctx.package_id.clear();
        assert(!ctx.bound());

        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto enable = make_matching_enable(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_disabled());
    }

    // Empty candidate_id
    {
        auto ctx = make_sample_context();
        ctx.candidate_id.clear();
        assert(!ctx.bound());

        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto enable = make_matching_enable(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_disabled());
    }
}

void test_h1_multi_revocation_sequence() {
    using nstu::client::TelemetryLifecycleState;
    const auto ctx_a = make_sample_context();
    const auto enable_a = make_matching_enable(ctx_a);

    auto ctx_b = ctx_a;
    ctx_b.package_id = "exam-package-phys-303";
    ctx_b.candidate_id = "candidate-bob-smith";
    const auto enable_b = make_matching_enable(ctx_b);

    nstu::client::ExamTelemetryLifecycle lifecycle(ctx_a);
    // Stop A
    auto stop_res = lifecycle.on_explicit_stop();
    assert(stop_res.accepted && stop_res.state_changed);
    assert(lifecycle.is_revoked());

    // Bind B (returns true)
    bool bound_b = lifecycle.set_context(ctx_b);
    assert(bound_b);
    assert(lifecycle.is_disabled());
    assert(lifecycle.context() == ctx_b);

    // Enable B
    auto enable_b_res = lifecycle.on_enable(enable_b);
    assert(enable_b_res.accepted && enable_b_res.state_changed);
    assert(lifecycle.is_enabled());

    // Stop B
    stop_res = lifecycle.on_explicit_stop();
    assert(stop_res.accepted && stop_res.state_changed);
    assert(lifecycle.is_revoked());

    // Bind A (returns true)
    bool bound_a = lifecycle.set_context(ctx_a);
    assert(bound_a);

    // Expect A revoked: on_enable(A) rejected with no state change, !can_upload(), !allows_replay()
    assert(lifecycle.is_revoked());
    assert(lifecycle.context() == ctx_a);
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());
    assert(!lifecycle.can_collect());

    auto enable_a_res = lifecycle.on_enable(enable_a);
    assert(!enable_a_res.accepted);
    assert(!enable_a_res.state_changed);
    assert(lifecycle.is_revoked());
    assert(!lifecycle.can_upload());
    assert(!lifecycle.allows_replay());

    // is_identity_revoked is true for both A and B, and revoked_identities() contains A and B
    assert(lifecycle.is_identity_revoked(ctx_a));
    assert(lifecycle.is_identity_revoked(ctx_b));

    const auto revoked = lifecycle.revoked_identities();
    assert(revoked.size() >= 2);
    bool found_a = false;
    bool found_b = false;
    for (const auto& item : revoked) {
        if (item == ctx_a) found_a = true;
        if (item == ctx_b) found_b = true;
    }
    assert(found_a);
    assert(found_b);
}

void test_constructor_initial_state_validation() {
    using nstu::client::TelemetryLifecycleState;
    const nstu::client::TelemetryContextIdentity unbound_ctx{};
    const auto bound_ctx = make_sample_context();
    const auto bound_enable = make_matching_enable(bound_ctx);
    const auto bad_state = static_cast<TelemetryLifecycleState>(0x7f);

    // Default or unbound context with enabled: disabled, !can_upload(), !can_collect()
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(unbound_ctx, TelemetryLifecycleState::enabled);
        assert(lifecycle.is_disabled());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.can_collect());
    }

    // Unbound context with suspended: disabled
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(unbound_ctx, TelemetryLifecycleState::suspended);
        assert(lifecycle.is_disabled());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.can_collect());
    }

    // Out-of-range state with a bound context: revoked, matching enable rejected
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(bound_ctx, bad_state);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.can_collect());
        const auto res = lifecycle.on_enable(bound_enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_revoked());
    }

    // Out-of-range state with an unbound context: revoked, matching enable rejected
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(unbound_ctx, bad_state);
        assert(lifecycle.is_revoked());
        assert(!lifecycle.can_upload());
        assert(!lifecycle.can_collect());
        const auto res = lifecycle.on_enable(bound_enable);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(lifecycle.is_revoked());
    }
}

void test_same_identity_rebind() {
    using nstu::client::TelemetryLifecycleState;
    const auto ctx = make_sample_context();
    const auto enable = make_matching_enable(ctx);

    // Enabled, then set_context(same): returns true, still enabled, can_upload()
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        const auto res = lifecycle.on_enable(enable);
        assert(res.accepted && lifecycle.is_enabled());
        assert(lifecycle.can_upload());

        const bool rebind_res = lifecycle.set_context(ctx);
        assert(rebind_res);
        assert(lifecycle.is_enabled());
        assert(lifecycle.can_upload());
        assert(lifecycle.state() == TelemetryLifecycleState::enabled);
    }

    // Suspended, then set_context(same): returns true, stays suspended
    {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx);
        (void)lifecycle.on_enable(enable);
        (void)lifecycle.on_disconnect();
        assert(lifecycle.is_suspended());

        const bool rebind_res = lifecycle.set_context(ctx);
        assert(rebind_res);
        assert(lifecycle.is_suspended());
        assert(lifecycle.state() == TelemetryLifecycleState::suspended);
        assert(!lifecycle.can_upload());
    }
}

void test_dispatch_event_enable_nullopt() {
    using nstu::client::TelemetryLifecycleEvent;
    using nstu::client::TelemetryLifecycleState;
    const auto ctx = make_sample_context();

    const std::vector<TelemetryLifecycleState> initial_states = {
        TelemetryLifecycleState::disabled,
        TelemetryLifecycleState::enabled,
        TelemetryLifecycleState::suspended,
        TelemetryLifecycleState::revoked,
    };

    for (const auto init_state : initial_states) {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx, init_state);
        const auto res = lifecycle.dispatch_event(
            TelemetryLifecycleEvent::enable, std::nullopt);
        assert(!res.accepted);
        assert(!res.state_changed);
        assert(res.previous_state == init_state);
        assert(res.current_state == init_state);
        assert(!res.clear_inflight);
        assert(!res.purge_outbox);
        assert(lifecycle.state() == init_state);
    }
}

void test_on_service_shutdown_table() {
    using nstu::client::TelemetryLifecycleState;
    const auto ctx = make_sample_context();

    struct ShutdownRow {
        TelemetryLifecycleState initial_state;
        TelemetryLifecycleState expected_state;
        bool expected_accepted;
        bool expected_changed;
        bool expected_clear_inflight;
        bool expected_purge_outbox;
        bool expected_retain_outbox;
        bool expected_allows_replay;
    };

    const std::vector<ShutdownRow> rows = {
        // From disabled: no change, clear_inflight=false
        {TelemetryLifecycleState::disabled, TelemetryLifecycleState::disabled, true, false, false, false, true, true},
        // From enabled: suspended, clear_inflight=true
        {TelemetryLifecycleState::enabled, TelemetryLifecycleState::suspended, true, true, true, false, true, true},
        // From suspended: no change, clear_inflight=false
        {TelemetryLifecycleState::suspended, TelemetryLifecycleState::suspended, true, false, false, false, true, true},
        // From revoked: no change, clear_inflight=false, retain=false, replay=false
        {TelemetryLifecycleState::revoked, TelemetryLifecycleState::revoked, true, false, false, false, false, false},
    };

    for (const auto& row : rows) {
        nstu::client::ExamTelemetryLifecycle lifecycle(ctx, row.initial_state);
        const auto res = lifecycle.on_service_shutdown();
        assert(res.accepted == row.expected_accepted);
        assert(res.state_changed == row.expected_changed);
        assert(res.previous_state == row.initial_state);
        assert(res.current_state == row.expected_state);
        assert(res.clear_inflight == row.expected_clear_inflight);
        assert(res.purge_outbox == row.expected_purge_outbox);
        assert(lifecycle.state() == row.expected_state);
        assert(lifecycle.should_retain_outbox() == row.expected_retain_outbox);
        assert(lifecycle.allows_replay() == row.expected_allows_replay);
    }
}

void test_full_revoked_set_fails_closed() {
    using nstu::client::kMaximumRevokedTelemetryIdentities;

    // 1. Build kMaximumRevokedTelemetryIdentities + 2 distinct bound identities
    std::vector<nstu::client::TelemetryContextIdentity> identities;
    identities.reserve(kMaximumRevokedTelemetryIdentities + 2);
    for (std::size_t i = 0; i < kMaximumRevokedTelemetryIdentities + 2; ++i) {
        auto c = make_sample_context();
        c.candidate_id = "candidate-" + std::to_string(i);
        identities.push_back(std::move(c));
    }

    // 2. Repeat stop current, then bind next, until
    // revoked_identities().size() == kMaximumRevokedTelemetryIdentities,
    // with current identity revoked and not yet archived.
    nstu::client::ExamTelemetryLifecycle lifecycle(identities[0]);

    for (std::size_t i = 0; i < kMaximumRevokedTelemetryIdentities; ++i) {
        const auto stop_res = lifecycle.on_explicit_stop();
        assert(stop_res.accepted);
        assert(lifecycle.is_revoked());
        // Bind next identity (i + 1)
        const bool bind_res = lifecycle.set_context(identities[i + 1]);
        assert(bind_res);
        assert(lifecycle.is_disabled());
        assert(lifecycle.context() == identities[i + 1]);
    }

    assert(lifecycle.revoked_identities().size() == kMaximumRevokedTelemetryIdentities);
    assert(lifecycle.context() == identities[kMaximumRevokedTelemetryIdentities]);

    // 3. Stop the current identity
    const auto stop_curr = lifecycle.on_explicit_stop();
    assert(stop_curr.accepted);
    assert(lifecycle.is_revoked());

    const auto current_ctx = lifecycle.context();
    const auto initial_archive_size = lifecycle.revoked_identities().size();
    assert(initial_archive_size == kMaximumRevokedTelemetryIdentities);

    // 4. Binding a fresh identity returns false; context and state unchanged, archive size unchanged
    const auto fresh_id = identities[kMaximumRevokedTelemetryIdentities + 1];
    const bool fresh_bind_res = lifecycle.set_context(fresh_id);
    assert(!fresh_bind_res);
    assert(lifecycle.context() == current_ctx);
    assert(lifecycle.is_revoked());
    assert(lifecycle.revoked_identities().size() == initial_archive_size);

    // 5. Binding an archived identity also returns false, because it would drop the current revocation
    const auto archived_id = identities[0];
    const bool archived_bind_res = lifecycle.set_context(archived_id);
    assert(!archived_bind_res);
    assert(lifecycle.context() == current_ctx);
    assert(lifecycle.is_revoked());
    assert(lifecycle.revoked_identities().size() == initial_archive_size);

    // 6. Every archived identity still reports is_identity_revoked
    for (std::size_t i = 0; i < kMaximumRevokedTelemetryIdentities; ++i) {
        assert(lifecycle.is_identity_revoked(identities[i]));
    }
    // And current identity also reports is_identity_revoked
    assert(lifecycle.is_identity_revoked(current_ctx));
}

} // namespace

int main() {
    std::cout << "Running exam_telemetry_lifecycle_tests...\n";
    test_context_matching();
    test_initial_state();
    test_table_driven_transitions();
    test_full_lifecycle_sequence();
    test_revoked_restart_and_enable_rejected();
    test_set_context_same_identity_absorbing();
    test_different_identity_binding_and_rebound_revocation();
    test_out_of_range_persisted_state_fails_closed();
    test_repeated_restart();
    test_default_lifecycle_rejects_default_enable();
    test_partially_zero_identity_not_bound_and_rejected();
    test_h1_multi_revocation_sequence();
    test_constructor_initial_state_validation();
    test_same_identity_rebind();
    test_dispatch_event_enable_nullopt();
    test_on_service_shutdown_table();
    test_full_revoked_set_fails_closed();
    std::cout << "All exam_telemetry_lifecycle_tests passed successfully.\n";
    return 0;
}
