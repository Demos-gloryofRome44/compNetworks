#include <iostream>
#include <vector>
#include <queue>
#include <string>
#include <cstring>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <csignal>
#include <atomic>
#include <unordered_map>
#include <cstdlib>

#include "message.h"

#define DEFAULT_PORT 8888
#define THREAD_POOL_SIZE 10
#define MAX_NICK_LEN 31

struct Client {
    int socket;
    char nickname[MAX_NICK_LEN + 1];
    bool authenticated;
    std::string address;
    Client(int s, const std::string& a) : socket(s), authenticated(false), address(a) {
        nickname[0] = '\0';
    }
};

std::atomic<bool> serverRunning{true};
void signalHandler(int) { serverRunning = false; }

#define LOG_L4(msg)  std::cerr << "[Layer 4 - Transport] " << msg << std::endl
#define LOG_L5(msg)  std::cerr << "[Layer 5 - Session] " << msg << std::endl
#define LOG_L6(msg)  std::cerr << "[Layer 6 - Presentation] " << msg << std::endl
#define LOG_L7(msg)  std::cerr << "[Layer 7 - Application] " << msg << std::endl

bool sendMessageStruct(int fd, const Message& msg);

class ThreadSafeQueue {
    std::queue<int> q;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool stopped = false;
public:
    void push(int v) {
        pthread_mutex_lock(&mtx);
        if (!stopped) { q.push(v); pthread_cond_signal(&cond); }
        pthread_mutex_unlock(&mtx);
    }
    bool pop(int& v) {
        pthread_mutex_lock(&mtx);
        while (q.empty() && !stopped && serverRunning) pthread_cond_wait(&cond, &mtx);
        if (q.empty()) { pthread_mutex_unlock(&mtx); return false; }
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
    std::unordered_map<std::string, Client*> byNickname;
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
public:
    bool add(Client* c, const std::string& nick) {
        pthread_mutex_lock(&mtx);
        if (byNickname.count(nick)) { pthread_mutex_unlock(&mtx); return false; }
        strncpy(c->nickname, nick.c_str(), MAX_NICK_LEN);
        c->nickname[MAX_NICK_LEN] = '\0';
        clients.push_back(c);
        byNickname[nick] = c;
        pthread_mutex_unlock(&mtx);
        return true;
    }
    void remove(Client* c) {
        pthread_mutex_lock(&mtx);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (*it == c) {
                if ((*it)->nickname[0]) byNickname.erase((*it)->nickname);
                clients.erase(it);
                break;
            }
        }
        pthread_mutex_unlock(&mtx);
    }
    Client* findByNick(const std::string& nick) {
        pthread_mutex_lock(&mtx);
        auto it = byNickname.find(nick);
        Client* res = (it != byNickname.end()) ? it->second : nullptr;
        pthread_mutex_unlock(&mtx);
        return res;
    }
    void broadcast(const Message& msg, int exclude) {
        pthread_mutex_lock(&mtx);
        for (auto* c : clients) {
            if (c->socket >= 0 && c->socket != exclude && c->authenticated)
                sendMessageStruct(c->socket, msg);
        }
        pthread_mutex_unlock(&mtx);
    }
    void sendTo(Client* c, const Message& msg) {
        if (c && c->socket >= 0 && c->authenticated)
            sendMessageStruct(c->socket, msg);
    }
};

ThreadSafeQueue connQueue;
ClientManager clients;

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
    LOG_L4("recv()");
    if (recvFull(fd, &msg, HEADER_SIZE) < 0) return false;
    LOG_L6("deserialize Message");
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
    LOG_L6("serialize Message");
    LOG_L4("send()");
    return sendFull(fd, &msg, HEADER_SIZE + payloadLen) == 0;
}

bool sendMessageStruct(int fd, const Message& msg) {
    if (msg.length < sizeof(uint8_t)) return false;
    const std::size_t payloadLen = msg.length - sizeof(uint8_t);
    if (payloadLen > MAX_PAYLOAD) return false;
    LOG_L4("send()");
    return sendFull(fd, &msg, HEADER_SIZE + payloadLen) == 0;
}

void sendError(int sock, const std::string& reason) {
    LOG_L7("prepare error response");
    sendMessageRaw(sock, MSG_ERROR, reason);
}

void* worker(void*) {
    int csock;
    while (connQueue.pop(csock)) {
        sockaddr_in addr; socklen_t alen = sizeof(addr);
        getpeername(csock, (sockaddr*)&addr, &alen);
        std::string ip(inet_ntoa(addr.sin_addr));
        std::string caddr = ip + ":" + std::to_string(ntohs(addr.sin_port));
        
        Client* client = new Client(csock, caddr);
        
        Message msg{};
        std::size_t plen = 0;
        if (!recvMessage(csock, msg, plen)) {
            delete client; close(csock); continue;
        }
        LOG_L5("session not authenticated");
        if (msg.type != MSG_HELLO) {
            LOG_L7("ignore non-HELLO before handshake");
            delete client; close(csock); continue;
        }
        LOG_L7("handle MSG_HELLO");
        LOG_L7("prepare MSG_WELCOME");
        sendMessageRaw(csock, MSG_WELCOME, "");
        
        std::string nick;
        bool authFailed = false;
        while (serverRunning) {
            if (!recvMessage(csock, msg, plen)) { authFailed = true; break; }
            LOG_L5("session not authenticated");
            if (msg.type != MSG_AUTH) {
                LOG_L7("ignore message before authentication");
                continue;
            }
            LOG_L7("handle MSG_AUTH");
            nick = msg.payload;
            if (nick.empty() || nick.size() > MAX_NICK_LEN) {
                LOG_L5("authentication failed: invalid nickname");
                sendError(csock, "Invalid nickname");
                authFailed = true;
                break;
            }
            if (!clients.add(client, nick)) {
                LOG_L5("authentication failed: nickname taken");
                sendError(csock, "Nickname already taken");
                authFailed = true;
                break;
            }
            client->authenticated = true;
            LOG_L5("authentication success");
            break;
        }
        if (!client->authenticated) {
            if (!authFailed) {
                LOG_L5("authentication failed: session ended");
            }
            delete client; close(csock); continue;
        }
        
        std::string sysMsg = "User [" + nick + "] connected";
        LOG_L7("prepare system message");
        Message sys{};
        sys.length = static_cast<uint32_t>(sizeof(uint8_t) + std::min(sysMsg.size() + 1, MAX_PAYLOAD));
        sys.type = MSG_SERVER_INFO;
        std::memcpy(sys.payload, sysMsg.c_str(), sys.length - sizeof(uint8_t) - 1);
        sys.payload[sys.length - sizeof(uint8_t) - 1] = '\0';
        LOG_L6("serialize MSG_SERVER_INFO");
        clients.broadcast(sys, -1);
        std::cerr << sysMsg << std::endl;
        
        while (serverRunning && client->authenticated) {
            if (!recvMessage(csock, msg, plen)) break;
            LOG_L5("session authenticated");
            
            switch (msg.type) {
                case MSG_TEXT: {
                    LOG_L7("handle MSG_TEXT");
                    LOG_L7("prepare broadcast message");
                    std::string out = "[" + std::string(client->nickname) + "]: " + msg.payload;
                    Message b{};
                    b.length = static_cast<uint32_t>(sizeof(uint8_t) + std::min(out.size() + 1, MAX_PAYLOAD));
                    b.type = MSG_TEXT;
                    std::memcpy(b.payload, out.c_str(), b.length - sizeof(uint8_t) - 1);
                    b.payload[b.length - sizeof(uint8_t) - 1] = '\0';
                    LOG_L6("serialize MSG_TEXT");
                    clients.broadcast(b, csock);
                    std::cerr << out << std::endl;
                    break;
                }
                case MSG_PRIVATE: {
                    LOG_L7("handle MSG_PRIVATE");
                    std::string payload(msg.payload);
                    size_t pos = payload.find(':');
                    if (pos == std::string::npos) {
                        sendError(csock, "Invalid private message format");
                        break;
                    }
                    std::string target = payload.substr(0, pos);
                    std::string text = payload.substr(pos + 1);
                    Client* recipient = clients.findByNick(target);
                    if (recipient) {
                        LOG_L7("prepare private message");
                        std::string out = "[PRIVATE][" + std::string(client->nickname) + "]: " + text;
                        Message priv{};
                        priv.length = static_cast<uint32_t>(sizeof(uint8_t) + std::min(out.size() + 1, MAX_PAYLOAD));
                        priv.type = MSG_PRIVATE;
                        std::memcpy(priv.payload, out.c_str(), priv.length - sizeof(uint8_t) - 1);
                        priv.payload[priv.length - sizeof(uint8_t) - 1] = '\0';
                        LOG_L6("serialize MSG_PRIVATE");
                        clients.sendTo(recipient, priv);
                        std::cerr << "[PRIVATE to " << target << "] " << text << std::endl;
                    } else {
                        sendError(csock, "User not found: " + target);
                    }
                    break;
                }
                case MSG_PING: {
                    LOG_L7("handle MSG_PING");
                    LOG_L7("prepare MSG_PONG");
                    sendMessageRaw(csock, MSG_PONG, "");
                    break;
                }
                case MSG_BYE:
                    LOG_L7("handle MSG_BYE");
                    goto cleanup;
                default:
                    LOG_L7("unknown message type: " + std::to_string(msg.type));
                    break;
            }
        }
        
    cleanup:
        std::string discMsg = "User [" + std::string(client->nickname) + "] disconnected";
        LOG_L7("prepare disconnect notification");
        Message disc{};
        disc.length = static_cast<uint32_t>(sizeof(uint8_t) + std::min(discMsg.size() + 1, MAX_PAYLOAD));
        disc.type = MSG_SERVER_INFO;
        std::memcpy(disc.payload, discMsg.c_str(), disc.length - sizeof(uint8_t) - 1);
        disc.payload[disc.length - sizeof(uint8_t) - 1] = '\0';
        LOG_L6("serialize MSG_SERVER_INFO");
        clients.broadcast(disc, csock);
        std::cerr << discMsg << std::endl;
        
        clients.remove(client);
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
        LOG_L4("accept() new connection");
        connQueue.push(c);
    }
    return nullptr;
}

int main(int argc, char* argv[]) {
    int PORT = DEFAULT_PORT;
    if (argc > 1) {
        PORT = std::atoi(argv[1]);
        if (PORT <= 0 || PORT > 65535) PORT = DEFAULT_PORT;
    }
    
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
