#include "util/coroutine.hpp"
#include "util/lock.hpp"
#include <boost/context/all.hpp>

using namespace axon::util;
Coroutine::Coroutine(): 
    stack_(1024 * 1024, 0), 
    context_callee_(NULL), 
    call_(std::function<void()>()) {
    pthread_mutex_init(&mutex_, NULL);
}

void Coroutine::set_function(std::function<void()>& f) {
    call_ = f;
    context_callee_ = boost::context::make_fcontext(&stack_[0] + stack_.size(), stack_.size(), dispatch);
}

void Coroutine::operator()() {
    axon::util::ScopedLock lock(&mutex_);
    boost::context::jump_fcontext(&context_caller_, context_callee_, (intptr_t)this);
    if (exception_ != std::exception_ptr()) {
        std::rethrow_exception(exception_);
    }
}
void Coroutine::yield() { 
    boost::context::jump_fcontext(context_callee_, &context_caller_, 0);
}

void Coroutine::dispatch(intptr_t arg) {
    Coroutine *co = (Coroutine*) arg;
    try {
        co->call_();
    } catch (...) {
        co->exception_ = std::current_exception();
    }
    co->call_ = std::function<void()>();
    co->yield();
    // guard 
    co->exception_ = std::exception_ptr();
    while (true) {
        co->yield();
    }
}