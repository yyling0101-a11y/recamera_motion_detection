#pragma once

#include <functional>
#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

// Returns the raw (still percent-encoded) value of a query parameter.
std::string httpQueryParam(const std::string& query, const std::string& key);

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest& request)>;

    explicit HttpServer(int port);
    void setHandler(Handler handler);
    void start();

private:
    // Each connection is served on its own thread: a slow client (or a long
    // MJPEG/snapshot download) must not stall the control API.
    void handleClient(int client);

    int port_;
    Handler handler_;
};
