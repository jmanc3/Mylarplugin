#pragma once

#include "vector"
#include "mutex"
#include "thread"
#include "functional"
#include <atomic>

struct Polled {
    int fd = -1;
    void *userdata = nullptr;
    std::function<void(int fd, void *userdata)> on_event = nullptr;
};

struct PollThread {
    std::atomic<bool> started = false;
    std::atomic<bool> keep_running = true;
    int main_wake_pipe[2] = {-1, -1};

    std::mutex polling_mutex;
    std::vector<Polled *> polling;

    std::thread thread;
    
    void start();
    void add(int fd, void *userdata, std::function<void(int fd, void *userdata)> on_event);
    void remove(int fd);
    void stop();
};

extern PollThread *poll_thread;
