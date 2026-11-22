#include "cancellable_command.h"
#include "plugin_lifetime.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

// Hyprland auto-reaps children (SA_NOCLDWAIT). A short-lived supervisor with
// normal SIGCHLD handling reports the command's actual status and keeps its
// process group alive until the caller has finished handling cancellation.
[[noreturn]] static void supervise(char *const *args, int control, int output, bool merge_stderr) {
    // This process waits without execing. Close other workers' pipe ends and
    // compositor sockets even when they have FD_CLOEXEC set.
    int low = std::min(control, output), high = std::max(control, output);
    if (low > 3) close_range(3, low - 1, 0);
    if (high > low + 1) close_range(low + 1, high - 1, 0);
    close_range(high + 1, UINT_MAX, 0);
    if (setpgid(0, 0) < 0)
        _exit(1);
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, nullptr);
    pid_t child = fork();
    if (child == 0) {
        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, nullptr);
        if (dup2(output, STDOUT_FILENO) < 0 || (merge_stderr && dup2(output, STDERR_FILENO) < 0))
            _exit(127);
        execvp(args[0], args);
        _exit(127);
    }
    close(output);
    int code = -1;
    if (child > 0) {
        int status;
        pid_t result;
        do {
            result = waitpid(child, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result == child && WIFEXITED(status))
            code = WEXITSTATUS(status);
    }
    send(control, &code, sizeof(code), MSG_NOSIGNAL);
    char ack;
    while (read(control, &ack, 1) < 0 && errno == EINTR) {}
    _exit(0);
}

int run_cancellable_command(const std::vector<std::string> &argv, std::string &output, bool merge_stderr) {
    if (argv.empty() || plugin_lifetime.stop_requested())
        return -1;
    std::vector<char *> args;
    for (auto &arg : argv)
        args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);

    int control[2], stream[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, control) < 0)
        return -1;
    if (pipe2(stream, O_CLOEXEC) < 0) {
        close(control[0]);
        close(control[1]);
        return -1;
    }
    pid_t supervisor = fork();
    if (supervisor == 0) {
        close(control[0]);
        close(stream[0]);
        supervise(args.data(), control[1], stream[1], merge_stderr);
    }
    close(control[1]);
    close(stream[1]);
    int code = -1;
    if (supervisor > 0) {
        setpgid(supervisor, supervisor);
        {
            // Wake poll even if a privileged descendant keeps stdout open.
            std::stop_callback wake(plugin_lifetime.get_token(), [&] { shutdown(control[0], SHUT_RD); });
            pollfd fds[] = {{stream[0], POLLIN, 0}, {control[0], POLLIN, 0}};
            bool got_status = false;
            while (!plugin_lifetime.stop_requested() && (fds[0].fd >= 0 || !got_status)) {
                if (poll(fds, 2, -1) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (fds[0].revents) {
                    char buffer[4096];
                    auto n = read(stream[0], buffer, sizeof(buffer));
                    if (n > 0) output.append(buffer, n);
                    else if (n == 0 || errno != EINTR) fds[0].fd = -1;
                }
                if (fds[1].revents) {
                    if (recv(control[0], &code, sizeof(code), 0) != sizeof(code))
                        code = -1;
                    got_status = true;
                }
            }
        }
        if (plugin_lifetime.stop_requested() || code < 0) {
            kill(-supervisor, SIGKILL);
            code = -1;
        }
    }
    close(stream[0]);
    shutdown(control[0], SHUT_WR);
    close(control[0]);
    if (supervisor > 0)
        while (waitpid(supervisor, nullptr, 0) < 0 && errno == EINTR) {}
    return code;
}
