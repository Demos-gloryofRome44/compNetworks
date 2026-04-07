#include <iostream>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdlib>

#include "message.h"

#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_PORT 8888
#define MAX_NICK_LEN 31

std::atomic<bool> running{true};
int clientSocket = -1;
std::string nickname;
std::string serverIp = DEFAULT_SERVER_IP;
int serverPort = DEFAULT_PORT;

int recvFull(int fd, void* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, (char*)buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += static_cast<size_t>(r);
    }
    return 0;
}

int sendFull(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t s = send(fd, (const char*)buf + sent, len - sent, 0);
        if (s <= 0) return -1;
        sent += static_cast<size_t>(s);
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

void* receiveThread(void*) {
    Message msg;
    std::size_t plen = 0;
    while (running) {
        if (!recvMessage(clientSocket, msg, plen)) break;
        
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << msg.payload << std::endl;
                break;
            case MSG_PRIVATE:
                std::cout << msg.payload << std::endl;
                break;
            case MSG_SERVER_INFO:
                std::cout << "[SERVER] " << msg.payload << std::endl;
                break;
            case MSG_ERROR:
                std::cerr << "[ERROR] " << msg.payload << std::endl;
                break;
            case MSG_PONG:
                std::cout << "[SERVER] Pong!" << std::endl;
                break;
            default:
                break;
        }
        std::cout.flush();
    }
    return nullptr;
}

bool performHello() {
    if (!sendMessageRaw(clientSocket, MSG_HELLO, nickname)) return false;
    Message resp{};
    std::size_t plen = 0;
    if (!recvMessage(clientSocket, resp, plen)) return false;
    if (resp.type != MSG_WELCOME) return false;
    return true;
}

bool authenticate() {
    return sendMessageRaw(clientSocket, MSG_AUTH, nickname);
}

bool connectToServer() {
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) { perror("socket"); return false; }
    
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverIp.c_str(), &addr.sin_addr) <= 0) {
        close(clientSocket); return false;
    }
    
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(clientSocket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(clientSocket); return false;
    }
    
    if (!performHello()) return false;
    return authenticate();
}

void sendMessage(uint8_t type, const std::string& text) {
    if (clientSocket < 0) return;
    sendMessageRaw(clientSocket, type, text);
}

void signalHandler(int) {
    running = false;
    if (clientSocket >= 0) {
        sendMessage(MSG_BYE, "");
        close(clientSocket);
        clientSocket = -1;
    }
}

int main(int argc, char* argv[]) {
    if (argc > 1) nickname = argv[1];
    if (argc > 2) serverIp = argv[2];
    if (argc > 3) serverPort = std::atoi(argv[3]);
    if (serverPort <= 0 || serverPort > 65535) serverPort = DEFAULT_PORT;

    while (true) {
        std::cout << "Enter nickname";
        if (!nickname.empty()) std::cout << " [" << nickname << "]";
        std::cout << ": " << std::flush;
        std::string input;
        if (!std::getline(std::cin, input)) return 1;
        if (!input.empty()) nickname = input;
        if (!nickname.empty() && nickname.size() <= MAX_NICK_LEN) break;
        std::cerr << "Invalid nickname" << std::endl;
        nickname.clear();
    }
    
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    if (connectToServer()) {
        std::cout << "Connected as [" << nickname << "]" << std::endl;
        std::cout.flush();
        
        pthread_t t; pthread_create(&t, nullptr, receiveThread, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        
        std::string line;
        while (running && std::getline(std::cin, line)) {
            if (line == "/quit" || line == "/exit") {
                sendMessage(MSG_BYE, "");
                break;
            } else if (line == "/ping") {
                sendMessage(MSG_PING, "");
            } else if (line.substr(0, 3) == "/w ") {
                std::string rest = line.substr(3);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos) {
                    std::cerr << "Usage: /w <nick> <message>" << std::endl;
                    continue;
                }
                std::string target = rest.substr(0, sp);
                std::string text = rest.substr(sp + 1);
                std::string payload = target + ":" + text;
                sendMessage(MSG_PRIVATE, payload);
            } else if (!line.empty()) {
                sendMessage(MSG_TEXT, line);
            }
        }
        
        running = false;
        pthread_join(t, nullptr);
        if (clientSocket >= 0) { close(clientSocket); clientSocket = -1; }
    } else {
        std::cerr << "Failed to connect to server" << std::endl;
        return 1;
    }
    
    std::cout << "Client stopped." << std::endl;
    return 0;
}
