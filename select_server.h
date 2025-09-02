#include "dispatcher_select.h"
#include <iostream>
#include <stdexcept>
#include <sys/select.h> 
#include "event_dispatcher.h"


class select_event_handler : public Socket{
public:
    select_event_handler(int port = 8080) : Socket(port) {
        select_event_loop = EventLoopFactory::create_event_loop(EventType::Select);
        if (!select_event_loop) {
            throw std::runtime_error("Failed to create event loop");
        }
        // Constructor implementation
    }

    ~select_event_handler() override {
        // Destructor implementation
        std::cout << "select_event_handler destructor called." << std::endl;
    }

    void start() override {
        create_fd();
        set_non_blocking(get_fd());
        std::cout << "Socket started on port " << _port << std::endl;
        select_event_loop->register_handler(sockfd, 
                                        EventIOType::READ, 
                                        std::make_shared<client_event_handler>(
                                            select_event_loop.get(), 
                                            [this](int fd) {
                                                handle_connections(); // Handle the connection
                                            }));

        select_event_loop->loop(); // Start the event loop
    }


private:
std::unique_ptr<Eventloop> select_event_loop; // Pointer to the select event loop

    void clientconnections(int clientfd) {
        char buffer[1024];
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
                select_event_loop->unregister_handler(clientfd, EventIOType::READ); // Unregister the read handler
                select_event_loop->close_fd_safely(clientfd); // Close the connection after handling
            }
        } else {
            if (bytes_read == 0) {
                std::cout << "Client " << clientfd << " disconnected." << std::endl;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read error");
                }
            }
            select_event_loop->unregister_handler(clientfd, EventIOType::READ);
            select_event_loop->close_fd_safely(clientfd); 
        }
    }

    void handle_connections() {
        int client_fd = accept_connection();
        if (client_fd < 0) {
            std::cerr << "Error accepting connection." << std::endl;
            return; // Continue to accept more connections
        }
        set_non_blocking(client_fd); // Set the client socket to non-blocking mode

        select_event_loop->register_handler(client_fd, 
                                            EventIOType::READ, 
                                            std::make_shared<client_event_handler>(
                                                select_event_loop.get(), 
                                                [this](int client_fd) {
                                                    clientconnections(client_fd); // Handle the connection
                                                }));
    }

};