#include "dispatcher_epoll.h"
#include "socket.h"
#include "event_dispatcher.h"
#include <stdexcept>
#include <unistd.h>
#include <cstring>
#include <unordered_map> 

class epoll_event_handler : public Socket {
public:
    epoll_event_handler(int port = 8000) : Socket(port) {
        epoll_event_loop = EventLoopFactory::create_event_loop(EventType::Epoll);
        if (!epoll_event_loop) {
            throw std::runtime_error("Failed to create event loop");
        }
    }
    ~epoll_event_handler() override {
        std::cout << "epoll_event_handler destructor called." << std::endl;
        if(epoll_event_loop){
            epoll_event_loop->stop(); // Stop the event loop
        }
        
        for (const auto& pair : client_recv_buffers) {
            if(epoll_event_loop) {
                epoll_event_loop->unregister_handler(pair.first, EventIOType::READ);
            }
        }
        client_recv_buffers.clear();

        if(sockfd >= 0 && epoll_event_loop) {
            epoll_event_loop->unregister_handler(sockfd, EventIOType::READ);
            std::cout << "Socket " << sockfd << " unregistered from event loop." << std::endl;
            close(sockfd); // Close the socket if it was created
        }
    }
    void start() override {
        create_fd();
        set_non_blocking(get_fd()); // 
        std::cout << "Socket started on port " << _port << std::endl;

        epoll_event_loop->register_handler( sockfd, 
                                            EventIOType::READ | EventIOType::EDGE_TRIGGERED, 
                                            std::make_shared<client_event_handler>(
                                                epoll_event_loop.get(), 
                                                [this](int fd) {
                                                    while (true) {
                                                        int client_fd = accept(fd, nullptr, nullptr);
                                                        if (client_fd < 0) {
                                                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                                break;
                                                            }
                                                            perror("accept error");
                                                            return;
                                                        }
                                                        set_non_blocking(client_fd); 

                                                        epoll_event_loop->register_handler(client_fd, 
                                                                                        EventIOType::READ | EventIOType::EDGE_TRIGGERED, 
                                                                                        std::make_shared<client_event_handler>(
                                                                                            epoll_event_loop.get(), 
                                                                                            [this](int client_fd_to_handle) {
                                                                                                clientconnections(client_fd_to_handle);
                                                                                            }));
                                                        client_recv_buffers[client_fd] = "";
                                                    }
                                                }
                                            )
                                        );
        epoll_event_loop->loop(); // Start the event loop
    }
private:
    std::unique_ptr<Eventloop> epoll_event_loop; 
    std::unordered_map<int, std::string> client_recv_buffers; 

    void clientconnections(int client_fd) {
        auto it = client_recv_buffers.find(client_fd);
        if (it == client_recv_buffers.end()) {
            std::cerr << "Error: No buffer found for client_fd " << client_fd << std::endl;
            epoll_event_loop->unregister_handler(client_fd, EventIOType::READ);
            return;
        }
        std::string& current_buffer = it->second;

        char buffer_chunk[4096]; 
        while (true) {
            ssize_t bytes_read = read(client_fd, buffer_chunk, sizeof(buffer_chunk));
            if (bytes_read > 0) {
                current_buffer.append(buffer_chunk, bytes_read);
                size_t header_end_pos = current_buffer.find("\r\n\r\n");
                if (header_end_pos != std::string::npos) {
                    const char* response = 
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html\r\n"
                        "Content-Length: 13\r\n"
                        "Connection: close\r\n" 
                        "\r\n"
                        "Hello, World!";
                    int bytes_written = write(client_fd, response, strlen(response));
                    if (bytes_written < 0) {
                        perror("write error");
                    }
                    epoll_event_loop->unregister_handler(client_fd, EventIOType::READ);
                    client_recv_buffers.erase(client_fd); 
                    break; 
                }

            } else if (bytes_read == 0) {
                epoll_event_loop->unregister_handler(client_fd, EventIOType::READ);
                client_recv_buffers.erase(client_fd); 
                break; 
            } else { // bytes_read < 0
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break; 
                } else {
                    perror("read error");
                    epoll_event_loop->unregister_handler(client_fd, EventIOType::READ);
                    client_recv_buffers.erase(client_fd); 
                    break; 
                }
            }
        }
    }
};