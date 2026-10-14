#pragma once

#include "motion_config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct NotifierStats {
    std::uint64_t queued   = 0;
    std::uint64_t sent     = 0;
    std::uint64_t failed   = 0;
    std::uint64_t dropped  = 0;
    std::int64_t lastAttemptUnixMs = 0;
    std::int64_t lastSuccessUnixMs = 0;
    int lastHttpStatus     = 0;
    std::string lastError;
};

// Pushes motion events to an HTTP endpoint (for example a Home Assistant
// webhook) from a dedicated worker thread so that the video pipeline and the
// HTTP API are never blocked by a slow or unreachable receiver.
class WebhookNotifier {
public:
    WebhookNotifier();
    ~WebhookNotifier();

    WebhookNotifier(const WebhookNotifier&)            = delete;
    WebhookNotifier& operator=(const WebhookNotifier&) = delete;

    void configure(const NotifyConfig& config);
    NotifyConfig config() const;
    NotifierStats stats() const;

    // Returns false when the queue is full and the payload was dropped.
    bool enqueue(const std::string& jsonBody, const std::string& label);

    void stop();

private:
    void workerLoop();
    bool sendOnce(const std::string& jsonBody, int& httpStatus, std::string& error);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::pair<std::string, std::string>> queue_;
    NotifyConfig config_;
    NotifierStats stats_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> started_{false};
};

bool parseHttpUrl(const std::string& url, std::string& host, int& port, std::string& path);
std::int64_t notifierNowUnixMs();
