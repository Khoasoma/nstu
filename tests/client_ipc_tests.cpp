#include "nstu/named_pipe.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <string>
#include <thread>

int main() {
    const std::wstring pipe_name = L"\\\\.\\pipe\\nstu-test-" +
                                   std::to_wstring(GetCurrentProcessId());
    nstu::client::NamedPipe server;
    std::string error;
    assert(server.create_server(pipe_name, &error));

    std::thread server_thread([&server] {
        std::string thread_error;
        assert(server.wait_for_client(&thread_error));
        std::array<std::byte, 1> command{};
        assert(server.read(command, &thread_error) == 1);
        assert(command[0] == std::byte{0x01});
        const std::array<std::byte, 1> response{std::byte{0x02}};
        assert(server.write(response, &thread_error) == 1);
    });

    nstu::client::NamedPipe client;
    assert(client.connect_client(pipe_name, 2000, &error));
    const std::array<std::byte, 1> command{std::byte{0x01}};
    assert(client.write(command, &error) == 1);
    std::array<std::byte, 1> response{};
    assert(client.read(response, &error) == 1);
    assert(response[0] == std::byte{0x02});
    server_thread.join();
    return 0;
}
