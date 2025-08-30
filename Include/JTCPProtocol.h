#pragma once
#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <queue>
#include <memory>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

// 包类型枚举
enum class PacketType : uint8_t
{
    MESSAGE = 0x01,
    FILE_CHUNK = 0x02,
    // 可以在此添加其他类型
};

// 协议常量
constexpr size_t MAX_PACKET_SIZE = 1024 * 1024; // 最大包大小 1MB
constexpr size_t FILE_CHUNK_SIZE = 8192;        // 文件块大小 8KB

// 包头部结构
#pragma pack(push, 1)
struct PacketHeader
{
    uint32_t packet_id;      // 包ID
    PacketType type;          // 包类型
    uint32_t data_size;       // 数据部分大小
    uint32_t total_packets;   // 总包数（用于文件）
    uint32_t packet_index;    // 当前包索引（用于文件）
    // 可以在此添加其他元数据字段

    // 转换为网络字节序
    void toNetworkOrder()
    {
        packet_id = htonl(packet_id);
        data_size = htonl(data_size);
        total_packets = htonl(total_packets);
        packet_index = htonl(packet_index);
    }

    // 转换为主机字节序
    void toHostOrder()
    {
        packet_id = ntohl(packet_id);
        data_size = ntohl(data_size);
        total_packets = ntohl(total_packets);
        packet_index = ntohl(packet_index);
    }
};
#pragma pack(pop)

// 包数据接口
class IPacketData
{
public:
    virtual ~IPacketData() = default;
    virtual PacketType getType() const = 0;
    virtual std::vector<uint8_t> serialize() const = 0;
    virtual bool deserialize(const std::vector<uint8_t>& data) = 0;
};

// 消息包数据
class MessagePacketData : public IPacketData
{
public:
    MessagePacketData() = default;
    MessagePacketData(const std::string& message) : message(message) {}

    PacketType getType() const override { return PacketType::MESSAGE; }

    std::vector<uint8_t> serialize() const override
    {
        std::vector<uint8_t> data(message.begin(), message.end());
        return data;
    }

    bool deserialize(const std::vector<uint8_t>& data) override
    {
        message.assign(data.begin(), data.end());
        return true;
    }

    const std::string& getMessage() const { return message; }

private:
    std::string message;
};

// 文件块包数据
class FileChunkPacketData : public IPacketData
{
public:
    FileChunkPacketData() = default;
    FileChunkPacketData(uint32_t fileId, uint32_t chunkIndex,
        uint32_t totalChunks, const std::vector<uint8_t>& chunkData)
        : file_id(fileId), chunk_index(chunkIndex),
        total_chunks(totalChunks), chunk_data(chunkData) {}

    PacketType getType() const override { return PacketType::FILE_CHUNK; }

    std::vector<uint8_t> serialize() const override;

    bool deserialize(const std::vector<uint8_t>& data) override;

    uint32_t getFileId() const { return file_id; }
    uint32_t getChunkIndex() const { return chunk_index; }
    uint32_t getTotalChunks() const { return total_chunks; }
    const std::vector<uint8_t>& getChunkData() const { return chunk_data; }

private:
    uint32_t file_id;
    uint32_t chunk_index;
    uint32_t total_chunks;
    std::vector<uint8_t> chunk_data;
};

// 待发送的包
struct PendingPacket
{
    std::shared_ptr<IPacketData> data;
    uint32_t packet_id;
    uint8_t priority;

    PendingPacket(std::shared_ptr<IPacketData> data, uint32_t id, uint8_t prio)
        : data(data), packet_id(id), priority(prio) {}

    // 优先级比较函数
    bool operator<(const PendingPacket& other) const {
        return priority < other.priority;
    }
};

// TCP通信协议类
class JTCPProtocol
{
private:
    // 文件重组状态结构
    struct FileReassembly {
        uint32_t file_id;
        uint32_t total_chunks;
        uint32_t received_count = 0;
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> received_flags;

        FileReassembly(uint32_t id, uint32_t total)
            : file_id(id), total_chunks(total) {
            chunks.resize(total);
            received_flags.resize(total, false);
        }

        bool isComplete() const {
            return received_count == total_chunks;
        }

        std::vector<uint8_t> assemble() const {
            std::vector<uint8_t> full_data;
            for (const auto& chunk : chunks) {
                full_data.insert(full_data.end(), chunk.begin(), chunk.end());
            }
            return full_data;
        }
    };

    std::mutex reassembly_mutex;
    std::unordered_map<uint32_t, FileReassembly> file_reassembly_map;
public:
    using SendFunction = std::function<int(const uint8_t* data, size_t length)>;
    using ReceiveCallback = std::function<void(PacketType type, std::shared_ptr<IPacketData> data)>;

    JTCPProtocol(SendFunction sender)
        : send_function(sender), next_packet_id(1), is_running(false) {}

    ~JTCPProtocol()
    {

    }

    // 设置接收回调
    void setReceiveCallback(ReceiveCallback callback)
    {
        receive_callback = callback;
    }

    // 外部调用驱动发送和接收循环
    void WorkTick();

    // 添加要发送的消息
    void sendMessage(const std::string& message, uint8_t priority = 0);

    // 添加要发送的文件
    void sendFile(const std::string& file_path, uint8_t priority = 0);

    // 处理接收到的数据
    void onDataReceived(const uint8_t* data, size_t length);

private:
    // 添加包到发送队列
    void addPacketToQueue(std::shared_ptr<IPacketData> data, uint8_t priority);

    // 处理接收到的数据
    void ProcessReceivedData();

    // 发送包
    bool sendPacket(const PendingPacket& packet);

    // 处理发送
    void ProcessSendData();

    SendFunction send_function;
    ReceiveCallback receive_callback;

    std::atomic<bool> is_running;

    // 发送相关
    std::mutex send_mutex;
    std::priority_queue<PendingPacket> send_queue;
    uint32_t next_packet_id;

    // 文件相关
    std::mutex file_mutex;
    uint32_t next_file_id = 1;

    // 接收相关
    std::mutex receive_mutex;
    std::vector<uint8_t> receive_buffer;
};
