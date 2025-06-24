#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    if (argc != 4) {
        printf("Usage: %s <naming_server_ip> <port> <command>\n", argv[0]);
        printf("Commands: READ <file>, WRITE <file>, INFO <file>\n");
        return 1;
    }

    char *ns_ip = argv[1];
    int ns_port = atoi(argv[2]);
    char *command = argv[3];
    
    int sock;
    struct sockaddr_in ns_addr;
    char buffer[BUFFER_SIZE];

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation error");
        return 1;
    }

    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    
    if (inet_pton(AF_INET, ns_ip, &ns_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return 1;
    }

    // Connect to naming server
    if (connect(sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0) {
        perror("Connection failed");
        return 1;
    }

    printf("Connected to naming server\n");

    // Send client identification
    send(sock, "CLIENT CONNECTING TO NAMING SERVER ...", 38, 0);
    sleep(1);

    // Send command
    if (strcmp(command, "READ") == 0) {
        send(sock, "READ test_storage1/file1.txt", 28, 0);
        printf("Sent: READ test_storage1/file1.txt\n");
    } else if (strcmp(command, "READ2") == 0) {
        send(sock, "READ test_storage2/file4.txt", 28, 0);
        printf("Sent: READ test_storage2/file4.txt\n");
    } else {
        printf("Unknown command\n");
        close(sock);
        return 1;
    }

    printf("Command sent: %s\n", command);

    // Wait for naming server to process
    sleep(2);

    // Receive response
    int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    printf("Received %d bytes from naming server\n", bytes_read);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("Response from naming server: %s\n", buffer);
        
        // Parse IP and port - look for the IP line in the response
        char ss_ip[16];
        int ss_port;
        char *ip_line = strstr(buffer, "IP:");
        if (ip_line && sscanf(ip_line, "IP: %15s Port: %d", ss_ip, &ss_port) == 2) {
            printf("Connecting to storage server %s:%d\n", ss_ip, ss_port);
            
            // Connect to storage server
            int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in ss_addr;
            ss_addr.sin_family = AF_INET;
            ss_addr.sin_port = htons(ss_port);
            inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);
            
            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == 0) {
                printf("Successfully connected to storage server\n");

                // Send read request
                if (strcmp(command, "READ") == 0) {
                    send(ss_sock, "READ test_storage1/file1.txt", 28, 0);
                    printf("Sent READ command to storage server\n");
                } else if (strcmp(command, "READ2") == 0) {
                    send(ss_sock, "read test_storage2/file4.txt", 28, 0);
                    printf("Sent READ2 command to storage server\n");
                }

                // Read file content
                printf("File content:\n");
                while ((bytes_read = recv(ss_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                    buffer[bytes_read] = '\0';
                    if (strcmp(buffer, "EOF\n") == 0) break;
                    printf("%s", buffer);
                }
                printf("\nEOF reached or connection closed\n");
                close(ss_sock);
            } else {
                perror("Failed to connect to storage server");
            }
        }
    }

    close(sock);
    return 0;
}
