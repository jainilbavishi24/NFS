#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/select.h>

#define BUFFER_SIZE 40960
#define TIMEOUT_SECONDS 5

int ns_sock;
int receiving_list = 0;
bool running = true;

typedef struct
{
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int reader_count;
    bool is_async_write;
    bool writer_present;
} FileLock;

#define MAX_FILES 1000

typedef struct
{
    char path[BUFFER_SIZE];
    FileLock lock;
} FileLockEntry;

FileLockEntry file_lock_map[MAX_FILES];
int file_lock_count = 0;
pthread_mutex_t file_map_lock = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t latest_ip_mutex = PTHREAD_MUTEX_INITIALIZER;

FileLock *get_file_lock(const char *path)
{
    pthread_mutex_lock(&file_map_lock);
    for (int i = 0; i < file_lock_count; i++)
    {
        if (strncmp(file_lock_map[i].path, path, BUFFER_SIZE - 1) == 0)
        {
            pthread_mutex_unlock(&file_map_lock);
            return &file_lock_map[i].lock;
        }
    }
    if (file_lock_count < MAX_FILES)
    {
        strncpy(file_lock_map[file_lock_count].path, path, BUFFER_SIZE - 1);
        file_lock_map[file_lock_count].path[BUFFER_SIZE - 1] = '\0';
        FileLock *new_lock = &file_lock_map[file_lock_count].lock;
        pthread_mutex_init(&new_lock->lock, NULL);
        pthread_cond_init(&new_lock->cond, NULL);
        new_lock->reader_count = 0;
        new_lock->is_async_write = false;
        new_lock->writer_present = false;
        file_lock_count++;
        pthread_mutex_unlock(&file_map_lock);
        return new_lock;
    }
    pthread_mutex_unlock(&file_map_lock);
    return NULL;
}

void read_file(const char *path)
{
    FileLock *file_lock = get_file_lock(path);
    if (!file_lock)
    {
        printf("Error: Unable to get lock for file %s\n", path);
        return;
    }
    pthread_mutex_lock(&file_lock->lock);
    while (file_lock->is_async_write || file_lock->writer_present)
    {
        pthread_cond_wait(&file_lock->cond, &file_lock->lock);
    }
    file_lock->reader_count++;
    pthread_mutex_unlock(&file_lock->lock);
    printf("Reading file: %s\n", path);
    sleep(1);
    pthread_mutex_lock(&file_lock->lock);
    file_lock->reader_count--;
    if (file_lock->reader_count == 0)
    {
        pthread_cond_signal(&file_lock->cond);
    }
    pthread_mutex_unlock(&file_lock->lock);
}

char critical_response_buffer[BUFFER_SIZE];
pthread_mutex_t response_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t response_cond = PTHREAD_COND_INITIALIZER;
bool critical_response_received = false;

void *listen_to_ns(void *arg);
void connect_and_read_from_ss(const char *ss_ip, int ss_port, const char *file_path);
void connect_and_write_to_ss(const char *ss_ip, int ss_port, const char *file_path, const char *data);
void connect_and_get_file_info(const char *ss_ip, int ss_port, const char *file_path);

char latest_IP_and_things_recieved_from_the_ns[BUFFER_SIZE];

void stream_from_server(const char *ss_ip, int ss_port, const char *file_path)
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char buffer[BUFFER_SIZE] = {0};
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    printf("IP: %s %d\n", ss_ip, ss_port);
    inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr);
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("Storage Server Connection failed");
        close(ss_sock);
        return;
    }
    snprintf(buffer, sizeof(buffer), "STREAM %s", file_path);
    send(ss_sock, buffer, strlen(buffer), 0);
    printf("\nRequest sent to Storage Server to stream: %s\n", buffer);
    FILE *audio_pipe = popen("mpv --no-terminal --ao=alsa -", "w");
    if (!audio_pipe)
    {
        perror("Error opening pipe to mpv");
        close(ss_sock);
        return;
    }
    int bytes_read;
    while ((bytes_read = recv(ss_sock, buffer, BUFFER_SIZE, 0)) > 0)
    {
        if (bytes_read == 3 && strncmp(buffer, "EOF", 3) == 0)
        {
            break;
        }
        fwrite(buffer, 1, bytes_read, audio_pipe);
    }
    if (bytes_read < 0)
    {
        perror("Error receiving audio data");
    }
    pclose(audio_pipe);
    close(ss_sock);
    printf("Streaming ended.\n");
}

void *listen_to_ns(void *arg)
{
    char buffer[BUFFER_SIZE] = {0};
    int flag = 0;
    while (running)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = read(ns_sock, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0)
        {
            buffer[bytes_read] = '\0';
            if (strstr(buffer, "File: ") != 0 || strstr(buffer, "Directory: ") != 0)
            {
                printf("%s", buffer);
                receiving_list = 1;
                continue;
            }
            else if (strncmp(buffer, "EOF", 3) == 0)
            {
                printf("End of list.\n");
                receiving_list = 0;
            }
            else if (strncmp("IP", buffer, 2) == 0 || strncmp("File not found in any storage server", buffer, 36) == 0 || strncmp("Unknown command", buffer, 15) == 0)
            {
                pthread_mutex_lock(&latest_ip_mutex);
                strncpy(latest_IP_and_things_recieved_from_the_ns, buffer, BUFFER_SIZE - 1);
                latest_IP_and_things_recieved_from_the_ns[BUFFER_SIZE - 1] = '\0';
                pthread_mutex_unlock(&latest_ip_mutex);
                continue;
            }
            else
            {
                printf("\n%s\n", buffer);
            }
            if (flag == 0)
            {
                printf("Enter a command: ");
            }
            flag = 1;
            fflush(stdout);
        }
        else if (bytes_read == 0)
        {
            printf("Naming Server connection closed.\n");
            running = false;
            break;
        }
        else
        {
            perror("Error reading from Naming Server");
            running = false;
            pthread_mutex_unlock(&response_mutex);
            break;
        }
    }
    return NULL;
}

pthread_t listener_thread;

void send_request_to_ns(const char *naming_server_ip, int ns_port, const char *command, const char *file_path)
{
    struct sockaddr_in ns_addr;
    char buffer[BUFFER_SIZE] = {0};
    if ((ns_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(ns_port);
    ns_addr.sin_addr.s_addr = inet_addr(naming_server_ip);
    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0)
    {
        perror("NS Connection failed");
        close(ns_sock);
        return;
    }
    snprintf(buffer, sizeof(buffer), "CLIENT CONNECTING TO NAMING SERVER ...");
    send(ns_sock, buffer, strlen(buffer), 0);
    printf("Connecting to Naming Server as client ...\n");
    sleep(1);
    pthread_create(&listener_thread, NULL, listen_to_ns, NULL);
    pthread_detach(listener_thread);
    int flag = 0;
    while (running)
    {
        char command[BUFFER_SIZE];
        char file_path[BUFFER_SIZE];
        int is_sync = 0;
        char file_name[BUFFER_SIZE];
        if (flag == 1)
        {
            printf("\nEnter a command: ");
        }
        flag = 1;
        if (!fgets(command, BUFFER_SIZE, stdin)) {
            printf("Error reading command.\n");
            continue;
        }
        if (strcmp(command, "\n") == 0 || strcmp(command, " ") == 0)
        {
            printf("Invalid Command\n");
            continue;
        }
        command[strcspn(command, "\n")] = 0;
        if (strcmp(command, "EXIT") == 0)
        {
            printf("Exiting.\n");
            running = false;
            break;
        }
        printf("Enter file path: ");
        if (!fgets(file_path, BUFFER_SIZE, stdin)) {
            printf("Error reading file path.\n");
            continue;
        }
        file_path[strcspn(file_path, "\n")] = 0;
        if (strcmp(command, "CREATE_DIC") == 0)
        {
            printf("Enter name of directory (without ./): ");
            if (!fgets(file_name, BUFFER_SIZE, stdin)) {
                printf("Error reading directory name.\n");
                continue;
            }
            file_name[strcspn(file_name, "\n")] = 0;
            strncat(file_path, "/", BUFFER_SIZE - strlen(file_path) - 1);
            strncat(file_path, file_name, BUFFER_SIZE - strlen(file_path) - 1);
        }
        else if (strcmp(command, "CREATE_F") == 0)
        {
            printf("Enter name of file (without ./): ");
            if (!fgets(file_name, BUFFER_SIZE, stdin)) {
                printf("Error reading file name.\n");
                continue;
            }
            file_name[strcspn(file_name, "\n")] = 0;
            strncat(file_path, "/", BUFFER_SIZE - strlen(file_path) - 1);
            strncat(file_path, file_name, BUFFER_SIZE - strlen(file_path) - 1);
        }
        snprintf(buffer, sizeof(buffer), "%s %s", command, file_path);
        send(ns_sock, buffer, strlen(buffer), 0);
        printf("Request sent to Naming Server: %s\n", buffer);
        sleep(0.7);
        if (strstr(buffer, "Unknown command") != 0)
        {
            printf("Unknown command: %s\n", command);
            continue;
        }
        if (strcmp(command, "LIST") == 0 || strcmp(command, "CREATE_DIC") == 0 || strcmp(command, "CREATE_F") == 0 || strcmp(command, "DELETE") == 0 || strcmp(command, "COPY") == 0)
        {
            continue;
        }
        if (strstr(buffer, "File not found in any storage server") != 0)
        {
            continue;
        }
        int bytes_read = 0;
        sleep(1);
        char local_latest_ip[BUFFER_SIZE];
        pthread_mutex_lock(&latest_ip_mutex);
        strncpy(local_latest_ip, latest_IP_and_things_recieved_from_the_ns, BUFFER_SIZE - 1);
        local_latest_ip[BUFFER_SIZE - 1] = '\0';
        pthread_mutex_unlock(&latest_ip_mutex);
        int byts_read = strlen(local_latest_ip);
        if (strncmp(local_latest_ip, "Unknown command", strlen("Unknown command")) == 0)
        {
            printf("Unknown command: %s\n", command);
            continue;
        }
        printf("Naming Server sent the details of the storage server:\n%s\n", local_latest_ip);
        bytes_read = 1;
        if (byts_read > 0)
        {
            buffer[byts_read] = '\0';
            if (strstr(local_latest_ip, "File not found in any storage server") != 0)
            {
                continue;
            }
            char ss_ip[INET_ADDRSTRLEN];
            int ss_port;
            sscanf(local_latest_ip, " IP: %s Port: %d", ss_ip, &ss_port);
            printf("Connecting to Storage Server at IP: %s, Port: %d\n", ss_ip, ss_port);
            if (strcmp(command, "READ") == 0)
            {
                connect_and_read_from_ss(ss_ip, ss_port, file_path);
            }
            else if (strcmp(command, "WRITE") == 0)
            {
                char sync_flag[BUFFER_SIZE];
                printf("Do you want to write synchronously irrespective of time overhead? (yes/no): ");
                if (!fgets(sync_flag, BUFFER_SIZE, stdin)) {
                    printf("Error reading sync flag.\n");
                    continue;
                }
                sync_flag[strcspn(sync_flag, "\n")] = 0;
                if (strcmp(sync_flag, "yes") == 0)
                    is_sync = 1;
                memset(buffer, 0, sizeof(buffer));
                if (is_sync)
                {
                    snprintf(buffer, sizeof(buffer), "--SYNC");
                }
                printf("Enter data to write: ");
                if (!fgets(buffer + strlen(buffer), BUFFER_SIZE - strlen(buffer), stdin)) {
                    printf("Error reading data to write.\n");
                    continue;
                }
                connect_and_write_to_ss(ss_ip, ss_port, file_path, buffer);
            }
            else if (strcmp(command, "INFO") == 0)
            {
                connect_and_get_file_info(ss_ip, ss_port, file_path);
            }
            else if (strcmp(command, "STREAM") == 0)
            {
                char *ext = strrchr(file_path, '.');
                if (ext == NULL || strcmp(ext, ".mp3") != 0)
                {
                    printf("Invalid file extension. Only .mp3 files are supported for streaming.\n");
                    continue;
                }
                if (system("which mpv > /dev/null 2>&1") != 0) {
                    printf("mpv is not installed. Please install mpv to use streaming.\n");
                    continue;
                }
                stream_from_server(ss_ip, ss_port, file_path);
            }
            else if (strcmp(command, "LIST") == 0)
            {
                printf("List of files in the directory:\n%s\n", local_latest_ip);
                send(ns_sock, "LIST", strlen("LIST"), 0);
                sleep(1);
            }
            else
            {
                printf("Wrong input command: %s\n", command);
            }
        }
    }
    close(ns_sock);
}

void connect_and_read_from_ss(const char *ss_ip, int ss_port, const char *file_path)
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char buffer[BUFFER_SIZE] = {0};
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    ss_addr.sin_addr.s_addr = inet_addr(ss_ip);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid address or address not supported\n");
        close(ss_sock);
        return;
    }
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("SS Connection failed");
        close(ss_sock);
        return;
    }
    snprintf(buffer, sizeof(buffer), "READ %s", file_path);
    send(ss_sock, buffer, strlen(buffer), 0);
    printf("File content:\n");
    while (1)
    {
        int bytes_read = read(ss_sock, buffer, BUFFER_SIZE - 1);
        if (bytes_read <= 0)
        {
            break;
        }
        buffer[bytes_read] = '\0';
        if (strcmp(buffer, "Write in progress. Cannot read the file right now. Please try again later.\n") == 0)
        {
            printf("Write in progress. Cannot read the file right now. Please try again later.\n");
            break;
        }
        if (strcmp(buffer, "EOF\n") == 0)
        {
            break;
        }
        printf("%s", buffer);
    }
    close(ss_sock);
}

void connect_and_write_to_ss(const char *ss_ip, int ss_port, const char *file_path, const char *data)
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char buffer[BUFFER_SIZE] = {0};
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid address or address not supported\n");
        close(ss_sock);
        return;
    }
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("SS Connection failed");
        close(ss_sock);
        return;
    }
    snprintf(buffer, sizeof(buffer), "WRITE %s %s", file_path, data);
    send(ss_sock, buffer, strlen(buffer), 0);
    sleep(1);
    if (strncmp(data, "--SYNC", 6) != 0)
    {
        size_t data_len = strlen(data);
        size_t sent_bytes = 0;
        while (sent_bytes < data_len)
        {
            size_t chunk_size = (data_len - sent_bytes > BUFFER_SIZE) ? BUFFER_SIZE : data_len - sent_bytes;
            ssize_t bytes_sent = send(ss_sock, data + sent_bytes, chunk_size, 0);
            if (bytes_sent < 0)
            {
                perror("Error sending data");
                break;
            }
            sent_bytes += bytes_sent;
        }
    }
    else
    {
        send(ss_sock, data, strlen(data), 0);
    }
    sleep(0.4);
    snprintf(data, sizeof(data), "EOF");
    send(ss_sock, data, strlen(data), 0);
    int bytes_read = read(ss_sock, buffer, BUFFER_SIZE - 1);
    if (bytes_read > 0)
    {
        buffer[bytes_read] = '\0';
        printf("Storage Server response: %s\n", buffer);
    }
    close(ss_sock);
}

void connect_and_get_file_info(const char *ss_ip, int ss_port, const char *file_path)
{
    int ss_sock;
    struct sockaddr_in ss_addr;
    char buffer[BUFFER_SIZE] = {0};
    if ((ss_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(ss_port);
    if (inet_pton(AF_INET, ss_ip, &ss_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid address or address not supported\n");
        close(ss_sock);
        return;
    }
    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0)
    {
        perror("SS Connection failed");
        close(ss_sock);
        return;
    }
    snprintf(buffer, sizeof(buffer), "INFO %s", file_path);
    send(ss_sock, buffer, strlen(buffer), 0);
    int bytes_read = read(ss_sock, buffer, BUFFER_SIZE - 1);
    if (bytes_read > 0)
    {
        buffer[bytes_read] = '\0';
        printf("File info from Storage Server: %s\n", buffer);
    }
    close(ss_sock);
}

int main(int argc, char *argv[])
{
    char naming_server_ip[BUFFER_SIZE];
    int ns_port;
    if (argc < 3)
    {
        printf("Usage: %s <naming_server_ip> <ns_port>\n", argv[0]);
        return 1;
    }
    strcpy(naming_server_ip, argv[1]);
    ns_port = atoi(argv[2]);
    send_request_to_ns(naming_server_ip, ns_port, "", "");
    return 0;
}
