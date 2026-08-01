#pragma once

#include "realtime_processor.h"

#include <atomic>
#include <map>
#include <string>
#include <thread>

class WebControlServer {
public:
    WebControlServer(unsigned int port, Processor& processor);
    ~WebControlServer();

    WebControlServer(const WebControlServer&) = delete;
    WebControlServer& operator=(const WebControlServer&) = delete;

private:
    static const char* page();
    static std::map<std::string, std::string> query_parameters(const std::string& target);
    static void send_response(int client, const char* status, const char* contentType,
                              const std::string& body);

    std::string state_json() const;
    void apply(const std::map<std::string, std::string>& parameters);
    void handle(int client);
    void serve();

    unsigned int port_;
    Processor& processor_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};
