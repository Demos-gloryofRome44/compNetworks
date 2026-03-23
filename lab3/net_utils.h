#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "message.h"

int recvFull(int fd, void* buf, std::size_t len);
int sendFull(int fd, const void* buf, std::size_t len);

bool recvMessage(int fd, Message& msg, std::size_t& payloadLen);
bool sendMessageRaw(int fd, uint8_t type, const std::string& text);
