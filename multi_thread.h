#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include <thread>

class multiThreadSocket : public Socket {
    public:
        multiThreadSocket(int port);
        static void signal_handler(int signum);
        void start();
};

void multiThreadSocket::signal_handler(int signum) {
    std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
    exit(signum);
}
multiThreadSocket::multiThreadSocket(int port) : Socket(port) {
    // Constructor implementation
    signal(SIGINT, multiThreadSocket::signal_handler);
    signal(SIGTERM, multiThreadSocket::signal_handler);
}
void multiThreadSocket::start() {
    // Call the base class method to create the socket
    Socket::create_fd();
    //accept connections or handle other tasks here
    std::cout << "Multi-threaded socket server started on port " << _port << std::endl;
    while (true) {
        int client_fd = accept_connection();
        if (client_fd < 0) {
            std::cerr << "Error accepting connection." << std::endl;
            continue; // Continue to accept more connections
        }
        // Handle the connection in a new thread
        std::thread([client_fd, this]() {
            handleconnections(client_fd); // Handle the connection
        }).detach(); // Detach the thread to allow it to run independently
    }
}

