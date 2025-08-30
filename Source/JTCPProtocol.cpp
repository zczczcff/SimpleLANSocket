#include "JTCPProtocol.h"

std::vector<uint8_t> FileChunkPacketData::serialize() const
{
    std::vector<uint8_t> data;
    // ����ļ�ID
    uint32_t netFileId = htonl(file_id);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netFileId),
        reinterpret_cast<const uint8_t*>(&netFileId) + sizeof(netFileId));
    // ��ӿ�����
    uint32_t netChunkIndex = htonl(chunk_index);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netChunkIndex),
        reinterpret_cast<const uint8_t*>(&netChunkIndex) + sizeof(netChunkIndex));
    // ����ܿ���
    uint32_t netTotalChunks = htonl(total_chunks);
    data.insert(data.end(),
        reinterpret_cast<const uint8_t*>(&netTotalChunks),
        reinterpret_cast<const uint8_t*>(&netTotalChunks) + sizeof(netTotalChunks));
    // ��ӿ�����
    data.insert(data.end(), chunk_data.begin(), chunk_data.end());
    return data;
}

bool FileChunkPacketData::deserialize(const std::vector<uint8_t>& data)
{
    if (data.size() < 12) return false; // �ļ�ID+������+�ܿ��� = 12�ֽ�

    size_t offset = 0;
    // ��ȡ�ļ�ID
    memcpy(&file_id, data.data() + offset, sizeof(file_id));
    file_id = ntohl(file_id);
    offset += sizeof(file_id);

    // ��ȡ������
    memcpy(&chunk_index, data.data() + offset, sizeof(chunk_index));
    chunk_index = ntohl(chunk_index);
    offset += sizeof(chunk_index);

    // ��ȡ�ܿ���
    memcpy(&total_chunks, data.data() + offset, sizeof(total_chunks));
    total_chunks = ntohl(total_chunks);
    offset += sizeof(total_chunks);

    // ��ȡ������
    chunk_data.assign(data.begin() + offset, data.end());
    return true;
}

// �ⲿ�����������ͺͽ���ѭ��

void JTCPProtocol::WorkTick()
{
    // �����������
    ProcessReceivedData();

    ProcessSendData();
}

// ���Ҫ���͵���Ϣ

void JTCPProtocol::sendMessage(const std::string& message, uint8_t priority)
{
    auto packet_data = std::make_shared<MessagePacketData>(message);
    addPacketToQueue(packet_data, priority);
}

// ���Ҫ���͵��ļ�

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

    // ��ȡ�ļ���С
    size_t file_size = file.tellg();
    file.seekg(0);

    // ������Ҫ���ٿ�
    uint32_t total_chunks = (file_size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;

    // ��ȡ����������ļ��鵽���Ͷ���
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

// ������յ�������

void JTCPProtocol::onDataReceived(const uint8_t* data, size_t length)
{
    std::lock_guard<std::mutex> lock(receive_mutex);
    receive_buffer.insert(receive_buffer.end(), data, data + length);
}

// ��Ӱ������Ͷ���

void JTCPProtocol::addPacketToQueue(std::shared_ptr<IPacketData> data, uint8_t priority)
{
    std::lock_guard<std::mutex> lock(send_mutex);
    uint32_t packet_id = next_packet_id++;
    send_queue.emplace(data, packet_id, priority);
}

// ������յ�������

void JTCPProtocol::ProcessReceivedData()
{
    std::lock_guard<std::mutex> lock(receive_mutex);

    while (!receive_buffer.empty())
    {
        // ����Ƿ��������İ�ͷ
        if (receive_buffer.size() < sizeof(PacketHeader))
        {
            break;
        }

        // ������ͷ
        PacketHeader header;
        memcpy(&header, receive_buffer.data(), sizeof(PacketHeader));
        header.toHostOrder();

        // ��������Ƿ�����
        size_t total_packet_size = sizeof(PacketHeader) + header.data_size;
        if (receive_buffer.size() < total_packet_size)
        {
            break;
        }

        // ��ȡ���ݲ���
        std::vector<uint8_t> packet_data(
            receive_buffer.begin() + sizeof(PacketHeader),
            receive_buffer.begin() + total_packet_size
        );

        // �����
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

                // ��ȡ�򴴽�����״̬
                auto& reassembly = file_reassembly_map.try_emplace(
                    file_id, file_id, total_chunks
                ).first->second;

                // ����������Ч��
                if (chunk_index >= total_chunks) {
                    std::cerr << "Invalid chunk index: " << chunk_index << std::endl;
                    break;
                }

                // ���¿����ݣ������ظ������ǣ�
                if (!reassembly.received_flags[chunk_index])
                {
                    reassembly.received_flags[chunk_index] = true;
                    reassembly.received_count++;
                }
                reassembly.chunks[chunk_index] = chunk_data->getChunkData();

                // ����ļ��Ƿ�����
                if (reassembly.isComplete())
                {
                    // ��װ�����ļ�
                    auto full_file_data = reassembly.assemble();
                    auto complete_file = std::make_shared<FileChunkPacketData>(
                        file_id, 0, 1, full_file_data  // ����������0��ʾ�����ļ�
                        );

                    // �����ص�
                    if (receive_callback)
                    {
                        receive_callback(PacketType::FILE_CHUNK, complete_file);
                    }

                    // ��������״̬
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

        // �ӻ������Ƴ��Ѵ��������
        receive_buffer.erase(receive_buffer.begin(), receive_buffer.begin() + total_packet_size);
    }
}

// ���Ͱ�

bool JTCPProtocol::sendPacket(const PendingPacket& packet)
{
    // ����������
    std::vector<uint8_t> serialized_data = packet.data->serialize();

    // ������ͷ
    PacketHeader header;
    header.packet_id = packet.packet_id;
    header.type = packet.data->getType();
    header.data_size = static_cast<uint32_t>(serialized_data.size());

    // �����ļ��飬��Ҫ���ö�����ֶ�
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

    // ת��Ϊ�����ֽ���
    header.toNetworkOrder();

    // ���Ͱ�ͷ
    int sent = send_function(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    if (sent <= 0)
    {
        return false;
    }

    // ��������
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

// ������

void JTCPProtocol::ProcessSendData()
{
    std::unique_lock<std::mutex> lock(send_mutex);

    // �������ȼ������Ͷ���
    if (!send_queue.empty())
    {
        // ��ȡ������ȼ��İ�
        PendingPacket packet = send_queue.top();
        send_queue.pop();

        lock.unlock();

        // ���Ͱ�
        if (!sendPacket(packet))
        {
            // ����ʧ�ܣ�������ӵ�����
            lock.lock();
            send_queue.push(packet);
            lock.unlock();
        }
    }
}
