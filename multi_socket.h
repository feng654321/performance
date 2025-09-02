#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include <signal.h>
#include <sys/wait.h>

class multiSocket : public Socket {
    public:
        multiSocket(int port);
        static void signal_handler(int signum);
        void start();

};
void clean_child(int){
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
multiSocket::multiSocket(int port) : Socket(port) {
    // Constructor implementation
    signal(SIGCHLD, clean_child);
    signal(SIGINT, multiSocket::signal_handler);
    signal(SIGTERM, multiSocket::signal_handler);
}

void multiSocket::signal_handler(int signum) {
    std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
    exit(signum);
}
void multiSocket::start() {
    // Call the base class method to create the socket
    Socket::create_fd();
    // Accept connections or handle other tasks here
    while (true) {
        int client_fd = accept_connection();
        if (client_fd < 0) {
            std::cerr << "Error accepting connection." << std::endl;
            continue;
        }
        pid_t child_pid = fork(); // Create a new process for each connection
        if (child_pid < 0) {
            std::cerr << "Error forking process." << std::endl;
            close(client_fd);
        } else if (child_pid == 0) {
            // Child process
            close(sockfd); // Close the server socket in the child process
            handleconnections(client_fd); // Handle the connection
            exit(0); // Exit child process after handling
        }else{
            // Parent process
            close(client_fd); // Close the client socket in the parent process
        }
    }
}