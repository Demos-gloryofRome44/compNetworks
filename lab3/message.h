#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::size_t MAX_PAYLOAD = 1024;
constexpr std::size_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint8_t);

enum MessageType {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};

#pragma pack(push, 1)
struct Message {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
};
#pragma pack(pop)
