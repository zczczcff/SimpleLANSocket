#include <iostream>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <cstring>
#include <vector>
#include <memory>
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

class JostickTcpClient
{
public:
    // 回调函数类型定义
    using MessageCallback = std::function<void(const std::string& message)>;
    using FileCallback = std::function<void(uint32_t fileId, const std::vector<uint8_t>& fileData)>;
    using FileProgressCallback = std::function<void(uint32_t fileId, uint32_t currentChunk, uint32_t totalChunks)>;
    using DisconnectCallback = std::function<void()>;

private:
    // 网络通信参数
    std::string server_ip_;
    int server_port_;
    int socket_;

    // 双缓冲管理
    struct ReceivedData {
        enum class Type { MESSAGE, FILE } type;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };

    std::queue<ReceivedData>* recv_buffers[2];
    std::queue<std::string>* send_buffers[2];  // 只发送文本消息

    // 当前使用的缓冲区指针
    std::queue<ReceivedData>* current_recv_buffer;
    std::queue<ReceivedData>* free_recv_buffer;
    std::queue<std::string>* current_send_buffer;
    std::queue<std::string>* free_send_buffer;

    // 线程控制
    std::atomic<bool> running_;
    std::thread work_thread_;

    // 协议对象
    std::unique_ptr<JTCPProtocol> protocol_;

    // 同步机制
    std::mutex recv_mutex_;
    std::mutex send_mutex_;
    std::condition_variable cv_;

    // 回调函数
    MessageCallback on_message_;
    FileCallback on_file_;
    FileProgressCallback on_file_progress_;
    DisconnectCallback on_disconnect_;

    std::atomic<bool> disconnected_flag_{ false };

public:
    /**
     * @param ip 服务器IP地址
     * @param port 服务器端口
     */
    JostickTcpClient(const std::string& ip, int port);

    ~JostickTcpClient();

    // 设置回调函数
    void SetOnMessage(MessageCallback callback) { on_message_ = callback; }
    void SetOnFile(FileCallback callback) { on_file_ = callback; }
    void SetOnFileProgress(FileProgressCallback callback) { on_file_progress_ = callback; }
    void SetOnDisconnect(DisconnectCallback callback) { on_disconnect_ = callback; }

    // 发送文本消息
    void SendMsg(const std::string& msg);

    // 连接到服务器
    bool Connect();

    // 断开连接
    void Disconnect();

    // 主线程调用：交换缓冲并处理接收数据
    void Tick();

    std::string GetServerIP() const { return server_ip_; }
    int GetServerPort() const { return server_port_; }

private:
    // 创建协议对象
    void CreateProtocol();

    // 工作线程函数
    void WorkerThread();

    // 处理发送数据
    void ProcessSending();

    // 检查socket连接状态
    bool IsSocketConnected() const;

    // 原始数据发送函数
    bool SendRaw(uint8_t* data, uint16_t len);

    // 设置socket非阻塞
    void SetNonBlocking(int sock);

    // 关闭socket
    void CloseSocket();
};