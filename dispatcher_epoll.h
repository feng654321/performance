#ifndef EPOLL_DISPATCHER_H
#define EPOLL_DISPATCHER_H

#include <signal.h>
#include <iostream>
#include <cstdlib>
#include <errno.h>
#include "event_dispatcher.h"
#include <sys/epoll.h>
#include <sys/eventfd.h>
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
#include <span>


class dispatcherepoll : public Eventloop {
public:
    explicit dispatcherepoll(int max_events = 1024) 
                : max_events_(max_events),
                  events(max_events),
                  loop_running(true),
                  epoll_fd_(create_epoll_fd()),
                  wakeup_fd_(create_wakeup_fd()) {

        register_wakeup_handler();
    }
    ~dispatcherepoll() {
        std::cout << "dispatcherepoll destructor called." << std::endl;
    }
    void close_fd_safely(int fd) override {
        pending_close_fds_.emplace_back(fd);
    }
    void register_handler(int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler) override {
        pending_operations_.emplace(PendingOperation::Type::REGISTER, fd, event_type, handler);
        wakeup(); // Wake up the event loop to process pending operations
    }
    void unregister_handler(int fd, EventIOType event_type) override {
        pending_operations_.emplace(PendingOperation::Type::UNREGISTER, fd, event_type, nullptr);
        wakeup(); // Wake up the event loop to process pending operations
    }
    void loop() override {
        while (loop_running) {

            int num_events = epoll_wait(epoll_fd_.get(), events.data(), max_events_, -1);
            if (num_events < 0) {
                if (errno == EINTR) {
                    // Interrupted by a signal, continue the loop
                    continue;
                }
                perror("epoll_wait error");
                continue; // Handle error and continue the loop
            }

            // Process active events
            dispatch_active_events(num_events);
            // Process pending close file descriptors
            process_pending_close_fds();
        }
    }
    void stop() override {
        loop_running = false;
        std::cout << "Stopping dispatcherepoll event loop." << std::endl;
    }
private:
    static int create_epoll_fd() {
        int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd < 0) {
            perror("epoll_create1");
            throw std::system_error(errno, std::generic_category(), "Failed to create epoll instance");
        }
        return epoll_fd;
    }
    static int create_wakeup_fd() {
        int wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wakeup_fd < 0) {
            perror("eventfd");
            throw std::system_error(errno, std::generic_category(), "Failed to create eventfd");
        }
        return wakeup_fd;
    }
    void register_wakeup_handler() {
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // Edge-triggered read event
        ev.data.ptr = this; // Store the pointer to the dispatcher itself
        if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wakeup_fd_.get(), &ev) < 0) {
            perror("epoll_ctl MOD wakeup_fd");
            throw std::system_error(errno, std::generic_category(), "Failed to re-register wakeup handler");
        }
    }
    void wakeup() {
        uint64_t u = 1;
        if (write(wakeup_fd_.get(), &u, sizeof(u)) != sizeof(u)) {
            perror("write to wakeup_fd");
        }
    }
    void handle_wakeup() {
        uint64_t u;
        while (read(wakeup_fd_.get(), &u, sizeof(u)) == sizeof(u))
        {
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("read from wakeup_fd");
        }

        //create a temp taskes to swap pending_operation
        std::queue<PendingOperation> temp_queue;
        {
            std::lock_guard<std::mutex> lock(pending_op_mutex_);
            std::swap(pending_operations_, temp_queue);
        }
        while (!temp_queue.empty()) {
            PendingOperation op = std::move(temp_queue.front());
            temp_queue.pop();
            if (op.type == PendingOperation::Type::REGISTER) {
                do_register_handler(op.fd, op.event_type, op.handler);
            } else if (op.type == PendingOperation::Type::UNREGISTER) {
                do_unregister_handler(op.fd, op.event_type);
            }
        }
    }
    uint32_t convert_to_epoll_events(EventIOType type) {
        uint32_t epoll_events = 0;

        if (has_event(type, EventIOType::READ)) {
            epoll_events |= EPOLLIN;
        }
        if (has_event(type, EventIOType::WRITE)) {
            epoll_events |= EPOLLOUT;
        }
        if (has_event(type, EventIOType::EXCEPTION)) {
            epoll_events |= EPOLLERR | EPOLLHUP;
        }
        if (has_event(type, EventIOType::EDGE_TRIGGERED)) {
            epoll_events |= EPOLLET; // Edge-triggered mode
        }
        if (has_event(type, EventIOType::HANGUP)) {
            epoll_events |= EPOLLHUP; // Hangup event
        }

        return epoll_events;
    }

    void do_register_handler(int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler) {
        if (fd < 0 || !handler) {
            throw std::invalid_argument("Invalid file descriptor or handler");
        }
        struct epoll_event ev;
        ev.events = convert_to_epoll_events(event_type);
        ev.data.fd = fd;

        int op = EPOLL_CTL_ADD;
        if (active_handlers_by_fd_.find(fd) != active_handlers_by_fd_.end() &&
            active_handlers_by_fd_[fd] == handler) {
            // If the handler already exists, modify it
            op = EPOLL_CTL_MOD;
        }

        if (epoll_ctl(epoll_fd_.get(), op, fd, &ev) < 0) {
            if(op == EPOLL_CTL_ADD && errno == EEXIST) {
                if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
                    // If it still fails, throw an error
                    perror("epoll_ctl MOD");
                    throw std::system_error(errno, std::generic_category(), "Failed to modify existing handler");
                }
            } else {
                perror("epoll_ctl ADD");
                throw std::system_error(errno, std::generic_category(), "Failed to register handler");
            }
        }
        // Store the handler in the active handlers map
        active_handlers_by_fd_[fd] = handler;
    }

    void do_unregister_handler(int fd, EventIOType event_type) {
        if (fd < 0) {
            throw std::invalid_argument("Invalid file descriptor");
        }
        if (active_handlers_by_fd_.find(fd) == active_handlers_by_fd_.end()) {
            std::cerr << "No handler registered for fd: " << fd << std::endl;
            return; // No handler to unregister
        }
        if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr) < 0) {
            perror("epoll_ctl DEL");
            throw std::system_error(errno, std::generic_category(), "Failed to unregister handler");
        }
        // Remove the handler from the active handlers map
        auto it = active_handlers_by_fd_.find(fd);
        if (it != active_handlers_by_fd_.end()) {
            active_handlers_by_fd_.erase(it);
        } else {
            std::cerr << "No handler found for fd: " << fd << std::endl;
        }
        // Add the fd to the pending close list
        pending_close_fds_.emplace_back(fd);
    }

    void process_pending_close_fds() {
        for (int fd : pending_close_fds_) {
            if (close(fd) < 0) {
                perror("close");
            }
        }
        pending_close_fds_.clear();
    }

    void dispatch_active_events(int num_events) {
        if (num_events <= 0) {
            std::cout << "No active events detected." << std::endl;
            return; // No active events, continue the loop
        }
        std::span<struct epoll_event> events_span(events.data(), num_events);

        for (const auto& event : events_span) {
            if(event.data.ptr == this){
                // Handle wakeup event
                handle_wakeup();
                continue; // Skip processing this event
            }else{
                auto it = active_handlers_by_fd_.find(event.data.fd);
                if (it != active_handlers_by_fd_.end()) {
                    EventHandler* handler = it->second.get();
                    if (handler) {
                        if (event.events & EPOLLIN) {
                            handler->handle_read(event.data.fd);
                        }
                        if (event.events & EPOLLOUT) {
                            handler->handle_write(event.data.fd);
                        }
                        if (event.events & (EPOLLERR | EPOLLHUP)) {
                            handler->handle_exception(event.data.fd);
                        }
                    }
                } else {
                    std::cerr << "No handler found for fd: " << event.data.fd << std::endl;
                }
            }
        }
    }

    struct PendingOperation {
        enum class Type { REGISTER, UNREGISTER };
        int fd; // File descriptor
        EventIOType event_type; // Event type
        std::shared_ptr<EventHandler> handler; // Event handler
        Type type; // Type of operation (register or unregister)
        PendingOperation(Type type, int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler)
            : type(type), fd(fd), event_type(event_type), handler(std::move(handler)) {
            if (fd < 0) {
                throw std::invalid_argument("File descriptor cannot be negative");
            }
        }
        PendingOperation() = default; // Default constructor for empty initialization
    };
    FileDescriptor epoll_fd_;
    FileDescriptor wakeup_fd_;
    int max_events_; // Maximum number of events to handle at once
    std::vector<struct epoll_event> events; // Vector to hold events from epoll
    std::queue<PendingOperation> pending_operations_;
    std::mutex pending_op_mutex_;
    std::unordered_map<int, std::shared_ptr<EventHandler>> active_handlers_by_fd_;

    std::vector<int> pending_close_fds_; // Vector to hold file descriptors to be closed
    bool loop_running = true; // Flag to control the event loop
};

#endif // EPOLL_DISPATCHER_H