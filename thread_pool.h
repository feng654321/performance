#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include    <thread>

class poolthread: public Socket {
    public:
        poolthread(int port) : Socket(port) {
            signal(SIGINT, poolthread::signal_handler);
            signal(SIGTERM, poolthread::signal_handler);
        }

        static void signal_handler(int signum) {
            std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
            exit(signum);
        }

        void start() {
            // Call the base class method to create the socket
            Socket::create_fd();
            //accept connections or handle other tasks here
            std::cout << "Thread pool server started on port " << _port << std::endl;
            while (true) {
                int client_fd = accept_connection();
                if (client_fd < 0) {
                    std::cerr << "Error accepting connection." << std::endl;
                    continue; // Continue to accept more connections
                }
                // Handle the connection in a new thread
                threadpool_instance.enqueue([client_fd, this]() {
                    handleconnections(client_fd); // Handle the connection
                });
            }
        }

    private:
        threadpool threadpool_instance;

};



