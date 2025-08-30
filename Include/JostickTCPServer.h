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

class JostickTcpSession;  // ǰ������

class JostickTcpServer {
public:
    friend class JostickTcpSession;
    using ClientID = uint64_t;
    using MessageCallback = std::function<void(ClientID clientId, const std::string& message)>;
    using FileCallback = std::function<void(ClientID clientId, uint32_t fileId, const std::vector<uint8_t>& fileData)>;
    using FileProgressCallback = std::function<void(ClientID clientId, uint32_t fileId, uint32_t currentChunk, uint32_t totalChunks)>;
    using NewConnectionCallback = std::function<void(ClientID clientId)>;
    using DisconnectCallback = std::function<void(ClientID clientId)>;
    // ˫������ն���
    struct ServerEvent 
    {
        enum class Type { MESSAGE, FILE, CONNECT, DISCONNECT } type;
        ClientID clientId;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };
private:
    // ����������
    int listen_port_;
    int listen_socket_;

    std::queue<ServerEvent>* event_buffers[2];
    std::queue<ServerEvent>* current_event_buffer;
    std::queue<ServerEvent>* free_event_buffer;

    // �ͻ��˹���
    std::atomic<bool> running_;
    std::unordered_map<ClientID, std::shared_ptr<JostickTcpSession>> sessions_;
    std::mutex sessions_mutex_;

    // �߳̿���
    std::thread accept_thread_;
    std::vector<std::thread> worker_threads_;

    // �ص�����
    MessageCallback on_message_;
    FileCallback on_file_;
    FileProgressCallback on_file_progress_;
    NewConnectionCallback on_new_connection_;
    DisconnectCallback on_disconnect_;

    // ͬ������
    std::mutex event_mutex_;
    std::condition_variable cv_;

    // �ͻ���ID������
    std::atomic<ClientID> next_client_id_{ 1 };

public:
    explicit JostickTcpServer(int port);
    ~JostickTcpServer();

    // ���ûص�����
    void SetOnMessage(MessageCallback callback) { on_message_ = callback; }
    void SetOnFile(FileCallback callback) { on_file_ = callback; }
    void SetOnFileProgress(FileProgressCallback callback) { on_file_progress_ = callback; }
    void SetOnNewConnection(NewConnectionCallback callback) { on_new_connection_ = callback; }
    void SetOnDisconnect(DisconnectCallback callback) { on_disconnect_ = callback; }

    // ����������
    bool Start(int worker_threads = 4);

    // ֹͣ������
    void Stop();

    // ���̵߳��ã������¼�
    void Tick();

    // ���ض��ͻ��˷�����Ϣ
    void SendToClient(ClientID clientId, const std::string& message);

    // �㲥��Ϣ�����пͻ���
    void Broadcast(const std::string& message);

    // �Ͽ��ض��ͻ���
    void DisconnectClient(ClientID clientId);

private:
    // ���������ӵ��߳�
    void AcceptThread();

    // �����̺߳���
    void WorkerThread();

    // ����¼�������
    void AddEvent(ServerEvent&& event);

    // ����socket������
    void SetNonBlocking(int sock);

    // �ر�socket
    void CloseSocket(int sock);
};

// �ͻ��˻Ự��
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
    // �������ݽṹ����ͻ������ƣ�
    struct ReceivedData {
        enum class Type { MESSAGE, FILE } type;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };

    // ���ĳ�Ա
    JostickTcpServer* server_;
    int socket_;
    JostickTcpServer::ClientID client_id_;
    std::atomic<bool> connected_{ false };
    std::atomic<bool> running_{ false };

    // Э�����
    std::unique_ptr<JTCPProtocol> protocol_;

    // ˫����
    std::queue<ReceivedData> recv_queue_;
    std::mutex recv_mutex_;

    // ���Ͷ���
    std::queue<std::string> send_queue_;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;

    // �����߳�
    std::thread work_thread_;

    void WorkerThread();
    void ProcessReceivedData();
    void ProcessSending();

    // ԭʼ���ݷ��ͺ���
    bool SendRaw(uint8_t* data, uint16_t len);
};