#include "JTCPProtocol.h"

std::vector<uint8_t> FileChunkPacketData::serialize() const
{
    std::vector<uint8_t> data;
    // 添加文件ID
    uint32_t netFileId = htonl(file_id);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netFileId),
        reinterpret_cast<const uint8_t*>(&netFileId) + sizeof(netFileId));
    // 添加块索引
    uint32_t netChunkIndex = htonl(chunk_index);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netChunkIndex),
        reinterpret_cast<const uint8_t*>(&netChunkIndex) + sizeof(netChunkIndex));
    // 添加总块数
    uint32_t netTotalChunks = htonl(total_chunks);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netTotalChunks),
        reinterpret_cast<const uint8_t*>(&netTotalChunks) + sizeof(netTotalChunks));
    // 添加块数据
    data.insert(data.end(), chunk_data.begin(), chunk_data.end());
    return data;
}

bool FileChunkPacketData::deserialize(const std::vector<uint8_t>& data)
{
    if (data.size() < 12) return false; // 文件ID+块索引+总块数 = 12字节

    size_t offset = 0;
    // 读取文件ID
    memcpy(&file_id, data.data() + offset, sizeof(file_id));
    file_id = ntohl(file_id);
    offset += sizeof(file_id);

    // 读取块索引
    memcpy(&chunk_index, data.data() + offset, sizeof(chunk_index));
    chunk_index = ntohl(chunk_index);
    offset += sizeof(chunk_index);

    // 读取总块数
    memcpy(&total_chunks, data.data() + offset, sizeof(total_chunks));
    total_chunks = ntohl(total_chunks);
    offset += sizeof(total_chunks);

    // 读取块数据
    chunk_data.assign(data.begin() + offset, data.end());
    return true;
}

// 外部调用驱动发送和接收循环

void JTCPProtocol::WorkTick()
{
    // 处理接收数据
    ProcessReceivedData();

    ProcessSendData();
}

// 添加要发送的消息

void JTCPProtocol::sendMessage(const std::string& message, uint8_t priority)
{
    auto packet_data = std::make_shared<MessagePacketData>(message);
    addPacketToQueue(packet_data, priority);
}

// 添加要发送的文件

void JTCPProtocol::sendFile(const std::string& file_path, uint8_t priority)
{
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(file_mutex);
    uint32_t file_id = next_file_id++;

    // 获取文件大小
    size_t file_size = file.tellg();
    file.seekg(0);

    // 计算需要多少块
    uint32_t total_chunks = (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;

    // 读取并添加所有文件块到发送队列
    for (uint32_t i = 0; i < total_chunks; ++i)
    {
        size_t chunk_size = std::min(FILE_CHUNK_SIZE, file_size - i * FILE_CHUNK_SIZE);
        std::vector<uint8_t> chunk_data(chunk_size);

        file.read(reinterpret_cast<char*>(chunk_data.data()), chunk_size);

        auto packet_data = std::make_shared<FileChunkPacketData>(
            file_id, i, total_chunks, chunk_data);

        addPacketToQueue(packet_data, priority);
    }

    file.close();
}

// 处理接收到的数据

void JTCPProtocol::onDataReceived(const uint8_t* data, size_t length)
{
    std::lock_guard<std::mutex> lock(receive_mutex);
    receive_buffer.insert(receive_buffer.end(), data, data + length);
}

// 添加包到发送队列

void JTCPProtocol::addPacketToQueue(std::shared_ptr<IPacketData> data, uint8_t priority)
{
    std::lock_guard<std::mutex> lock(send_mutex);
    uint32_t packet_id = next_packet_id++;
    send_queue.emplace(data, packet_id, priority);
}

// 处理接收到的数据

void JTCPProtocol::ProcessReceivedData()
{
    std::lock_guard<std::mutex> lock(receive_mutex);

    while (!receive_buffer.empty())
    {
        // 检查是否有完整的包头
        if (receive_buffer.size() < sizeof(PacketHeader))
        {
            break;
        }

        // 解析包头
        PacketHeader header;
        memcpy(&header, receive_buffer.data(), sizeof(PacketHeader));
        header.toHostOrder();

        // 检查数据是否完整
        size_t total_packet_size = sizeof(PacketHeader) + header.data_size;
        if (receive_buffer.size() < total_packet_size)
        {
            break;
        }

        // 提取数据部分
        std::vector<uint8_t> packet_data(
            receive_buffer.begin() + sizeof(PacketHeader),
            receive_buffer.begin() + total_packet_size
        );

        // 处理包
        std::shared_ptr<IPacketData> data;

        switch (header.type)
        {
        case PacketType::MESSAGE:
            data = std::make_shared<MessagePacketData>();
            break;
        case PacketType::FILE_CHUNK:
        {
            auto chunk_data = std::make_shared<FileChunkPacketData>();
            if (chunk_data->deserialize(packet_data))
            {
                uint32_t file_id = chunk_data->getFileId();
                uint32_t chunk_index = chunk_data->getChunkIndex();
                uint32_t total_chunks = chunk_data->getTotalChunks();

                std::lock_guard<std::mutex> reassembly_lock(reassembly_mutex);

                // 获取或创建重组状态
                auto& reassembly = file_reassembly_map.try_emplace(
                    file_id, file_id, total_chunks
                ).first->second;

                // 检查块索引有效性
                if (chunk_index >= total_chunks) {
                    std::cerr << "Invalid chunk index: " << chunk_index << std::endl;
                    break;
                }

                // 更新块数据（允许重复包覆盖）
                if (!reassembly.received_flags[chunk_index])
                {
                    reassembly.received_flags[chunk_index] = true;
                    reassembly.received_count++;
                }
                reassembly.chunks[chunk_index] = chunk_data->getChunkData();

                // 检查文件是否完整
                if (reassembly.isComplete())
                {
                    // 组装完整文件
                    auto full_file_data = reassembly.assemble();
                    auto complete_file = std::make_shared<FileChunkPacketData>(
                        file_id, 0, 1, full_file_data  // 用特殊索引0表示完整文件
                        );

                    // 触发回调
                    if (receive_callback)
                    {
                        receive_callback(PacketType::FILE_CHUNK, complete_file);
                    }

                    // 清理重组状态
                    file_reassembly_map.erase(file_id);
                }
            }
            break;
        }
        default:
            std::cerr << "Unknown packet type: " << static_cast<int>(header.type) << std::endl;
            break;
        }

        if (data && data->deserialize(packet_data))
        {
            if (receive_callback)
            {
                receive_callback(header.type, data);
            }
        }

        // 从缓冲区移除已处理的数据
        receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + total_packet_size);
    }
}

// 发送包

bool JTCPProtocol::sendPacket(const PendingPacket& packet)
{
    // 创建包数据
    std::vector<uint8_t> serialized_data = packet.data->serialize();

    // 创建包头
    PacketHeader header;
    header.packet_id = packet.packet_id;
    header.type = packet.data->getType();
    header.data_size = static_cast<uint32_t>(serialized_data.size());

    // 对于文件块，需要设置额外的字段
    if (header.type == PacketType::FILE_CHUNK)
    {
        auto file_data = std::dynamic_pointer_cast<FileChunkPacketData>(packet.data);
        if (file_data)
        {
            header.total_packets = file_data->getTotalChunks();
            header.packet_index = file_data->getChunkIndex();
        }
    }
    else
    {
        header.total_packets = 1;
        header.packet_index = 0;
    }

    // 转换为网络字节序
    header.toNetworkOrder();

    // 发送包头
    int sent = send_function(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    if (sent <= 0)
    {
        return false;
    }

    // 发送数据
    if (!serialized_data.empty())
    {
        sent = send_function(serialized_data.data(), serialized_data.size());
        if (sent <= 0)
        {
            return false;
        }
    }

    return true;
}

// 处理发送

void JTCPProtocol::ProcessSendData()
{
    std::unique_lock<std::mutex> lock(send_mutex);

    // 根据优先级处理发送队列
    if (!send_queue.empty())
    {
        // 获取最高优先级的包
        PendingPacket packet = send_queue.top();
        send_queue.pop();

        lock.unlock();

        // 发送包
        if (!sendPacket(packet))
        {
            // 发送失败，重新添加到队列
            lock.lock();
            send_queue.push(packet);
            lock.unlock();
        }
    }
}
