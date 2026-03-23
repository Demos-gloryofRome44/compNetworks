#include "net_utils.h"

#include <algorithm>
#include <cstring>
#include <sys/socket.h>

int recvFull(int fd, void* buf, std::size_t len) {
    std::size_t got = 0;
    while (got < len) {
        const int r = recv(fd, static_cast<char*>(buf) + got, len - got, 0);
        if (r <= 0) return -1;
        got += static_cast<std::size_t>(r);
    }
    return 0;
}

int sendFull(int fd, const void* buf, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const int s = send(fd, static_cast<const char*>(buf) + sent, len - sent, 0);
        if (s <= 0) return -1;
        sent += static_cast<std::size_t>(s);
    }
    return 0;
}

bool recvMessage(int fd, Message& msg, std::size_t& payloadLen) {
    if (recvFull(fd, &msg, HEADER_SIZE) < 0) return false;
    if (msg.length < sizeof(uint8_t)) return false;

    payloadLen = msg.length - sizeof(uint8_t);
    if (payloadLen > MAX_PAYLOAD) return false;

    if (payloadLen > 0 && recvFull(fd, msg.payload, payloadLen) < 0) return false;
    if (payloadLen < MAX_PAYLOAD) msg.payload[payloadLen] = '\0';
    else msg.payload[MAX_PAYLOAD - 1] = '\0';

    return true;
}

bool sendMessageRaw(int fd, uint8_t type, const std::string& text) {
    const std::size_t payloadLen = std::min(text.size() + 1, MAX_PAYLOAD);
    Message msg{};
    msg.length = static_cast<uint32_t>(sizeof(uint8_t) + payloadLen);
    msg.type = type;
    std::memcpy(msg.payload, text.c_str(), payloadLen - 1);
    msg.payload[payloadLen - 1] = '\0';

    return sendFull(fd, &msg, HEADER_SIZE + payloadLen) == 0;
}
