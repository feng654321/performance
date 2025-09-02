#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm> // For std::min

#include <liburing.h>     // io_uring library
#include <sys/socket.h>   // For socket, bind, listen, accept
#include <netinet/in.h>   // For sockaddr_in
#include <arpa/inet.h>    // For inet_ntoa
#include <unistd.h>       // For close, dup2
#include <fcntl.h>        // For fcntl, O_NONBLOCK
#include <errno.h>        // For errno, strerror
#include <sys/eventfd.h>  // For eventfd()
#include <sys/epoll.h>    // For epoll

// --- 配置参数 ---
#define QUEUE_DEPTH 256        // io_uring 队列深度
#define MAX_CONNECTIONS 1024   // 最大连接数
#define BUFFER_SIZE 4096       // 每个连接的读写缓冲区大小
#define LISTEN_PORT 8080       // 服务器监听端口
#define EPOLL_MAX_EVENTS 64    // epoll_wait 一次最多返回的事件数

// --- io_uring 相关宏和辅助函数 ---
#define CHECK_LIBURING_ERROR(res, msg) \
    if (res < 0) { \
        fprintf(stderr, "%s failed: %s (errno: %d)\n", msg, strerror(-res), -res); \
        exit(EXIT_FAILURE); \
    }

// 定义请求类型
enum RequestType {
    ACCEPT, // For new connections
    READ,   // For reading data from a client
    WRITE,  // For writing data to a client
    // No specific CLOSE type needed for io_uring here, as close is handled manually or implicitly.
    // For file updates etc., the user_data can be null.
};

// 自定义用户数据结构，关联请求和其类型
struct UserData {
    RequestType type;
    int client_fd; // 实际的客户端文件描述符
    int file_idx;  // 在 io_uring 注册文件数组中的索引
    int buffer_idx; // 对应的缓冲区 ID (如果使用固定缓冲区)
    // 可以添加其他需要的信息，比如读写的偏移量、数据长度等
};

// --- 全局变量和数据结构 ---
struct io_uring ring;
int listen_fd = -1;
int event_fd = -1; // For io_uring completion notifications
int epoll_fd = -1; // For epoll event loop

// 客户端文件描述符到其在注册文件数组中的索引的映射
std::map<int, int> client_fd_to_idx;
// 注册文件数组的副本，方便管理
std::vector<int> registered_fds(MAX_CONNECTIONS + 1, -1); // 0 reserved for listen_fd

// 预分配的固定缓冲区
std::vector<char> global_fixed_buffer_storage;
// 存储每个连接的iovec，用于读写操作
std::vector<struct iovec> registered_iovecs(MAX_CONNECTIONS, {nullptr, 0});

// 跟踪可用和已用文件索引
std::vector<bool> file_idx_in_use(MAX_CONNECTIONS + 1, false); // 0 for listen_fd
std::vector<bool> buffer_idx_in_use(MAX_CONNECTIONS, false);

// --- 函数声明 ---
void setup_listening_socket();
void init_io_uring();
void register_resources();
void setup_eventfd_and_epoll();
void start_accept_request();
void queue_read_request(int client_fd, int file_idx, int buffer_idx);
void queue_write_request(int client_fd, int file_idx, int buffer_idx, int bytes_to_write);
void handle_io_uring_completions();
void handle_new_connection_ready();
void handle_accept_completion(struct io_uring_cqe *cqe, UserData *data);
void handle_read_completion(struct io_uring_cqe *cqe, UserData *data);
void handle_write_completion(struct io_uring_cqe *cqe, UserData *data);
void close_client_connection(int client_fd);
int get_free_file_idx();
void release_file_idx(int idx);
int get_free_buffer_idx();
void release_buffer_idx(int idx);

// --- 主函数 ---
int main() {
    setup_listening_socket();
    init_io_uring();
    register_resources();
    setup_eventfd_and_epoll();

    // 初始提交一个 accept 请求
    start_accept_request();
    io_uring_submit(&ring); // 提交初始的 accept 请求

    std::cout << "TCP server listening on port " << LISTEN_PORT << " with io_uring/epoll..." << std::endl;

    struct epoll_event events[EPOLL_MAX_EVENTS];

    while (true) {
        int num_events = epoll_wait(epoll_fd, events, EPOLL_MAX_EVENTS, -1); // 阻塞等待事件
        if (num_events < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].data.fd == listen_fd) {
                // 监听套接字可读，表示有新连接
                handle_new_connection_ready();
            } else if (events[i].data.fd == event_fd) {
                // eventfd 可读，表示 io_uring 有完成事件
                handle_io_uring_completions();
            }
        }
    }

    // 清理
    io_uring_queue_exit(&ring);
    close(listen_fd);
    close(event_fd);
    close(epoll_fd);
    return 0;
}

// --- 函数实现 ---

void setup_listening_socket() {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // 将监听套接字设置为非阻塞模式 (虽然 io_uring accept 本身是异步的，但习惯上还是设置)
    if (fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(LISTEN_PORT);

    if (bind(listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(listen_fd, 512) < 0) {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
}

void init_io_uring() {
    int ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    CHECK_LIBURING_ERROR(ret, "io_uring_queue_init");
}

void register_resources() {
    // 1. 注册缓冲区
    global_fixed_buffer_storage.resize(MAX_CONNECTIONS * BUFFER_SIZE);
    registered_iovecs.resize(MAX_CONNECTIONS);

    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        registered_iovecs[i].iov_base = global_fixed_buffer_storage.data() + (long)i * BUFFER_SIZE;
        registered_iovecs[i].iov_len = BUFFER_SIZE;
    }

    int ret = io_uring_register_buffers(&ring, registered_iovecs.data(), MAX_CONNECTIONS);
    CHECK_LIBURING_ERROR(ret, "io_uring_register_buffers");
    std::cout << "Registered " << MAX_CONNECTIONS << " fixed buffers." << std::endl;

    // 2. 注册文件 (包括监听fd和为客户端fd预留的槽位)
    registered_fds[0] = listen_fd;
    file_idx_in_use[0] = true;

    ret = io_uring_register_files(&ring, registered_fds.data(), MAX_CONNECTIONS + 1);
    CHECK_LIBURING_ERROR(ret, "io_uring_register_files");
    std::cout << "Registered " << (MAX_CONNECTIONS + 1) << " fixed file descriptors." << std::endl;
}

void setup_eventfd_and_epoll() {
    // 创建 eventfd
    event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd < 0) {
        perror("eventfd");
        io_uring_queue_exit(&ring);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    std::cout << "Eventfd created (FD: " << event_fd << ")." << std::endl;

    // 注册 eventfd 到 io_uring
    int ret = io_uring_register_eventfd(&ring, event_fd);
    CHECK_LIBURING_ERROR(ret, "io_uring_register_eventfd");
    std::cout << "Eventfd registered to io_uring." << std::endl;

    // 创建 epoll 实例
    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(event_fd);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    std::cout << "Epoll instance created (FD: " << epoll_fd << ")." << std::endl;

    // 将监听套接字添加到 epoll (关注可读事件)
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event);
    if (ret < 0) {
        perror("epoll_ctl add listen_fd");
        close(epoll_fd);
        close(event_fd);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    std::cout << "Listen FD " << listen_fd << " added to epoll." << std::endl;

    // 将 eventfd 添加到 epoll (关注可读事件)
    event.events = EPOLLIN;
    event.data.fd = event_fd;
    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, event_fd, &event);
    if (ret < 0) {
        perror("epoll_ctl add event_fd");
        close(epoll_fd);
        close(event_fd);
        io_uring_queue_exit(&ring);
        close(listen_fd);
        exit(EXIT_FAILURE);
    }
    std::cout << "Eventfd " << event_fd << " added to epoll." << std::endl;
}

int get_free_file_idx() {
    for (int i = 1; i <= MAX_CONNECTIONS; ++i) { // 从 1 开始，0 留给 listen_fd
        if (!file_idx_in_use[i]) {
            file_idx_in_use[i] = true;
            return i;
        }
    }
    return -1; // 没有空闲索引
}

void release_file_idx(int idx) {
    if (idx > 0 && idx <= MAX_CONNECTIONS) { // 确保不是 listen_fd
        // 标记为 -1，并通过 io_uring 更新注册文件表
        registered_fds[idx] = -1;
        file_idx_in_use[idx] = false;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
        if (sqe) {
            int update_fd = -1; // 标记为移除
            io_uring_prep_files_update(sqe, &update_fd, 1, idx);
            sqe->flags |= IOSQE_F_ASYNC; // 异步更新
            io_uring_submit(&ring); // 提交更新
        } else {
            fprintf(stderr, "Warning: Failed to get SQE for file unregister update (idx %d). Resource cleanup might be delayed.\n", idx);
        }
    }
}

int get_free_buffer_idx() {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (!buffer_idx_in_use[i]) {
            buffer_idx_in_use[i] = true;
            return i;
        }
    }
    return -1; // 没有空闲索引
}

void release_buffer_idx(int idx) {
    if (idx >= 0 && idx < MAX_CONNECTIONS) {
        buffer_idx_in_use[idx] = false;
    }
}

void start_accept_request() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "No SQE available for accept request. This should not happen if QUEUE_DEPTH is sufficient.\n");
        // 如果这里没有SQE，通常表示队列已满，但我们会在epoll_wait之后立即提交
        // 且accept是持续提交的，所以应该总能获得SQE。
        // 极端情况下可能需要更复杂的流控
        return;
    }

    io_uring_prep_accept(sqe, listen_fd, nullptr, nullptr, 0); // 不阻塞，但需要epoll通知
    
    // 为这个请求分配一个用户数据
    UserData *data = new UserData();
    data->type = ACCEPT;
    data->client_fd = -1;
    data->file_idx = -1;
    data->buffer_idx = -1;
    io_uring_sqe_set_data(sqe, data);
}

void queue_read_request(int client_fd, int file_idx, int buffer_idx) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "No SQE available for read request (client_fd: %d). Closing connection.\n", client_fd);
        close_client_connection(client_fd);
        return;
    }

    io_uring_prep_read_fixed(sqe, file_idx, registered_iovecs[buffer_idx].iov_base, BUFFER_SIZE, 0, buffer_idx);
    sqe->flags |= IOSQE_FIXED_FILE; // 标记使用固定文件

    UserData *data = new UserData();
    data->type = READ;
    data->client_fd = client_fd;
    data->file_idx = file_idx;
    data->buffer_idx = buffer_idx;
    io_uring_sqe_set_data(sqe, data);
}

void queue_write_request(int client_fd, int file_idx, int buffer_idx, int bytes_to_write) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe) {
        fprintf(stderr, "No SQE available for write request (client_fd: %d). Closing connection.\n", client_fd);
        close_client_connection(client_fd);
        return;
    }

    io_uring_prep_write_fixed(sqe, file_idx, registered_iovecs[buffer_idx].iov_base, bytes_to_write, 0, buffer_idx);
    sqe->flags |= IOSQE_FIXED_FILE; // 标记使用固定文件

    UserData *data = new UserData();
    data->type = WRITE;
    data->client_fd = client_fd;
    data->file_idx = file_idx;
    data->buffer_idx = buffer_idx;
    io_uring_sqe_set_data(sqe, data);
}

// 处理来自 io_uring 的所有完成事件
void handle_io_uring_completions() {
    uint64_t val;
    // 从eventfd读取，清空计数器。即使io_uring写了多次，这里也只关心有事件发生
    ssize_t bytes_read = read(event_fd, &val, sizeof(val));
    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 理论上不会发生，因为epoll_wait已经表明可读
            fprintf(stderr, "Eventfd read EAGAIN/EWOULDBLOCK, continuing...\n");
            return;
        }
        perror("read eventfd");
        exit(EXIT_FAILURE);
    }
    // std::cout << "Eventfd triggered! Value: " << val << ". Processing io_uring completions." << std::endl;

    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring, cqe) {
        count++;
        UserData *data = static_cast<UserData*>(io_uring_cqe_get_data(cqe));
        if (!data) continue; // 可能是一些内部完成事件，或者没有user_data的请求

        if (cqe->res < 0 && cqe->res != -ECANCELED) {
            fprintf(stderr, "Error in completion for type %d (fd %d): %s (errno: %d)\n",
                    data->type, data->client_fd, strerror(-cqe->res), -cqe->res);
            if (data->type != ACCEPT) {
                close_client_connection(data->client_fd);
            }
        } else {
            switch (data->type) {
                case ACCEPT:
                    handle_accept_completion(cqe, data);
                    // 重新提交 accept 请求 (io_uring_submit 会在后面集中处理)
                    start_accept_request();
                    break;
                case READ:
                    handle_read_completion(cqe, data);
                    break;
                case WRITE:
                    handle_write_completion(cqe, data);
                    break;
            }
        }
        delete data; // 释放 user_data 内存
    }
    io_uring_cq_advance(&ring, count); // 标记所有已处理的 CQE 为已消费

    // 在处理完所有完成事件后，提交任何新排队的请求
    // 比如 handle_accept_completion 中新提交的 read/write 请求
    // 以及 start_accept_request 提交的新 accept 请求
    io_uring_submit(&ring);
}

// 处理来自 epoll 的监听套接字可读事件
void handle_new_connection_ready() {
    // 监听套接字可读通常意味着有新的连接，但 io_uring 的 accept 是异步的
    // 所以这里无需直接调用 accept(2)，而是确认 io_uring 已经处理了 accept 完成事件
    // 并排队了新的 accept 请求。
    // 如果epoll通知监听套接字可读，而io_uring的accept没有立即完成，
    // 意味着io_uring队列可能被占满或存在其他问题。
    // 最简单的方式是确保 start_accept_request 被持续调用，
    // 并且 io_uring_submit 会及时提交这些请求。
    // 对于这个设计，epoll通知仅仅是告诉我们，我们应该确保有 pending 的 accept 请求。
    // 如果没有，应该立即提交一个。
    // 通常，我们通过 start_accept_request 持续提交 accept，所以这里不需要额外操作。
    // 这个函数只是作为 epoll 事件的一个触发点，实际的 accept 处理在 io_uring 完成事件中。
}

void handle_accept_completion(struct io_uring_cqe *cqe, UserData *data) {
    int client_fd = cqe->res;
    if (client_fd <= 0) {
        std::cerr << "Accept failed or returned invalid FD: " << client_fd << std::endl;
        return;
    }

    // 将新接受的客户端FD设置为非阻塞
    if (fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK for client_fd");
        close(client_fd);
        return;
    }

    int file_idx = get_free_file_idx();
    if (file_idx == -1) {
        std::cerr << "Max connections reached, rejecting new client_fd: " << client_fd << std::endl;
        close(client_fd);
        return;
    }

    int buffer_idx = get_free_buffer_idx();
    if (buffer_idx == -1) {
        std::cerr << "No free buffer for client_fd: " << client_fd << ". Rejecting." << std::endl;
        release_file_idx(file_idx);
        close(client_fd);
        return;
    }

    // 更新注册文件表中的对应槽位
    registered_fds[file_idx] = client_fd;
    // 提交文件注册更新请求到io_uring，让内核知道这个FD的索引
    struct io_uring_sqe *sqe_update = io_uring_get_sqe(&ring);
    if (!sqe_update) {
        fprintf(stderr, "Failed to get SQE for file update during accept. Client %d may not be registered correctly.\n", client_fd);
        release_file_idx(file_idx);
        release_buffer_idx(buffer_idx);
        close(client_fd);
        return;
    }
    int new_fd_for_update[1] = {client_fd};
    io_uring_prep_files_update(sqe_update, new_fd_for_update, 1, file_idx);
    sqe_update->flags |= IOSQE_F_ASYNC; // 异步更新，不阻塞
    // 提交更新将在 handle_io_uring_completions() 最后的 io_uring_submit(&ring) 统一处理


    client_fd_to_idx[client_fd] = file_idx;
    client_fd_to_buffer_id[client_fd] = buffer_idx; // 这里 client_fd_to_buffer_id 是 map<int, int>

    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    getpeername(client_fd, (struct sockaddr*)&peer_addr, &peer_addr_len);
    std::cout << "New connection accepted: FD " << client_fd
              << " (registered at file_idx " << file_idx << ")"
              << " from " << inet_ntoa(peer_addr.sin_addr)
              << ":" << ntohs(peer_addr.sin_port) << std::endl;

    // 接受新连接后，立即为它排队一个读请求
    queue_read_request(client_fd, file_idx, buffer_idx);
}

void handle_read_completion(struct io_uring_cqe *cqe, UserData *data) {
    int client_fd = data->client_fd;
    int buffer_idx = data->buffer_idx;
    int bytes_read = cqe->res;
    int file_idx = data->file_idx; // 从 UserData 中获取文件索引

    if (bytes_read <= 0) {
        if (bytes_read == 0) {
            std::cout << "Client FD " << client_fd << " closed connection gracefully." << std::endl;
        } else {
            std::cerr << "Read error on client FD " << client_fd << ": " << strerror(-bytes_read) << std::endl;
        }
        close_client_connection(client_fd);
        return;
    }

    // std::cout << "Read " << bytes_read << " bytes from client FD " << client_fd << std::endl;

    // 将读取到的数据回显回去
    // 数据在 registered_iovecs[buffer_idx].iov_base 中
    queue_write_request(client_fd, file_idx, buffer_idx, bytes_read);
}

void handle_write_completion(struct io_uring_cqe *cqe, UserData *data) {
    int client_fd = data->client_fd;
    int buffer_idx = data->buffer_idx;
    int bytes_written = cqe->res;
    int file_idx = data->file_idx; // 从 UserData 中获取文件索引

    if (bytes_written < 0) {
        std::cerr << "Write error on client FD " << client_fd << ": " << strerror(-bytes_written) << std::endl;
        close_client_connection(client_fd);
        return;
    }

    // std::cout << "Written " << bytes_written << " bytes to client FD " << client_fd << std::endl;

    // 写入完成后，再次为该连接排队一个读请求，以继续处理数据
    queue_read_request(client_fd, file_idx, buffer_idx);
}

void close_client_connection(int client_fd) {
    if (client_fd == -1) return;

    // 1. 从管理数据结构中移除
    int file_idx = -1;
    auto it_fd_idx = client_fd_to_idx.find(client_fd);
    if (it_fd_idx != client_fd_to_idx.end()) {
        file_idx = it_fd_idx->second;
        client_fd_to_idx.erase(it_fd_idx);
    }

    int buffer_idx = -1;
    auto it_fd_buf_idx = client_fd_to_buffer_id.find(client_fd);
    if (it_fd_buf_idx != client_fd_to_buffer_id.end()) {
        buffer_idx = it_fd_buf_idx->second;
        client_fd_to_buffer_id.erase(it_fd_buf_idx);
    }

    // 2. 释放文件索引和缓冲区索引
    if (file_idx != -1) {
        release_file_idx(file_idx);
    }
    if (buffer_idx != -1) {
        release_buffer_idx(buffer_idx);
    }

    // 3. 关闭实际文件描述符
    close(client_fd);
    std::cout << "Client FD " << client_fd << " disconnected and resources released." << std::endl;
}