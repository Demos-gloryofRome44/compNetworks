#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    char msg[512];
    char resp[512];

    while (true) {
        std::cout << "Send: ";
        std::cin.getline(msg, sizeof(msg));
        if (strcmp(msg, "quit") == 0) break;

        sendto(sock, msg, strlen(msg), 0, (sockaddr*)&addr, sizeof(addr));

        socklen_t len = sizeof(addr);
        int n = recvfrom(sock, resp, sizeof(resp)-1, 0, (sockaddr*)&addr, &len);
        if (n > 0) {
            resp[n] = 0;
            std::cout << "Got: " << resp << std::endl;
        }
    }
    close(sock);
    return 0;
}