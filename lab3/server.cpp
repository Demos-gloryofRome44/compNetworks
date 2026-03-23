#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstring>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <csignal>
#include <atomic>
#include <algorithm>

#define PORT 9999
#define MAX_PAYLOAD 1024
#define THREAD_POOL_SIZE 10
constexpr size_t HEADER_SIZE = sizeof(uint32_t) + sizeof(uint8_t);

enum MessageType {
    MSG_HELLO = 1, MSG_WELCOME = 2, MSG_TEXT = 3,
    MSG_PING = 4, MSG_PONG = 5, MSG_BYE = 6
};

#pragma pack(push, 1)
struct Message {
    uint32_t length;
    uint8_t  type;
    char     payload[MAX_PAYLOAD];
};
#pragma pack(pop)

struct Client {
    int socket;
    std::string nickname;
    std::string address;
    Client(int s, const std::string& n, const std::string& a)
        : socket(s), nickname(n), address(a) {}
};

std::atomic<bool> serverRunning{true};
void signalHandler(int) { serverRunning = false; }

class ThreadSafeQueue {
    std::queue<int> q;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool stopped = false;
public:
    void push(int v) {
        pthread_mutex_lock(&mtx);
        if (!stopped) {
            q.push(v);
            pthread_cond_signal(&cond);
        }
        pthread_mutex_unlock(&mtx);
    }
    bool pop(int& v) {
        pthread_mutex_lock(&mtx);
        while (q.empty() && !stopped && serverRunning)
            pthread_cond_wait(&cond, &mtx);
        if (q.empty()) {
            pthread_mutex_unlock(&mtx);
            return false;
        }
        v = q.front(); q.pop();
        pthread_mutex_unlock(&mtx);
        return true;
    }
    void stop() {
        pthread_mutex_lock(&mtx);
        stopped = true;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mtx);
    }
};

class ClientManager {
    std::vector<Client*> clients;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
public:
    void add(Client* c) {
        pthread_mutex_lock(&mtx);
        clients.push_back(c);
        pthread_mutex_unlock(&mtx);
    }
    void remove(Client* c) {
        pthread_mutex_lock(&mtx);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (*it == c) {
                clients.erase(it);
                break;
            }
        }
        pthread_mutex_unlock(&mtx);
    }
    template <typename Sender>
    void forEachClient(Sender sender) {
        pthread_mutex_lock(&mtx);
        for (auto* c : clients) {
            if (c->socket >= 0) {
                sender(c->socket);
            }
        }
        pthread_mutex_unlock(&mtx);
    }
    void logClients() {
        pthread_mutex_lock(&mtx);
        std::cerr << "Connected clients: " << clients.size() << std::endl;
        pthread_mutex_unlock(&mtx);
    }
};

ThreadSafeQueue connQueue;
ClientManager clients;

int recvFull(int fd, void* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int r = recv(fd, (char*)buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return 0;
}

int sendFull(int fd, const void* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int s = send(fd, (const char*)buf + sent, len - sent, 0);
        if (s <= 0) return -1;
        sent += s;
    }
    return 0;
}

bool recvMessage(int fd, Message& msg, size_t& payloadLen) {
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
    const size_t payloadLen = std::min(text.size() + 1, static_cast<size_t>(MAX_PAYLOAD));
    Message msg{};
    msg.length = static_cast<uint32_t>(sizeof(uint8_t) + payloadLen);
    msg.type = type;
    std::memcpy(msg.payload, text.c_str(), payloadLen - 1);
    msg.payload[payloadLen - 1] = '\0';
    return sendFull(fd, &msg, HEADER_SIZE + payloadLen) == 0;
}

void* worker(void*) {
    int csock;
    while (connQueue.pop(csock)) {
        sockaddr_in addr; socklen_t alen = sizeof(addr);
        getpeername(csock, (sockaddr*)&addr, &alen);
        std::string ip(inet_ntoa(addr.sin_addr));
        std::string caddr = ip + ":" + std::to_string(ntohs(addr.sin_port));
        
        Message msg{};
        size_t plen = 0;
        if (!recvMessage(csock, msg, plen)) {
            close(csock); continue;
        }
        if (msg.type != MSG_HELLO) {
            close(csock); continue;
        }
        
        std::string nick = (msg.payload[0] != '\0') ? msg.payload : "Anonymous";
        Client* client = new Client(csock, nick, caddr);
        clients.add(client);
        std::cerr << "Client connected: " << nick << " [" << caddr << "]" << std::endl;
        clients.logClients();
        
        sendMessageRaw(csock, MSG_WELCOME, "");
        
        while (serverRunning) {
            if (!recvMessage(csock, msg, plen)) break;
            
            switch (msg.type) {
                case MSG_TEXT: {
                    std::string out = client->nickname + " [" + client->address + "]: " + msg.payload;
                    clients.forEachClient([&out](int fd) {
                        sendMessageRaw(fd, MSG_TEXT, out);
                    });
                    std::cerr << out << std::endl;
                    break;
                }
                case MSG_PING: {
                    sendMessageRaw(csock, MSG_PONG, "");
                    break;
                }
                case MSG_BYE:
                    std::cerr << "Client disconnected: " << client->nickname << " [" << caddr << "]" << std::endl;
                    goto cleanup;
                default: break;
            }
        }
        std::cerr << "Connection lost: " << nick << " [" << caddr << "]" << std::endl;
        
    cleanup:
        clients.remove(client);
        clients.logClients();
        close(csock);
        delete client;
    }
    return nullptr;
}

void* acceptor(void* arg) {
    int srv = *(int*)arg; delete (int*)arg;
    while (serverRunning) {
        sockaddr_in caddr; socklen_t clen = sizeof(caddr);
        int c = accept(srv, (sockaddr*)&caddr, &clen);
        if (c < 0) { if (serverRunning) perror("accept"); continue; }
        connQueue.push(c);
    }
    return nullptr;
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
    
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(PORT);
    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind failed: " << strerror(errno) << std::endl;
        return 1;
    }
    if (listen(srv, 50) < 0) { perror("listen"); return 1; }
    
    std::cerr << "Server started on port " << PORT << std::endl;
    
    pthread_t acc; pthread_create(&acc, nullptr, acceptor, new int(srv));
    pthread_t workers[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i)
        pthread_create(&workers[i], nullptr, worker, nullptr);
    
    while (serverRunning) sleep(1);
    
    connQueue.stop();
    close(srv);
    
    pthread_join(acc, nullptr);
    for (auto& t : workers) pthread_join(t, nullptr);
    
    std::cerr << "Server stopped." << std::endl;
    return 0;
}