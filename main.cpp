#include  "singlesocket.h"
#include "multi_socket.h"
#include "multi_thread.h"
#include "process_pool.h"
#include "process_pool_1.h"
#include "thread_pool.h"
#include "lead_follow.h"
#include "select_server.h"
#include "epoll_server.h"
#include <memory>


void print_usage() {
    std::cout << "Usage: ./server type [port]" << std::endl;
    std::cout << "Available types:" << std::endl;
    std::cout << "1: singleSocket" << std::endl;
    std::cout << "Default port is 8080." << std::endl;
    exit(EXIT_FAILURE);
}

std::unique_ptr<Socket> create_server(const std::string& type, int port) {
    if (type == "singleSocket") {
        return std::make_unique<singleSocket>(port);
    } else if (type == "multiSocket") {
        return std::make_unique<multiSocket>(port);
    } else if (type == "multiThreadSocket") {
        return std::make_unique<multiThreadSocket>(port);
    } else if (type == "processPool") {
        return std::make_unique<processPool>(port);
    } else if (type == "processPool1") {
        return std::make_unique<processPool1>(port);
    } else if (type == "poolthread") {
        return std::make_unique<poolthread>(port);
    } else if (type == "lead_follow") {
        return std::make_unique<lead_follow>(port);
    } else if (type == "selectserver") {
        return std::make_unique<select_event_handler>(port);
    } else if (type == "epollserver") {
        return std::make_unique<epoll_event_handler>(port);
    } else {
        throw std::invalid_argument("Unknown socket type: " + type);
    }
}

int main(int argc, char* argv[]) {

    // Check if the user provided a port number as an argument
    if(argc < 2){
        print_usage();
        return EXIT_FAILURE;
    }
    std::string type = argv[1];
    std::string port_str = argv[2];
    int port = std::stoi(port_str);
    auto server =  create_server(type, port);

    try {
        server->start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}