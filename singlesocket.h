#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"

class singleSocket : public Socket {
    public:
        singleSocket(int port);
        static void signal_handler(int signum);
        void start();

};
singleSocket::singleSocket(int port) : Socket(port) {
    // Constructor implementation
    signal(SIGINT, singleSocket::signal_handler);
    signal(SIGTERM, singleSocket::signal_handler);
}

void singleSocket::signal_handler(int signum) {
    std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
    exit(signum);
}

void singleSocket::start() {
    // Call the base class method to create the socket
    Socket::create_fd();
    //accept connections or handle other tasks here
    while (1) {
        // Here you would typically accept connections or handle other tasks
        // For demonstration, we will just sleep to simulate server activity
        int client_fd = accept_connection();
        if (client_fd >= 0) {
            std::cout << "Accepted connection on client_fd: " << client_fd << std::endl;
        } else {
            std::cout << "No connections available, continuing..." << std::endl;
        }
        handleconnections(client_fd);
    }
}
