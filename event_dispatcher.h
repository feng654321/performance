#ifndef DISPATCHER_SELECT_H
#define DISPATCHER_SELECT_H
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
#include <iostream>
#include <functional>


class FileDescriptor {
    private:
        int fd_;
    public:
        explicit FileDescriptor(int fd = -1) : fd_(fd) {}
        ~FileDescriptor() { if (fd_ >= 0) close(fd_); }
        
        FileDescriptor(const FileDescriptor&) = delete;
        FileDescriptor& operator=(const FileDescriptor&) = delete;
        
        FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
            other.fd_ = -1;
        }
        
        int get() const { return fd_; }
        bool valid() const { return fd_ >= 0; }
};

class EventHandler{
public:
    virtual ~EventHandler() = default;
    virtual void handle_read(int fd) = 0; // Handle read events
    virtual void handle_write(int fd) = 0; // Handle write events
    virtual void handle_exception(int fd) = 0; // Handle exception events
};

enum class EventType {
    Select,
    Poll,
    Epoll,
    AUTO
};

enum class EventIOType {
    READ = 0x01, // Read event
    WRITE = 0x02, // Write event
    HANGUP = 0x04, // Hangup event
    EDGE_TRIGGERED = 0x08, // Edge-triggered event
    EXCEPTION = 0x10 // Exception event
};

inline EventIOType operator|(EventIOType lhs, EventIOType rhs) {
    return static_cast<EventIOType>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
inline EventIOType operator&(EventIOType lhs, EventIOType rhs) {
    return static_cast<EventIOType>(static_cast<int>(lhs) & static_cast<int>(rhs));
}
inline bool has_event(EventIOType type, EventIOType event) {
    return (type & event) == event;
}

class Eventloop {
public:
    Eventloop() = default;
    ~Eventloop() = default;
    Eventloop(const Eventloop&) = delete;
    Eventloop& operator=(const Eventloop&) = delete;

    virtual void loop() = 0; // Start the event loop
    virtual void stop() = 0; // Stop the event loop

    virtual void register_handler(int fd, EventIOType event_type, std::shared_ptr<EventHandler> handler) = 0;
    virtual void unregister_handler(int fd, EventIOType event_type) = 0;
    virtual void close_fd_safely(int fd) = 0; // Safely close file descriptor
};

class EventLoopFactory {
public:
    EventLoopFactory() = default;
    ~EventLoopFactory() = default;
    static std::unique_ptr<Eventloop> create_event_loop(EventType type);
};

class client_event_handler : public EventHandler {
public:
    client_event_handler(Eventloop* loop, std::function<void(int)> callback) : event_loop(loop), on_read_callback(callback) {
        if (!event_loop) {
            throw std::runtime_error("Event loop is not initialized");
        }
    }
    void handle_read(int client_fd) override {
        // Implement read handling logic here
        if (on_read_callback) {
            on_read_callback(client_fd); // Call the callback function if set
        }
    }
    void handle_write(int fd) override {
        std::cout << "Handling write event for fd: " << fd << std::endl;
        // Implement write handling logic here
    }
    void handle_exception(int fd) override {
        std::cout << "Handling exception event for fd: " << fd << std::endl;
        event_loop->stop(); // Stop the event loop on exception
        // Implement exception handling logic here
    }
private:
    Eventloop* event_loop;
    std::function<void(int)> on_read_callback; // Callback for read events
};

#endif // DISPATCHER_SELECT_H



