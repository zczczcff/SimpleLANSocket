// JostickTCPServer.h
#pragma once

#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <queue>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include "JTCPProtocol.h"

class JostickTcpSession;  // 前向声明

class JostickTcpServer {
public:
    friend class JostickTcpSession;
    using ClientID = uint64_t;
    using MessageCallback = std::function<void(ClientID clientId, const std::string& message)>;
    using FileCallback = std::function<void(ClientID clientId, uint32_t fileId, const std::vector<uint8_t>& fileData)>;
    using FileProgressCallback = std::function<void(ClientID clientId, uint32_t fileId, uint32_t currentChunk, uint32_t totalChunks)>;
    using NewConnectionCallback = std::function<void(ClientID clientId)>;
    using DisconnectCallback = std::function<void(ClientID clientId)>;
    // 双缓冲接收队列
    struct ServerEvent 
    {
        enum class Type { MESSAGE, FILE, CONNECT, DISCONNECT } type;
        ClientID clientId;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };
private:
    // 服务器配置
    int listen_port_;
    int listen_socket_;

    std::queue<ServerEvent>* event_buffers[2];
    std::queue<ServerEvent>* current_event_buffer;
    std::queue<ServerEvent>* free_event_buffer;

    // 客户端管理
    std::atomic<bool> running_;
    std::unordered_map<ClientID, std::shared_ptr<JostickTcpSession>> sessions_;
    std::mutex sessions_mutex_;

    // 线程控制
    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;

    // 回调函数
    MessageCallback on_message_;
    FileCallback on_file_;
    FileProgressCallback on_file_progress_;
    NewConnectionCallback on_new_connection_;
    DisconnectCallback on_disconnect_;

    // 同步机制
    std::mutex event_mutex_;
    std::condition_variable cv_;

    // 客户端ID计数器
    std::atomic<ClientID> next_client_id_{ 1 };

public:
    explicit JostickTcpServer(int port);
    ~JostickTcpServer();

    // 设置回调函数
    void SetOnMessage(MessageCallback callback) { on_message_ = callback; }
    void SetOnFile(FileCallback callback) { on_file_ = callback; }
    void SetOnFileProgress(FileProgressCallback callback) { on_file_progress_ = callback; }
    void SetOnNewConnection(NewConnectionCallback callback) { on_new_connection_ = callback; }
    void SetOnDisconnect(DisconnectCallback callback) { on_disconnect_ = callback; }

    // 启动服务器
    bool Start(int worker_threads = 4);

    // 停止服务器
    void Stop();

    // 主线程调用：处理事件
    void Tick();

    // 向特定客户端发送消息
    void SendToClient(ClientID clientId, const std::string& message);

    // 广播消息给所有客户端
    void Broadcast(const std::string& message);

    // 断开特定客户端
    void DisconnectClient(ClientID clientId);

private:
    // 接受新连接的线程
    void AcceptThread();

    // 工作线程函数
    void WorkerThread();

    // 添加事件到缓冲
    void AddEvent(ServerEvent&& event);

    // 设置socket非阻塞
    void SetNonBlocking(int sock);

    // 关闭socket
    void CloseSocket(int sock);
};

// 客户端会话类
class JostickTcpSession : public std::enable_shared_from_this<JostickTcpSession> {
public:
    
    using SendFunction = std::function<bool(uint8_t* data, uint16_t len)>;

    JostickTcpSession(JostickTcpServer* server, int socket, JostickTcpServer::ClientID clientId);
    ~JostickTcpSession();

    void Start();
    void Stop();
    bool SendMessage(const std::string& message);

    JostickTcpServer::ClientID GetClientId() const { return client_id_; }
    bool IsConnected() const { return connected_; }

private:
    // 接收数据结构（与客户端类似）
    struct ReceivedData {
        enum class Type { MESSAGE, FILE } type;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };

    // 核心成员
    JostickTcpServer* server_;
    int socket_;
    JostickTcpServer::ClientID client_id_;
    std::atomic<bool> connected_{ false };
    std::atomic<bool> running_{ false };

    // 协议对象
    std::unique_ptr<JTCPProtocol> protocol_;

    // 双缓冲
    std::queue<ReceivedData> recv_queue_;
    std::mutex recv_mutex_;

    // 发送队列
    std::queue<std::string> send_queue_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;

    // 工作线程
    std::thread work_thread_;

    void WorkerThread();
    void ProcessReceivedData();
    void ProcessSending();

    // 原始数据发送函数
    bool SendRaw(uint8_t* data, uint16_t len);
};