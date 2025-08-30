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
    // �ص��������Ͷ���
    using MessageCallback = std::function<void(const std::string& message)>;
    using FileCallback = std::function<void(uint32_t fileId, const std::vector<uint8_t>& fileData)>;
    using FileProgressCallback = std::function<void(uint32_t fileId, uint32_t currentChunk, uint32_t totalChunks)>;
    using DisconnectCallback = std::function<void()>;

private:
    // ����ͨ�Ų���
    std::string server_ip_;
    int server_port_;
    int socket_;

    // ˫�������
    struct ReceivedData {
        enum class Type { MESSAGE, FILE } type;
        std::string message;
        uint32_t fileId;
        std::vector<uint8_t> fileData;
    };

    std::queue<ReceivedData>* recv_buffers[2];
    std::queue<std::string>* send_buffers[2];  // ֻ�����ı���Ϣ

    // ��ǰʹ�õĻ�����ָ��
    std::queue<ReceivedData>* current_recv_buffer;
    std::queue<ReceivedData>* free_recv_buffer;
    std::queue<std::string>* current_send_buffer;
    std::queue<std::string>* free_send_buffer;

    // �߳̿���
    std::atomic<bool> running_;
    std::thread work_thread_;

    // Э�����
    std::unique_ptr<JTCPProtocol> protocol_;

    // ͬ������
    std::mutex recv_mutex_;
    std::mutex send_mutex_;
    std::condition_variable cv_;

    // �ص�����
    MessageCallback on_message_;
    FileCallback on_file_;
    FileProgressCallback on_file_progress_;
    DisconnectCallback on_disconnect_;

    std::atomic<bool> disconnected_flag_{ false };

public:
    /**
     * @param ip ������IP��ַ
     * @param port �������˿�
     */
    JostickTcpClient(const std::string& ip, int port);

    ~JostickTcpClient();

    // ���ûص�����
    void SetOnMessage(MessageCallback callback) { on_message_ = callback; }
    void SetOnFile(FileCallback callback) { on_file_ = callback; }
    void SetOnFileProgress(FileProgressCallback callback) { on_file_progress_ = callback; }
    void SetOnDisconnect(DisconnectCallback callback) { on_disconnect_ = callback; }

    // �����ı���Ϣ
    void SendMsg(const std::string& msg);

    // ���ӵ�������
    bool Connect();

    // �Ͽ�����
    void Disconnect();

    // ���̵߳��ã��������岢�����������
    void Tick();

    std::string GetServerIP() const { return server_ip_; }
    int GetServerPort() const { return server_port_; }

private:
    // ����Э�����
    void CreateProtocol();

    // �����̺߳���
    void WorkerThread();

    // ����������
    void ProcessSending();

    // ���socket����״̬
    bool IsSocketConnected() const;

    // ԭʼ���ݷ��ͺ���
    bool SendRaw(uint8_t* data, uint16_t len);

    // ����socket������
    void SetNonBlocking(int sock);

    // �ر�socket
    void CloseSocket();
};