#include "nstu/iocp_dispatcher.hpp"

#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nstu::net {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string ipv4_source(SOCKET socket) {
    sockaddr_in address{};
    int address_bytes = sizeof(address);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&address),
                    &address_bytes) == SOCKET_ERROR) {
        return "unknown";
    }
    char text[INET_ADDRSTRLEN]{};
    return InetNtopA(AF_INET, &address.sin_addr, text,
                     static_cast<DWORD>(std::size(text))) != nullptr
        ? std::string(text)
        : std::string("unknown");
}

std::int64_t steady_milliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

class IocpDispatcher::Impl {
public:
    explicit Impl() = default;
    ~Impl() { stop(); }

    struct Connection {
        ConnectionId id = 0;
        std::string source;
        TcpSocket socket;
        protocol::TcpFrameParser parser;
        std::mutex send_mutex;
        std::deque<std::vector<std::byte>> send_queue;
        std::size_t pending_send_bytes = 0;
        bool send_active = false;
        std::atomic_bool closed = false;
        std::atomic<std::int64_t> last_receive_ms = 0;
    };

    enum class OperationKind : std::uint8_t { accept, receive, send };

    struct Operation {
        OVERLAPPED overlapped{};
        OperationKind kind = OperationKind::receive;
        std::shared_ptr<Connection> connection;
        SOCKET accept_socket = INVALID_SOCKET;
        std::vector<std::byte> buffer;
        std::size_t offset = 0;
    };

    bool start(const IocpDispatcherConfig& requested,
               IocpDispatcherCallbacks requested_callbacks,
               std::string* error) {
        if (running_.load()) {
            set_error(error, "IOCP dispatcher is already running");
            return false;
        }
        config_ = requested;
        config_.backlog = std::max(config_.backlog, 1);
        config_.maximum_connections = std::clamp<std::size_t>(
            config_.maximum_connections, 1, 65'536);
        config_.accept_depth = std::clamp<std::size_t>(config_.accept_depth,
                                                       1, 256);
        config_.receive_buffer_bytes = std::clamp<std::size_t>(
            config_.receive_buffer_bytes, 1024, protocol::kMaxTcpBufferedBytes);
        config_.maximum_pending_send_bytes = std::clamp<std::size_t>(
            config_.maximum_pending_send_bytes, 64 * 1024, 64 * 1024 * 1024);
        config_.idle_timeout = std::clamp(config_.idle_timeout,
                                          std::chrono::milliseconds(5'000),
                                          std::chrono::milliseconds(86'400'000));
        const auto hardware_threads = std::max(1u,
                                               std::thread::hardware_concurrency());
        config_.worker_threads = std::clamp<std::size_t>(
            config_.worker_threads == 0 ? hardware_threads
                                        : config_.worker_threads,
            1, 64);
        callbacks_ = std::move(requested_callbacks);
        rate_limiter_ = std::make_unique<security::HandshakeRateLimiter>(
            config_.handshake_rate_limit);
        if (!winsock_.ready()) {
            set_error(error, "Winsock initialization failed");
            return false;
        }
        if (!listener_.listen(config_.port, config_.backlog, error) ||
            !iocp_.create(static_cast<std::uint32_t>(config_.worker_threads),
                          error) ||
            !iocp_.associate(listener_, 0, error)) {
            listener_.close();
            iocp_.close();
            return false;
        }
        DWORD bytes = 0;
        GUID accept_ex_guid = WSAID_ACCEPTEX;
        if (WSAIoctl(static_cast<SOCKET>(listener_.native_handle()),
                     SIO_GET_EXTENSION_FUNCTION_POINTER, &accept_ex_guid,
                     sizeof(accept_ex_guid), &accept_ex_, sizeof(accept_ex_),
                     &bytes, nullptr, nullptr) == SOCKET_ERROR ||
            accept_ex_ == nullptr) {
            set_error(error, "AcceptEx lookup failed");
            listener_.close();
            iocp_.close();
            return false;
        }
        bound_port_ = listener_.local_port(error);
        if (bound_port_ == 0) {
            listener_.close();
            iocp_.close();
            return false;
        }
        running_ = true;
        workers_.reserve(config_.worker_threads);
        for (std::size_t index = 0; index < config_.worker_threads; ++index) {
            workers_.emplace_back([this] { worker_loop(); });
        }
        watchdog_ = std::thread([this] { watchdog_loop(); });
        for (std::size_t index = 0; index < config_.accept_depth; ++index) {
            if (!post_accept()) {
                stop();
                set_error(error, "initial AcceptEx posting failed");
                return false;
            }
        }
        return true;
    }

    void stop() noexcept {
        if (!running_.exchange(false)) {
            return;
        }
        if (watchdog_.joinable()) {
            watchdog_.join();
        }
        if (listener_.is_open()) {
            CancelIoEx(reinterpret_cast<HANDLE>(listener_.native_handle()),
                       nullptr);
            listener_.close();
        }
        std::vector<std::shared_ptr<Connection>> connections;
        {
            std::scoped_lock lock(connections_mutex_);
            connections.reserve(connections_.size());
            for (const auto& [id, connection] : connections_) {
                (void)id;
                connections.push_back(connection);
            }
        }
        for (const auto& connection : connections) {
            close_connection(connection, "dispatcher stopped");
        }
        {
            std::unique_lock lock(pending_mutex_);
            pending_cv_.wait(lock, [this] { return pending_operations_ == 0; });
        }
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            IocpPort::Completion completion{};
            completion.completion_key = kStopCompletionKey;
            (void)iocp_.post(completion, nullptr);
        }
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
        {
            std::scoped_lock lock(connections_mutex_);
            connections_.clear();
        }
        iocp_.close();
        bound_port_ = 0;
        accept_ex_ = nullptr;
    }

    bool send(ConnectionId connection_id, std::span<const std::byte> wire,
              std::string* error) {
        if (!running_.load() || wire.empty() ||
            wire.size() > std::numeric_limits<ULONG>::max()) {
            set_error(error, "invalid dispatcher send");
            return false;
        }
        auto connection = find_connection(connection_id);
        if (!connection || connection->closed.load()) {
            set_error(error, "connection is not available");
            return false;
        }
        bool start_send = false;
        {
            std::scoped_lock lock(connection->send_mutex);
            if (connection->closed.load()) {
                set_error(error, "connection is closed");
                return false;
            }
            if (wire.size() > config_.maximum_pending_send_bytes -
                                  std::min(connection->pending_send_bytes,
                                           config_.maximum_pending_send_bytes)) {
                set_error(error, "connection send queue limit exceeded");
                return false;
            }
            connection->send_queue.emplace_back(wire.begin(), wire.end());
            connection->pending_send_bytes += wire.size();
            if (!connection->send_active) {
                connection->send_active = true;
                start_send = true;
            }
        }
        if (start_send && !post_next_send(connection)) {
            close_connection(connection, "WSASend posting failed");
            set_error(error, "WSASend posting failed");
            return false;
        }
        return true;
    }

    void disconnect(ConnectionId id) noexcept {
        if (auto connection = find_connection(id)) {
            close_connection(connection, "local disconnect");
        }
    }

    void record_handshake_success(ConnectionId id) {
        const auto connection = find_connection(id);
        if (!connection) {
            return;
        }
        rate_limiter_->record_success(connection->source);
        audit({AuditEventKind::handshake_succeeded, id, connection->source,
               "authenticated handshake completed"});
    }

    void record_handshake_failure(ConnectionId id, std::string detail) {
        const auto connection = find_connection(id);
        if (!connection) {
            return;
        }
        rate_limiter_->record_failure(connection->source);
        audit({AuditEventKind::handshake_failed, id, connection->source,
               std::move(detail)});
        close_connection(connection, "handshake failed");
    }

    std::size_t connection_count() const noexcept {
        std::scoped_lock lock(connections_mutex_);
        return connections_.size();
    }

    std::uint16_t local_port() const noexcept { return bound_port_; }
    bool running() const noexcept { return running_.load(); }

private:
    static constexpr std::uintptr_t kStopCompletionKey =
        std::numeric_limits<std::uintptr_t>::max();
    static constexpr DWORD kAcceptAddressBytes = sizeof(sockaddr_in) + 16;

    bool post_accept() {
        if (!running_.load()) {
            return false;
        }
        auto operation = std::make_unique<Operation>();
        operation->kind = OperationKind::accept;
        operation->accept_socket = WSASocketW(
            AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        operation->buffer.resize(kAcceptAddressBytes * 2);
        if (operation->accept_socket == INVALID_SOCKET) {
            audit({AuditEventKind::transport_error, 0, {},
                   "accept socket creation failed"});
            return false;
        }
        begin_operation();
        DWORD received = 0;
        const BOOL result = accept_ex_(
            static_cast<SOCKET>(listener_.native_handle()),
            operation->accept_socket, operation->buffer.data(), 0,
            kAcceptAddressBytes, kAcceptAddressBytes, &received,
            &operation->overlapped);
        if (result == FALSE && WSAGetLastError() != ERROR_IO_PENDING) {
            closesocket(operation->accept_socket);
            finish_operation();
            audit({AuditEventKind::transport_error, 0, {},
                   "AcceptEx posting failed"});
            return false;
        }
        (void)operation.release();
        return true;
    }

    bool post_receive(const std::shared_ptr<Connection>& connection) {
        if (!running_.load() || connection->closed.load()) {
            return false;
        }
        auto operation = std::make_unique<Operation>();
        operation->kind = OperationKind::receive;
        operation->connection = connection;
        operation->buffer.resize(config_.receive_buffer_bytes);
        WSABUF buffer{
            static_cast<ULONG>(operation->buffer.size()),
            reinterpret_cast<char*>(operation->buffer.data())};
        DWORD flags = 0;
        DWORD received = 0;
        begin_operation();
        const int result = WSARecv(
            static_cast<SOCKET>(connection->socket.native_handle()), &buffer, 1,
            &received, &flags, &operation->overlapped, nullptr);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            finish_operation();
            return false;
        }
        (void)operation.release();
        return true;
    }

    bool post_next_send(const std::shared_ptr<Connection>& connection) {
        auto operation = std::make_unique<Operation>();
        operation->kind = OperationKind::send;
        operation->connection = connection;
        {
            std::scoped_lock lock(connection->send_mutex);
            if (connection->send_queue.empty() || connection->closed.load()) {
                connection->send_active = false;
                return false;
            }
            operation->buffer = std::move(connection->send_queue.front());
            connection->send_queue.pop_front();
        }
        return post_send_operation(std::move(operation));
    }

    bool post_send_operation(std::unique_ptr<Operation> operation) {
        if (!running_.load() || !operation->connection ||
            operation->connection->closed.load() ||
            operation->offset >= operation->buffer.size()) {
            return false;
        }
        WSABUF buffer{
            static_cast<ULONG>(operation->buffer.size() - operation->offset),
            reinterpret_cast<char*>(operation->buffer.data() +
                                     operation->offset)};
        DWORD sent = 0;
        std::memset(&operation->overlapped, 0, sizeof(operation->overlapped));
        begin_operation();
        const int result = WSASend(
            static_cast<SOCKET>(operation->connection->socket.native_handle()),
            &buffer, 1, &sent, 0, &operation->overlapped, nullptr);
        if (result == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            finish_operation();
            return false;
        }
        (void)operation.release();
        return true;
    }

    void worker_loop() noexcept {
        while (true) {
            IocpPort::Completion completion{};
            if (!iocp_.wait(completion, INFINITE, nullptr)) {
                if (!running_.load()) {
                    return;
                }
                continue;
            }
            if (completion.completion_key == kStopCompletionKey &&
                completion.overlapped == 0) {
                return;
            }
            if (completion.overlapped == 0) {
                continue;
            }
            auto operation = std::unique_ptr<Operation>(
                reinterpret_cast<Operation*>(completion.overlapped));
            finish_operation();
            switch (operation->kind) {
            case OperationKind::accept:
                handle_accept(std::move(operation), completion.error_code);
                break;
            case OperationKind::receive:
                handle_receive(std::move(operation),
                               completion.bytes_transferred,
                               completion.error_code);
                break;
            case OperationKind::send:
                handle_send(std::move(operation), completion.bytes_transferred,
                            completion.error_code);
                break;
            }
        }
    }

    void handle_accept(std::unique_ptr<Operation> operation,
                       std::uint32_t error_code) {
        const SOCKET accepted = operation->accept_socket;
        operation->accept_socket = INVALID_SOCKET;
        if (running_.load()) {
            (void)post_accept();
        }
        if (error_code != ERROR_SUCCESS || !running_.load()) {
            closesocket(accepted);
            return;
        }
        const SOCKET listener_handle =
            static_cast<SOCKET>(listener_.native_handle());
        if (setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&listener_handle),
                       sizeof(listener_handle)) == SOCKET_ERROR) {
            closesocket(accepted);
            audit({AuditEventKind::transport_error, 0, {},
                   "accepted socket context update failed"});
            return;
        }
        const auto source = ipv4_source(accepted);
        if (!rate_limiter_->allow(source)) {
            closesocket(accepted);
            audit({AuditEventKind::admission_blocked, 0, source,
                   "source is temporarily blocked"});
            return;
        }
        auto connection = std::make_shared<Connection>();
        connection->id = next_connection_id_.fetch_add(1);
        connection->source = source;
        connection->socket = TcpSocket(static_cast<std::uintptr_t>(accepted));
        connection->last_receive_ms = steady_milliseconds();
        if (!iocp_.associate(connection->socket,
                             static_cast<std::uintptr_t>(connection->id),
                             nullptr)) {
            connection->socket.close();
            audit({AuditEventKind::transport_error, connection->id, source,
                   "accepted socket IOCP association failed"});
            return;
        }
        (void)connection->socket.enable_keepalive(30'000, 5'000, nullptr);
        bool capacity_rejected = false;
        {
            std::scoped_lock lock(connections_mutex_);
            if (connections_.size() >= config_.maximum_connections) {
                capacity_rejected = true;
            } else {
                connections_.emplace(connection->id, connection);
            }
        }
        if (capacity_rejected) {
            connection->socket.close();
            audit({AuditEventKind::capacity_rejected, 0, source,
                   "connection capacity reached"});
            return;
        }
        audit({AuditEventKind::accepted, connection->id, source,
               "connection accepted"});
        invoke_connected(connection->id, source);
        if (!post_receive(connection)) {
            close_connection(connection, "initial WSARecv posting failed");
        }
    }

    void handle_receive(std::unique_ptr<Operation> operation,
                        std::uint32_t bytes_transferred,
                        std::uint32_t error_code) {
        const auto& connection = operation->connection;
        if (!connection || connection->closed.load()) {
            return;
        }
        if (error_code != ERROR_SUCCESS || bytes_transferred == 0) {
            close_connection(connection, error_code == ERROR_SUCCESS
                ? "peer closed the connection"
                : "WSARecv completion failed");
            return;
        }
        connection->last_receive_ms = steady_milliseconds();
        if (callbacks_.on_bytes) {
            std::vector<std::byte> bytes(
                operation->buffer.begin(),
                operation->buffer.begin() + bytes_transferred);
            invoke_bytes(connection->id, std::move(bytes));
            if (!connection->closed.load() && !post_receive(connection)) {
                close_connection(connection, "WSARecv posting failed");
            }
            return;
        }
        std::vector<protocol::TcpFrame> frames;
        if (!connection->parser.feed(
                std::span<const std::byte>(operation->buffer.data(),
                                           bytes_transferred),
                frames)) {
            rate_limiter_->record_failure(connection->source);
            audit({AuditEventKind::protocol_error, connection->id,
                   connection->source, "invalid or oversized TCP frame"});
            close_connection(connection, "protocol framing rejected");
            return;
        }
        for (auto& frame : frames) {
            invoke_frame(connection->id, std::move(frame));
            if (connection->closed.load()) {
                return;
            }
        }
        if (!post_receive(connection)) {
            close_connection(connection, "WSARecv posting failed");
        }
    }

    void handle_send(std::unique_ptr<Operation> operation,
                     std::uint32_t bytes_transferred,
                     std::uint32_t error_code) {
        const auto& connection = operation->connection;
        if (!connection || connection->closed.load()) {
            return;
        }
        if (error_code != ERROR_SUCCESS || bytes_transferred == 0) {
            close_connection(connection, "WSASend completion failed");
            return;
        }
        operation->offset += bytes_transferred;
        if (operation->offset < operation->buffer.size()) {
            if (!post_send_operation(std::move(operation))) {
                close_connection(connection, "partial WSASend repost failed");
            }
            return;
        }
        bool has_more = false;
        {
            std::scoped_lock lock(connection->send_mutex);
            connection->pending_send_bytes -= std::min(
                connection->pending_send_bytes, operation->buffer.size());
            has_more = !connection->send_queue.empty();
            if (!has_more) {
                connection->send_active = false;
            }
        }
        if (has_more && !post_next_send(connection)) {
            close_connection(connection, "queued WSASend posting failed");
        }
    }

    void close_connection(const std::shared_ptr<Connection>& connection,
                          const char* detail) noexcept {
        if (!connection || connection->closed.exchange(true)) {
            return;
        }
        connection->socket.close();
        {
            std::scoped_lock lock(connections_mutex_);
            const auto found = connections_.find(connection->id);
            if (found != connections_.end() &&
                found->second.get() == connection.get()) {
                connections_.erase(found);
            }
        }
        {
            std::scoped_lock lock(connection->send_mutex);
            connection->send_queue.clear();
            connection->pending_send_bytes = 0;
            connection->send_active = false;
        }
        audit({AuditEventKind::disconnected, connection->id,
               connection->source, detail});
        invoke_disconnected(connection->id);
    }

    std::shared_ptr<Connection> find_connection(ConnectionId id) const {
        std::scoped_lock lock(connections_mutex_);
        const auto found = connections_.find(id);
        return found == connections_.end() ? nullptr : found->second;
    }

    void begin_operation() noexcept {
        std::scoped_lock lock(pending_mutex_);
        ++pending_operations_;
    }

    void watchdog_loop() noexcept {
        while (running_.load()) {
            const auto now = steady_milliseconds();
            const auto timeout = config_.idle_timeout.count();
            std::vector<std::shared_ptr<Connection>> stale;
            {
                std::scoped_lock lock(connections_mutex_);
                for (const auto& [id, connection] : connections_) {
                    (void)id;
                    if (now - connection->last_receive_ms.load() > timeout) {
                        stale.push_back(connection);
                    }
                }
            }
            for (const auto& connection : stale) {
                close_connection(connection, "connection idle timeout");
            }
            Sleep(250);
        }
    }

    void finish_operation() noexcept {
        std::scoped_lock lock(pending_mutex_);
        if (pending_operations_ != 0) {
            --pending_operations_;
        }
        if (pending_operations_ == 0) {
            pending_cv_.notify_all();
        }
    }

    void invoke_connected(ConnectionId id, const std::string& source) noexcept {
        try {
            if (callbacks_.on_connected) {
                callbacks_.on_connected(id, source);
            }
        } catch (...) {
            audit({AuditEventKind::transport_error, id, source,
                   "connected callback threw an exception"});
        }
    }

    void invoke_frame(ConnectionId id, protocol::TcpFrame frame) noexcept {
        try {
            if (callbacks_.on_frame) {
                callbacks_.on_frame(id, std::move(frame));
            }
        } catch (...) {
            if (auto connection = find_connection(id)) {
                audit({AuditEventKind::transport_error, id,
                       connection->source,
                       "frame callback threw an exception"});
                close_connection(connection, "frame callback failed");
            }
        }
    }

    void invoke_bytes(ConnectionId id, std::vector<std::byte> bytes) noexcept {
        try {
            callbacks_.on_bytes(id, std::move(bytes));
        } catch (...) {
            if (auto connection = find_connection(id)) {
                audit({AuditEventKind::transport_error, id,
                       connection->source,
                       "raw receive callback threw an exception"});
                close_connection(connection, "raw receive callback failed");
            }
        }
    }

    void invoke_disconnected(ConnectionId id) noexcept {
        try {
            if (callbacks_.on_disconnected) {
                callbacks_.on_disconnected(id);
            }
        } catch (...) {
        }
    }

    void audit(AuditEvent event) noexcept {
        try {
            if (callbacks_.on_audit) {
                callbacks_.on_audit(event);
            }
        } catch (...) {
        }
    }

    WinsockRuntime winsock_;
    TcpSocket listener_;
    IocpPort iocp_;
    LPFN_ACCEPTEX accept_ex_ = nullptr;
    IocpDispatcherConfig config_;
    IocpDispatcherCallbacks callbacks_;
    std::unique_ptr<security::HandshakeRateLimiter> rate_limiter_;
    std::atomic_bool running_ = false;
    std::atomic<ConnectionId> next_connection_id_ = 1;
    std::uint16_t bound_port_ = 0;
    mutable std::mutex connections_mutex_;
    std::unordered_map<ConnectionId, std::shared_ptr<Connection>> connections_;
    std::vector<std::thread> workers_;
    std::thread watchdog_;
    std::mutex pending_mutex_;
    std::condition_variable pending_cv_;
    std::size_t pending_operations_ = 0;
};

IocpDispatcher::IocpDispatcher() : impl_(std::make_unique<Impl>()) {}
IocpDispatcher::~IocpDispatcher() = default;

bool IocpDispatcher::start(const IocpDispatcherConfig& config,
                           IocpDispatcherCallbacks callbacks,
                           std::string* error) {
    return impl_->start(config, std::move(callbacks), error);
}

void IocpDispatcher::stop() noexcept { impl_->stop(); }

bool IocpDispatcher::send(ConnectionId connection_id,
                          std::span<const std::byte> wire,
                          std::string* error) {
    return impl_->send(connection_id, wire, error);
}

void IocpDispatcher::disconnect(ConnectionId id) noexcept {
    impl_->disconnect(id);
}

void IocpDispatcher::record_handshake_success(ConnectionId id) {
    impl_->record_handshake_success(id);
}

void IocpDispatcher::record_handshake_failure(ConnectionId id,
                                              std::string detail) {
    impl_->record_handshake_failure(id, std::move(detail));
}

bool IocpDispatcher::running() const noexcept { return impl_->running(); }

std::size_t IocpDispatcher::connection_count() const noexcept {
    return impl_->connection_count();
}

std::uint16_t IocpDispatcher::local_port() const noexcept {
    return impl_->local_port();
}

} // namespace nstu::net
