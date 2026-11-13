#include "nstu/programming_broker.hpp"

#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

int main() {
    using namespace nstu::server;
    using namespace nstu::exam::programming;
    const ProgrammingBrokerConfig config{"https://judge.example.test:8443", std::string(32, 's')};
    assert(validate_programming_broker_config(config));
    assert(validate_programming_broker_config({"http://127.0.0.1:8080", std::string(32, 's')}));
    for (const std::string& url : {std::string{"http://judge.example.test"}, std::string{"https://user:pass@judge.test"},
                                   std::string{"https://judge.test/private"}, std::string{"file:///judge"},
                                   std::string{"https://judge.test?token=secret"}}) {
        assert(!validate_programming_broker_config({url, std::string(32, 's')}));
    }
    assert(!validate_programming_broker_config({config.url, "short"}));
    Request request;
    request.context.package_id = "cs-package";
    request.context.candidate_id = "candidate-01";
    request.context.package_digest[0] = std::byte{1};
    request.context.client_id[0] = std::byte{2};
    request.context.session_id[0] = std::byte{3};
    request.operation = Operation::draft;
    request.request_id = "draft-01";
    request.problem_id = "sum";
    request.problem_revision = 2;
    request.language = "cpp20";
    request.draft_revision = 1;
    request.source = "int main() {}";
    auto cancel = request;
    cancel.operation = Operation::cancel;
    cancel.request_id = "cancel-01";
    cancel.draft_revision = 0;
    cancel.source.clear();
    cancel.target_run_id = "owned-run-01";
    auto ready = request;
    ready.operation = Operation::ready;
    ready.request_id = "ready-01";
    ready.problem_id.clear();
    ready.problem_revision = 1;
    ready.language.clear();
    ready.draft_revision = 0;
    ready.source.clear();
    std::mutex mutex;
    std::condition_variable changed;
    unsigned running = 0;
    unsigned completed = 0;
    unsigned maximum_running = 0;
    bool release = false;
    bool cancelled = false;
    ProgrammingBroker broker(config, [&](std::string_view route, std::string_view body) -> std::optional<std::string> {
        assert(route == "/v1/programming");
        assert(body.find("candidate-01") != std::string_view::npos);
        if (body.find("\"operation\":\"cancel\"") != std::string_view::npos) {
            return unavailable_response_json(cancel, "runner_unavailable");
        }
        std::unique_lock lock(mutex);
        ++running;
        maximum_running = std::max(maximum_running, running);
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        --running;
        return unavailable_response_json(request, "runner_unavailable");
    });
    const auto completion = [&](std::string json) {
        assert(parse_response_json(json, &request));
        std::scoped_lock lock(mutex);
        ++completed;
        changed.notify_all();
    };
    assert(broker.submit(request, completion));
    assert(broker.submit(request, completion));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return running == 2; }));
    }
    assert(broker.submit(cancel, [&](std::string json) {
        assert(parse_response_json(json, &cancel));
        std::scoped_lock lock(mutex);
        cancelled = true;
        changed.notify_all();
    }));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return cancelled; }));
        assert(running == 2);
    }
    for (unsigned index = 0; index < 64; ++index) assert(broker.submit(request, completion));
    assert(!broker.submit(request, completion));
    {
        std::scoped_lock lock(mutex);
        release = true;
        changed.notify_all();
    }
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(5), [&] { return completed == 66; }));
        assert(maximum_running == 2);
    }
    broker.stop();
    assert(!broker.submit(request, completion));
    auto recovery = request;
    recovery.operation = Operation::draft;
    recovery.problem_id = "sum";
    recovery.problem_revision = 2;
    recovery.language = "cpp20";
    recovery.draft_revision = 1;
    recovery.source = "int main() {}";
    unsigned routes = 0;
    bool recovered = false;
    ProgrammingBroker replay(config, [&](std::string_view route, std::string_view body) -> std::optional<std::string> {
        if (route == "/v1/validate-context") {
            assert(routes == 0);
            ++routes;
            auto known = context_json(recovery.context);
            known.pop_back();
            return known + ",\"programmingVersion\":1,\"status\":\"known\",\"serverTime\":1700000000000}";
        }
        assert(route == "/v1/programming" && routes == 1);
        assert(body.find("\"recoveryOnly\":true") != std::string_view::npos);
        ++routes;
        return unavailable_response_json(recovery, "runner_unavailable");
    });
    assert(!replay.submit(ready, [](auto) {}, true));
    assert(replay.submit(recovery, [&](std::string json) {
        assert(parse_response_json(json, &recovery));
        std::scoped_lock lock(mutex);
        recovered = true;
        changed.notify_all();
    }, true));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return recovered; }));
    }
    replay.stop();
    assert(routes == 2);
    for (const std::string_view status : {"unknown", "known"}) {
        bool returned = false;
        unsigned calls = 0;
        ProgrammingBroker rejected(config, [&](std::string_view route, std::string_view) -> std::optional<std::string> {
            assert(route == "/v1/validate-context");
            ++calls;
            auto invalid = context_json(recovery.context);
            invalid.pop_back();
            return invalid + ",\"programmingVersion\":1,\"status\":\"" + std::string(status) +
                "\",\"serverTime\":1700000000000,\"privateResult\":100}";
        });
        assert(rejected.submit(recovery, [&](std::string json) {
            const auto response = parse_response_json(json, &recovery);
            assert(response && response->status == "unavailable");
            std::scoped_lock lock(mutex);
            returned = true;
            changed.notify_all();
        }, true));
        {
            std::unique_lock lock(mutex);
            assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return returned; }));
        }
        rejected.stop();
        assert(calls == 1);
    }
    return 0;
}
