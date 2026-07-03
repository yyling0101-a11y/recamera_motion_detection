#pragma once

#include <functional>
#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

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
    int port_;
    Handler handler_;
};
