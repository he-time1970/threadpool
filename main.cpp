#include <iostream>
#include <future>
#include <type_traits>
#include <thread>
#include <chrono>
#include "threadpool.h"

void fun()
{
    std::chrono::seconds sec(10);
    std::this_thread::sleep_for(sec);
}

int main()
{
    ThreadPool thread_pool(3, 60);
    for (int i = 0; i < 70; i++)
        thread_pool.submit(fun);

    std::chrono::seconds sec(5);
    while (1)
    {
        std::this_thread::sleep_for(sec);
        std::cout << "busy workers: " << thread_pool.getBusyNum() << " idle workers: " << thread_pool.getIdleNum() << std::endl;
    }
    thread_pool.shutdown();

    return 0;
}
