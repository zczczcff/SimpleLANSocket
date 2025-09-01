// JostickTCPServer.cpp
#include "JostickTCPServer.h"
#include "JTCPProtocol.h"

// --- JostickTcpSession 实现 ---
JostickTcpSession::JostickTcpSession(JostickTcpServer* server, int socket,
    JostickTcpServer::ClientID clientId)
    : server_(server), socket_(socket), client_id_(clientId) {

    // 初始化协议对象
    auto sender = [this](const uint8_t* data, size_t len) -> int {
        return SendRaw(const_cast<uint8_t*>(data), static_cast<uint16_t>(len)) ? len : -1;
    };

    protocol_ = std::make_unique<JTCPProtocol>(sender);

    // 设置协议回调
    protocol_->setReceiveCallback([this](PacketType type, auto data_ptr) {
        std::lock_guard<std::mutex> lock(recv_mutex_);

        switch (type) {
        case PacketType::MESSAGE: {
            auto msg_data = dynamic_cast<MessagePacketData*>(data_ptr.get());
            if (msg_data) {
                recv_queue_.push({
                    ReceivedData::Type::MESSAGE,
                    msg_data->getMessage(),
                    0, {}
                    });
            }
            break;
        }
        case PacketType::FILE_CHUNK: {
            auto file_data = dynamic_cast<FileChunkPacketData*>(data_ptr.get());
            if (file_data) {
                // 文件进度回调
                if (server_->on_file_progress_) {
                    server_->on_file_progress_(
                        client_id_,
                        file_data->getFileId(),
                        file_data->getChunkIndex() + 1,
                        file_data->getTotalChunks()
                    );
                }

                // 完整文件接收
                if (file_data->getChunkIndex() == 0 &&
                    file_data->getTotalChunks() == 1)
                {
                    recv_queue_.push({
                        ReceivedData::Type::FILE,
                        "",
                        file_data->getFileId(),
                        file_data->getFileName(),
                        file_data->getChunkData()
                        });
                }
            }
            break;
        }
        }
        });
}

JostickTcpSession::~JostickTcpSession()
{
}

void JostickTcpSession::Start() {
    if (running_) return;

    running_ = true;
    connected_ = true;
    work_thread_ = std::thread(&JostickTcpSession::WorkerThread, this);
}

void JostickTcpSession::Stop() {
    if (running_) {
        running_ = false;
        connected_ = false;
        send_cv_.notify_one();
        if (work_thread_.joinable()) {
            work_thread_.join();
        }
    }

#ifdef _WIN32
    closesocket(socket_);
#else
    close(socket_);
#endif
    socket_ = -1;
}

bool JostickTcpSession::SendMessage(const std::string& message) {
    if (!connected_) return false;

    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue_.push(message);
    }
    send_cv_.notify_one();
    return true;
}

void JostickTcpSession::WorkerThread() {
    while (running_) {
        // 1. 处理接收
        char buffer[1024];
        int bytes_received = recv(socket_, buffer, sizeof(buffer), 0);

        if (bytes_received > 0) {
            protocol_->onDataReceived(
                reinterpret_cast<uint8_t*>(buffer),
                bytes_received
            );
            ProcessReceivedData();
        }
        else if (bytes_received == 0) {
            // 连接断开
            connected_ = false;
            server_->AddEvent({
                JostickTcpServer::ServerEvent::Type::DISCONNECT,
                client_id_
                });
            break;
        }
        else 
        {
#ifdef _WIN32
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
				connected_ = false;
				server_->AddEvent({
					JostickTcpServer::ServerEvent::Type::DISCONNECT,
					client_id_
					});
				break;
			}
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                connected_ = false;
                server_->AddEvent({
                    ServerEvent::Type::DISCONNECT,
                    client_id_
                    });
                break;
            }
#endif
        }

        // 2. 处理发送
        ProcessSending();

        // 3. 短暂休眠
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void JostickTcpSession::ProcessReceivedData() {
    std::queue<ReceivedData> process_queue;
    {
        std::lock_guard<std::mutex> lock(recv_mutex_);
        std::swap(process_queue, recv_queue_);
    }

    while (!process_queue.empty()) {
        auto& data = process_queue.front();

        switch (data.type) {
        case ReceivedData::Type::MESSAGE:
            server_->AddEvent({
                JostickTcpServer::ServerEvent::Type::MESSAGE,
                client_id_,
                data.message
                });
            break;
        case ReceivedData::Type::FILE:
            server_->AddEvent({
                JostickTcpServer::ServerEvent::Type::FILE,
                client_id_,
                "",
                data.fileId,
                data.fileName,
                data.fileData
                });
            break;
        }

        process_queue.pop();
    }
}

void JostickTcpSession::ProcessSending() {
    std::queue<std::string> send_queue;
    {
        std::unique_lock<std::mutex> lock(send_mutex_);
        if (send_queue_.empty()) {
            send_cv_.wait_for(lock, std::chrono::milliseconds(10));
        }
        std::swap(send_queue, send_queue_);
    }

    while (!send_queue.empty() && connected_) {
        auto& message = send_queue.front();
        protocol_->sendMessage(message);
        send_queue.pop();
    }
}

bool JostickTcpSession::SendRaw(uint8_t* data, uint16_t len)
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

// --- JostickTcpServer 实现 ---
JostickTcpServer::JostickTcpServer(int port)
    : listen_port_(port), listen_socket_(-1) {

    // 初始化双缓冲
    event_buffers[0] = new std::queue<ServerEvent>();
    event_buffers[1] = new std::queue<ServerEvent>();
    current_event_buffer = event_buffers[0];
    free_event_buffer = event_buffers[1];
}

JostickTcpServer::~JostickTcpServer() {
    Stop();

    delete event_buffers[0];
    delete event_buffers[1];
}

bool JostickTcpServer::Start(int worker_threads) {
    if (running_) return false;

    // 初始化Winsock (Windows only)
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
    }
#endif

    // 创建监听socket
    listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_ < 0) {
        std::cerr << "Socket creation failed" << std::endl;
        return false;
    }

    // 设置SO_REUSEADDR
    int opt = 1;
#ifdef _WIN32
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    // 绑定地址
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port_);

    if (bind(listen_socket_, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Bind failed" << std::endl;
        CloseSocket(listen_socket_);
        return false;
    }

    // 开始监听
    if (listen(listen_socket_, SOMAXCONN) < 0) {
        std::cerr << "Listen failed" << std::endl;
        CloseSocket(listen_socket_);
        return false;
    }

    // 设置非阻塞
    SetNonBlocking(listen_socket_);

    // 启动线程
    running_ = true;
    accept_thread_ = std::thread(&JostickTcpServer::AcceptThread, this);

    for (int i = 0; i < worker_threads; ++i) {
        worker_threads_.emplace_back(&JostickTcpServer::WorkerThread, this);
    }

    return true;
}

void JostickTcpServer::Stop() {
    if (running_) {
        running_ = false;

        // 关闭所有客户端会话
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [id, session] : sessions_) {
                session->Stop();
            }
            sessions_.clear();
        }

        // 停止线程
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        for (auto& thread : worker_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        worker_threads_.clear();

        // 关闭监听socket
        CloseSocket(listen_socket_);

#ifdef _WIN32
        WSACleanup();
#endif
    }
}

void JostickTcpServer::Tick() {
    // 交换事件缓冲
    std::queue<ServerEvent>* event_swap = nullptr;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        event_swap = current_event_buffer;
        current_event_buffer = free_event_buffer;
        free_event_buffer = event_swap;
    }

    // 处理事件
    while (!event_swap->empty()) {
        auto& event = event_swap->front();

        switch (event.type) {
        case ServerEvent::Type::MESSAGE:
            if (on_message_) {
                on_message_(event.clientId, event.message);
            }
            break;
        case ServerEvent::Type::FILE:
            if (on_file_) {
                on_file_(event.clientId, event.fileId, event.fileName, event.fileData);
            }
            break;
        case ServerEvent::Type::CONNECT:
            if (on_new_connection_) {
                on_new_connection_(event.clientId);
            }
            break;
        case ServerEvent::Type::DISCONNECT:
            if (on_disconnect_) {
                on_disconnect_(event.clientId);
            }
            break;
        }

        event_swap->pop();
    }
}

void JostickTcpServer::AcceptThread() {
    while (running_) {
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

#ifdef _WIN32
        SOCKET client_socket = accept(listen_socket_, (sockaddr*)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                // 错误处理
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
#else
        int client_socket = accept(listen_socket_, (sockaddr*)&client_addr, &addr_len);
        if (client_socket < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // 错误处理
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
#endif

        // 设置新socket为非阻塞
        SetNonBlocking(client_socket);

        // 创建新会话
        ClientID clientId = next_client_id_++;
        auto session = std::make_shared<JostickTcpSession>(this, client_socket, clientId);

        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_[clientId] = session;
        }

        // 启动会话
        session->Start();

        // 添加连接事件
        AddEvent({
            ServerEvent::Type::CONNECT,
            clientId
            });
    }
}

void JostickTcpServer::WorkerThread() {
    while (running_) {
        // 定期检查所有客户端状态
        std::vector<ClientID> to_remove;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [id, session] : sessions_) {
                if (!session->IsConnected()) {
                    to_remove.push_back(id);
                }
            }
        }

        for (auto id : to_remove) {
            DisconnectClient(id);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void JostickTcpServer::SendToClient(ClientID clientId, const std::string& message) {
    std::shared_ptr<JostickTcpSession> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(clientId);
        if (it != sessions_.end()) {
            session = it->second;
        }
    }

    if (session && session->IsConnected()) {
        session->SendMessage(message);
    }
}

void JostickTcpServer::Broadcast(const std::string& message) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, session] : sessions_) {
        if (session->IsConnected()) {
            session->SendMessage(message);
        }
    }
}

void JostickTcpServer::DisconnectClient(ClientID clientId) {
    std::shared_ptr<JostickTcpSession> session;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(clientId);
        if (it != sessions_.end()) {
            session = it->second;
            sessions_.erase(it);
        }
    }

    if (session) {
        session->Stop();
        AddEvent({
            ServerEvent::Type::DISCONNECT,
            clientId
            });
    }
}

void JostickTcpServer::AddEvent(ServerEvent&& event) {
    std::lock_guard<std::mutex> lock(event_mutex_);
    current_event_buffer->push(std::move(event));
}

void JostickTcpServer::SetNonBlocking(int sock) {
#ifdef _WIN32
    unsigned long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

void JostickTcpServer::CloseSocket(int sock) {
    if (sock >= 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }
}