#include <iostream>
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "message.h"

namespace
{
constexpr int kPort = 9090;
}

bool recv_all(int socket_fd, void *buffer, size_t length)
{
    size_t received = 0;
    while (received < length)
    {
        ssize_t n = recv(socket_fd, static_cast<char *>(buffer) + received, length - received, 0);
        if (n <= 0)
        {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int socket_fd, const void *buffer, size_t length)
{
    size_t sent_total = 0;
    while (sent_total < length)
    {
        ssize_t n = send(socket_fd, static_cast<const char *>(buffer) + sent_total, length - sent_total, 0);
        if (n <= 0)
        {
            return false;
        }
        sent_total += static_cast<size_t>(n);
    }
    return true;
}

int main()
{
    setbuf(stdout, nullptr);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(kPort);

    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        std::cerr << "bind() failed on port " << kPort << ": " << std::strerror(errno) << "\n";
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 1) < 0)
    {
        std::cerr << "listen() failed: " << std::strerror(errno) << "\n";
        close(listen_fd);
        return 1;
    }

    std::cout << "Server started on port " << kPort << "\n";
    std::cout << "Waiting for client...\n";

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0)
    {
        std::cerr << "accept() failed: " << std::strerror(errno) << "\n";
        close(listen_fd);
        return 1;
    }

    char info[64];
    snprintf(info, sizeof(info), "[%s:%d]", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    std::cout << "Client connected\n";

    uint32_t net_len;
    uint8_t type;
    char payload[MAX_PAYLOAD + 1] = {0};

    if (!recv_all(client_fd, &net_len, sizeof(net_len)))
    {
        std::cerr << "Failed to read hello length\n";
        close(client_fd);
        close(listen_fd);
        return 1;
    }
    uint32_t len = ntohl(net_len);
    if (len < 1 || len > MAX_PAYLOAD + 1)
    {
        std::cerr << "Invalid hello length: " << len << "\n";
        close(client_fd);
        close(listen_fd);
        return 1;
    }
    if (!recv_all(client_fd, &type, 1))
    {
        std::cerr << "Failed to read hello type\n";
        close(client_fd);
        close(listen_fd);
        return 1;
    }
    if (len > 1 && !recv_all(client_fd, payload, len - 1))
    {
        std::cerr << "Failed to read hello payload\n";
        close(client_fd);
        close(listen_fd);
        return 1;
    }

    if (type != MSG_HELLO)
    {
        std::cerr << "First message is not MSG_HELLO\n";
        close(client_fd);
        close(listen_fd);
        return 1;
    }
    std::cout << info << ": " << payload << "\n";

    std::string welcome = "Welcome " + std::string(info);
    uint32_t wlen = 1 + welcome.size() + 1;
    net_len = htonl(wlen);
    uint8_t wtype = MSG_WELCOME;
    send_all(client_fd, &net_len, sizeof(net_len));
    send_all(client_fd, &wtype, 1);
    send_all(client_fd, welcome.c_str(), welcome.size() + 1);

    while (true)
    {
        if (!recv_all(client_fd, &net_len, sizeof(net_len)))
        {
            std::cout << "Client disconnected\n";
            break;
        }
        len = ntohl(net_len);
        if (len < 1 || len > MAX_PAYLOAD + 1)
            break;
        if (!recv_all(client_fd, &type, 1))
            break;
        if (len > 1 && !recv_all(client_fd, payload, len - 1))
            break;
        payload[len > 1 ? len - 1 : 0] = '\0';

        if (type == MSG_TEXT)
        {
            std::cout << info << ": " << payload << "\n";
        }
        else if (type == MSG_PING)
        {
            uint32_t pong_len = htonl(1);
            uint8_t pong_type = MSG_PONG;
            send_all(client_fd, &pong_len, sizeof(pong_len));
            send_all(client_fd, &pong_type, 1);
        }
        else if (type == MSG_BYE)
        {
            std::cout << "Client disconnected\n";
            break;
        }
    }

    close(client_fd);
    close(listen_fd);
    return 0;
}