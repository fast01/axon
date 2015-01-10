#include "rpc/session.hpp"
#include "rpc/base_rpc_service.hpp"
#include "util/log.hpp"
using namespace axon::rpc;
using namespace axon::service;
using namespace axon::socket;

Session::Session(axon::service::IOService* service, BaseRPCService* rpc) {
    socket_ = ConsistentSocket::create(service);
    io_service_ = service;
    rpc_service_ = rpc;
    recv_coro_.set_function(std::bind(&Session::event_loop, this));
    pthread_mutex_init(&mutex_, NULL);
    shutdown_ = false;
}

Session::~Session() {
    socket_.reset();
    io_service_ = NULL;
    pthread_mutex_destroy(&mutex_);
}

void Session::start_event_loop() {
    axon::util::ScopedLock lock(&mutex_);
    recv_coro_();
}

void Session::event_loop() {
    while (!shutdown_) {
        Message message;
        ConsistentSocket::SocketResult recv_result;
        socket_->async_recv(message, std::bind(&Session::safe_callback_quick, this, shared_from_this(), &recv_coro_, std::ref(recv_result), std::placeholders::_1));
        /*
        socket_->async_recv(message, safe_callback([this, &recv_result](const ConsistentSocket::SocketResult& sr) {
            recv_result = sr;
            recv_coro_();
        }));
        */
        recv_coro_.yield();

        // if socket is shutdown, just quit
        if (shutdown_) {
            return;
        }
        if (recv_result == ConsistentSocket::SocketResult::SUCCESS) {
            io_service_->post(std::bind(&Session::dispatch_request, shared_from_this(), message));
        } else {
            // recv failed, abort this session
            rpc_service_->remove_session(shared_from_this());
            return;
        }
    }
}

void Session::dispatch_request(const axon::socket::Message& message) {
    rpc_service_->dispatch_request(message, shared_from_this());
}

void Session::send_response(axon::socket::Message& message) {
    socket_->async_send(message, [](const ConsistentSocket::SocketResult& sr){
        if (sr != ConsistentSocket::SocketResult::SUCCESS) {
            LOG_INFO("send response failed %d", (int)sr);
        } else {
            // LOG_INFO("response sent");
        }
    });
}

void Session::shutdown() {
    axon::util::ScopedLock lock(&mutex_);
    shutdown_ = true;
    socket_->shutdown();
}
