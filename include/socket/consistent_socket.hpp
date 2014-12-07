#pragma once
#include <string>
#include <memory>
#include <queue>
#include "service/io_service.hpp"
#include "util/coroutine.hpp"
#include "util/timer.hpp"
#include "buffer/nonfree_sequence_buffer.hpp"
#include "ip/tcp/socket.hpp"
#include "socket/message.hpp"

namespace axon {
namespace socket {

class ConsistentSocket: public std::enable_shared_from_this<ConsistentSocket> {
private:
    ConsistentSocket(axon::service::IOService* service);
    ConsistentSocket(axon::service::IOService* service, const std::string& addr, uint32_t port);
public:
    ~ConsistentSocket();
    struct SocketResult {
        enum socket_result_t {
            SUCCESS = 0,
            CANCELED = 1,
            BUFFER_FULL = 2,
            DOWN = 3,
            UNKNOWN = 4
        };
        socket_result_t result_;
        SocketResult():SocketResult(UNKNOWN) {}
        SocketResult(int result): result_(static_cast<socket_result_t>(result)) {}
        operator int() const { return result_; }
    };
    typedef std::function<void(const SocketResult&)> CallBack;
    typedef axon::ip::tcp::Socket BaseSocket;
    typedef std::shared_ptr<ConsistentSocket> Ptr;
    void async_recv(axon::socket::Message& message, CallBack callback);
    void async_send(axon::socket::Message& message, CallBack callback);
    void shutdown();
    void start_connecting();

    // following two methods are used to set an accepted (not connecting to anyware) socket ready
    BaseSocket& base_socket() { return base_socket_; };
    void set_ready() { status_ |= SOCKET_READY; }

    enum SocketStatus {
        SOCKET_CONNECTING = 1,
        SOCKET_READY = 2,
        SOCKET_WRITING = 4,
        SOCKET_READING = 8,
        SOCKET_DOWN = 16
    };
    static Ptr create(axon::service::IOService* service) {
        return Ptr(new ConsistentSocket(service));
    }
    static Ptr create(axon::service::IOService* service, const std::string& addr, uint32_t port) {
        return Ptr(new ConsistentSocket(service, addr, port));
    }
protected:

    axon::service::IOService* io_service_;
    BaseSocket base_socket_;
    axon::util::Timer reconnect_timer_, wait_timer_;
    std::string addr_;
    uint32_t port_;
    bool should_connect_;
    uint32_t status_;
    pthread_mutex_t mutex_;
    axon::buffer::NonfreeSequenceBuffer<char> send_buffer_;

    struct Operation {
        Message& message;
        CallBack callback;
        Operation(Message& message, CallBack callback):message(message), callback(callback) { }
    };
    std::queue<Operation> read_queue_;
    std::queue<Operation> write_queue_;
    bool queue_full(const std::queue<Operation>& q) { return q.size() >= 1000; }
private:
    void connect_loop();
    void read_loop();
    void write_loop();
    axon::util::Coroutine connect_coro_, read_coro_, write_coro_;

    std::function<void()> wrap(axon::util::Coroutine& coro, int flag) {
        Ptr ptr = shared_from_this();
        return [&coro, this, flag, ptr]() {
            Ptr _ref __attribute__((unused)) = ptr;
            axon::util::ScopedLock lock(&this->mutex_);
            if (!(status_ & flag)) {
                coro();
            }
        };
    }
    std::function<void(const axon::util::ErrorCode&, size_t)> safe_callback(std::function<void(const axon::util::ErrorCode&, size_t)> handler) {
        Ptr ptr = shared_from_this();
        return [this, ptr, handler](const axon::util::ErrorCode& ec, size_t bt) {
            Ptr _ref __attribute__((unused)) = ptr;
            axon::util::ScopedLock lock(&this->mutex_);
            handler(ec, bt);
        };
    
    }
    void init_coros();
    void do_reconnect() {
        status_ &= ~SOCKET_READY;
        // read failed, initiate connection
        if (should_connect_ && !(status_ & SOCKET_CONNECTING)) {
            printf("do reconnection\n");
            fflush(stdout);
            connect_coro_();
        } else {
            printf("not reconnecting for should_connect_ %d status %d\n", should_connect_, status_);
            fflush(stdout);
        }

    }
};

}
}
