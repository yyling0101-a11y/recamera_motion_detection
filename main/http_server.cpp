#include "http_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string reasonPhrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::string trimQuery(const std::string& path) {
    const std::size_t pos = path.find('?');
    return pos == std::string::npos ? path : path.substr(0, pos);
}

bool parseRequestLine(const std::string& request, std::string& method, std::string& path) {
    const std::size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::istringstream line(request.substr(0, lineEnd));
    std::string version;
    if (!(line >> method >> path >> version)) return false;
    path = trimQuery(path);
    return version.find("HTTP/") == 0;
}

std::size_t parseContentLength(const std::string& headers) {
    std::istringstream input(headers);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        for (char& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (key != "content-length") continue;
        try {
            return static_cast<std::size_t>(std::stoul(line.substr(colon + 1)));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

bool readHttpRequest(int fd, std::string& request, bool& tooLarge) {
    constexpr std::size_t kMaxRequest = 1024 * 1024;
    char buffer[4096];
    std::size_t expectedTotal = 0;
    tooLarge = false;

    while (request.size() < kMaxRequest) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count == 0) return !request.empty();
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        request.append(buffer, static_cast<std::size_t>(count));

        const std::size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            if (expectedTotal == 0) {
                const std::size_t contentLength = parseContentLength(request.substr(0, headerEnd));
                expectedTotal = headerEnd + 4 + contentLength;
                if (expectedTotal > kMaxRequest) {
                    tooLarge = true;
                    return false;
                }
            }
            if (request.size() >= expectedTotal) return true;
        }
    }

    tooLarge = true;
    return false;
}

bool writeAll(int fd, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = write(fd, data.data() + offset, data.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

HttpResponse jsonError(int status, const std::string& message) {
    HttpResponse response;
    response.status = status;
    response.body = std::string("{\"ok\":false,\"error\":\"");
    for (char ch : message) {
        if (ch == '"' || ch == '\\') response.body.push_back('\\');
        if (ch == '\n') {
            response.body += "\\n";
        } else {
            response.body.push_back(ch);
        }
    }
    response.body += "\"}";
    return response;
}

std::string buildResponse(const HttpResponse& response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << reasonPhrase(response.status) << "\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    out << "Access-Control-Allow-Headers: Content-Type\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n\r\n";
    out << response.body;
    return out.str();
}

}  // namespace

HttpServer::HttpServer(int port) : port_(port) {}

void HttpServer::setHandler(Handler handler) {
    handler_ = std::move(handler);
}

void HttpServer::start() {
    const int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "[http] socket failed: " << std::strerror(errno) << '\n';
        return;
    }

    int option = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "[http] bind failed: " << std::strerror(errno) << '\n';
        close(serverFd);
        return;
    }

    if (listen(serverFd, 16) < 0) {
        std::cerr << "[http] listen failed: " << std::strerror(errno) << '\n';
        close(serverFd);
        return;
    }

    std::cout << "[http] listening on 0.0.0.0:" << port_ << std::endl;

    while (true) {
        const int client = accept(serverFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            std::cerr << "[http] accept failed: " << std::strerror(errno) << '\n';
            continue;
        }

        std::string rawRequest;
        bool tooLarge = false;
        if (!readHttpRequest(client, rawRequest, tooLarge)) {
            const HttpResponse response = jsonError(tooLarge ? 413 : 400, tooLarge ? "request too large" : "invalid request");
            writeAll(client, buildResponse(response));
            close(client);
            continue;
        }

        HttpRequest request;
        if (!parseRequestLine(rawRequest, request.method, request.path)) {
            writeAll(client, buildResponse(jsonError(400, "invalid request line")));
            close(client);
            continue;
        }

        const std::size_t bodyPos = rawRequest.find("\r\n\r\n");
        request.body = bodyPos == std::string::npos ? std::string() : rawRequest.substr(bodyPos + 4);

        HttpResponse response;
        if (request.method == "OPTIONS") {
            response.status = 204;
            response.body.clear();
        } else if (handler_) {
            try {
                response = handler_(request);
            } catch (const std::exception& e) {
                response = jsonError(500, e.what());
            } catch (...) {
                response = jsonError(500, "unknown server error");
            }
        } else {
            response = jsonError(500, "no HTTP handler installed");
        }

        writeAll(client, buildResponse(response));
        close(client);
    }
}
