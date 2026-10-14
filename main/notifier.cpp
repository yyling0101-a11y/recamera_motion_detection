#include "notifier.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <sstream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxQueueDepth = 32;

bool writeAll(int fd, const std::string& data, int timeoutMs) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        pollfd ready{};
        ready.fd      = fd;
        ready.events  = POLLOUT;
        const int polled = poll(&ready, 1, timeoutMs);
        if (polled == 0)
            return false;
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }

        const ssize_t written = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            if (written < 0 && errno == EINTR)
                continue;
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

int readStatusLine(int fd, int timeoutMs, std::string& error) {
    std::string head;
    char buffer[512];
    const std::int64_t deadline = notifierNowUnixMs() + timeoutMs;

    while (head.find("\r\n") == std::string::npos) {
        const int remaining = static_cast<int>(deadline - notifierNowUnixMs());
        if (remaining <= 0) {
            error = "timeout waiting for HTTP response";
            return 0;
        }

        pollfd ready{};
        ready.fd     = fd;
        ready.events = POLLIN;
        const int polled = poll(&ready, 1, remaining);
        if (polled == 0) {
            error = "timeout waiting for HTTP response";
            return 0;
        }
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            error = std::string("poll failed: ") + std::strerror(errno);
            return 0;
        }

        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) {
            error = "connection closed before response";
            return 0;
        }
        if (count < 0) {
            if (errno == EINTR)
                continue;
            error = std::string("recv failed: ") + std::strerror(errno);
            return 0;
        }
        head.append(buffer, static_cast<std::size_t>(count));
        if (head.size() > 8192) {
            error = "malformed HTTP response";
            return 0;
        }
    }

    // HTTP/1.1 200 OK
    const std::string line = head.substr(0, head.find("\r\n"));
    const std::size_t firstSpace  = line.find(' ');
    if (firstSpace == std::string::npos || line.compare(0, 4, "HTTP") != 0) {
        error = "malformed HTTP status line";
        return 0;
    }
    try {
        return std::stoi(line.substr(firstSpace + 1));
    } catch (...) {
        error = "unparsable HTTP status code";
        return 0;
    }
}

int connectWithTimeout(const std::string& host, int port, int timeoutMs, std::string& error) {
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    const int resolved = getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (resolved != 0 || result == nullptr) {
        error = std::string("cannot resolve host: ") + gai_strerror(resolved);
        if (result != nullptr)
            freeaddrinfo(result);
        return -1;
    }

    int fd = -1;
    for (addrinfo* current = result; current != nullptr; current = current->ai_next) {
        fd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (fd < 0)
            continue;

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        const int connected = connect(fd, current->ai_addr, current->ai_addrlen);
        if (connected == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            error = std::string("connect failed: ") + std::strerror(errno);
            close(fd);
            fd = -1;
            continue;
        }

        pollfd ready{};
        ready.fd     = fd;
        ready.events = POLLOUT;
        const int polled = poll(&ready, 1, timeoutMs);
        if (polled <= 0) {
            error = polled == 0 ? "connect timeout" : std::string("connect poll failed: ") + std::strerror(errno);
            close(fd);
            fd = -1;
            continue;
        }

        int socketError = 0;
        socklen_t length = sizeof(socketError);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length);
        if (socketError != 0) {
            error = std::string("connect failed: ") + std::strerror(socketError);
            close(fd);
            fd = -1;
            continue;
        }

        fcntl(fd, F_SETFL, flags);
        break;
    }

    freeaddrinfo(result);
    if (fd < 0 && error.empty())
        error = "no usable address for host";
    return fd;
}

}  // namespace

std::int64_t notifierNowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool parseHttpUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0)
        return false;

    std::string rest = url.substr(prefix.size());
    const std::size_t slash = rest.find('/');
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (slash != std::string::npos)
        rest = rest.substr(0, slash);

    const std::size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        host = rest.substr(0, colon);
        try {
            port = std::stoi(rest.substr(colon + 1));
        } catch (...) {
            return false;
        }
        if (port <= 0 || port > 65535)
            return false;
    } else {
        host = rest;
        port = 80;
    }
    return !host.empty();
}

WebhookNotifier::WebhookNotifier() {
    worker_ = std::thread([this] { workerLoop(); });
    started_.store(true);
}

WebhookNotifier::~WebhookNotifier() {
    stop();
}

void WebhookNotifier::stop() {
    if (!started_.exchange(false))
        return;

    stopping_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void WebhookNotifier::configure(const NotifyConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

NotifyConfig WebhookNotifier::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

NotifierStats WebhookNotifier::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool WebhookNotifier::enqueue(const std::string& jsonBody, const std::string& label) {
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_.enabled || config_.url.empty())
            return false;
        ++stats_.queued;
        if (queue_.size() >= kMaxQueueDepth) {
            queue_.pop_front();
            ++stats_.dropped;
        }
        queue_.emplace_back(jsonBody, label);
        accepted = true;
    }
    if (accepted)
        condition_.notify_one();
    return accepted;
}

void WebhookNotifier::workerLoop() {
    while (!stopping_.load()) {
        std::pair<std::string, std::string> item;
        NotifyConfig config;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(500), [this] {
                return stopping_.load() || !queue_.empty();
            });
            if (stopping_.load())
                return;
            if (queue_.empty())
                continue;
            item   = std::move(queue_.front());
            queue_.pop_front();
            config = config_;
        }

        if (!config.enabled || config.url.empty())
            continue;

        int httpStatus = 0;
        std::string error;
        bool ok = false;
        const int attempts = std::max(1, config.retries + 1);
        for (int attempt = 0; attempt < attempts && !stopping_.load(); ++attempt) {
            if (attempt > 0) {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait_for(lock, std::chrono::milliseconds(250 * attempt), [this] { return stopping_.load(); });
                if (stopping_.load())
                    return;
            }
            httpStatus = 0;
            error.clear();
            if (sendOnce(item.first, httpStatus, error)) {
                ok = true;
                break;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        stats_.lastAttemptUnixMs = notifierNowUnixMs();
        stats_.lastHttpStatus    = httpStatus;
        if (ok) {
            ++stats_.sent;
            stats_.lastSuccessUnixMs = stats_.lastAttemptUnixMs;
            stats_.lastError.clear();
        } else {
            ++stats_.failed;
            stats_.lastError = error.empty() ? "unknown error" : error;
            std::fprintf(stderr, "[notify] %s failed: %s (http=%d)\n", item.second.c_str(), stats_.lastError.c_str(), httpStatus);
        }
    }
}

bool WebhookNotifier::sendOnce(const std::string& jsonBody, int& httpStatus, std::string& error) {
    NotifyConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
    }

    std::string host;
    std::string path;
    int port = 80;
    if (!parseHttpUrl(config.url, host, port, path)) {
        error = "notify url must start with http://";
        return false;
    }

    const int timeoutMs = config.timeoutMs > 0 ? config.timeoutMs : 3000;
    const int fd = connectWithTimeout(host, port, timeoutMs, error);
    if (fd < 0)
        return false;

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n";
    request << "Host: " << host << ':' << port << "\r\n";
    request << "User-Agent: recamera-motion-detection\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << jsonBody.size() << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << jsonBody;

    const bool sent = writeAll(fd, request.str(), timeoutMs);
    if (!sent) {
        error = "failed to send request body";
        close(fd);
        return false;
    }

    httpStatus = readStatusLine(fd, timeoutMs, error);
    close(fd);
    if (httpStatus == 0)
        return false;
    if (httpStatus < 200 || httpStatus >= 300) {
        error = "receiver returned HTTP " + std::to_string(httpStatus);
        return false;
    }
    return true;
}
