#include "polling_thread.h"
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <mutex>
#include <hypriso.h>

PollThread *poll_thread = new PollThread;

void PollThread::start() {
    if (started)
        return;
    stop();
    if (pipe2(main_wake_pipe, O_CLOEXEC | O_NONBLOCK) == -1)
        return;
    started = true;
    keep_running = true;
 
    thread = std::thread([this] {
        while (keep_running) {
            std::vector<std::shared_ptr<Polled>> localFds;
            {
                std::lock_guard<std::mutex> lock(polling_mutex);
                localFds = polling;
            }

            std::vector<struct pollfd> pollfds;
            pollfds.reserve(localFds.size() + 1);
            pollfds.push_back({main_wake_pipe[0], POLLIN, 0});
            for (const auto &watched : localFds) {
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

            std::vector<std::shared_ptr<Polled>> callbacks;
            std::vector<std::shared_ptr<Polled>> toRemove;
            for (size_t i = 1; i < pollfds.size(); ++i) {
                if (pollfds[i].revents & POLLIN) {
                    const auto &watched = localFds[i - 1];
                    if (watched && watched->on_event)
                        callbacks.push_back(watched);
                }
                if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL))
                    toRemove.push_back(localFds[i - 1]);
            }

            for (const auto &cb : callbacks)
                cb->on_event(cb->fd, cb->userdata);

            if (!toRemove.empty()) {
                std::lock_guard<std::mutex> lock(polling_mutex);
                // Compare entries, not fd numbers: the fd may have been closed and
                // its number reused by a newer add() in the meantime.
                for (const auto &dead : toRemove)
                    std::erase_if(polling, [&dead](const std::shared_ptr<Polled> &watched) {
                        return !watched || watched == dead;
                    });
            }
        }

        started = false;
    });
}

static void wake(PollThread *pt) {
    (void) !write(pt->main_wake_pipe[1], "x", 1);
}

void PollThread::add(int fd, void *userdata, std::function<void(int fd, void *userdata)> on_event) {
    if (!started)
        return;
 
    auto p = std::make_shared<Polled>();
    p->fd = fd;
    p->userdata = userdata;
    p->on_event = std::move(on_event);

    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        polling.push_back(std::move(p));
    }

    wake(this);
}

void PollThread::remove(int fd) {
    if (!started)
        return;
 
    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        std::erase_if(polling, [fd](const std::shared_ptr<Polled> &watched) {
            return !watched || watched->fd == fd;
        });
    }

    wake(this);
}

void PollThread::stop() {
    if (!thread.joinable())
        return;
    
    {
        std::lock_guard<std::mutex> lock(polling_mutex);
        keep_running = false;
    }
    
    wake(this);

    thread.join();
    close(main_wake_pipe[0]);
    close(main_wake_pipe[1]);
    main_wake_pipe[0] = main_wake_pipe[1] = -1;
}
