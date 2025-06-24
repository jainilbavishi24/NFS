#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

int main() {
    printf("=== NFS Working Test ===\n");
    
    // Test 1: Connect to naming server
    printf("Test 1: Connecting to naming server...\n");
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }
    
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(8090);
    inet_pton(AF_INET, "172.18.246.198", &ns_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0) {
        perror("Connection to naming server failed");
        return 1;
    }
    
    printf("✅ Connected to naming server\n");
    
    // Test 2: Send client identification
    printf("Test 2: Sending client identification...\n");
    send(sock, "CLIENT CONNECTING TO NAMING SERVER ...", 38, 0);
    
    char buffer[BUFFER_SIZE];
    int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("✅ Naming server response: %s", buffer);
    }
    
    // Test 3: Send READ command
    printf("Test 3: Sending READ command...\n");
    send(sock, "READ test_storage1/file1.txt", 28, 0);
    
    sleep(1);
    
    bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        printf("✅ Storage server info: %s", buffer);
        
        // Parse IP and port
        char ss_ip[16];
        int ss_port;
        if (sscanf(buffer, "IP: %15s Port: %d", ss_ip, &ss_port) == 2) {
            printf("✅ Parsed storage server: %s:%d\n", ss_ip, ss_port);
            
            // Test 4: Connect to storage server
            printf("Test 4: Connecting to storage server...\n");
            
            int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in ss_addr;
            ss_addr.sin_family = AF_INET;
            ss_addr.sin_port = htons(ss_port);
            inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);
            
            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == 0) {
                printf("✅ Connected to storage server\n");
                
                // Test 5: Send read request to storage server
                printf("Test 5: Requesting file from storage server...\n");
                send(ss_sock, "READ test_storage1/file1.txt", 28, 0);
                
                printf("File content:\n");
                printf("--- START ---\n");
                while ((bytes_read = recv(ss_sock, buffer, BUFFER_SIZE - 1, 0)) > 0) {
                    buffer[bytes_read] = '\0';
                    if (strncmp(buffer, "EOF", 3) == 0) break;
                    printf("%s", buffer);
                }
                printf("\n--- END ---\n");
                printf("✅ File read successfully\n");
                
                close(ss_sock);
            } else {
                printf("❌ Failed to connect to storage server\n");
            }
        } else {
            printf("❌ Failed to parse storage server info\n");
        }
    } else {
        printf("❌ No response from naming server\n");
    }
    
    close(sock);
    printf("\n=== Test Complete ===\n");
    return 0;
}
