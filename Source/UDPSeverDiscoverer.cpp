#include "UDPServerDiscoverer.h"


UDPServerDiscoverer::UDPServerDiscoverer(unsigned short port)
    : port_(port), running_(false), new_ip_available_(false)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

UDPServerDiscoverer::~UDPServerDiscoverer()
{
    Stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void UDPServerDiscoverer::Start()
{
    if (running_) return;
    running_ = true;
    worker_thread_ = std::thread(&UDPServerDiscoverer::DiscoveryLoop, this);
}

void UDPServerDiscoverer::Stop()
{
    running_ = false;
    if (worker_thread_.joinable())
    {
        worker_thread_.join();
    }
}

//主循环中调用

void UDPServerDiscoverer::Tick()
{
    std::lock_guard<std::mutex> lock(ip_mutex_);
    if (new_ip_available_ && On_SeverIPCovered)
    {
        On_SeverIPCovered(CoveredSeverIP);
        new_ip_available_ = false;
    }
}

void UDPServerDiscoverer::SetOnSeverIPCovered(std::function<void(const std::string&)> callback)
{
    On_SeverIPCovered = callback;
}

std::string UDPServerDiscoverer::GetLocalIP()
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

        // 排除回环地址
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

            // 排除回环地址和docker等虚拟接口
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

void UDPServerDiscoverer::DiscoveryLoop()
{
    int sock = CreateSocket();
    if (sock < 0) return;

    while (running_)
    {
        // 发送广播
        SendBroadcast(sock);

        // 非阻塞接收
        std::string received_ip = TryReceive(sock);
        if (!received_ip.empty())
        {
            std::lock_guard<std::mutex> lock(ip_mutex_);
            CoveredSeverIP = received_ip;
            new_ip_available_ = true;
        }

        // 等待1秒
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    CloseSocket(sock);
}

int UDPServerDiscoverer::CreateSocket()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    // 设置广播选项
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
        reinterpret_cast<char*>(&broadcast), sizeof(broadcast));

    // 设置非阻塞
#ifdef _WIN32
    unsigned long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
#else
    fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

    // 绑定端口
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(0);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        CloseSocket(sock);
        return -1;
    }

    return sock;
}

void UDPServerDiscoverer::SendBroadcast(int sock)
{
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    // 获取本机IP并构造消息
    std::string localIP = GetLocalIP();
    if (localIP.empty()) {
        // 获取失败时使用默认消息
        const char* msg = "request connect:unknown";
        sendto(sock, msg, strlen(msg), 0,
            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
    else {
        // 构造指定格式的消息
        std::string msg = "request connect:" + localIP;
        sendto(sock, msg.c_str(), msg.length(), 0,
            reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
}

std::string UDPServerDiscoverer::TryReceive(int sock)
{
    char buffer[1024];
    sockaddr_in from;
    socklen_t from_len = sizeof(from);

    int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
        reinterpret_cast<sockaddr*>(&from), &from_len);

    if (received > 0)
    {
        buffer[received] = '\0';
        std::string msg(buffer);

        // 检查消息格式
        const std::string prefix = "severIP:";
        if (msg.find(prefix) == 0)
        {
            return msg.substr(prefix.length());
        }
    }
    return "";
}

void UDPServerDiscoverer::CloseSocket(int sock)
{
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}
