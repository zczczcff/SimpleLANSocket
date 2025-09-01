#pragma once
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <set>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

class UDPClientReciver 
{
public:
    UDPClientReciver(unsigned short port)
        : port_(port), running_(false), new_ip_available_(false) 
    {
#ifdef _WIN32
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
        serverIP_ = GetLocalIP(); // ��ʼ��ʱ��ȡ����IP
    }

    ~UDPClientReciver() 
    {
        Stop();
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void Start() 
    {
        if (running_) return;
        running_ = true;
        worker_thread_ = std::thread(&UDPClientReciver::DiscoveryLoop, this);
    }

    void Stop() 
    {
        running_ = false;
        if (worker_thread_.joinable()) 
        {
            worker_thread_.join();
        }
    }

    // ���߳������Ե���
    void Tick() 
    {
        std::lock_guard<std::mutex> lock(ip_mutex_);
        if (new_ip_available_ && On_NewClientIP) 
        {
            for (const auto& ip : new_ips_) 
            {
                On_NewClientIP(ip); // ����ÿ����IP�Ļص�
            }
            new_ips_.clear();
            new_ip_available_ = false;
        }
    }

    // �����¿ͻ���IP�Ļص�
    void SetOnNewClientIP(std::function<void(const std::string&)> callback) 
    {
        On_NewClientIP = callback;
    }

private:
    unsigned short port_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
    std::mutex ip_mutex_;
    std::set<std::string> known_ips_;  // ������֪IP
    std::set<std::string> new_ips_;    // ����IP����ʱ�洢��
    std::atomic<bool> new_ip_available_;
    std::function<void(const std::string&)> On_NewClientIP;
    std::string serverIP_;  // ����IP

    // ��ȡ����IP����ƽ̨��
    std::string GetLocalIP() 
    {
#ifdef _WIN32
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)))
        {
            return "";
        }

        struct hostent* host = gethostbyname(hostname);
        if (!host) return "";

        for (int i = 0; host->h_addr_list[i] != 0; ++i)
        {
            struct in_addr addr;
            memcpy(&addr, host->h_addr_list[i], sizeof(struct in_addr));
            std::string ip = inet_ntoa(addr);

            // �ų��ػ���ַ
            if (ip.substr(0, 3) != "127")
            {
                return ip;
            }
        }
#else
        struct ifaddrs* ifap;
        if (getifaddrs(&ifap) != 0) return "";

        for (struct ifaddrs* ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;

            if (ifa->ifa_addr->sa_family == AF_INET) {
                void* tmpAddr = &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr;
                char ipBuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, tmpAddr, ipBuf, INET_ADDRSTRLEN);

                // �ų��ػ���ַ��docker������ӿ�
                std::string ip(ipBuf);
                if (ip != "127.0.0.1" &&
                    std::string(ifa->ifa_name).find("lo") == std::string::npos &&
                    std::string(ifa->ifa_name).find("docker") == std::string::npos)
                {
                    freeifaddrs(ifap);
                    return ip;
                }
            }
        }
        freeifaddrs(ifap);
#endif
        return "";
    }

    void DiscoveryLoop() 
    {
        int sock = CreateSocket();
        if (sock < 0) return;

        while (running_) 
        {
            ProcessIncoming(sock); // �������
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        CloseSocket(sock);
    }

    int CreateSocket() 
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) return -1;

        // ����㲥�Ͷ˿ڸ���
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<char*>(&opt), sizeof(opt));
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<char*>(&opt), sizeof(opt));
#endif

        // �󶨶˿�
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            CloseSocket(sock);
            return -1;
        }

        // ������ģʽ
#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket(sock, FIONBIO, &nonblocking);
#else
        fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

        return sock;
    }

    void ProcessIncoming(int sock) 
    {
        char buffer[1024];
        sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int len = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
            reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (len <= 0) return;

        buffer[len] = '\0';
        std::string msg(buffer);

        // �����Ϣ��ʽ
        const std::string prefix = "request connect:";
        if (msg.find(prefix) != 0) return;

        // ��ȡ�ͻ���IP������Ϣ��socket��ַ��
        std::string client_ip = inet_ntoa(client_addr.sin_addr);

        // �ظ�������IP
        std::string response = "severIP:" + serverIP_;
        sendto(sock, response.c_str(), response.size(), 0,
            reinterpret_cast<sockaddr*>(&client_addr), addr_len);

        // ������IP
        {
            std::lock_guard<std::mutex> lock(ip_mutex_);
            if (known_ips_.find(client_ip) == known_ips_.end()) 
            {
                known_ips_.insert(client_ip);
                new_ips_.insert(client_ip);
                new_ip_available_ = true;
            }
        }
    }

    void CloseSocket(int sock) 
    {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }
};