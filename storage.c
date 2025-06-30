#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h> // Include pthread for threading
#include <stdbool.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#define BUFFER_SIZE 40960
// #define DEFAULT_PORT 9099  // Default port for Storage Server
#define ASYNC_THRESHOLD 10 // Define a threshold for switching between sync/async
#define CHUNK_SIZE 2       // Size of chunks for flushing to persistent memory
#define MAX_FILES 1000     // Maximum number of files supported by the server

int storage_port;
int naming_server_sock;

typedef struct FileAccessControl
{
    char file_path[BUFFER_SIZE];
    pthread_mutex_t write_mutex; // Mutex to lock write operations
    pthread_mutex_t read_mutex;  // Mutex to protect read count updates
    pthread_cond_t write_cond;   // Condition variable to signal write availability
    int read_count;              // Track active readers
} FileAccessControl;

typedef struct AsyncWriteTask
{
    char file_path[BUFFER_SIZE];
    char *data;
    int data_size;
    int client_socket;
    int bytes_written;
    FileAccessControl *file_access; // Add this line to reference the file's access control
} AsyncWriteTask;

typedef struct
{

    char filepath[BUFFER_SIZE];

    char content[BUFFER_SIZE];

    size_t content_length;

} AsyncFileWriteArgs;

FileAccessControl file_access_controls[MAX_FILES];
int file_access_count = 0;
pthread_mutex_t access_management_mutex = PTHREAD_MUTEX_INITIALIZER;

FileAccessControl *get_file_access(const char *file_path)
{
    printf("Requesting access control for file: %s\n", file_path);
    pthread_mutex_lock(&access_management_mutex);

    // Check if the file already has access control initialized
    for (int i = 0; i < file_access_count; i++)
    {
        if (strcmp(file_access_controls[i].file_path, file_path) == 0)
        {
            printf("Found existing access control for file: %s\n", file_path);
            pthread_mutex_unlock(&access_management_mutex);
            return &file_access_controls[i];
        }
    }

    // If no entry exists, create a new one
    if (file_access_count < MAX_FILES)
    {
        printf("Creating new access control for file: %s\n", file_path);
        strncpy(file_access_controls[file_access_count].file_path, file_path, BUFFER_SIZE - 1);
        file_access_controls[file_access_count].file_path[BUFFER_SIZE - 1] = '\0';
        pthread_mutex_init(&file_access_controls[file_access_count].write_mutex, NULL);
        pthread_mutex_init(&file_access_controls[file_access_count].read_mutex, NULL);
        pthread_cond_init(&file_access_controls[file_access_count].write_cond, NULL);
        file_access_controls[file_access_count].read_count = 0;
        file_access_count++;
        pthread_mutex_unlock(&access_management_mutex);
        return &file_access_controls[file_access_count - 1];
    }

    printf("Failed to create access control for file: %s (max limit reached)\n", file_path);
    pthread_mutex_unlock(&access_management_mutex);
    return NULL; // Failed to allocate a new entry
}

// Function prototypes
void list_files_recursive(const char *path, char *file_list);
void handle_command(const char *command, const char *path);
void listen_for_commands(int sock);
void register_with_naming_server(const char *ip, int port, int storage_port, const char *storage_server_ip, const char *file_name);
void start_storage_server(int port);
void *handle_client_thread(void *client_sock); // Use pthread for concurrent client handling
void process_command(const char *command, const char *file_path, char *data, int client_sock);
void send_file_content(const char *file_path, int client_sock);
void receive_file_content(const char *file_path, int client_sock, char *data, bool async);
void send_file_info(const char *file_path, int client_sock);
void *async_write_handler(void *arg);
void notify_naming_server(const char *status);
void *naming_server_communication_thread(void *arg);

void list_files_recursive(const char *path, char *file_list)
{
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(path)))
    {
        perror("opendir");
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_DIR)
        {
            char new_path[BUFFER_SIZE];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            {
                continue;
            }

            snprintf(new_path, sizeof(new_path), "%s/%s", path, entry->d_name);
            snprintf(file_list + strlen(file_list), BUFFER_SIZE - strlen(file_list), "Directory: %s\n", new_path);
            list_files_recursive(new_path, file_list);
        }
        else
        {
            snprintf(file_list + strlen(file_list), BUFFER_SIZE - strlen(file_list), "File: %s/%s\n", path, entry->d_name);
        }
    }
    closedir(dir);
}

#define INITIAL_CAPACITY 10000
#define PATH_SIZE 10250

typedef struct
{
    char **paths;
    int size;
    int capacity;
} DeletedPaths;

void initialize_deleted_paths(DeletedPaths *deleted_paths)
{
    deleted_paths->paths = malloc(INITIAL_CAPACITY * sizeof(char *));
    if (deleted_paths->paths == NULL)
    {
        perror("Failed to allocate memory for paths");
        exit(EXIT_FAILURE);
    }
    deleted_paths->size = 0;
    deleted_paths->capacity = INITIAL_CAPACITY;
}

void add_deleted_path(DeletedPaths *deleted_paths, const char *path)
{
    if (deleted_paths->size >= deleted_paths->capacity)
    {
        deleted_paths->capacity *= 2;
        deleted_paths->paths = realloc(deleted_paths->paths, deleted_paths->capacity * sizeof(char *));
        if (deleted_paths->paths == NULL)
        {
            perror("Failed to reallocate memory for paths");
            exit(EXIT_FAILURE);
        }
    }

    deleted_paths->paths[deleted_paths->size] = strdup(path);
    if (deleted_paths->paths[deleted_paths->size] == NULL)
    {
        perror("Failed to duplicate path");
        exit(EXIT_FAILURE);
    }

    deleted_paths->size++;
}

void free_deleted_paths(DeletedPaths *deleted_paths)
{
    for (int i = 0; i < deleted_paths->size; i++)
    {
        free(deleted_paths->paths[i]);
    }
    free(deleted_paths->paths);
}

void delete_directory_recursive(const char *path, DeletedPaths *deleted_paths)
{
    struct dirent *entry;
    DIR *dir = opendir(path);

    if (dir == NULL)
    {
        perror("Failed to open directory");
        return;
    }

    char full_path[10240];
    struct stat path_stat;

    while ((entry = readdir(dir)) != NULL)
    {
        // Skip `.` and `..`
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Build the full path for the entry
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        // Check if the entry is a directory or file
        if (stat(full_path, &path_stat) == 0)
        {
            if (S_ISDIR(path_stat.st_mode))
            {
                // Recursively delete subdirectory
                delete_directory_recursive(full_path, deleted_paths);
            }
            else
            {
                // Delete file
                if (remove(full_path) == 0)
                {
                    printf("Deleted file: %s\n", full_path);
                    add_deleted_path(deleted_paths, full_path);
                }
                else
                {
                    perror("Failed to delete file");
                }
            }
        }
    }

    closedir(dir);

    // Remove the directory itself
    if (remove(path) == 0)
    {
        printf("Deleted directory: %s\n", path);
        add_deleted_path(deleted_paths, path);
    }
    else
    {
        perror("Failed to remove directory");
    }
}

void display_deleted_paths(const DeletedPaths *deleted_paths)
{
    printf("Deleted paths:\n");
    for (int i = 0; i < deleted_paths->size; i++)
    {
        printf("%s\n", deleted_paths->paths[i]);
    }
}

void *async_file_write(void *args)

{

    AsyncFileWriteArgs *file_args = (AsyncFileWriteArgs *)args;

    // Create directories for the filepath if necessary

    char directory_path[BUFFER_SIZE];

    strncpy(directory_path, file_args->filepath, BUFFER_SIZE);

    char *last_slash = strrchr(directory_path, '/');

    if (last_slash)

    {

        *last_slash = '\0'; // Remove the file name to isolate the directory path

        // mkdir(directory_path, 0755);
        char temp_path[1024];
        strcpy(temp_path, directory_path);
        size_t len = strlen(temp_path);

        if (temp_path[len - 1] == '/')
        {
            temp_path[len - 1] = '\0';
        }

        for (char *p = temp_path + 1; *p; p++)
        {
            if (*p == '/')
            {
                *p = '\0';
                mkdir(temp_path, 0755);
                *p = '/';
            }
        }

        mkdir(temp_path, 0755);
    }

    // Write the file content

    FILE *file = fopen(file_args->filepath, "w");

    if (!file)

    {

        perror("Error opening file for writing");

        free(file_args); // Clean up memory

        return NULL;
    }

    if (fwrite(file_args->content, 1, file_args->content_length, file) != file_args->content_length)

    {

        perror("Error writing file content");

        fclose(file);

        free(file_args); // Clean up memory

        return NULL;
    }

    fclose(file);

    printf("File successfully stored at: %s\n", file_args->filepath);

    free(file_args); // Clean up memory

    return NULL;
}

void handle_command(const char *command, const char *path)
{
    printf("handle Received command '%s' for path '%s'\n", command, path);

    if (strcmp(command, "CREATE_DIC") == 0)
    {
        printf("Creating directory at %s\n", path);

        char pwd[BUFFER_SIZE];

        // getcwd(pwd, sizeof(pwd));

        // strcat(pwd, path);

        mkdir(path, 0755);

        printf("Created directory at %s\n", pwd);
    }

    else if (strcmp(command, "CREATE_F") == 0)
    {

        printf("Creating file at %s\n", path);

        FILE *file = fopen(path, "w");

        if (file == NULL)

        {

            perror("File creation failed");

            return;
        }

        fclose(file);

        printf("Created file at %s\n", path);
    }
    else if (strcmp(command, "DELETE") == 0)
    {
        struct stat path_stat;

        // Check if the path exists and get its properties
        if (stat(path, &path_stat) != 0)
        {
            perror("Path does not exist or stat failed");
            return;
        }

        if (S_ISDIR(path_stat.st_mode))
        {
            // If it's a directory, delete it recursively
            DeletedPaths deleted_paths;
            initialize_deleted_paths(&deleted_paths);
            delete_directory_recursive(path, &deleted_paths);
            display_deleted_paths(&deleted_paths);
            free_deleted_paths(&deleted_paths);
        }
        else
        {
            // If it's a file, delete it
            if (remove(path) == 0)
            {
                printf("Deleted file: %s\n", path);
            }
            else
            {
                perror("Failed to delete file");
            }
        }
    }
    else if (strcmp(command, "COPY") == 0)
    {
        // Implement file copy logic here if needed
        printf("Copy command received for path %s\n", path);
    }
}

void *naming_server_communication_thread(void *arg)
{

    int sock = *(int *)arg;

    char buffer[BUFFER_SIZE];

    while (1)

    {

        printf("Waiting for message from Naming Server...\n");

        memset(buffer, 0, BUFFER_SIZE);

        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0)

        {

            printf("Connection to Naming Server lost\n");

            break;
        }

        printf("Bytes received: %d\n", bytes_received);

        buffer[bytes_received] = '\0';

        printf("Raw message received from Naming Server: %s\n", buffer);

        // Parse the command and path

        char command[BUFFER_SIZE] = {0};

        char path[BUFFER_SIZE] = {0};

        // Use a more robust parsing approach

        if (sscanf(buffer, "%s %[^\n]", command, path) >= 2)

        {

            printf("Parsed Command: %s, Path: %s\n", command, path);

            if (strcmp(command, "CREATE_DIC") == 0)

            {

                handle_command("CREATE_DIC", path);
            }

            else if (strcmp(command, "CREATE_F") == 0)

            {

                handle_command("CREATE_F", path);
            }

            else if (strcmp(command, "DELETE") == 0)
            {
                FileAccessControl *file_access = get_file_access(path);
                if (file_access == NULL)
                {
                    perror("Failed to get file access for deletion");
                    pthread_exit(NULL);
                }

                // Attempt to acquire the write mutex
                if (pthread_mutex_trylock(&file_access->write_mutex) != 0)
                {
                    // If the mutex is already locked, notify the client
                    printf("File is currently being written: %s\n", path);
                    const char *busy_msg = "Write in progress. Cannot delete the file right now. Please try again later.\n";
                    send(sock, busy_msg, strlen(busy_msg), 0);
                    continue;
                }

                // Check if there are active readers
                pthread_mutex_lock(&file_access->read_mutex);
                if (file_access->read_count > 0)
                {
                    // If there are active readers, notify the client
                    printf("File is currently being read: %s\n", path);
                    const char *busy_msg = "Read in progress. Cannot delete the file right now. Please try again later.\n";
                    send(sock, busy_msg, strlen(busy_msg), 0);
                    pthread_mutex_unlock(&file_access->read_mutex);
                    pthread_mutex_unlock(&file_access->write_mutex);
                    continue;
                }
                pthread_mutex_unlock(&file_access->read_mutex);

                handle_command("DELETE", path);
            }

            else if (strcmp(command, "STOP") == 0)

            {

                printf("Received STOP command from Naming Server\n");

                send(sock, "STOP_ACK", strlen("STOP_ACK"), 0);

                break;
            }

            else

            {

                printf("Unknown command received: %s\n", command);
            }
        }

        else

        {

            printf("Failed to parse command and path from: %s\n", buffer);
        }
    }

    close(sock);

    pthread_exit(NULL);
}

void listen_for_commands(int sock)
{
    char buffer[BUFFER_SIZE];
    while (1)
    {
        printf("Listening for commands from Naming Server...\n");
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received <= 0)
        {
            printf("Connection closed by Naming Server or error occurred\n");
            break;
        }

        buffer[strcspn(buffer, "\n")] = 0;

        // Check if received message is "STOP"
        if (strcmp(buffer, "STOP") == 0)
        {
            printf("Received STOP command, closing connection\n");
            send(sock, "STOP_ACK", strlen("STOP_ACK"), 0); // Acknowledge stop
            break;
        }

        // Parse the received command and path
        char command[BUFFER_SIZE], path[BUFFER_SIZE];
        sscanf(buffer, "%s %s", command, path);
        printf("Received command '%s' for path '%s'\n", command, path);
        handle_command(command, path);
    }
    close(sock);
}

void register_with_naming_server(const char *ip, int port, int storage_port, const char *storage_server_ip, const char *file_name)
{
    int sock;
    struct sockaddr_in server_address;
    char file_list[BUFFER_SIZE] = {0};

    // List files in accessible path
    // char cwd[BUFFER_SIZE];
    // strcpy(cwd, ".");

    // printf("CWD is %s\n", cwd);

    list_files_recursive(file_name, file_list);

    char ip_port_info[BUFFER_SIZE];
    snprintf(ip_port_info, sizeof(ip_port_info), "IP: %s Port: %d", ip, storage_port);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        return;
    }

    naming_server_sock = sock;

    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = inet_addr(ip);
    server_address.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_address.sin_addr) <= 0)
    {
        perror("Invalid address/ Address not supported");
        return;
    }

    if (connect(sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Connection to Naming Server failed");
        return;
    }

    // Send file list to Naming Server
    send(sock, "STORAGE", strlen("STORAGE"), 0);
    sleep(1);

    char my_ip_and_port[BUFFER_SIZE];
    snprintf(my_ip_and_port, sizeof(my_ip_and_port), "%s %d", storage_server_ip, storage_port);

    send(sock, my_ip_and_port, strlen(my_ip_and_port), 0);

    sleep(1);

    printf("Sending file list to Naming Server...\n");
    send(sock, file_list, strlen(file_list), 0);

    // Receive assigned IP and port from Naming Server
    // char ip_port_info[BUFFER_SIZE];
    // snprintf(ip_port_info, sizeof(ip_port_info), "%s %d", "127.0.0.1", storage_port);
    // send(sock, ip_port_info, strlen(ip_port_info), 0);

    // printf("Sent IP and port to Naming Server: %s %d\n", "127.0.0.1", storage_port);

    // Create thread for continuous communication with naming server

    pthread_t naming_server_thread;

    int *sock_ptr = malloc(sizeof(int));

    *sock_ptr = naming_server_sock;

    if (pthread_create(&naming_server_thread, NULL, naming_server_communication_thread, sock_ptr) != 0)
    {

        perror("Failed to create naming server communication thread");

        free(sock_ptr);

        close(naming_server_sock);

        return;
    }

    pthread_detach(naming_server_thread);
}

void start_storage_server(int port)
{
    int server_sock, client_sock;
    struct sockaddr_in server_address, client_address;
    socklen_t client_address_len = sizeof(client_address);
    int opt = 1;

    // Create a socket for the Storage Server
    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse port
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Configure server address
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    printf("Storage Server started on port %d\n", port);

    // Bind the socket to the specified port
    if (bind(server_sock, (struct sockaddr *)&server_address, sizeof(server_address)) < 0)
    {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // Start listening for incoming connections
    if (listen(server_sock, 1555) < 0)
    {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("Storage Server is listening on port %d...\n", port);

    // Accept client connections and handle them
    while (1)
    {
        client_sock = accept(server_sock, (struct sockaddr *)&client_address, &client_address_len);
        if (client_sock < 0)
        {
            perror("Accept failed");
            continue;
        }

        printf("Client connected!\n");

        // Create a new thread to handle each client
        pthread_t client_thread;
        int *client_sock_ptr = malloc(sizeof(int));
        *client_sock_ptr = client_sock;
        if (pthread_create(&client_thread, NULL, handle_client_thread, client_sock_ptr) != 0)
        {
            perror("Failed to create thread");
            free(client_sock_ptr);
        }
    }

    close(server_sock);
}

void send_audio_file(const char *file_path, int client_sock)

{

    FILE *file = fopen(file_path, "rb");

    if (!file)

    {

        perror("Error opening audio file");

        send(client_sock, "Error: Unable to open file", strlen("Error: Unable to open file"), 0);

        return;
    }

    char buffer[BUFFER_SIZE];

    size_t bytes_read;

    // Send the file in binary chunks

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)

    {

        // printf("Sending audio data...\n");

        if (send(client_sock, buffer, bytes_read, 0) == -1)

        {

            printf("Error sending audio data\n");

            perror("Error sending audio data");

            break;
        }
    }

    // printf("sdbffdghggfnbfgng \n");

    sleep(1);

    // After streaming the audio data

    send(client_sock, "EOF", strlen("EOF"), 0);

    fclose(file);

    printf("Finished streaming audio file: %s\n", file_path);
}

void fetch_directory(int client_sock, const char *dir_path)

{

    DIR *dir = opendir(dir_path);

    if (!dir)

    {

        perror("Error opening directory");

        char response[] = "ERROR: Directory not found\n";

        send(client_sock, response, strlen(response), 0);

        return;
    }

    struct dirent *entry;

    char file_path[BUFFER_SIZE];

    char buffer[BUFFER_SIZE];

    while ((entry = readdir(dir)) != NULL)

    {

        // Skip current and parent directories

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)

            continue;

        snprintf(file_path, BUFFER_SIZE, "%s/%s", dir_path, entry->d_name);

        struct stat file_stat;

        if (stat(file_path, &file_stat) == -1)

        {

            perror("Error getting file stats");

            continue;
        }

        if (S_ISDIR(file_stat.st_mode))

        {

            // Recursively fetch subdirectory

            fetch_directory(client_sock, file_path);
        }

        else if (S_ISREG(file_stat.st_mode))

        {

            // Send file name and content

            snprintf(buffer, BUFFER_SIZE, "FILENAME %s\n", file_path);

            send(client_sock, buffer, strlen(buffer), 0);

            FILE *file = fopen(file_path, "r");

            if (!file)

            {

                perror("Error opening file for reading");

                continue;
            }

            size_t bytes_read;

            while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, file)) > 0)

            {

                if (send(client_sock, buffer, bytes_read, 0) < 0)

                {

                    perror("Error sending file data");

                    fclose(file);

                    closedir(dir);

                    return;
                }
            }

            fclose(file);

            // Indicate end of file

            send(client_sock, "EOF", 3, 0);
        }
    }

    closedir(dir);

    printf("Directory %s transferred successfully.\n", dir_path);
}

void handle_client(int client_sock)
{
    char buffer[BUFFER_SIZE];

    // Read command from the client
    while (1)
    {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_received = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received <= 0)
        {
            printf("Connection closed by client or error occurred\n");
            break;
        }

        buffer[bytes_received] = '\0';
        // printf("Received from client: %s\n", buffer);

        // Parse the command and file path (handle READ or WRITE commands)
        char command[BUFFER_SIZE], file_path[BUFFER_SIZE], file_data[BUFFER_SIZE], filepath[BUFFER_SIZE];

        const char *file_content = NULL;

        const char *first_space = strchr(buffer, ' ');

        if (!first_space)

        {

            fprintf(stderr, "Error: Invalid format, no filepath found\n");

            return;
        }

        // Extract the command

        size_t command_length = first_space - buffer;

        strncpy(command, buffer, command_length);

        command[command_length] = '\0'; // Null-terminate the command

        // Check if the command is "STORE"

        if (strcmp(command, "STORE") == 0)
        {
                        size_t content_length;

            char writable_content[BUFFER_SIZE];

            const char *second_space = strchr(first_space + 1, ' ');

            if (!second_space)

            {

                fprintf(stderr, "Error: Invalid format, no file content found\n");

                return;
            }

            size_t filepath_length = second_space - first_space - 1;

            strncpy(filepath, first_space + 1, filepath_length);

            filepath[filepath_length] = '\0'; // Null-terminate the filepath

            file_content = second_space + 1;

            // Check for EOF marker

            char *eof_marker = strstr(file_content, "EOF");

            if (eof_marker)

            {

                content_length = eof_marker - file_content; // Exclude EOF
            }

            else

            {

                fprintf(stderr, "Warning: EOF marker missing, writing all received data\n");

                content_length = strlen(file_content); // Write everything received
            }

            if (content_length >= BUFFER_SIZE)

            {

                fprintf(stderr, "Error: Content too large to handle\n");

                return;
            }

            strncpy(writable_content, file_content, content_length);

            writable_content[content_length] = '\0'; // Null-terminate
            printf("FILE CONTE %s", writable_content);
            // Prepare arguments for the asynchronous task
            if (strcmp(writable_content, "JUST") == 0)
            {
                char temp_path[1024];
                strcpy(temp_path, filepath);
                size_t len = strlen(temp_path);

                if (temp_path[len - 1] == '/')
                {
                    temp_path[len - 1] = '\0';
                }

                for (char *p = temp_path + 1; *p; p++)
                {
                    if (*p == '/')
                    {
                        *p = '\0';
                        mkdir(temp_path, 0755);
                        *p = '/';
                    }
                }

                mkdir(temp_path, 0755);

                // mkdir(file_path, 0755);
                printf("Directory created successfully at: '%s'\n", filepath);
            }
            else
            {

                AsyncFileWriteArgs *args = malloc(sizeof(AsyncFileWriteArgs));

                if (!args)

                {

                    perror("Failed to allocate memory for asynchronous task");

                    return;
                }

                strncpy(args->filepath, filepath, BUFFER_SIZE - 1);
                strncpy(args->content, writable_content, BUFFER_SIZE - 1);
                args->content_length = content_length;
                FileAccessControl *file_access = get_file_access(filepath);

                pthread_mutex_lock(&file_access->read_mutex);
                file_access->read_count++;
                pthread_t thread;

                if (pthread_create(&thread, NULL, async_file_write, args) != 0)

                {

                    perror("Failed to create thread for asynchronous file writing");

                    free(args); // Clean up memory

                    return;
                }

                file_access->read_count--;
                pthread_mutex_unlock(&file_access->read_mutex);
                // Detach the thread so it cleans up after itself

                pthread_detach(thread);
            }
        }

        int n = sscanf(buffer, "%s %s", command, file_path);

        if (n < 2)
        {
            printf("Invalid command format\n");
            continue;
        }

        if (strcmp(command, "READ") == 0)
        {
            // For READ command, only file_path is used
            process_command(command, file_path, NULL, client_sock);
        }
        else if (strcmp(command, "WRITE") == 0)
        {
            // For WRITE command, the file data needs to be received after the file path
            // Wait for the file data from the client (assuming it is sent after the path)
            printf("Waiting for file data...\n");

            int total_bytes_received = 0;

            while (total_bytes_received < BUFFER_SIZE - 1)
            {
                int bytes_received = recv(client_sock, file_data + total_bytes_received, BUFFER_SIZE - 1 - total_bytes_received, 0);
                if (bytes_received <= 0)
                {
                    printf("Error or connection closed while receiving file data\n");
                    break;
                }
                total_bytes_received += bytes_received;
                if (strstr(file_data, "EOF") != NULL)
                {
                    break;
                }
            }
            file_data[total_bytes_received] = '\0'; // Null-terminate the file data

            // int file_data_length = recv(client_sock, file_data, BUFFER_SIZE - 1, 0);
            // if (file_data_length <= 0)
            // {
            //     printf("Error or connection closed while receiving file data\n");
            //     break;
            // }

            // file_data[file_data_length] = '\0'; // Null-terminate the file data

            printf("Received file data: %s\n", file_data);
            process_command(command, file_path, file_data, client_sock);
        }
        else if (strcmp(command, "STREAM") == 0)

        {

            // char crt_dir[4096];

            // getcwd(crt_dir, sizeof(crt_dir));

            // strcat(crt_dir, file_path);

            process_command(command, file_path, file_data, client_sock);
        }

        else if ((strcmp(command, "INFO")) == 0)

        {

            process_command(command, file_path, NULL, client_sock);
        }
        else if ((strcmp(command, "FETCH")) == 0)

        {

            // sleep(2);

            struct stat file_stat;

            if (stat(file_path, &file_stat) == -1)

            {

                perror("Error checking path");

                char response[] = "ERROR: File or directory not found\n";

                send(client_sock, response, strlen(response), 0);

                return;
            }

            if (S_ISDIR(file_stat.st_mode))

            {

                printf("Cannot error a directory\n");
                // fetch_directory(client_sock, file_path);
            }

            else if (S_ISREG(file_stat.st_mode))

            {

                if (access(file_path, F_OK) != 0)

                {

                    perror("File not found");

                    char response[] = "ERROR: File not found\n";

                    send(client_sock, response, strlen(response), 0);
                }

                else

                {

                    FILE *file = fopen(file_path, "r");

                    if (!file)

                    {

                        perror("Error opening file");

                        char response[] = "ERROR: Unable to open file\n";

                        send(client_sock, response, strlen(response), 0);
                    }

                    else

                    {

                        // Read the file content and send it to the client

                        char file_buffer[BUFFER_SIZE];

                        size_t bytes_read;

                        FileAccessControl *file_access = get_file_access(file_path);
                        pthread_mutex_lock(&file_access->read_mutex);
                        file_access->read_count++;

                        while ((bytes_read = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0)

                        {

                            if (send(client_sock, file_buffer, bytes_read, 0) < 0)

                            {

                                perror("Error sending file data");

                                fclose(file);

                                close(client_sock);

                                return;
                            }
                        }

                        file_access->read_count--;
                        pthread_mutex_unlock(&file_access->read_mutex);

                        fclose(file);

                        // Indicate end of file transfer

                        char eof_message[] = "EOF";

                        send(client_sock, eof_message, strlen(eof_message), 0);

                        printf("File sent successfully: %s\n", file_path);
                    }
                }
            }
        }
        else
        {
            printf("Unknown command: %s\n", command);
            // send(client_sock, "Unknown command", strlen("Unknown command"), 0);
        }
    }
}

void *handle_client_thread(void *client_sock_ptr)
{
    int client_sock = *(int *)client_sock_ptr;
    free(client_sock_ptr);

    handle_client(client_sock);
    close(client_sock);

    printf("Client thread finished.\n");
    pthread_exit(NULL); // Exit the thread after handling the client
}

void process_command(const char *command, const char *file_path, char *data, int client_sock)
{
    if (strcmp(command, "READ") == 0)
    {
        send_file_content(file_path, client_sock);
    }
    else if (strcmp(command, "WRITE") == 0 && data != NULL)
    {
        printf("Came here and file path is %s\n", file_path);

        bool async;
        if (strncmp(data, "--SYNC", 6) == 0)
        {
            async = false;
            data += 6;
        }
        else
        {
            async = (strlen(data) > ASYNC_THRESHOLD) ? true : false;
        }

        receive_file_content(file_path, client_sock, data, async);
    }
    else if (strcmp(command, "INFO") == 0)
    {
        send_file_info(file_path, client_sock);
    }
    else if (strcmp(command, "STREAM") == 0)
    {
        send_audio_file(file_path, client_sock);
    }
    else
    {
        printf("Unknown command: %s\n", command);
        send(client_sock, "Unknown command", strlen("Unknown command"), 0);
    }
}

void send_file_content(const char *file_path, int client_sock)
{
    FileAccessControl *file_access = get_file_access(file_path);
    if (file_access == NULL)
    {
        perror("Failed to get file access for reading");
        const char *error_msg = "Concurrent reading error\n";
        send(client_sock, error_msg, strlen(error_msg), 0);
        return;
    }

    // Attempt to acquire read access by checking if the write lock is free
    if (pthread_mutex_trylock(&file_access->write_mutex) != 0)
    {
        // If write mutex is already locked, notify the reader
        printf("File is currently being written: %s\n", file_path);
        const char *busy_msg = "Write in progress. Cannot read the file right now. Please try again later.\n";
        send(client_sock, busy_msg, strlen(busy_msg), 0);
        return; // Return early without proceeding further
    }

    // Acquired write_mutex successfully, release it immediately for proper read control
    pthread_mutex_unlock(&file_access->write_mutex);

    // Proceed to acquire read access
    pthread_mutex_lock(&file_access->read_mutex);
    file_access->read_count++;
    if (file_access->read_count == 1)
    {
        // First reader locks the write access to prevent concurrent writes
        pthread_mutex_lock(&file_access->write_mutex);
    }
    pthread_mutex_unlock(&file_access->read_mutex);

    char temp[BUFFER_SIZE];
    strcpy(temp, file_path);

    FILE *file = fopen(temp, "r");
    char buffer[BUFFER_SIZE];

    if (file == NULL)
    {
        perror("File open failed");
        const char *error_msg = "File not found\n";
        send(client_sock, error_msg, strlen(error_msg), 0);

        pthread_mutex_lock(&file_access->read_mutex);
        file_access->read_count--;
        if (file_access->read_count == 0)
        {
            pthread_mutex_unlock(&file_access->write_mutex);
        }
        pthread_mutex_unlock(&file_access->read_mutex);

        return;
    }

    // Send file content to the client, line by line
    while (fgets(buffer, sizeof(buffer), file) != NULL)
    {
        // Send each line followed by a newline character
        if (send(client_sock, buffer, strlen(buffer), 0) == -1)
        {
            perror("Send failed");
            fclose(file);

            pthread_mutex_lock(&file_access->read_mutex);
            file_access->read_count--;
            if (file_access->read_count == 0)
            {
                pthread_mutex_unlock(&file_access->write_mutex);
            }
            pthread_mutex_unlock(&file_access->read_mutex);

            return;
        }
    }

    printf("File sent successfully\n");

    pthread_mutex_lock(&file_access->read_mutex);
    file_access->read_count--;
    if (file_access->read_count == 0)
    {
        pthread_mutex_unlock(&file_access->write_mutex);
    }
    pthread_mutex_unlock(&file_access->read_mutex);

    fclose(file);
    close(client_sock); // Close the client connection after sending the file
}

void receive_file_content(const char *file_path, int client_sock, char *data, bool async)
{
    printf("Receiving file content for path: %s\n", file_path);
    FileAccessControl *file_access = get_file_access(file_path);
    if (file_access == NULL)
    {
        perror("Failed to get file access for writing");
        const char *error_msg = "Internal error\n";
        send(client_sock, error_msg, strlen(error_msg), 0);
        return;
    }

    // Attempt to acquire the write mutex
    if (pthread_mutex_trylock(&file_access->write_mutex) != 0)
    {
        // If the mutex is already locked, notify the client
        printf("File is currently being written: %s\n", file_path);
        const char *busy_msg = "Write in progress. Please wait...\n";
        send(client_sock, busy_msg, strlen(busy_msg), 0);
        return; // Return early without proceeding further
    }

    printf("Acquired write mutex for file: %s\n", file_path);

    if (async)
    {
        printf("Performing asynchronous write for file: %s\n", file_path);
        AsyncWriteTask *task = (AsyncWriteTask *)malloc(sizeof(AsyncWriteTask));
        strcpy(task->file_path, file_path);
        task->data = strdup(data); // Copy data for async processing
        task->data_size = strlen(data);
        task->client_socket = client_sock;
        task->file_access = file_access; // This line assigns the FileAccessControl to the task

        send(client_sock, "Asynchronous write accepted\n", strlen("Asynchronous write accepted\n"), 0);

        pthread_t async_thread;
        if (pthread_create(&async_thread, NULL, async_write_handler, (void *)task) != 0)
        {
            perror("Failed to create async write thread");
            free(task->data);
            free(task);
            pthread_mutex_unlock(&file_access->write_mutex);
            return;
        }

        // Send immediate acknowledgment
        close(client_sock);
    }
    else
    {
        printf("Performing synchronous write for file: %s\n", file_path);
        FILE *file = fopen(file_path, "w");
        if (file == NULL)
        {
            perror("File open failed");
            const char *error_msg = "Cannot create file\n";
            send(client_sock, error_msg, strlen(error_msg), 0);
            pthread_mutex_unlock(&file_access->write_mutex); // Release write access
            return;
        }

        // Write data to the file
        fputs(data, file);
        printf("Data written to file: %s\n", file_path);

        send(client_sock, "File written successfully\n", strlen("File written successfully\n"), 0);

        char message[BUFFER_SIZE];

        snprintf(message, sizeof(message), "WRITE_SUCCESS %s", file_path);

        printf("Notifying Naming Server: %s\n", message);

        notify_naming_server(message);

        printf("Releasing write mutex for file: %s\n", file_path);
        pthread_mutex_unlock(&file_access->write_mutex);

        fclose(file);
        close(client_sock);
    }
}

void *async_write_handler(void *arg)
{

    printf("Async write handler started\n");
    printf("Path Given is %s\n", ((AsyncWriteTask *)arg)->file_path);

    char temp[BUFFER_SIZE];

    AsyncWriteTask *task = (AsyncWriteTask *)arg;
    printf("Async write handler started for file: %s\n", task->file_path);

    // Perform the asynchronous write
    FILE *file = fopen(task->file_path, "w");
    if (file == NULL)
    {
        perror("File open failed for async write");
        notify_naming_server("ASYNC_WRITE_FAIL");
        free(task->data);
        pthread_mutex_unlock(&task->file_access->write_mutex); // Unlock the mutex on failure
        free(task);
        return NULL;
    }

    // Flush data in chunks to the file
    int bytes_written = 0;
    while (bytes_written < task->data_size)
    {
        int chunk_size = (task->data_size - bytes_written > CHUNK_SIZE) ? CHUNK_SIZE : (task->data_size - bytes_written);
        fwrite(task->data + bytes_written, 1, chunk_size, file);
        bytes_written += chunk_size;

        char update_msg[BUFFER_SIZE];
        snprintf(update_msg, sizeof(update_msg), "ASYNC_WRITE_PROGRESS %d %s",
                 task->client_socket, task->file_path);
        notify_naming_server(update_msg);

        sleep(2);
        fflush(file); // Flush each chunk to persistent memory
    }

    fclose(file);

    // Notify Naming Server about successful write
    char notification[BUFFER_SIZE];
    snprintf(notification, sizeof(notification), "%s %d %s", "ASYNC_WRITE_SUCCESS", task->client_socket, task->file_path);
    notify_naming_server(notification);

    // Release the write mutex only after the write is complete
    printf("Releasing write mutex after async write for file: %s\n", task->file_path);
    pthread_mutex_unlock(&task->file_access->write_mutex);

    free(task->data);
    free(task);
    return NULL;
}

void notify_naming_server(const char *status)
{
    send(naming_server_sock, status, strlen(status), 0);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>

void send_file_info(const char *file_path, int client_sock)
{
    char temp[BUFFER_SIZE];
    strcpy(temp, file_path);

    struct stat file_stat;

    // Get file stats
    if (stat(temp, &file_stat) == -1)
    {
        perror("stat");
        send(client_sock, "Error: File not found\n", strlen("Error: File not found\n"), 0);
        return;
    }

    char info[BUFFER_SIZE];
    memset(info, 0, sizeof(info));

    // File size
    snprintf(info, sizeof(info), "File size: %ld bytes\n", file_stat.st_size);

    // Permissions
    char permissions[10];
    permissions[0] = (file_stat.st_mode & S_IRUSR) ? 'r' : '-';
    permissions[1] = (file_stat.st_mode & S_IWUSR) ? 'w' : '-';
    permissions[2] = (file_stat.st_mode & S_IXUSR) ? 'x' : '-';
    permissions[3] = (file_stat.st_mode & S_IRGRP) ? 'r' : '-';
    permissions[4] = (file_stat.st_mode & S_IWGRP) ? 'w' : '-';
    permissions[5] = (file_stat.st_mode & S_IXGRP) ? 'x' : '-';
    permissions[6] = (file_stat.st_mode & S_IROTH) ? 'r' : '-';
    permissions[7] = (file_stat.st_mode & S_IWOTH) ? 'w' : '-';
    permissions[8] = (file_stat.st_mode & S_IXOTH) ? 'x' : '-';
    permissions[9] = '\0';

    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Permissions: %s\n", permissions);

    // Ownership
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Owner UID: %d\n", file_stat.st_uid);
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Group GID: %d\n", file_stat.st_gid);

    // Timestamps
    char time_buffer[100];
    struct tm *tm_info;

    tm_info = localtime(&file_stat.st_atime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Last accessed: %s\n", time_buffer);

    tm_info = localtime(&file_stat.st_mtime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Last modified: %s\n", time_buffer);

    tm_info = localtime(&file_stat.st_ctime);
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "Last status change: %s\n", time_buffer);

    // Other metadata
    snprintf(info + strlen(info), sizeof(info) - strlen(info), "File type: ");
    if (S_ISREG(file_stat.st_mode))
        strcat(info, "Regular file\n");
    else if (S_ISDIR(file_stat.st_mode))
        strcat(info, "Directory\n");
    else if (S_ISLNK(file_stat.st_mode))
        strcat(info, "Symbolic link\n");
    else
        strcat(info, "Other\n");

    // Send the information to the client
    send(client_sock, info, strlen(info), 0);
}

int main(int argc, char *argv[])
{

    if (argc < 5)
    {
        fprintf(stderr, "Usage: %s <Naming Server IP> <Naming Server Port> <Storage Server Port> <Folder Name>\n", argv[0]);
        return 1;
    }

    const char *naming_server_ip = argv[1];
    int naming_server_port = atoi(argv[2]);
    int storage_server_port = atoi(argv[3]);
    const char *folder_name = argv[4];

    // Retrieve the IP address of the Storage Server using 'hostname -I'
    char storage_ip[BUFFER_SIZE];
    FILE *fp = popen("hostname -I", "r");
    if (fp == NULL)
    {
        perror("Failed to run command to get Storage Server IP");
        return 1;
    }

    // Read the first IP address returned by 'hostname -I'
    if (fgets(storage_ip, sizeof(storage_ip), fp) != NULL)
    {
        // Remove any trailing newline or spaces
        storage_ip[strcspn(storage_ip, " \n")] = '\0';
    }
    pclose(fp);

    // Check if we successfully retrieved the Storage Server IP
    if (strlen(storage_ip) == 0)
    {
        fprintf(stderr, "Could not determine Storage Server IP.\n");
        return 1;
    }

    printf("Registering with Naming Server at %s:%d\n", naming_server_ip, naming_server_port);
    printf("Storage Server IP: %s, Port: %d\n", storage_ip, storage_server_port);

    register_with_naming_server(naming_server_ip, naming_server_port, storage_server_port, storage_ip, folder_name);

    sleep(2);

    // Start the Storage Server to listen for client requests

    start_storage_server(storage_server_port);

    return 0;
}