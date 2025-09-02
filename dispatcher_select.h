#ifndef SELECT_DISPATCHER_H
#define SELECT_DISPATCHER_H

#include <signal.h>
#include <iostream>
#include <cstdlib>
#include <errno.h>
#include "event_dispatcher.h"
#include <sys/select.h>
#include <iostream>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <cstring>
#include <utility>      // For std::move
#include <algorithm>    // For std::max
#include <stdexcept>    // For std::runtime_error

class dispatcherselect : public Eventloop {
public:
    dispatcherselect() {
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_ZERO(&except_fds);
    }

    ~dispatcherselect()  {
        // Cleanup if necessary
        std::cout << "dispatcherselect destructor called." << std::endl;
    }

    void close_fd_safely(int fd) override{
        pending_close_fds_.emplace_back(fd);
    }

    void register_handler(int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler) override {
        pending_operations_.emplace(PendingOperation::Type::REGISTER, fd, event_type, handler);
    }

    void unregister_handler(int fd, EventIOType event_type) override {
        pending_operations_.emplace(PendingOperation::Type::UNREGISTER, fd, event_type, nullptr);
    }

    void loop() override {
        while (loop_running) {

            // Process pending operations
            process_pending_operations();

            read_fds_copy = read_fds;
            write_fds_copy = write_fds;
            except_fds_copy = except_fds;
            // Use select to wait for events
            int activity = select(max_fd + 1, &read_fds_copy, &write_fds_copy, &except_fds_copy, nullptr);
            if (activity < 0) {
                if(EINTR == errno) {
                    // Interrupted by a signal, continue the loop
                    continue;
                }
                perror("select error");
                continue; // Handle error and continue the loop
            }

            // collect active events
            collect_active_events();
             // Process active events
            dispatch_active_events();
            // Process pending close file descriptors
            process_pending_close_fds();
        }
    }

    void stop() override {
        // Implement logic to stop the event loop
        // This could involve breaking the loop or setting a flag
        loop_running = false;
        std::cout << "Stopping dispatcherselect event loop." << std::endl;
    }

private:

    void process_pending_operations() {
        while (!pending_operations_.empty()) {
            PendingOperation op = pending_operations_.front();
            pending_operations_.pop();

            if (op.type == PendingOperation::Type::REGISTER) {
                do_register_handler(op.fd, op.event_type, op.handler);
            } else if (op.type == PendingOperation::Type::UNREGISTER) {
                do_unregister_handler(op.fd, op.event_type);
            }
        }
        process_pending_close_fds(); // Process any pending close file descriptors
    }

    void process_pending_close_fds() {
        for (int fd : pending_close_fds_) {
            if(handlers_.count(fd) == 0) {
                std::cerr << "No handler registered for fd: " << fd << std::endl;
                continue; // No handler registered, skip closing
            }
            do_unregister_handler(fd, EventIOType::READ);
            do_unregister_handler(fd, EventIOType::WRITE);
            do_unregister_handler(fd, EventIOType::EXCEPTION);
            close(fd); // Close the file descriptor
        }
        pending_close_fds_.clear();
    }

    void do_register_handler(int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler) {
        if (event_type == EventIOType::READ) {
            FD_SET(fd, &read_fds);
            handlers_[fd][event_type] = handler;
        } else if (event_type == EventIOType::WRITE) {
            FD_SET(fd, &write_fds);
            handlers_[fd][event_type] = handler;
        } else if (event_type == EventIOType::EXCEPTION) {
            FD_SET(fd, &except_fds);
            handlers_[fd][event_type] = handler;
        }
        // Update max_fd if necessary
        if (fd > max_fd) {
            max_fd = fd;
        }
    }

    void do_unregister_handler(int fd, EventIOType event_type) {
        if (handlers_.find(fd) == handlers_.end()) return;

        handlers_[fd].erase(event_type);

        if (event_type == EventIOType::READ) FD_CLR(fd, &read_fds);
        else if (event_type == EventIOType::WRITE) FD_CLR(fd, &write_fds);
        else if (event_type == EventIOType::EXCEPTION) FD_CLR(fd, &except_fds);

        if (handlers_[fd].empty()) {
            handlers_.erase(fd);
        }

        // Recalculate max_fd if necessary
        if (fd == max_fd) {
            max_fd = 0;
            for (int i = 0; i < FD_SETSIZE; ++i) {
                if (FD_ISSET(i, &read_fds) || FD_ISSET(i, &write_fds) || FD_ISSET(i, &except_fds)) {
                    max_fd = std::max(max_fd, i);
                }
            }
        }
    }

    void collect_active_events() {
        active_events.clear();
        for (const auto& [fd, handler_map] : handlers_) {
            if (FD_ISSET(fd, &read_fds_copy)) {
                active_events.emplace_back(fd, EventIOType::READ);
            }
            if (FD_ISSET(fd, &write_fds_copy)) {
                active_events.emplace_back(fd, EventIOType::WRITE);
            }
            if (FD_ISSET(fd, &except_fds_copy)) {
                active_events.emplace_back(fd, EventIOType::EXCEPTION);
            }
        }
    }
    void dispatch_active_events() {
        if (active_events.empty()) {
            std::cout << "No active events detected." << std::endl;
            return; // No active events, continue the loop
        }

        for (const auto& [fd, event_type] : active_events) {
            auto it = handlers_.find(fd);
            if (it != handlers_.end()) {
                auto handler_it = it->second.find(event_type);
                if (handler_it != it->second.end()) {
                    if (event_type == EventIOType::READ) {
                        handler_it->second->handle_read(fd);
                    } else if (event_type == EventIOType::WRITE) {
                        handler_it->second->handle_write(fd);
                    } else if (event_type == EventIOType::EXCEPTION) {
                        handler_it->second->handle_exception(fd);
                    }
                }
            }
        }
    }

    struct PendingOperation {
        enum class Type { REGISTER, UNREGISTER };
        int fd; // File descriptor
        Type type; // Type of operation (register or unregister)
        EventIOType event_type; // Type of event (read, write, exception)
        std::shared_ptr<EventHandler> handler; // Associated handler
        PendingOperation(Type type, int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler)
            : type(type), fd(fd), event_type(event_type), handler(std::move(handler)) {}
        PendingOperation() = default; // Default constructor for empty initialization
    };
    
    //pending close fd
    std::vector<int> pending_close_fds_;
    //save active events
    std::vector<std::pair<int, EventIOType>> active_events;
    //pending queue for active events
    std::queue<PendingOperation> pending_operations_;
    std::map<int, std::map<EventIOType, std::shared_ptr<EventHandler>>> handlers_;

    bool loop_running = true; // Flag to control the event loop
    fd_set read_fds;   // Set of file descriptors to monitor for read events
    fd_set write_fds;  // Set of file descriptors to monitor for write events
    fd_set except_fds; // Set of file descriptors to monitor for exception events

    fd_set read_fds_copy;   // Copy of read_fds for select
    fd_set write_fds_copy;  // Copy of write_fds for select
    fd_set except_fds_copy; // Copy of except_fds for select
    int max_fd = -1;    // Maximum file descriptor currently being monitored
};
#endif // SELECT_DISPATCHER_H
