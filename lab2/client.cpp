#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
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

void send_message(int socket_fd, uint8_t type, const char *data, uint32_t data_len)
{
    uint32_t net_len = htonl(1 + data_len);
    send_all(socket_fd, &net_len, sizeof(net_len));
    send_all(socket_fd, &type, 1);
    if (data_len)
    {
        send_all(socket_fd, data, data_len);
    }
}

bool receive_message(int socket_fd, uint8_t *type, char *payload, size_t max_payload_len)
{
    uint32_t net_len;
    if (!recv_all(socket_fd, &net_len, sizeof(net_len)))
        return false;
    uint32_t len = ntohl(net_len);
    if (len < 1 || len > max_payload_len + 1)
        return false;
    if (!recv_all(socket_fd, type, 1))
        return false;
    if (len > 1 && !recv_all(socket_fd, payload, len - 1))
        return false;
    payload[len > 1 ? len - 1 : 0] = '\0';
    return true;
}

int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);

    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(kPort);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0)
    {
        std::cerr << "Invalid server IP address: " << ip << "\n";
        close(socket_fd);
        return 1;
    }

    if (connect(socket_fd, reinterpret_cast<sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
    {
        std::cerr << "connect() failed: " << std::strerror(errno) << "\n";
        close(socket_fd);
        return 1;
    }
    std::cout << "Connected\n";

    send_message(socket_fd, MSG_HELLO, "user", 5);

    uint8_t type;
    char payload[MAX_PAYLOAD + 1];
    if (receive_message(socket_fd, &type, payload, MAX_PAYLOAD) && type == MSG_WELCOME)
    {
        std::cout << payload << "\n";
    }

    bool waiting_pong = false;

    while (true)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(socket_fd, &rfds);

        int maxfd = socket_fd;
        if (!waiting_pong)
        {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd)
                maxfd = STDIN_FILENO;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0)
            break;

        if (FD_ISSET(socket_fd, &rfds))
        {
            if (receive_message(socket_fd, &type, payload, MAX_PAYLOAD))
            {
                if (type == MSG_TEXT)
                {
                    std::cout << payload << "\n";
                }
                else if (type == MSG_PONG)
                {
                    std::cout << "PONG\n";
                    waiting_pong = false;
                }
                else if (type == MSG_BYE)
                {
                    std::cout << "Disconnected\n";
                    break;
                }
            }
            else
            {
                std::cout << "Disconnected\n";
                break;
            }
        }

        if (!waiting_pong && FD_ISSET(STDIN_FILENO, &rfds))
        {
            std::string line;
            if (!std::getline(std::cin, line))
                break;
            if (line.empty())
                continue;

            if (line == "/ping")
            {
                send_message(socket_fd, MSG_PING, nullptr, 0);
                waiting_pong = true;
            }
            else if (line == "/quit")
            {
                send_message(socket_fd, MSG_BYE, nullptr, 0);
                break;
            }
            else
            {
                send_message(socket_fd, MSG_TEXT, line.c_str(), line.size() + 1);
            }
            std::cout << "> " << line << "\n";
        }
    }

    close(socket_fd);
    return 0;
}