#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstring>
#include <chrono>
#include <condition_variable>

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

class UDPServerDiscoverer 
{

private:
    unsigned short port_;
    std::atomic<bool> running_;
    std::thread worker_thread_;
    std::mutex ip_mutex_;
    std::string CoveredSeverIP;
    std::atomic<bool> new_ip_available_;
    std::function<void(const std::string&)> On_SeverIPCovered;
public:
    UDPServerDiscoverer(unsigned short port);

    ~UDPServerDiscoverer();

    void Start();

    void Stop();
    //主循环中调用
    void Tick();

    void SetOnSeverIPCovered(std::function<void(const std::string&)> callback);

    //std::string GetServerIP() 
    //{
    //    std::lock_guard<std::mutex> lock(ip_mutex_);
    //    return CoveredSeverIP;
    //}

private:

    std::string GetLocalIP();

    void DiscoveryLoop();

    int CreateSocket();

    void SendBroadcast(int sock);

    std::string TryReceive(int sock);

    void CloseSocket(int sock);
};