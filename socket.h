#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <vector>
#include <sys/epoll.h>
#ifndef SOCKET_H
#define SOCKET_H
class Socket {
    public:
        // Constructor that initializes the socket with a default port
        Socket( int port = 8080): _port(port), is_running(false), sockfd(-1), addr{} { }

        bool is_created() const {
            return is_running;
        }

        virtual ~Socket() {
            // Close the socket if it was created
            if (sockfd >= 0) {
                close(sockfd);
            }
            is_running = false;
        }
        virtual void start()=0;
        // Initialize the socket options
        void setoption(int option, int value) {
            if (setsockopt(sockfd, SOL_SOCKET, option, &value, sizeof(value)) < 0) {
                throw std::runtime_error("Failed to set socket option");
            }
        }
        // Create the socket file descriptor
        void create_fd(){
            // Check if the socket is already created
            if (is_created()) {
                throw std::runtime_error("Socket already created");
            }
            is_running = true;

            sockfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sockfd < 0) {
                throw std::runtime_error("Failed to create socket");
            }
            addr.sin_family = AF_INET;
            addr.sin_port = htons(_port);
            addr.sin_addr.s_addr = INADDR_ANY;

            // Set the socket to allow reuse of the address
            setoption(SO_REUSEADDR, 1);
            // Bind the socket to the address and port
            if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(sockfd);
                perror("bind failed");
                throw std::runtime_error("Failed to bind socket");
            }
            // Listen for incoming connections
            if (listen(sockfd, 5) < 0) {
                close(sockfd);
                throw std::runtime_error("Failed to listen on socket");
            }
            std::cout << "Socket:" << sockfd << " created and listening on port " << _port << std::endl;
        }
        // Set the socket to non-blocking mode
        void set_non_blocking(int fd) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) {
                close(fd);
                throw std::runtime_error("Failed to get socket flags");
            }
            if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                close(fd);
                throw std::runtime_error("Failed to set socket to non-blocking mode");
            }
        }
        int get_fd(void) const {
            if (!is_created()) {
                throw std::runtime_error("Socket not created");
            }
            return sockfd; // Return the socket file descriptor
        }

        // Start the socket to epoll incoming connections
        /*
        void start() override{
            create_fd();
            set_non_blocking();
            epoll_event ev;
            ev.events = EPOLLIN | EPOLLRDHUP; // Monitor for incoming connections and disconnections
            ev.data.ptr = this;
            int epoll_fd = epoll_create1(0);
            if (epoll_fd < 0) {
                close(sockfd);
                throw std::runtime_error("Failed to create epoll instance");
            }
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sockfd, &ev) < 0) {
                close(sockfd);
                close(epoll_fd);
                throw std::runtime_error("Failed to add socket to epoll instance");
            }
            std::cout << "Socket started on port " << _port << std::endl;
        }*/

        int accept_connection() {
            socklen_t addr_len = sizeof(addr);
            int client_sockfd = accept(sockfd, (struct sockaddr*)&addr, &addr_len);
            if (client_sockfd < 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    // No connections available, return -1
                    std::cerr << "No connections available, returning -1." << std::endl;
                    return -1;
                } else {
                    throw std::runtime_error("Failed to accept connection");
                }
            }
            return client_sockfd;
        }
        void handleconnections(int clientfd) {
            int buffer[1024];
            // You can add your connection handling logic here
            int bytes_read = read(clientfd, buffer, sizeof(buffer));
            if (bytes_read > 0) {
                // Simple HTTP response
                const char* response = 
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: 13\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "Hello, World!";
                // Write the response back to the client
                int bytes_written = write(clientfd, response, strlen(response));
                if (bytes_written < 0) {
                    std::cerr << "Error writing to client_fd: " << clientfd << std::endl;
                } else {
                    close(clientfd); // Close the connection after handling
                }
            } else if (bytes_read < 0) {
                std::cerr << "Error reading from client_fd: " << clientfd << std::endl;
            } else {
                close(clientfd);
            }
        }

    protected:
        int sockfd;
        int _port;
        int is_running = 1; // Flag to indicate if the socket is running
        struct sockaddr_in addr;
};

class threadpool{
    public:
        threadpool(int _num_cpus = std::thread::hardware_concurrency()) : num_cpus(_num_cpus),stop(false) {
            for (size_t i = 0; i < num_cpus; i++)
            {
                threads.emplace_back([this] {
                    while (true){
                        std::function<void()> task;
                            {
                                std::unique_lock<std::mutex> lock(mutex);
                                condition.wait(lock, [this] { return !tasks.empty() || stop; });
                                if (stop && tasks.empty()) {
                                    return; // Exit the thread if stop is true and no tasks are left
                                }
                                task = std::move(tasks.front());
                                tasks.pop();
                            }
                            task(); // Execute the task
                    }
                });
            }
        }
        ~threadpool() {
            {
                std::unique_lock<std::mutex> lock(mutex);
                stop = true; // Set the stop flag to true
            }
            condition.notify_all(); // Notify all threads to wake up and exit
            for (std::thread &t : threads) {
                if (t.joinable()) {
                    t.join(); // Wait for all threads to finish
                }
            }
        }

// Enqueue a new task to be executed by the thread pool
        template<typename F>
        void enqueue(F&& f) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                tasks.emplace(std::forward<F>(f)); // Add the task to the queue
            }
            condition.notify_one(); // Notify one thread to wake up and execute the task
        }

    private:
        std::vector<std::thread> threads; // Vector to hold worker threads
        int num_cpus;
        bool stop = false; // Flag to indicate if the thread pool is stopping
        std::condition_variable condition; // Condition variable for thread synchronization
        std::mutex mutex; // Mutex for protecting the task queue
        std::queue<std::function<void()>> tasks; // Queue to hold tasks for the thread pool
};

#endif