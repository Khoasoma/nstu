#include "exam_operator_action.hpp"

#include <cassert>
#include <future>
#include <stdexcept>

int main() {
    nstu::ui::ExamOperatorAction action;
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    assert(action.start([&] { entered.set_value(); released.wait(); return std::string{"received"}; }));
    entered.get_future().wait();
    assert(action.busy());
    assert(!action.poll());
    assert(!action.start([] { return std::string{"must not run"}; }));
    release.set_value();
    action.wait();
    const auto result = action.poll();
    assert(result && *result == "received" && !action.busy());
    assert(action.start([]() -> std::string { throw std::runtime_error("private detail"); }));
    action.wait();
    const auto failed = action.poll();
    assert(failed && *failed == "Exam operation failed; retry or ask for assistance.");
    assert(!action.busy() && !action.poll());
}
