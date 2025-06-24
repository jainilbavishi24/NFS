#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

int main() {
    printf("📝 === NFS WRITE OPERATION TEST ===\n");
    
    // Step 1: Connect to naming server
    printf("Step 1: Connecting to naming server...\n");
    int ns_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ns_addr;
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(8090);
    inet_pton(AF_INET, "172.18.246.198", &ns_addr.sin_addr);
    
    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0) {
        printf("❌ Failed to connect to naming server\n");
        return 1;
    }
    printf("✅ Connected to naming server\n");
    
    // Step 2: Send client identification
    send(ns_sock, "CLIENT CONNECTING TO NAMING SERVER ...", 38, 0);
    char buffer[BUFFER_SIZE];
    recv(ns_sock, buffer, BUFFER_SIZE - 1, 0);
    printf("✅ Client identified\n");
    
    // Step 3: Send WRITE command
    printf("Step 2: Sending WRITE command...\n");
    send(ns_sock, "WRITE test_storage1/file2.txt", 29, 0);
    sleep(1);
    
    int bytes = recv(ns_sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        printf("✅ Naming server response: %s", buffer);
        
        // Parse storage server info
        char ss_ip[16];
        int ss_port;
        if (sscanf(buffer, "IP: %15s Port: %d", ss_ip, &ss_port) == 2) {
            printf("✅ Storage server: %s:%d\n", ss_ip, ss_port);
            
            // Step 4: Connect to storage server
            printf("Step 3: Connecting to storage server...\n");
            int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in ss_addr;
            ss_addr.sin_family = AF_INET;
            ss_addr.sin_port = htons(ss_port);
            inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);
            
            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == 0) {
                printf("✅ Connected to storage server\n");
                
                // Step 5: Send WRITE command with data
                printf("Step 4: Sending WRITE command with data...\n");
                char write_cmd[] = "WRITE test_storage1/file2.txt Hello from NFS test!";
                send(ss_sock, write_cmd, strlen(write_cmd), 0);
                printf("✅ WRITE command sent\n");
                
                // Wait for response
                sleep(1);
                bytes = recv(ss_sock, buffer, BUFFER_SIZE - 1, 0);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    printf("✅ Storage server response: %s", buffer);
                }
                
                close(ss_sock);
            } else {
                printf("❌ Failed to connect to storage server\n");
            }
        }
    }
    
    close(ns_sock);
    
    // Step 6: Verify the write by reading the file directly
    printf("Step 5: Verifying write operation...\n");
    FILE *file = fopen("test_storage1/file2.txt", "r");
    if (file) {
        char content[256];
        if (fgets(content, sizeof(content), file)) {
            printf("✅ File content after write: %s", content);
        }
        fclose(file);
    } else {
        printf("❌ Could not read file after write\n");
    }
    
    printf("\n🎉 WRITE test completed!\n");
    return 0;
}
