#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include <signal.h>
#include <sys/wait.h>
#include <vector>

#define NUM_WORKERS 5 // Number of worker processes in the pool
#include <unistd.h>

class processPool : public Socket {
    public:
        processPool(int port);
        static void signal_handler(int signum);
        void start();
        void create_pool();
        void stop();
        void work_process(); // Function to be executed by each worker process

    private:
        void clean_child(int);
        std::vector<pid_t>  worker_pids; // Vector to hold worker process IDs
};
processPool::processPool(int port) : Socket(port) {
    // Constructor implementation
    signal(SIGCHLD, [](int){ while (waitpid(-1, NULL, WNOHANG) > 0); });
    signal(SIGINT, processPool::signal_handler);
    signal(SIGTERM, processPool::signal_handler);
}

void processPool::work_process() {
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

void processPool::create_pool() {
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
            work_process();
        }else{
            // Parent process
            worker_pids.push_back(child_pid); // Store the worker PID
        }
    }
}



void processPool::signal_handler(int signum) {
    std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
    exit(signum);
}

void processPool::stop() {
    // This function can be used to stop the worker processes
    for (pid_t pid : worker_pids) {
        kill(pid, SIGTERM); // Send termination signal to each worker
    }
    worker_pids.clear(); // Clear the vector of worker PIDs
    std::cout << "Process pool stopped." << std::endl;
}

void processPool::start() {
    // Call the base class method to create the socket
    Socket::create_fd();
    create_pool(); // Create the worker process pool
    // Accept connections or handle other tasks here

}
