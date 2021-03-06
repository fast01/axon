#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <atomic>
#include <cassert>
#include <functional>
#include <algorithm>
#include <gtest/gtest.h>
#include <sys/time.h>
#include "util/blocking_queue.hpp"
#include "ip/tcp/socket.hpp"
#include "service/io_service.hpp"
#include "buffer/nonfree_sequence_buffer.hpp"
#include "util/coroutine.hpp"
#include "util/timer.hpp"
#include "util/thread.hpp"
#include "util/strand.hpp"

using namespace axon::service;
using namespace axon::ip::tcp;
using namespace axon::util;
using namespace axon::buffer;
class MiscTest: public ::testing::Test {
protected:
    virtual void SetUp() {
    }

    virtual void TearDown() {
    }

public:
};

TEST_F(MiscTest, get_baidu) {
    IOService service;
    Socket socket(&service);

    char message[] = "GET /index.html HTTP/1.1\r\nHost: www.baidu.com\r\n\r\n";
    axon::buffer::NonfreeSequenceBuffer<char> outbuf, inbuf;
    outbuf.prepare(strlen(message) + 1);
    strcpy(outbuf.write_head(), message);
    outbuf.accept(strlen(message));
    printf("will connect\n");
    socket.connect("115.239.210.27", 80);
    printf("connected\n");
    socket.async_send(outbuf, [](const ErrorCode &ec, size_t sz) {
        printf("send %lu %d\n", sz, ec.code());
    });

    inbuf.prepare(100000);
    socket.async_recv(inbuf, [&inbuf](const ErrorCode &erc, size_t sz) {
        char expected_response[] = "HTTP/1.1 200 OK";
        std::string response(inbuf.read_head(), inbuf.read_head() + inbuf.read_size());
        printf("Response: \n");
        printf("%s\n", response.c_str());
        EXPECT_EQ(response.substr(0, strlen(expected_response)), expected_response);
    });

    service.run();
    

}

TEST_F(MiscTest, corotine) {
    int val = 0;
    Coroutine coro;
    std::function<void()> func = [&val, &coro] {
        int stack = 0;
        printf("stack addr %p\n", &stack);
        val++;
        coro.yield();
        val++;
        coro.yield();
        val++;
        coro.yield();
        val++;
    };
    coro.set_function(std::move(func));

    for (int i = 0; i < 4; i++) {
        coro();
        EXPECT_EQ(val, i+1);
    }
    coro();
    coro();
    EXPECT_EQ(val, 4);
}

TEST_F(MiscTest, corotine_exception) {
    int val = 0;
    Coroutine coro;
    std::function<void()> func = [&val, &coro] {
        val++;
        coro.yield();
        throw std::runtime_error("exception");
        coro.yield();
    };
    coro.set_function(std::move(func));

    coro();
    EXPECT_EQ(val, 1);
    EXPECT_THROW(coro(), std::runtime_error);
    EXPECT_EQ(val, 1);
    coro();
    coro();
    EXPECT_EQ(val, 1);
}


TEST_F(MiscTest, timer) {
    IOService service;
    Timer timer(&service);
    timeval begin;
    gettimeofday(&begin, NULL);
    timer.expires_from_now(100); // 100ms
    timer.async_wait([&begin](const ErrorCode& ec) {
        timeval end;
        gettimeofday(&end, NULL);
        double elapsed = (end.tv_sec - begin.tv_sec) + (end.tv_usec - begin.tv_usec) / 1000000.0;
        printf("%lf\n", elapsed);
        EXPECT_EQ(ec.code(), ErrorCode::success);
        EXPECT_LE(elapsed, 0.11);
        EXPECT_GE(elapsed, 0.09);
    });
    service.run();
}

const int timer_count = 1000;
TEST_F(MiscTest, multiple_timer) {
    IOService service;
    std::vector<Timer*> timers(timer_count);
    for (int i = 0; i < timer_count; i++) {
        timers[i] = new Timer(&service);
    }

    timeval before;
    gettimeofday(&before, NULL);
    service.add_work();
    axon::util::Thread thread([&service]() {service.run();});
    for (int i = 1; i < timer_count; i++) {
        timeval begin;
        gettimeofday(&begin, NULL);
        timers[i]->expires_from_now(i); // 1ms
        timers[i]->async_wait([begin, i](const ErrorCode& ec) {
            timeval end;
            gettimeofday(&end, NULL);
            double elapsed = (end.tv_sec - begin.tv_sec) + (end.tv_usec - begin.tv_usec) / 1000000.0;
            EXPECT_EQ(ec.code(), ErrorCode::success);
            EXPECT_LE(elapsed, (0.0011 * i + 0.01));
            EXPECT_GE(elapsed, (0.0009 * i - 0.01));
        });
    }
    timeval after;
    gettimeofday(&after, NULL);
    printf("creating %d request cost %ld ms\n", timer_count, (after.tv_sec - before.tv_sec)*1000 + (after.tv_usec - before.tv_usec)/1000);
    service.remove_work();
    service.run();
    thread.join();
    for (int i = 0; i < timer_count; i++) {
        delete timers[i];
    }
}

TEST_F(MiscTest, nested_coro) {
    Coroutine coro1;
    Coroutine coro2;
    int n = 0;
    coro2.set_function([&coro2, &n]() {
        n++;
        coro2.yield();
        n++;
        coro2.yield();
        n++;
    });
    coro1.set_function([&coro1, &coro2]() {
        coro2();
        coro1.yield();
        coro2();
        coro1.yield();
        coro2();
    });
    coro1();
    EXPECT_EQ(n, 1);
    coro1();
    EXPECT_EQ(n, 2);
    coro1();
    EXPECT_EQ(n, 3);
}

TEST_F(MiscTest, coro_speed) {
    Coroutine coro;
    int n = 0;
    coro.set_function([&coro, &n]() {
        while (true) {
            n++;
            coro.yield();
        }
    });
    for (int i = 0; i < 10000000; i++) {
        coro();
    }
    EXPECT_EQ(n, 10000000);
}

namespace {
    int counter;
}
TEST_F(MiscTest, spinlock) {
    counter = 0;
    Thread *thrs[8];
    axon::util::SpinLock lock;
    for (int i = 0; i < 8; i++) {
        thrs[i] = new Thread([&lock](){
            for (int s = 0; s < 1000000; s++) {
                lock.lock();
                counter++;
                lock.unlock();
            }
        });
    }
    for (int i = 0; i < 8; i++) {
        thrs[i]->join();
        delete thrs[i];
    }
    EXPECT_EQ(counter, 8 * 1000000);
}

TEST_F(MiscTest, strand_order) {
    Thread *thrs[1];
    IOService service;
    Strand::Ptr strand = Strand::create(&service);
    int last = -1;
    for (int i = 0; i < 1; i++) {
        thrs[i] = new Thread([&strand, &last, &service](){
            for (int s = 0; s < 1000000; s++) {
                strand->post([s, &last]() {
                    EXPECT_EQ(last + 1, s);
                    last = s;
                });
            }
            service.remove_work();
        });
    }
    service.add_work();
    service.run();
    for (int i = 0; i < 1; i++) {
        thrs[i]->join();
        delete thrs[i];
    }
    EXPECT_EQ(last, 1 * 1000000 - 1);
}

TEST_F(MiscTest, strand_check_syncd) {
    IOService service;
    Strand::Ptr strand = Strand::create(&service);
    int counter = 0;
    const int n_produce = 8;
    const int n_dispatch = 0;
    const int n_wrap = 0;
    const int n_run = 8;
    const int nn = 100000;
    Thread *thrs[n_produce];
    for (int i = 0; i < n_produce; i++) {
        thrs[i] = new Thread([&strand, &counter, &service](){
            for (int s = 0; s < nn; s++) {
                strand->post([&counter]() {
                    counter++;
                });
            }
            service.remove_work();
            printf("producer done\n");
        });
    }
    Thread *thrs_dis[n_dispatch];
    for (int i = 0; i < n_dispatch; i++) {
        thrs_dis[i] = new Thread([&strand, &counter, &service](){
            for (int s = 0; s < nn; s++) {
                strand->dispatch([&counter]() {
                    counter++;
                });
            }
            service.remove_work();
            printf("dispatcher done\n");
        });
    }
    Thread *thrs_wrap[n_wrap];
    for (int i = 0; i < n_wrap; i++) {
        thrs_wrap[i] = new Thread([&strand, &counter, &service](){
            for (int s = 0; s < nn; s++) {
                service.post(strand->wrap(std::function<void()>([&counter](){
                    counter++;
                })));
            }
            service.remove_work();
            printf("wrapper done\n");
        });
    }
    for (int i = 0; i < n_produce + n_dispatch + n_wrap; i++) {
        service.add_work();
    }
    Thread *run_thrs[n_run];
    for (int i = 0; i < n_run; i++) {
        run_thrs[i] = new Thread(std::bind(&IOService::run, &service));
    }
    for (int i = 0; i < n_dispatch; i++) {
        thrs_dis[i]->join();
        delete thrs_dis[i];
    }
    for (int i = 0; i < n_produce; i++) {
        thrs[i]->join();
        delete thrs[i];
    }
    for (int i = 0; i < n_wrap; i++) {
        thrs_wrap[i]->join();
        delete thrs_wrap[i];
    }
    for (int i = 0; i < n_run; i++) {
        run_thrs[i]->join();
        delete run_thrs[i];
    }
    EXPECT_EQ(counter, (n_produce + n_dispatch + n_wrap) * nn);
}

TEST_F(MiscTest, strand_check_done) {
    IOService service;
    Strand::Ptr strand = Strand::create(&service);
    const int n_produce = 1;
    const int n_run = 4;
    const int nn = 1000000;

    for (int s = 0; s < 10; s++) {
        int counter = 0;
        Coroutine coros[n_produce];
	    for (int i = 0; i < n_produce; i++) {
	        coros[i].set_function([&strand, &counter, &service, &coros, i](){
	            for (int s = 0; s < nn; s++) {
	                counter++;
	                service.post([&strand, &counter, &coros, i]() {
                        strand->post([&counter, &coros, i]() {
	                        coros[i]();
    	                });
                    });
	                coros[i].yield();
	            }
	            service.remove_work();
	        });
	        strand->post([&coros, i]() {
	            coros[i]();
	        });
	    }
	    for (int i = 0; i < n_produce; i++) {
	        service.add_work();
	    }
	    Thread *run_thrs[n_run];
	    for (int i = 0; i < n_run; i++) {
	        run_thrs[i] = new Thread(std::bind(&IOService::run, &service));
	    }
	    for (int i = 0; i < n_run; i++) {
	        run_thrs[i]->join();
	        delete run_thrs[i];
	    }
	    EXPECT_EQ(counter, (n_produce) * nn);
        LOG_INFO("done %d", s);
	}
}
