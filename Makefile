# Makefile for Concurrency Server Models

CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -g -O0 -pthread
TARGET = concurrency_server
SOURCES = main.cpp event_dispatcher.cpp
HEADERS = socket.h \
		  singlesocket.h \
		  multi_socket.h \
		  multi_thread.h \
		  process_pool.h \
		  process_pool_1.h \
		  thread_pool.h \
		  select_server.h \
		  dispatcher_select.h \
		  event_dispatcher.h \
		  epoll_server.h \
		  dispatcher_epoll.h \
		  lead_follow.h

# 检测操作系统
UNAME_S := $(shell uname -s)

    CXXFLAGS += -DLINUX


.PHONY: all clean test help

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

# 测试不同的服务器模型
test-single: $(TARGET)
	./$(TARGET) single_process 8080

test-multi-process: $(TARGET)
	./$(TARGET) multi_process 8080

test-multi-thread: $(TARGET)
	./$(TARGET) multi_thread 8080

test-process-pool1: $(TARGET)
	./$(TARGET) process_pool1 8080

test-process-pool2: $(TARGET)
	./$(TARGET) process_pool2 8080

test-thread-pool: $(TARGET)
	./$(TARGET) thread_pool 8080

test-leader-follower: $(TARGET)
	./$(TARGET) leader_follower 8080

test-select: $(TARGET)
	./$(TARGET) select 8080

test-poll: $(TARGET)
	./$(TARGET) poll 8080

test-epoll: $(TARGET)
	./$(TARGET) epoll 8080

test-kqueue: $(TARGET)
	./$(TARGET) kqueue 8080

test-reactor: $(TARGET)
	./$(TARGET) reactor 8080

test-coroutine: $(TARGET)
	./$(TARGET) coroutine 8080

test-work-stealing: $(TARGET)
	./$(TARGET) work_stealing 8080

test-actor: $(TARGET)
	./$(TARGET) actor 8080

test-fiber: $(TARGET)
	./$(TARGET) fiber 8080

test-producer-consumer: $(TARGET)
	./$(TARGET) producer_consumer 8080

test-half-sync-async: $(TARGET)
	./$(TARGET) half_sync_async 8080

test-proactor: $(TARGET)
	./$(TARGET) proactor 8080

test-pipeline: $(TARGET)
	./$(TARGET) pipeline 8080

test-hybrid: $(TARGET)
	./$(TARGET) hybrid 8080

# 测试所有模型
test-all: test-single

# 性能测试（需要安装ab工具）
bench: $(TARGET)
	@echo "Starting server in background..."
	./$(TARGET) thread_pool 8080 &
	@sleep 2
	@echo "Running benchmark..."
	ab -n 1000 -c 10 http://localhost:8080/
	@echo "Stopping server..."
	pkill -f $(TARGET)

help:
	@echo "Available targets:"
	@echo "  all              - Build the server"
	@echo "  clean            - Remove built files"
	@echo "  test-<model>     - Test specific server model"
	@echo "  bench            - Run performance benchmark"
	@echo "  help             - Show this help"
	@echo ""
	@echo "Available server models:"
	@echo "  single_process, multi_process, multi_thread"
	@echo "  process_pool1, process_pool2, thread_pool"
	@echo "  leader_follower, select, poll, epoll, kqueue"
	@echo "  reactor, coroutine"
	@echo ""
	@echo "Usage: ./$(TARGET) <model> [port]"
	@echo "Example: ./$(TARGET) thread_pool 8080"