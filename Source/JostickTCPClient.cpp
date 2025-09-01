#include "JostickTCPClient.h"

/**
* @param ip 服务器IP地址
* @param port 服务器端口
*/

JostickTcpClient::JostickTcpClient(const std::string& ip, int port)
    : server_ip_(ip),
    server_port_(port),
    running_(false),
    socket_(-1)
{
    // 初始化双缓冲
    recv_buffers[0] = new std::queue<ReceivedData>();
    recv_buffers[1] = new std::queue<ReceivedData>();
    send_buffers[0] = new std::queue<std::string>();
    send_buffers[1] = new std::queue<std::string>();

    current_recv_buffer = recv_buffers[0];
    free_recv_buffer = recv_buffers[1];
    current_send_buffer = send_buffers[0];
    free_send_buffer = send_buffers[1];
}

JostickTcpClient::~JostickTcpClient()
{
    Disconnect();

    // 清理双缓冲
    delete recv_buffers[0];
    delete recv_buffers[1];
    delete send_buffers[0];
    delete send_buffers[1];
}

// 发送文本消息

void JostickTcpClient::SendMsg(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(send_mutex_);
    free_send_buffer->push(msg);
}

void JostickTcpClient::SendFile(const std::string& file_path)
{
    std::lock_guard<std::mutex> lock(file_send_mutex_);
    file_send_queue_.push(file_path);
}

// 连接到服务器

bool JostickTcpClient::Connect()
{
    if (running_)
    {
        return false;
    }

    // 创建socket
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0)
    {
        std::cerr << "Socket creation failed" << std::endl;
        return false;
    }

    // 设置服务器地址
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port_);
    if (inet_pton(AF_INET, server_ip_.c_str(), &server_addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid address" << std::endl;
        CloseSocket();
        return false;
    }

    // 建立连接
    if (connect(socket_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cerr << "Connection failed" << std::endl;
        CloseSocket();
        return false;
    }

    // 设置非阻塞模式
    SetNonBlocking(socket_);

    // 创建协议对象
    CreateProtocol();

    // 启动工作线程
    running_ = true;
    work_thread_ = std::thread(&JostickTcpClient::WorkerThread, this);

    return true;
}

// 断开连接

void JostickTcpClient::Disconnect()
{
    if (running_)
    {
        running_ = false;
        if (work_thread_.joinable())
        {
            work_thread_.join();
        }
    }
    CloseSocket();
}

// 主线程调用：交换缓冲并处理接收数据

void JostickTcpClient::Tick()
{
    // 处理可能的断开连接事件
    if (disconnected_flag_)
    {
        disconnected_flag_ = false;
        if (on_disconnect_)
        {
            on_disconnect_();
        }
    }

    // 交换接收缓冲
    std::queue<ReceivedData>* recv_swap = nullptr;
    {
        std::lock_guard<std::mutex> recv_lock(recv_mutex_);
        recv_swap = current_recv_buffer;
        current_recv_buffer = free_recv_buffer;
        free_recv_buffer = recv_swap;
    }

    // 处理接收缓冲中的数据
    while (!recv_swap->empty())
    {
        auto& data = recv_swap->front();

        switch (data.type) {
        case ReceivedData::Type::MESSAGE:
            if (on_message_) {
                on_message_(data.message);
            }
            break;
        case ReceivedData::Type::FILE:
            if (on_file_) {
                on_file_(data.fileId, data.fileData);
            }
            break;
        }

        recv_swap->pop();
    }

    // 交换发送缓冲
    std::queue<std::string>* send_swap = nullptr;
    {
        std::lock_guard<std::mutex> send_lock(send_mutex_);
        send_swap = current_send_buffer;
        current_send_buffer = free_send_buffer;
        free_send_buffer = send_swap;
    }
}

// 创建协议对象

void JostickTcpClient::CreateProtocol()
{
    // 发送函数适配器
    auto sender = [this](const uint8_t* data, size_t len) -> int {
        return SendRaw(const_cast<uint8_t*>(data), static_cast<uint16_t>(len)) ? len : -1;
    };

    protocol_ = std::make_unique<JTCPProtocol>(sender);

    // 设置协议接收回调
    protocol_->setReceiveCallback([this](PacketType type, auto data_ptr) {
        std::lock_guard<std::mutex> lock(recv_mutex_);

        switch (type) {
        case PacketType::MESSAGE: {
            auto msg_data = dynamic_cast<MessagePacketData*>(data_ptr.get());
            if (msg_data) {
                current_recv_buffer->push({
                    ReceivedData::Type::MESSAGE,
                    msg_data->getMessage(),
                    0,{}
                    });
            }
            break;
        }
        case PacketType::FILE_CHUNK: {
            auto file_data = dynamic_cast<FileChunkPacketData*>(data_ptr.get());
            if (file_data) {
                // 文件进度回调（实时调用，不放入缓冲）
                if (on_file_progress_) {
                    on_file_progress_(
                        file_data->getFileId(),
                        file_data->getChunkIndex() + 1,
                        file_data->getTotalChunks()
                    );
                }

                // 完整文件接收（由协议层保证）
                if (file_data->getChunkIndex() == 0 &&
                    file_data->getTotalChunks() == 1)
                {
                    current_recv_buffer->push({
                        ReceivedData::Type::FILE,
                        "",
                        file_data->getFileId(),
                        file_data->getChunkData()
                        });
                }
            }
            break;
        }
        }
        });
}

// 工作线程函数

void JostickTcpClient::WorkerThread()
{
    bool was_connected = true;

    while (running_)
    {
        // 1. 接收网络数据
        char buffer[1024];
        int bytes_received = recv(socket_, buffer, sizeof(buffer), 0);

        if (bytes_received > 0) {
            protocol_->onDataReceived(
                reinterpret_cast<uint8_t*>(buffer),
                bytes_received
            );
        }
        else if (bytes_received == 0) {
            // 连接已关闭
            was_connected = false;
        }
        else {
            // 错误处理
#ifdef _WIN32
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                was_connected = false;
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                was_connected = false;
            }
#endif
        }

        // 2. 驱动协议处理
        protocol_->WorkTick();

        // 3. 处理发送队列
        ProcessSendMsg();
        ProcessSendFile();

        // 4. 检查连接状态
        if (was_connected && !IsSocketConnected()) {
            was_connected = false;
            disconnected_flag_ = true;
        }

        // 短暂休眠
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 处理发送数据

void JostickTcpClient::ProcessSendMsg()
{
    std::queue<std::string>* send_queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue = current_send_buffer;
    }

    while (!send_queue->empty()) {
        auto& message = send_queue->front();
        protocol_->sendMessage(message);
        send_queue->pop();
    }
}

void JostickTcpClient::ProcessSendFile()
{
    // 处理文件发送请求
    std::unique_lock<std::mutex> file_lock(file_send_mutex_);
    while (!file_send_queue_.empty()) {
        auto file_path = file_send_queue_.front();
        file_send_queue_.pop();

        // 临时释放锁避免阻塞
        file_lock.unlock();

        // 调用协议层发送文件
        protocol_->sendFile(file_path);

        file_lock.lock();
    }
}

// 检查socket连接状态

bool JostickTcpClient::IsSocketConnected() const
{
    if (socket_ < 0) return false;

    char dummy;
#ifdef _WIN32
    WSASetLastError(0);
    if (recv(socket_, &dummy, 1, MSG_PEEK) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        return (err == WSAEWOULDBLOCK);
    }
#else
    errno = 0;
    if (recv(socket_, &dummy, 1, MSG_PEEK | MSG_DONTWAIT) < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK);
    }
#endif
    return true;
}

// 原始数据发送函数

bool JostickTcpClient::SendRaw(uint8_t* data, uint16_t len)
{
    int total_sent = 0;
    bool success = true;

    while (total_sent < len && running_ && success)
    {
        int sent = send(socket_, reinterpret_cast<char*>(data + total_sent),
            len - total_sent, 0);

        if (sent > 0) {
            total_sent += sent;
        }
#ifdef _WIN32
        else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            success = false;
        }
#else
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else {
            success = false;
        }
#endif
    }

    return success && (total_sent == len);
}

// 设置socket非阻塞

void JostickTcpClient::SetNonBlocking(int sock)
{
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// 关闭socket

void JostickTcpClient::CloseSocket()
{
    if (socket_ >= 0)
    {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = -1;
    }
}
