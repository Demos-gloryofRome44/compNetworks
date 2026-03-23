#include <iostream>
#include <string>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <atomic>

#include "message.h"
#include "net_utils.h"

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 9999

std::atomic<bool> running{true};
std::atomic<int> messagesReceived{0};
int clientSocket = -1;
std::string nickname;

void* receiveThread(void*) {
    Message msg;
    while (running) {
        size_t plen = 0;
        if (!recvMessage(clientSocket, msg, plen)) break;
        
        switch (msg.type) {
            case MSG_WELCOME:
                std::cout << "[SERVER] Welcome!" << std::endl;
                messagesReceived++;
                break;
            case MSG_TEXT:
                std::cout << "[BROADCAST] " << msg.payload << std::endl;
                messagesReceived++;
                break;
            case MSG_PONG:
                std::cout << "[SERVER] Pong!" << std::endl;
                messagesReceived++;
                break;
            default: break;
        }
    }
    return nullptr;
}

bool connectToServer() {
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) { perror("socket"); return false; }
    
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) <= 0) {
        close(clientSocket); return false;
    }
    
    if (connect(clientSocket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(clientSocket); return false;
    }
    
    if (!sendMessageRaw(clientSocket, MSG_HELLO, nickname)) {
        close(clientSocket); return false;
    }
    
    Message welcome{};
    size_t plen = 0;
    if (!recvMessage(clientSocket, welcome, plen) || welcome.type != MSG_WELCOME) {
        close(clientSocket); return false;
    }
    
    return true;
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
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <nickname>" << std::endl;
        return 1;
    }
    nickname = argv[1];
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    while (running) {
        if (connectToServer()) {
            std::cout << "Connected to server as " << nickname << std::endl;
            std::cout.flush();
            
            pthread_t t; pthread_create(&t, nullptr, receiveThread, nullptr);
            
            usleep(100000);
            
            std::string line;
            while (running && std::getline(std::cin, line)) {
                if (line == "/quit" || line == "/exit") {
                    sendMessage(MSG_BYE, "");
                    running = false;
                    break;
                } else if (line == "/ping") {
                    sendMessage(MSG_PING, "");
                    usleep(200000);
                } else if (!line.empty()) {
                    sendMessage(MSG_TEXT, line);
                    usleep(100000);
                }
            }
            usleep(300000);
            
            pthread_join(t, nullptr);
            if (clientSocket >= 0) { close(clientSocket); clientSocket = -1; }
            if (!running) break;
        }
        if (running) {
            std::cerr << "Reconnecting in 2s..." << std::endl;
            sleep(2);
        }
    }
    std::cout << "Client stopped." << std::endl;
    return 0;
}