#include   <signal.h>
#include   <iostream>
#include   <cstdlib>
#include   "socket.h"
#include    <thread>

class lead_follow : public Socket
{
private:
    /* data */
    int thread_count = std::thread::hardware_concurrency(); // Get the number of available CPU cores
    bool is_running = true; // Flag to indicate if the server is running
    std::condition_variable cv; // Condition variable for thread synchronization
    std::vector<std::thread> threads_;
    std::mutex lead_mutex_; 
    size_t lead_index = 0; // Index of the lead thread

protected:

    void change_lead() {
        // This function can be used to change the lead thread
        std::unique_lock<std::mutex> lock(lead_mutex_);
        lead_index = (lead_index + 1) % thread_count; // Change the lead index to the next thread
        cv.notify_all(); // Notify all threads to wake up and check if they are the lead
    }

    void worker_thread(int thread_id) {
        // This function will be executed by each worker thread
        while (is_running) {
            std::unique_lock<std::mutex> lock(lead_mutex_);
            cv.wait(lock, [this, thread_id] { return !is_running || thread_id == lead_index; }); // Wait until the server is running
            if (!is_running) {
                break; // Exit the thread if the server is not running
            }
            lock.unlock(); // Unlock the mutex to allow other threads to run

            int client_fd = accept_connection();
            if (client_fd < 0) {
                std::cerr << "Error accepting connection." << std::endl;
                change_lead(); // Change the lead thread if there is an error
                continue; // Continue to accept more connections
            }
            change_lead(); // Change the lead thread after accepting a connection
            handleconnections(client_fd); // Handle the connection
        }
    }

public:
    lead_follow(int port) : Socket(port) {
        signal(SIGINT, lead_follow::signal_handler);
        signal(SIGTERM, lead_follow::signal_handler);
    }
    ~lead_follow() {
        // Cleanup code if needed
    }

    void static signal_handler(int signum) {
        std::cout << "Signal received: " << signum << ". Shutting down gracefully." << std::endl;
        exit(signum);
    }

    void start() {
        // Call the base class method to create the socket
        Socket::create_fd();

        for (size_t i = 0; i < thread_count; i++)
        {
            threads_.emplace_back(&lead_follow::worker_thread, this, i);
        }

        for(auto & thread : threads_) {
            if (thread.joinable()) {
                thread.join(); // Wait for all threads to finish
            }
        }
    }

    void stop() {
        is_running = false; // Set the running flag to false to stop the server
        cv.notify_all(); // Notify all threads to wake up and exit
        for (auto & thread : threads_) {
            if (thread.joinable()) {
                thread.join(); // Wait for all threads to finish
            }
        }
    }

};

