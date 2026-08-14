#include "polling_thread.h"
#include <fcntl.h>
#include <poll.h>
#include <mutex>
#include <hypriso.h>

PollThread *poll_thread = new PollThread;

void PollThread::start() {
    started = true;
    keep_running = true;
    pipe2(main_wake_pipe, O_CLOEXEC | O_NONBLOCK);
 
    thread = std::thread([this] {
        while (keep_running) {
            std::vector<Polled *> localFds;
            {
                std::lock_guard<std::mutex> lock(polling_mutex);
                localFds = polling;
            }

            std::vector<struct pollfd> pollfds;
            pollfds.reserve(localFds.size() + 1);
            pollfds.push_back({main_wake_pipe[0], POLLIN, 0});
            for (const auto* watched : localFds) {
                if (watched && watched->fd >= 0)
                    pollfds.push_back({watched->fd, POLLIN, 0});
            }

            const int numReady = ::poll(pollfds.data(), pollfds.size(), -1);
            if (numReady < 0) {
                if (errno == EINTR)
                    continue;
                keep_running = false;
                break;
            }

            if (pollfds[0].revents & POLLIN) {
                char drain[64];
                while (::read(main_wake_pipe[0], drain, sizeof(drain)) > 0) {
                }
            }

            if (!keep_running)
                break;

            std::vector<std::pair<std::function<void(int fd, void *userdata)>, Polled *>> callbacks;
            std::vector<int> toRemove;
            for (size_t i = 1; i < pollfds.size(); ++i) {
                if (pollfds[i].revents & POLLIN) {
                    auto* watched = localFds[i - 1];
                    if (watched && watched->on_event)
                        callbacks.push_back({watched->on_event, watched});
                }
                if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                    toRemove.push_back(pollfds[i].fd);
            }

            for (const auto& cb : callbacks)
                cb.first(cb.second->fd, cb.second->userdata);

            if (!toRemove.empty()) {
                std::lock_guard<std::mutex> lock(polling_mutex);
                for (const int fd : toRemove)
                    std::erase_if(polling, [fd](Polled* watched) {
                        if (!watched)
                            return true;
                        if (watched->fd == fd) {
                            delete watched;
                            return true;
                        }
                        return false;
                    });
            }
        }

        close(main_wake_pipe[0]);
        close(main_wake_pipe[1]);

        started = false;
    });
    thread.detach();
}

static void wake(PollThread *poll_thread) {
    write(poll_thread->main_wake_pipe[1], "x", 1);
}

void PollThread::add(int fd, void *userdata, std::function<void(int fd, void *userdata)> on_event) {
    if (!started)
        return;
 
    Polled *p = new Polled;
    p->fd = fd;
    p->userdata = userdata;
    p->on_event = std::move(on_event);

    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        polling.push_back(p);
    }

    wake(poll_thread);
}

void PollThread::remove(int fd) {
    if (!started)
        return;
 
    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        for (int i = polling.size() - 1; i >= 0; i--) {
            polling.erase(polling.begin() + i);
        }
    }
    
    wake(poll_thread);
}

void PollThread::stop() {
    if (!started)
        return;
    
    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        keep_running = false;
    }
    
    wake(poll_thread);

    if (thread.joinable())
        thread.join();
}

