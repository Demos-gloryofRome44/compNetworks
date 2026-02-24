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

    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(8080);
    srv.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind");
        return 1;
    }

    std::cout << "Server on port 8080" << std::endl;

    char buf[512];
    sockaddr_in cli;
    socklen_t clen = sizeof(cli);

    while (true) {
        int n = recvfrom(sock, buf, sizeof(buf)-1, 0, (sockaddr*)&cli, &clen);
        if (n <= 0) continue;

        buf[n] = 0;
        std::cout << "From " << inet_ntoa(cli.sin_addr) << ":" 
                  << ntohs(cli.sin_port) << " - " << buf << std::endl;

        sendto(sock, buf, n, 0, (sockaddr*)&cli, clen);
    }
    close(sock);
    return 0;
}