#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

int main() {
    printf("=== Direct Storage Server Test ===\n");
    
    // Connect directly to storage server
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }
    
    struct sockaddr_in ss_addr;
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(9091);  // Storage server port
    inet_pton(AF_INET, "172.18.246.198", &ss_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Connection to storage server failed");
        return 1;
    }
    
    printf("✅ Connected to storage server\n");
    
    // Send READ command
    printf("Sending READ command...\n");
    send(sock, "READ test_storage1/file1.txt", 28, 0);
    
    // Read response
    char buffer[BUFFER_SIZE];
    printf("File content:\n");
    printf("--- START ---\n");
    
    int total_bytes = 0;
    while (1) {
        int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_read <= 0) {
            printf("Connection closed or error (bytes_read: %d)\n", bytes_read);
            break;
        }
        
        buffer[bytes_read] = '\0';
        total_bytes += bytes_read;
        
        // Check for EOF marker
        if (strncmp(buffer, "EOF", 3) == 0) {
            printf("EOF marker received\n");
            break;
        }
        
        printf("%s", buffer);
    }
    
    printf("\n--- END ---\n");
    printf("Total bytes received: %d\n", total_bytes);
    
    close(sock);
    printf("✅ Test completed\n");
    return 0;
}
