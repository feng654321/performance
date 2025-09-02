#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include <signal.h>
#include <sys/wait.h>
#include <vector>
#include <sys/prctl.h>  // for prctl, PR_SET_PDEATHSIG
#include <signal.h>     // for SIGTERM


#define NUM_WORKERS 5 // Number of worker processes in the pool
#include <unistd.h>

class processPool1 : public Socket {
    public:
        processPool1(int port);
        ~processPool1() {
            stop(); // Ensure the server is stopped when the object is destroyed
        }
        static void signal_handler(int signum);
        void start();
        void create_socket();
        void create_pool();
        void stop();
        void work_process(); // Function to be executed by each worker process

    private:
        std::vector<pid_t>  worker_pids; // Vector to hold worker process IDs
};

processPool1::processPool1(int port) : Socket(port) {
    // Constructor implementation
    signal(SIGCHLD, [](int){ while (waitpid(-1, NULL, WNOHANG) > 0); });
    signal(SIGINT, processPool1::signal_handler);
    signal(SIGTERM, processPool1::signal_handler);
    //set the process group ID to the process ID
    pid_t pid = getpid();
    if (setpgid(pid, pid) < 0) {
        std::cerr << "Error setting process group ID." << std::endl;
        exit(EXIT_FAILURE);
    }
}
void processPool1::work_process() {
    // This function will be executed by each worker process
    while (true) {
        int client_fd = accept_connection();
        if (client_fd < 0) {
            std::cerr << "Error accepting connection." << std::endl;
            continue;
        }
        handleconnections(client_fd); // Handle the connection
    }
}

void processPool1::create_pool() {
    // This function can be used to create a pool of worker processes
    // For simplicity, we will not implement a full pool here
    std::cout << "Process pool created." << std::endl;
    for (size_t i = 0; i < NUM_WORKERS; i++)
    {
        pid_t child_pid = fork();
        if (child_pid < 0) {
            std::cerr << "Error forking process." << std::endl;
        } else if (child_pid == 0) {
            // Child process
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            work_process();
        } else {
            // Parent process
            worker_pids.push_back(child_pid); // Store the worker PID
        }
    }
}

void processPool1::signal_handler(int signum) {
    // Handle signals like SIGINT and SIGTERM
    std::cout << "Received signal: " << signum << ". Stopping the server." << std::endl;
    killpg(getpgrp(), SIGTERM);  // Kill the entire process group
    exit(0);
}

void processPool1::stop() {
    // Stop the server and clean up resources
    for (pid_t pid : worker_pids) {
        kill(pid, SIGTERM); // Send termination signal to each worker
    }
    worker_pids.clear();
    std::cout << "Server stopped." << std::endl;
}

void processPool1::create_socket() {

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        throw std::runtime_error("Failed to create socket");
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    // Set the socket to allow reuse of the address
    setoption(SO_REUSEADDR, 1);
    setoption(SO_REUSEPORT, 1);
    // Bind the socket to the address and port
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sockfd);
        throw std::runtime_error("Failed to bind socket");
    }
    // Listen for incoming connections
    if (listen(sockfd, 5) < 0) {
        close(sockfd);
        throw std::runtime_error("Failed to listen on socket");
    }
}

void processPool1::start() {
    create_socket(); // Create the socket
    create_pool(); // Create the worker pool
    std::cout << "Process pool started." << std::endl;
    while (true) {
        pause(); // Wait for signals
    }
}