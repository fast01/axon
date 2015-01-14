#pragma once
#include <cassert>
#include <atomic>
#include <functional>
#include <stack>
#include "util/lock.hpp"
#include "service/io_service.hpp"

namespace axon {
namespace util {

class Strand {
    template <typename T>
    class LockFreeQueue{
    public:
        struct Node {
            Node* next;
            T data;
            Node(T&& d): next(NULL), data(std::move(d)) {
            }
        };

        LockFreeQueue(): head_(NULL) {
        }

        // it seems that we do not suffer ABA problem here
        void push(T&& data) {
            Node* new_head = new Node(std::move(data));
            new_head->next = head_.load(std::memory_order_relaxed);

            Node* old_head  = head_;
            do {
                new_head->next = old_head;
            } while (!head_.compare_exchange_weak(old_head, new_head, std::memory_order_release, std::memory_order_relaxed));
        }

        void push(T& data) {
            push(std::move(data));
        }

        // following calls are not synced, only one thread can enter
        Node* take_all() {
            return head_.exchange(NULL);
        }

        bool empty() {
            return head_.load() == NULL;
        }
    private:
        std::atomic<Node* > head_;
    };

    typedef axon::service::IOService::CallBack CallBack;

    LockFreeQueue<CallBack>::Node* reverse_list(LockFreeQueue<CallBack>::Node* head) {
        if (head == NULL || head->next == NULL) {
            return head;
        }
        LockFreeQueue<CallBack>::Node* last = head;
        LockFreeQueue<CallBack>::Node* p = head->next;
        head->next = NULL;
        while (p) {
            LockFreeQueue<CallBack>::Node* next = p->next;
            p->next = last;
            last = p;
            p = next;
        }
        return last;
    }
public:
    Strand(axon::service::IOService* io_service): io_service_(io_service) {
        has_pending_tests_ = false;
    }

    void post(CallBack callback) {
        queue_.push(std::move(callback));

        bool expected = false;
        if (has_pending_tests_.compare_exchange_strong(expected, true)) {
            io_service_->post(std::bind(&Strand::perform, this));
        }
    }
    void perform() {
        while (true) {
            assert(has_pending_tests_);
            bool queue_empty = queue_.empty();
            // here has_pending_tests_ = true, hence if has_pending_tests_ == queue_empty, mark it false and return
            if (has_pending_tests_.compare_exchange_strong(queue_empty, false)) {
                return;
            }
            LockFreeQueue<CallBack>::Node* list = queue_.take_all();
            LockFreeQueue<CallBack>::Node* reversed_list = reverse_list(list);
            while (reversed_list) {
                reversed_list->data();
                auto last = reversed_list;
                reversed_list = reversed_list->next;
                delete last;
            }
        }
    
    }
private:
    std::atomic_bool has_pending_tests_;
    LockFreeQueue<axon::service::IOService::CallBack> queue_;
    axon::service::IOService* io_service_;


};

}
}
