#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>

#include <sys/stat.h>

#include <dirent.h>

#include <sys/types.h>

// #include "trie.h"

char my_ip[INET_ADDRSTRLEN];

#define PORT 8090
// #define STORAGE_PORT 8081
#define REPLICATION_FACTOR 3
#define MAX_CLIENTS 10
#define BUFFER_SIZE 40960
#define ALPHABET_SIZE 128 // ASCII range to cover all characters
#define CACHE_SIZE 5      // LRU cache size for recent searches

typedef struct
{
    char ip[INET_ADDRSTRLEN];
    int port;
    int socket_fd;
    int is_async_write_in_progress;
    int async_writer_socket;
    int is_server_down;
    char **path_list;
    int backup_ss[2];
    int path_count;
} StorageServer;

typedef StorageServer StorageServer;
// Trie Node definition
typedef struct TrieNode
{
    struct TrieNode *children[ALPHABET_SIZE];
    int is_end_of_path;    // 1 if the node represents the end of a valid path
    StorageServer *server; // Pointer to the associated StorageServer
    int is_directory;
    int is_deleted;
} TrieNode;

typedef struct
{
    char path[BUFFER_SIZE];
    int found_index;
    time_t last_accessed;
} CacheEntry;

typedef struct
{
    CacheEntry entries[CACHE_SIZE];
    int count;
} LRUCache;

TrieNode *global_trie_root = NULL;

StorageServer *storage_servers = NULL;
int server_count = 0;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
LRUCache lru_cache = {.count = 0};

// Function prototypes
void log_message(const char *format, ...);
void cache_insert(const char *path, int found_index);
int cache_lookup(const char *path);

TrieNode *create_trie_node(StorageServer *server);
void insert_path(TrieNode *root, const char *path, StorageServer *server, int is_directory);
StorageServer *search_path(TrieNode *root, const char *path, int want_to_delete);
void free_trie(TrieNode *root);
StorageServer *path_exists(const char *path, StorageServer **tempo);

StorageServer *find_storage_server_by_path(const char *path);
void remove_storage_server(int socket_fd);
void parse_and_store_files(StorageServer *server, const char *buffer);

void storage_server_thread(int client_sock);
void *handle_client(void *arg);
void *main_server_thread(void *arg);

// Additional function prototypes
int perform_copy_between_servers(int src_port, int dest_port, const char *source, const char *destination);
int perform_copy_between_servers1(int src_port, int dest_port, const char *source, const char *destination);
void send_command_to_storage(const StorageServer *server, const char *command, const char *path);

char buffer_back[BUFFER_SIZE] = {0};

// Print all trie paths that match a given prefix, sending results to the client socket.
void print_trie_paths1(TrieNode *root, char *path, int level, int client_sock, char *prefix)
{
    // If this node or its subtree is deleted, skip it
    if (root->is_deleted)
    {
        return;
    }

    // If this node is the end of a valid path and the server is up
    if (root->is_end_of_path && root->server != NULL && !root->server->is_server_down && !root->is_deleted)
    {
        path[level] = '\0'; // Null-terminate the string
        // Check if this is a directory or file and send the appropriate message
        if (root->is_directory)
        {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "Directory: %s\n", path);
            if (strstr(path, prefix) != NULL)
                send(client_sock, temp, strlen(temp), 0);
        }
        else if (root->is_directory == 0)
        {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "File: %s\n", path);
            if (strstr(path, prefix) != NULL)
                send(client_sock, temp, strlen(temp), 0);
        }
    }
    // Recursively traverse all children
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (root->children[i])
        {
            path[level] = i; // Add current character to the path
            print_trie_paths1(root->children[i], path, level + 1, client_sock, prefix);
        }
    }
}

// Wrapper function to print all trie paths matching a prefix
void print_all_trie_paths1(int client_sock, char *prefix)
{
    char path[BUFFER_SIZE]; // Buffer to store the path as it's built
    print_trie_paths1(global_trie_root, path, 0, client_sock, prefix);
    sleep(0.5);
    send(client_sock, "EOF", strlen("EOF"), 0);
}

// Print all trie paths (no prefix filtering)
void print_trie_paths(TrieNode *root, char *path, int level, int client_sock)
{
    if (root->is_deleted)
    {
        return;
    }
    if (root->is_end_of_path && root->server != NULL && !root->server->is_server_down && !root->is_deleted)
    {
        path[level] = '\0'; // Null-terminate the string
        if (root->is_directory)
        {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "Directory: %s\n", path);
            send(client_sock, temp, strlen(temp), 0);
        }
        else if (root->is_directory == 0)
        {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "File: %s\n", path);
            send(client_sock, temp, strlen(temp), 0);
        }
    }
    // Recursively traverse the children
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (root->children[i])
        {
            path[level] = i; // Add current character to the path
            print_trie_paths(root->children[i], path, level + 1, client_sock);
        }
    }
}

// Wrapper function to print all paths in the global Trie
void print_all_trie_paths(int client_sock)
{
    char path[BUFFER_SIZE]; // Buffer to store the path as it's built
    print_trie_paths(global_trie_root, path, 0, client_sock);
    sleep(0.5);
    send(client_sock, "EOF", strlen("EOF"), 0);
}

// LRU cache lookup function
int cache_lookup(const char *path)
{
    for (int i = 0; i < lru_cache.count; i++)
    {
        if (strcmp(lru_cache.entries[i].path, path) == 0)
        {
            log_message("Found in cache\n");
            // printf("Found in cache\n");
            lru_cache.entries[i].last_accessed = time(NULL); // Update access time
            // printf("Found in cache\n");
            return lru_cache.entries[i].found_index;
        }
    }
    return -1; // Not found in cache
}

// LRU cache insert function
void cache_insert(const char *path, int found_index)
{
    if (lru_cache.count < CACHE_SIZE)
    {
        strcpy(lru_cache.entries[lru_cache.count].path, path);
        lru_cache.entries[lru_cache.count].found_index = found_index;
        lru_cache.entries[lru_cache.count].last_accessed = time(NULL);
        lru_cache.count++;
    }
    else
    {
        // Evict least recently used
        int lru_index = 0;
        for (int i = 1; i < CACHE_SIZE; i++)
        {
            if (lru_cache.entries[i].last_accessed < lru_cache.entries[lru_index].last_accessed)
                lru_index = i;
        }
        strcpy(lru_cache.entries[lru_index].path, path);
        lru_cache.entries[lru_index].found_index = found_index;
        lru_cache.entries[lru_index].last_accessed = time(NULL);
        // log_message("Evicted from cache\n");
        // printf("Evicted from cache\n");
    }
}

// Function to log messages with IP, port, and status
void log_message(const char *format, ...)
{
    FILE *log_file = fopen("naming_server.log", "a");
    if (!log_file)
    {
        perror("Failed to open log file");
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fclose(log_file);
}

// Function to create a new Trie Node
// Allocates and initializes a new TrieNode. Returns NULL on failure.
TrieNode *create_trie_node(StorageServer *server)
{
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    if (!node)
    {
        printf("Error: Failed to allocate memory for trie node\n");
        return NULL;
    }
    // Initialize all fields
    node->is_end_of_path = 0;
    node->is_deleted = 0;
    node->is_directory = 0;
    node->server = server;
    if (server)
    {
        server->is_server_down = 0;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++)
        node->children[i] = NULL;
    return node;
}

// Function to insert a path into the Trie
// Associates the path with a StorageServer and marks as directory or file.
void insert_path(TrieNode *root, const char *path, StorageServer *server, int is_directory)
{
    if (!root || !path || !server)
    {
        printf("Error: Invalid parameters in insert_path\n");
        return;
    }
    TrieNode *crawler = root;
    while (*path)
    {
        if (!crawler->children[(int)*path])
        {
            crawler->children[(int)*path] = create_trie_node(server);
            if (!crawler->children[(int)*path])
            {
                printf("Error: Failed to create trie node in insert_path\n");
                return;
            }
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    crawler->is_end_of_path = 1;
    crawler->is_deleted = 0;
    crawler->is_directory = is_directory;
    crawler->server = server; // Associate the path with the StorageServer
    if (crawler->server)
    {
        crawler->server->is_server_down = 0;
    }
}

// Recursively marks a subtree as deleted (used for delete operations)
void mark_subtree_as_deleted(TrieNode *node)
{
    if (node == NULL)
        return;
    if (node->is_end_of_path)
    {
        node->is_deleted = 1;
    }
    // Recursively mark all children as deleted
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (node->children[i] != NULL)
        {
            mark_subtree_as_deleted(node->children[i]);
        }
    }
}

// Function to search for a path in the Trie
// Returns the associated StorageServer, or NULL if not found or deleted.
// If want_to_delete is set, marks the subtree as deleted.
StorageServer *search_path(TrieNode *root, const char *path, int want_to_delete)
{
    TrieNode *crawler = root;
    while (*path)
    {
        if (!crawler->children[(int)*path] || crawler->is_deleted)
        {
            return NULL;
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    if (crawler != NULL && crawler->is_end_of_path && crawler->server != NULL)
    {
        if (want_to_delete)
        {
            mark_subtree_as_deleted(crawler);
        }
        if (crawler->server->is_server_down || crawler->is_deleted)
        {
            return NULL;
        }
        return crawler->server;
    }
    return NULL;
}

// Function to search for a path in the Trie and return the associated server (no delete)
StorageServer *search_trie(TrieNode *root, const char *path)
{
    TrieNode *current = root;
    // Traverse the Trie based on the characters in the path
    for (int i = 0; path[i] != '\0'; i++)
    {
        int index = path[i];
        // If the current character does not have a corresponding child node, return NULL
        if (!current->children[index])
        {
            return NULL;
        }
        // Move to the next node
        current = current->children[index];
        // If the current node is deleted, the path is invalid
        if (current->is_deleted)
        {
            return NULL;
        }
    }
    // Check if the current node marks the end of a valid path and return the server
    if (current->is_end_of_path && current->server != NULL && current->server->is_server_down == 0)
    {
        return current->server;
    }
    // If it's not the end of a valid path, return NULL
    return NULL;
}

// Recursively frees all nodes in the Trie
void free_trie(TrieNode *root)
{
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (root->children[i])
        {
            free_trie(root->children[i]);
        }
    }
    free(root);
}

StorageServer *path_exists(const char *path, StorageServer **tempo)
{
    pthread_mutex_lock(&lock);
    int exists = (search_path(global_trie_root, path, 0) != NULL);
    if (exists == 0)
    {
        char temp1[1024] = "Backup1";
        strcat(temp1, path);
        exists = (search_path(global_trie_root, temp1, 0) != NULL);
        if (exists == 0)
        {
            char temp2[1024] = "Backup2";
            strcat(temp2, path);
            // *tempo = search_path(global_trie_root, temp2,0);
            int exists = (search_path(global_trie_root, temp2, 0) != NULL);
            pthread_mutex_unlock(&lock);
            return search_path(global_trie_root, temp2, 0);
        }
        pthread_mutex_unlock(&lock);
        return search_path(global_trie_root, temp1, 0);
    }
    else
    {
        pthread_mutex_unlock(&lock);
        return search_path(global_trie_root, path, 0);
    }
    pthread_mutex_unlock(&lock);
    return *tempo;
}

StorageServer *find_storage_server_by_path(const char *path)
{
    if (cache_lookup(path) != -1)
    {
        return &storage_servers[cache_lookup(path)];
    }
    StorageServer *server = search_path(global_trie_root, path, 0);
    if (server)
    {
        for (int i = 0; i < server_count; i++)
        {
            if (&storage_servers[i] == server)
            {
                cache_insert(path, i);
                return server;
            }
        }
    }
    return NULL;
}

static bool has_children(TrieNode *node)
{
    if (node == NULL)
        return false;
    for (int i = 0; i < 26; i++)
    {
        if (node->children[i] != NULL)
        {
            return true;
        }
    }
    return false;
}

// Helper function to safely free a node
static void free_trie_node(TrieNode *node)
{
    if (node == NULL)
        return;
    // Don't free the server pointer as it's managed elsewhere
    node->server = NULL;
    free(node);
}

// Removes a path from the trie recursively, freeing nodes as needed
// Returns true if the node can be deleted
bool remove_path_from_trie(TrieNode *node, const char *path, int depth)
{
    int index = path[depth] - 'a';
    if (index < 0 || index >= 26)
    {
        return false; // Invalid character in path
    }
    // Recursively remove from child
    if (node->children[index] != NULL)
    {
        bool should_delete_child = remove_path_from_trie(node->children[index], path, depth + 1);
        if (should_delete_child)
        {
            free_trie_node(node->children[index]);
            node->children[index] = NULL;
            // If this node is not an endpoint and has no other children, signal it can be deleted
            if (!node->is_end_of_path && !has_children(node))
            {
                return true;
            }
        }
    }
    return false;
}

// Removes all paths associated with a storage server from the trie
void remove_paths_for_server(StorageServer *server)
{
    if (global_trie_root == NULL || server == NULL || server->path_list == NULL)
    {
        return;
    }
    pthread_mutex_lock(&lock); // Add lock for thread safety
    for (int i = 0; i < server->path_count; i++)
    {
        if (server->path_list[i] != NULL)
        {
            remove_path_from_trie(global_trie_root, server->path_list[i], 0);
            free(server->path_list[i]);
            server->path_list[i] = NULL;
        }
    }
    // Free the path list array itself
    free(server->path_list);
    server->path_list = NULL;
    server->path_count = 0;
    pthread_mutex_unlock(&lock); // Release lock
}

// Helper to clean up TrieNode->server pointers for a removed server
void clean_trie_server_pointers(TrieNode *node, StorageServer *removed_server) {
    if (!node) return;
    if (node->server == removed_server) {
        node->server = NULL;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            clean_trie_server_pointers(node->children[i], removed_server);
        }
    }
}

// Removes a storage server from the list and updates the trie
void remove_storage_server(int socket_fd)
{
    pthread_mutex_lock(&lock);
    int i;
    for (i = 0; i < server_count; i++)
    {
        if (storage_servers[i].socket_fd == socket_fd)
        {
            // Clean up trie pointers before realloc
            clean_trie_server_pointers(global_trie_root, &storage_servers[i]);
            // Shift remaining servers down in the array
            for (int j = i; j < server_count - 1; j++)
            {
                storage_servers[j] = storage_servers[j + 1];
            }
            server_count--;
            StorageServer *tmp = realloc(storage_servers, server_count * sizeof(StorageServer));
            if (server_count > 0 && tmp == NULL) {
                perror("Failed to realloc storage_servers");
            } else {
                storage_servers = tmp;
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock);
}

// Adds a path to a storage server's path list
void add_path_to_server(StorageServer *server, const char *path)
{
    if (server == NULL || path == NULL)
    {
        return;
    }
    if (server->path_list == NULL)
    {
        server->path_list = malloc(sizeof(char *));
        if (server->path_list == NULL)
        {
            perror("Failed to allocate memory for path list");
            return;
        }
    }
    else
    {
        char **tmp = realloc(server->path_list, (server->path_count + 1) * sizeof(char *));
        if (tmp == NULL)
        {
            perror("Failed to reallocate memory for path list");
            return;
        }
        server->path_list = tmp;
    }
    server->path_list[server->path_count] = strdup(path);
    if (server->path_list[server->path_count] == NULL)
    {
        perror("Failed to allocate memory for path string");
        return;
    }
    server->path_count++;
}

// Handles backup/replication: parses file list and stores backups on backup servers
void parse_and_store_backup(StorageServer *server, const char *buffer)
{
    if (!server || !buffer)
    {
        printf("Error: Invalid server or buffer in parse_and_store_backup\n");
        return;
    }
    char buffer1[1024];
    strncpy(buffer1, buffer, sizeof(buffer1) - 1);
    buffer1[sizeof(buffer1) - 1] = '\0';
    char *line = strtok((char *)buffer, "\n");
    while (line != NULL)
    {
        sleep(3);
        if (strncmp(line, "Directory:", 10) == 0 || strncmp(line, "File:", 5) == 0)
        {
            int a = (strncmp(line, "Directory:", 10) == 0) ? 1 : 0;
            char *space_pos = strchr(line, ' ');
            if (!space_pos)
            {
                printf("Error: Invalid line format in backup: %s\n", line);
                line = strtok(NULL, "\n");
                continue;
            }
            const char *path = space_pos + 1;
            // Backup 1
            if (server->backup_ss[0] >= 0 && server->backup_ss[0] < server_count)
            {
                char buf[1024];
                strcpy(buf, path);
                perform_copy_between_servers(server->port, storage_servers[server->backup_ss[0]].port, path, buf);
                char temp1[1024] = "Backup1";
                strcat(temp1, path);
                insert_path(global_trie_root, temp1, &storage_servers[server->backup_ss[0]], a);
            }
            // Backup 2
            if (server->backup_ss[1] >= 0 && server->backup_ss[1] < server_count)
            {
                perform_copy_between_servers(server->port, storage_servers[server->backup_ss[1]].port, path, path);
                char temp2[1024] = "Backup2";
                strcat(temp2, path);
                insert_path(global_trie_root, temp2, &storage_servers[server->backup_ss[1]], a);
            }
        }
        line = strtok(NULL, "\n");
    }
}

// Parses file list from a storage server and inserts into the trie
void parse_and_store_files(StorageServer *server, const char *buffer)
{
    if (!server || !buffer)
    {
        printf("Error: Invalid server or buffer in parse_and_store_files\n");
        return;
    }
    // Initialize global trie root if not already done
    if (!global_trie_root)
    {
        global_trie_root = create_trie_node(NULL);
        if (!global_trie_root)
        {
            printf("Error: Failed to create global trie root\n");
            return;
        }
    }

    char buffer1[1024];
    strncpy(buffer1, buffer, sizeof(buffer1) - 1);
    buffer1[sizeof(buffer1) - 1] = '\0';

    char *line = strtok((char *)buffer, "\n");

    while (line != NULL)
    {
        if (strncmp(line, "Directory:", 10) == 0 || strncmp(line, "File:", 5) == 0)
        {
            int a = (strncmp(line, "Directory:", 10) == 0) ? 1 : 0;
            char *space_pos = strchr(line, ' ');
            if (!space_pos)
            {
                printf("Error: Invalid line format: %s\n", line);
                line = strtok(NULL, "\n");
                continue;
            }
            const char *path = space_pos + 1;
            if (!path || strlen(path) == 0)
            {
                printf("Error: Empty path in line: %s\n", line);
                line = strtok(NULL, "\n");
                continue;
            }
            add_path_to_server(server, path);
            insert_path(global_trie_root, path, server, a);
            if (server_count > 2 && server->backup_ss[0] >= 0 && server->backup_ss[0] < server_count)        
            {
                char buf[1024];
                strcpy(buf, path);
                perform_copy_between_servers(server->port, storage_servers[server->backup_ss[0]].port, path, buf);
                char temp1[1024] = "Backup1";

                strcat(temp1, path);

                insert_path(global_trie_root, temp1, &storage_servers[server->backup_ss[0]], a);
            }

            // Backup 2
            if (server_count > 2 && server->backup_ss[1] >= 0 && server->backup_ss[1] < server_count)
            {
                perform_copy_between_servers(server->port, storage_servers[server->backup_ss[1]].port, path, path);
                char temp2[1024] = "Backup2";
                strcat(temp2, path);
                insert_path(global_trie_root, temp2, &storage_servers[server->backup_ss[1]], a);
            }
        }
        line = strtok(NULL, "\n");
    }
}

// Collects all paths associated with a storage server into a buffer
void collect_paths_to_buffer(TrieNode *node, char *current_path, int depth, char *buffer, StorageServer *server)
{

    if (node == NULL)

        return;

    // If this node marks the end of a path, append it to the buffer

    if (node->is_end_of_path && node->server == server)
    {
        current_path[depth] = '\0';
        strcat(buffer, "File: "); // Null-terminate the current path
        strcat(buffer, current_path); // Append the path to the buffer
        strcat(buffer, "\n"); // Add a newline
    }

    // Recur for each child

    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (node->children[i] != NULL)
        {
            current_path[depth] = (char)i; // Add the current character to the path
            collect_paths_to_buffer(node->children[i], current_path, depth + 1, buffer, server);
        }
    }
}

// Retrieves all paths associated with a storage server into a buffer
void retrieve_paths_to_buffer(TrieNode *root, char *buffer, StorageServer *server)
{
    if (root == NULL)
        return;

    char current_path[BUFFER_SIZE] = {0}; // Temporary array for constructing paths

    buffer[0] = '\0'; // Ensure buffer starts empty

    collect_paths_to_buffer(root, current_path, 0, buffer, server);
}

// Checks if a path exists in the trie and is a valid endpoint
int validate_path(TrieNode *root, const char *path)
{
    TrieNode *crawler = root;
    while (*path)
    {
        if (!crawler->children[(int)*path])
            return 0; // Path does not exist
        crawler = crawler->children[(int)*path];
        path++;
    }
    return crawler->is_end_of_path; // Return 1 if it is the end of a valid path
}

// Recursively finds all file paths with a given prefix and stores them in results
void find_paths_with_prefix(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count)
{
    if (!node)
        return;
    if (node->is_end_of_path && !node->is_directory)
    {
        current_path[depth] = '\0'; // Null-terminate the path
        results[*count] = strdup(current_path);
        if (!results[*count]) {
            perror("strdup failed in find_paths_with_prefix");
            return;
        }
        (*count)++;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (node->children[i])
        {
            current_path[depth] = (char)i;
            find_paths_with_prefix(node->children[i], current_path, depth + 1, prefix, results, count);
        }
    }
}

// Recursively finds all paths (files and directories) with a given prefix
void find_paths_with_prefix_two(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count)
{
    if (!node)
        return;
    if (node->is_end_of_path)
    {
        current_path[depth] = '\0'; // Null-terminate the path
        results[*count] = strdup(current_path);
        if (!results[*count]) {
            perror("strdup failed in find_paths_with_prefix_two");
            return;
        }
        (*count)++;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++)
    {
        if (node->children[i])
        {
            current_path[depth] = (char)i;
            find_paths_with_prefix_two(node->children[i], current_path, depth + 1, prefix, results, count);
        }
    }
}

// Returns a list of file paths with a given prefix (files only)
char **search_trie_for_prefix(const char *prefix, int *result_count)
{
    TrieNode *current = global_trie_root;
    for (const char *p = prefix; *p; p++)
    {
        int index = (unsigned char)*p;
        if (!current->children[index])
        {
            *result_count = 0;
            return NULL;
        }
        current = current->children[index];
    }
    // At this point, 'current' points to the last node in the prefix
    char **results = (char **)malloc(100 * sizeof(char *)); // Allocate memory for up to 100 results
    if (!results) {
        perror("malloc failed in search_trie_for_prefix");
        *result_count = 0;
        return NULL;
    }
    char current_path[1024];
    strcpy(current_path, prefix); // Start with the prefix
    *result_count = 0;
    find_paths_with_prefix(current, current_path, strlen(prefix), prefix, results, result_count);
    return results;
}

// Returns a list of all paths (files and directories) with a given prefix
char **search_trie_for_prefix_two(const char *prefix, int *result_count)
{
    TrieNode *current = global_trie_root;
    for (const char *p = prefix; *p; p++)
    {
        int index = (unsigned char)*p;
        if (!current->children[index])
        {
            *result_count = 0;
            return NULL;
        }
        current = current->children[index];
    }
    // At this point, 'current' points to the last node in the prefix
    char **results = (char **)malloc(100 * sizeof(char *)); // Allocate memory for up to 100 results
    if (!results) {
        perror("malloc failed in search_trie_for_prefix_two");
        *result_count = 0;
        return NULL;
    }
    char current_path[1024];
    strcpy(current_path, prefix); // Start with the prefix
    *result_count = 0;
    find_paths_with_prefix_two(current, current_path, strlen(prefix), prefix, results, result_count);
    return results;
}

// Returns 1 if the path is a directory, 0 otherwise
int return_one_if_directory(const char *path)
{
    TrieNode *crawler = global_trie_root;
    while (*path)
    {
        if (!crawler->children[(int)*path])
        {
            return 0; // Path not found, so it's not a directory
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    // Check if the final node is marked as a directory
    if (crawler != NULL && crawler->is_end_of_path && crawler->is_directory)
    {
        return 1; // It's a directory
    }
    return 0; // Not a directory
}

// Searches for a path in the trie, skipping slashes
TrieNode *search_in_trie(TrieNode *root, const char *path)
{
    TrieNode *current = root;
    for (const char *p = path; *p; ++p)
    {
        if (*p == '/')
            continue; // Skip slashes in the path
        int index = *p - 'a'; // Convert character to index (adjust based on ALPHABET_SIZE)
        if (index < 0 || index >= ALPHABET_SIZE || !current->children[index])
            return NULL; // Path not found
        current = current->children[index];
    }
    return current;
}

// Connects to a storage server by port, returns socket fd or -1 on error
int connect_to_server(int port)
{
    char temporary[1024];
    for (int i = 0; i < server_count; i++)
    {
        if (storage_servers[i].port == port)
        {
            strcpy(temporary, storage_servers[i].ip);
        }
    }
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("Socket creation failed");
        return -1;
    }
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(temporary)};
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Connection to server failed");
        return -1;
    }
    return sock;
}

// Sends a FETCH request to a storage server for a given path
int send_fetch_request(int sock, const char *path)
{
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "FETCH %s", path);
    if (send(sock, command, strlen(command), 0) < 0)
    {
        perror("Failed to send FETCH command");
        return -1;
    }
    return 0;
}

// Receives file content from a storage server into buffer
int receive_file_content(int sock, char *buffer, size_t buffer_size)
{
    int bytes_read = recv(sock, buffer, buffer_size - 1, 0);
    if (bytes_read <= 0)
    {
        perror("Failed to receive file content");
        return -1;
    }
    buffer[bytes_read] = '\0';
    return 0;
}

int flago[1000] = {0};

// Sends a STORE request to a storage server for a given path and content
int send_store_request(int sock, const char *path, const char *content)
{
    char command[BUFFER_SIZE];
    snprintf(command, sizeof(command), "STORE %s ", path);
    // Skip backup paths
    if (strncmp(path, "Backup", 6) == 0)
    {
        return 0;
    }
    if (send(sock, command, strlen(command), 0) < 0)
    {
        perror("Failed to send STORE command");
        return -1;
    }
    if (send(sock, content, strlen(content), 0) < 0)
    {
        perror("Failed to send file content");
        return -1;
    }
    return 0;
}

int perform_copy_between_servers1(int src_port, int dest_port, const char *source, const char *destination)
{
    int num = return_one_if_directory(source);
    int num1 = return_one_if_directory(destination);
    if ((num && num1))
    {
        int result_count = 0;
        int flag2 = 0;
        char **matched_paths = search_trie_for_prefix_two(source, &result_count);
        if (result_count > 0)
        {
            char temporary[1024];
            char command[BUFFER_SIZE];
            char file_content[BUFFER_SIZE];
            for (int i = 0; i < result_count; i++)
            {
                const char *sub_path = matched_paths[i] + strlen(source);
                flag2 = return_one_if_directory(matched_paths[i]);
                char dest_path[1024];
                snprintf(dest_path, sizeof(dest_path), "%s%s", destination, sub_path);
                if (flag2 == 0)
                {
                    int src_sock = socket(AF_INET, SOCK_STREAM, 0);
                    if (src_sock < 0)
                    {
                        perror("Source server socket creation failed");
                        return -1;
                    }
                    for (int i = 0; i < server_count; i++)
                    {
                        if (storage_servers[i].port == src_port)
                        {
                            strcpy(temporary, storage_servers[i].ip);
                        }
                    }
                    struct sockaddr_in src_addr = {
                        .sin_family = AF_INET,
                        .sin_port = htons(src_port),
                        .sin_addr.s_addr = inet_addr(temporary)};
                    if (inet_ntop(AF_INET, &src_addr.sin_addr, temporary, sizeof(temporary)) == NULL)
                    {
                        perror("inet_ntop failed");
                        return -1;
                    }
                    if (connect(src_sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
                    {
                        perror("Connection to source server failed");
                        close(src_sock);
                        return -1;
                    }
                    // Send FETCH command (should be implemented here if needed)
                    if (send(src_sock, command, strlen(command), 0) < 0)
                    {
                        perror("Failed to send FETCH command to source server");
                        close(src_sock);
                        return -1;
                    }
                    // Receive the file content from the source server
                    int bytes_read = recv(src_sock, file_content, sizeof(file_content) - 1, 0);
                    if (bytes_read <= 0)
                    {
                        perror("Failed to receive file content from source server");
                        close(src_sock);
                        return -1;
                    }
                    file_content[bytes_read] = '\0';
                    close(src_sock);
                    // Establish a connection to the destination server
                    StorageServer *ss = NULL;
                    for (int i = 0; i < server_count; i++)
                    {
                        if (storage_servers[i].port == dest_port)
                        {
                            ss = &storage_servers[i];
                            strcpy(temporary, storage_servers[i].ip);
                        }
                    }
                    if (ss != NULL)
                    {
                        insert_path(global_trie_root, dest_path, ss, 0);
                    }
                }
                else
                {
                    StorageServer *ss = NULL;
                    for (int i = 0; i < server_count; i++)
                    {
                        if (storage_servers[i].port == dest_port)
                        {
                            ss = &storage_servers[i];
                            strcpy(temporary, storage_servers[i].ip);
                        }
                    }
                    if (ss != NULL)
                    {
                        insert_path(global_trie_root, dest_path, ss, 1);
                    }
                    strcpy(file_content, "JUST");
                }
                int dest_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (dest_sock < 0)
                {
                    perror("Destination server socket creation failed");
                    return -1;
                }
                struct sockaddr_in dest_addr = {
                    .sin_family = AF_INET,
                    .sin_port = htons(dest_port),
                    .sin_addr.s_addr = inet_addr(temporary)};
                if (connect(dest_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
                {
                    perror("Connection to destination server failed");
                    close(dest_sock);
                    return -1;
                }
                // Send the file content to the destination server
                snprintf(command, sizeof(command), "STORE %s ", dest_path);
                if (send(dest_sock, command, strlen(command), 0) < 0)
                {
                    perror("Failed to send STORE command to destination server");
                    close(dest_sock);
                    return -1;
                }
                if (send(dest_sock, file_content, strlen(file_content), 0) < 0)
                {
                    perror("Failed to send file content to destination server");
                    close(dest_sock);
                    return -1;
                }
                close(dest_sock);
                free(matched_paths[i]);
            }
        }
        else
        {
            // No paths found with prefix
        }
        free(matched_paths);
    }
    else if ((!num && !num1))
    {

        int src_sock = socket(AF_INET, SOCK_STREAM, 0);

        if (src_sock < 0)

        {

            perror("Source server socket creation failed");

            return -1;
        }

        char temporary[1024];
        for (int i = 0; i < server_count; i++)
        {
            if (storage_servers[i].port == src_port)
            {
                strcpy(temporary, storage_servers[i].ip);
            }
        }
        struct sockaddr_in src_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(src_port),
            .sin_addr.s_addr = inet_addr(temporary)};
        if (connect(src_sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
        {
            perror("Connection to source server failed");
            close(src_sock);
            return -1;
        }
        char command[BUFFER_SIZE];
        snprintf(command, sizeof(command), "FETCH %s", source);
        if (send(src_sock, command, strlen(command), 0) < 0)
        {
            perror("Failed to send FETCH command to source server");
            close(src_sock);
            return -1;
        }
        char file_content[BUFFER_SIZE];
        int bytes_read = recv(src_sock, file_content, sizeof(file_content) - 1, 0);
        if (bytes_read <= 0)
        {
            perror("Failed to receive file content from source server");
            close(src_sock);
            return -1;
        }
        file_content[bytes_read] = '\0';
        close(src_sock);
        int dest_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (dest_sock < 0)
        {
            perror("Destination server socket creation failed");
            return -1;
        }
        for (int i = 0; i < server_count; i++)
        {
            if (storage_servers[i].port == dest_port)
            {
                strcpy(temporary, storage_servers[i].ip);
            }
        }
        struct sockaddr_in dest_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(dest_port),
            .sin_addr.s_addr = inet_addr(temporary)};
        if (connect(dest_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
        {
            perror("Connection to destination server failed");
            close(dest_sock);
            return -1;
        }
        snprintf(command, sizeof(command), "STORE %s ", destination);
        if (send(dest_sock, command, strlen(command), 0) < 0)
        {
            perror("Failed to send STORE command to destination server");
            close(dest_sock);
            return -1;
        }
        if (send(dest_sock, file_content, strlen(file_content), 0) < 0)
        {
            perror("Failed to send file content to destination server");
            close(dest_sock);
            return -1;
        }
        close(dest_sock);
    }
    else if (!num && num1)
    {
        char dest_path[BUFFER_SIZE];
        strcpy(dest_path, destination);
        char *last_slash = strrchr(source, '/');
        if (last_slash != NULL)
        {
            strcat(dest_path, last_slash);
        }
        char temp[BUFFER_SIZE];
        int src_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (src_sock < 0)
        {
            perror("Source server socket creation failed");
            return -1;
        }
        char temporary[1024];
        for (int i = 0; i < server_count; i++)
        {
            if (storage_servers[i].port == src_port)
            {
                strcpy(temporary, storage_servers[i].ip);
            }
        }
        struct sockaddr_in src_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(src_port),
            .sin_addr.s_addr = inet_addr(temporary)};
        if (connect(src_sock, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0)
        {
            perror("Connection to source server failed");
            close(src_sock);
            return -1;
        }
        char command[BUFFER_SIZE];
        snprintf(command, sizeof(command), "FETCH %s", source);
        if (send(src_sock, command, strlen(command), 0) < 0)
        {
            perror("Failed to send FETCH command to source server");
            close(src_sock);
            return -1;
        }
        char file_content[BUFFER_SIZE];
        int bytes_read = recv(src_sock, file_content, sizeof(file_content) - 1, 0);
        if (bytes_read <= 0)
        {
            perror("Failed to receive file content from source server");
            close(src_sock);
            return -1;
        }
        file_content[bytes_read] = '\0';
        close(src_sock);
        int dest_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (dest_sock < 0)
        {
            perror("Destination server socket creation failed");
            return -1;
        }
        for (int i = 0; i < server_count; i++)
        {
            if (storage_servers[i].port == dest_port)
            {
                strcpy(temporary, storage_servers[i].ip);
            }
        }
        struct sockaddr_in dest_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(dest_port),
            .sin_addr.s_addr = inet_addr(temporary)};
        if (connect(dest_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0)
        {
            perror("Connection to destination server failed");
            close(dest_sock);
            return -1;
        }
        snprintf(command, sizeof(command), "STORE %s ", dest_path);
        if (send(dest_sock, command, strlen(command), 0) < 0)
        {
            perror("Failed to send STORE command to destination server");
            close(dest_sock);
            return -1;
        }
        if (send(dest_sock, file_content, strlen(file_content), 0) < 0)
        {
            perror("Failed to send file content to destination server");
            close(dest_sock);
            return -1;
        }
        close(dest_sock);
    }
    else if (num && !num1)
    {
        // printf("Cannot copy a directory to a file\n");
    }
    return 0;
}

int perform_copy_between_servers(int src_port, int dest_port, const char *source, const char *destination)
{
    if (flago[server_count] == 0) {
        flago[server_count] = 1;
        sleep(10);
    }
    int num = return_one_if_directory(source);
    if (num) {
        int result_count = 0;
        int flag2 = 0;
        char **matched_paths = search_trie_for_prefix(source, &result_count);
        if (result_count > 0) {
            for (int i = 0; i < result_count; i++) {
                flag2 = return_one_if_directory(matched_paths[i]);
                const char *sub_path = matched_paths[i] + strlen(source);
                char dest_path[1024];
                char file_content[BUFFER_SIZE];
                snprintf(dest_path, sizeof(dest_path), "%s%s", destination, sub_path);
                if (flag2 == 0) {
                    int src_sock = connect_to_server(src_port);
                    if (src_sock < 0) {
                        log_message("Failed to connect to source server\n");
                        free(matched_paths[i]);
                        continue;
                    }
                    if (send_fetch_request(src_sock, matched_paths[i]) < 0) {
                        close(src_sock);
                        free(matched_paths[i]);
                        continue;
                    }
                    if (receive_file_content(src_sock, file_content, sizeof(file_content)) < 0) {
                        close(src_sock);
                        free(matched_paths[i]);
                        continue;
                    }
                    close(src_sock);
                } else {
                    strcpy(file_content, "JUST");
                }
                int dest_sock = connect_to_server(dest_port);
                if (dest_sock < 0) {
                    log_message("Failed to connect to destination server\n");
                    free(matched_paths[i]);
                    continue;
                }
                if (send_store_request(dest_sock, dest_path, file_content) < 0) {
                    close(dest_sock);
                    free(matched_paths[i]);
                    continue;
                }
                close(dest_sock);
                free(matched_paths[i]);
            }
        }
        free(matched_paths);
    } else {
        int src_sock = connect_to_server(src_port);
        if (src_sock < 0) {
            log_message("Failed to connect to source server\n");
            return -1;
        }
        if (send_fetch_request(src_sock, source) < 0) {
            close(src_sock);
            return -1;
        }
        char file_content[BUFFER_SIZE];
        if (receive_file_content(src_sock, file_content, sizeof(file_content)) < 0) {
            close(src_sock);
            return -1;
        }
        close(src_sock);
        int dest_sock = connect_to_server(dest_port);
        if (dest_sock < 0) {
            log_message("Failed to connect to destination server\n");
            return -1;
        }
        if (send_store_request(dest_sock, destination, file_content) < 0) {
            close(dest_sock);
            return -1;
        }
        close(dest_sock);
    }
    return 0;
}

// Function to remove a specific path from the cache
void remove_paths_from_cache(const char *path)
{
    int shift_index = 0;
    // Iterate over the cache entries
    for (int i = 0; i < lru_cache.count; i++)
    {
        // If the current entry matches the given path, skip it
        if (strcmp(lru_cache.entries[i].path, path) == 0)
        {
            log_message("Removing path from cache: %s\n", lru_cache.entries[i].path);
            printf("Removing path from cache: %s\n", lru_cache.entries[i].path);
        }
        else
        {
            // Shift the entry to maintain a compact cache
            lru_cache.entries[shift_index++] = lru_cache.entries[i];
        }
    }
    // Update cache count to reflect removed entries
    lru_cache.count = shift_index;
}

void mark_subtree_as_revived(TrieNode *node)
{
    if (node == NULL) return;
    if (node->is_end_of_path) {
        node->is_deleted = 0;
    }
    // Recursively mark all children as revived
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i] != NULL) {
            mark_subtree_as_revived(node->children[i]);
        }
    }
}

StorageServer *search_path_two(TrieNode *root, const char *path)
{
    TrieNode *crawler = root;
    while (*path)
    {
        if (!crawler->children[(int)*path])
        {
            return NULL;
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    if (crawler != NULL && crawler->is_end_of_path && crawler->server != NULL)
    {
        crawler->is_deleted = 0;
        crawler->server->is_server_down = 0;
        mark_subtree_as_revived(crawler);
        return crawler->server;
    }
    return NULL;
}

void storage_server_thread(int client_sock)
{
    int new_socket = client_sock;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    getpeername(new_socket, (struct sockaddr *)&client_addr, &client_len);
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(new_socket, buffer, BUFFER_SIZE);
    char my_ip[INET_ADDRSTRLEN];
    int my_port = 0;
    sscanf(buffer, "%s %d", my_ip, &my_port);
    printf("buffer is %s\n", buffer);
    printf("My IP is %s and My Port is %d\n", my_ip, my_port);
    log_message("My IP is %s and My Port is %d\n", my_ip, my_port);
    memset(buffer, 0, BUFFER_SIZE);
    bytes_read = read(new_socket, buffer, BUFFER_SIZE);
    if (bytes_read > 0) {
        pthread_mutex_lock(&lock);
        StorageServer *server = NULL;
        int flag = 0;
        for (int i = 0; i < server_count; i++) {
            if (strcmp(storage_servers[i].ip, my_ip) == 0 && storage_servers[i].port == my_port) {
                server = &storage_servers[i];
                server->is_server_down = 0;
                int counter_for_paths = server->path_count;
                StorageServer *hello;
                while (counter_for_paths--) {
                    hello = search_path_two(global_trie_root, server->path_list[counter_for_paths]);
                }
                flag = 1;
                printf("Server is already registered\n");
            }
        }
        if (flag == 0) {
            storage_servers = realloc(storage_servers, (server_count + 1) * sizeof(StorageServer));
            if (!storage_servers) {
                perror("Failed to allocate memory for storage servers");
                pthread_mutex_unlock(&lock);
                close(new_socket);
                pthread_exit(NULL);
            }
            server = &storage_servers[server_count];
            // Initialize all fields to safe defaults
            memset(server, 0, sizeof(StorageServer));
            inet_ntop(AF_INET, &client_addr.sin_addr, server->ip, INET_ADDRSTRLEN);
            server->port = ntohs(client_addr.sin_port);
            char extracted_ip[INET_ADDRSTRLEN] = {0};
            int extracted_port = 0;
            server->port = my_port;
            strcpy(server->ip, my_ip);
            server->socket_fd = new_socket;
            server->is_async_write_in_progress = 0;
            server->async_writer_socket = -1;
            server->is_server_down = 0;
            server->path_list = NULL;
            server->path_count = 0;
            server->backup_ss[0] = -1;
            server->backup_ss[1] = -1;
            server_count++;
            if (server_count < REPLICATION_FACTOR) {
                for (int i = 0; i < 2; i++) {
                    server->backup_ss[i] = -1;
                }
            } else {
                for (int j = 0; j < REPLICATION_FACTOR - 1; j++) {
                    if (storage_servers[j].backup_ss[0] == -1) {
                        if (j == 0) {
                            storage_servers[j].backup_ss[0] = 1;
                            storage_servers[j].backup_ss[1] = 2;
                        } else {
                            storage_servers[j].backup_ss[0] = 0;
                            storage_servers[j].backup_ss[1] = 2;
                        }
                        char buffer1[BUFFER_SIZE] = {0};
                        if (j == 1) {
                            retrieve_paths_to_buffer(global_trie_root, buffer1, &storage_servers[j]);
                            sleep(7);
                            parse_and_store_backup(&storage_servers[j], buffer1);
                        } else {
                            sleep(7);
                            parse_and_store_backup(&storage_servers[j], buffer_back);
                        }
                    } else
                        break;
                }
                server->backup_ss[0] = rand() % (server_count - 1);
                int temp = rand() % (server_count - 1);
                while (temp == server->backup_ss[0]) {
                    temp = rand() % (server_count - 1);
                }
                server->backup_ss[1] = temp;
            }
            pthread_mutex_unlock(&lock);
            log_message("Registered Storage Server from IP: %s, Port: %d\n", server->ip, server->port);
            printf("Registered Storage Server from IP: %s, Port: %d\n", server->ip, server->port);
            log_message("Received file list:\n%s\n", buffer);
            printf("Received file list:\n%s\n", buffer);
            sleep(1);
            char polo_temporary[BUFFER_SIZE];
            snprintf(polo_temporary, sizeof(polo_temporary), "IP: %s Port: %d\n", server->ip, server->port);
            send(new_socket, polo_temporary, strlen(polo_temporary), 0);
            parse_and_store_files(server, buffer);
            if (server_count == 1) {
                retrieve_paths_to_buffer(global_trie_root, buffer_back, server);
            }
        } else {
            pthread_mutex_unlock(&lock);
            log_message("Registered Storage Server from IP: %s, Port: %d\n", server->ip, server->port);
            printf("Registered Storage Server from IP: %s, Port: %d\n", server->ip, server->port);
            log_message("Received file list:\n%s\n", buffer);
            printf("Received file list:\n%s\n", buffer);
            parse_and_store_files(server, buffer);
            if (server_count == 1) {
                retrieve_paths_to_buffer(global_trie_root, buffer_back, server);
            }
            char polo_temporary[BUFFER_SIZE];
            snprintf(polo_temporary, sizeof(polo_temporary), "IP: %s Port: %d\n", server->ip, server->port);
            send(new_socket, polo_temporary, strlen(polo_temporary), 0);
        }
        while (1) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_read = recv(new_socket, buffer, BUFFER_SIZE, 0);
            int client_socket_number = 0;
            if (strncmp(buffer, "ASYNC_WRITE_PROGRESS", 20) == 0) {
                char path[BUFFER_SIZE];
                sscanf(buffer, "ASYNC_WRITE_PROGRESS %d %s", &client_socket_number, path);
                server->is_async_write_in_progress = 1;
                server->async_writer_socket = client_socket_number;
                StorageServer *idx = path_exists(path, NULL);
                if (idx == NULL) {
                    send(client_socket_number, "ASYNC_WRITE_SUCCESS", strlen("ASYNC_WRITE_SUCCESS"), 0);
                    continue;
                }
                if (idx->backup_ss[0] != (-1) && idx->backup_ss[1] != (-1)) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[0]].port, path, path);
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[1]].port, path, path);
                    send(client_socket_number, "ASYNC_WRITE_SUCCESS", strlen("ASYNC_WRITE_SUCCESS"), 0);
                    continue;
                } else if (idx->backup_ss[0] == -1) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[1]].port, path, path);
                    send(client_socket_number, "ASYNC_WRITE_SUCCESS", strlen("ASYNC_WRITE_SUCCESS"), 0);
                    continue;
                } else if (idx->backup_ss[1] == -1) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[0]].port, path, path);
                    send(client_socket_number, "ASYNC_WRITE_SUCCESS", strlen("ASYNC_WRITE_SUCCESS"), 0);
                    continue;
                }
                continue;
            }
            printf("Received from Storage Server %s:%d: %s\n", server->ip, server->port, buffer);
            log_message("Received from Storage Server %s:%d: %s\n", server->ip, server->port, buffer);
            if (strncmp(buffer, "ASYNC_WRITE_SUCCESS", 19) == 0) {
                log_message("ASYNC_WRITE_SUCCESS received\n");
                printf("ASYNC_WRITE_SUCCESS received\n");
                int client_socket_number = 0;
                char path[BUFFER_SIZE];
                sscanf(buffer, "ASYNC_WRITE_SUCCESS %d %s", &client_socket_number, path);
                log_message("Client Socket Number is %d\n", client_socket_number);
                printf("Client Socket Number is %d\n", client_socket_number);
                sleep(1);
                server->is_async_write_in_progress = 0;
                send(client_socket_number, "ASYNC_WRITE_SUCCESS", strlen("ASYNC_WRITE_SUCCESS"), 0);
            }
            if (strncmp(buffer, "WRITE_SUCCESS", strlen("WRITE_SUCCESS")) == 0) {
                printf("WRITE_SUCCESS received\n");
                log_message("WRITE_SUCCESS received\n");
                char path[BUFFER_SIZE];
                sscanf(buffer, "WRITE_SUCCESS %s", path);
                sleep(1);
                StorageServer *idx = path_exists(path, NULL);
                if (idx == NULL) {
                    send(client_socket_number, "WRITE_SUCCESS", strlen("WRITE_SUCCESS"), 0);
                    continue;
                }
                if (idx->backup_ss[0] != (-1) && idx->backup_ss[1] != (-1)) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[0]].port, path, path);
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[1]].port, path, path);
                    send(client_socket_number, "WRITE_SUCCESS", strlen("WRITE_SUCCESS"), 0);
                    continue;
                } else if (idx->backup_ss[0] == -1) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[1]].port, path, path);
                    send(client_socket_number, "WRITE_SUCCESS", strlen("WRITE_SUCCESS"), 0);
                    continue;
                } else if (idx->backup_ss[1] == -1) {
                    perform_copy_between_servers(idx->port, storage_servers[idx->backup_ss[0]].port, path, path);
                    send(client_socket_number, "WRITE_SUCCESS", strlen("WRITE_SUCCESS"), 0);
                    continue;
                }
            }
            if (bytes_read <= 0 || strcmp(buffer, "STOP") == 0) {
                if (server->is_async_write_in_progress) {
                    server->is_async_write_in_progress = 0;
                    send(server->async_writer_socket, "ASYNC_WRITE_FAIL", strlen("ASYNC_WRITE_FAIL"), 0);
                }
                log_message("STOP received or connection error\n");
                server->is_server_down = 1;
                int counter_for_paths = server->path_count;
                StorageServer *hello;
                while (counter_for_paths--) {
                    remove_paths_from_cache(server->path_list[counter_for_paths]);
                    hello = search_path(global_trie_root, server->path_list[counter_for_paths], 1);
                }
                printf("STOP received or connection error\n");
                log_message("STOP received or connection error\n");
                break;
            }
        }
        close(new_socket);
    } else {
        log_message("Error reading from Storage Server\n");
        printf("Error reading from Storage Server\n");
        close(new_socket);
    }
}

void *handle_storage_connection_thread(void *arg)
{
    int client_socket = *((int *)arg);
    free(arg);
    storage_server_thread(client_socket);
    close(client_socket);
    return NULL;
}

void *handle_client(void *arg) {
    int client_sock = *((int *)arg);
    free(arg);
    char buffer[BUFFER_SIZE] = {0};
    while (1) {
        int bytes_read = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        log_message("Received from client: %s\n", buffer);
        sleep(0.3);
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                log_message("Client disconnected\n");
                printf("Client disconnected\n");
            } else {
                perror("Client read error");
            }
            close(client_sock);
            pthread_exit(NULL);
        }
        buffer[bytes_read] = '\0';
        printf("DEBUG: Received from client: '%s'\n", buffer);
        log_message("Received from client: %s\n", buffer);
        char command[BUFFER_SIZE], path[BUFFER_SIZE], path1[BUFFER_SIZE];
        if (sscanf(buffer, "%s %s %s", command, path, path1) == 3 && strcmp(command, "COPY") == 0) {
            printf("Processing COPY command from client: Source: %s, Destination: %s\n", path, path1);
            log_message("Processing COPY command from client: Source: %s, Destination: %s\n", path, path1);
            // Validate path and path1 paths
            StorageServer *src_server = path_exists(path, NULL);
            StorageServer *dest_server = path_exists(path1, NULL);
            int src_valid = 1;
            int dest_valid = 1;
            if (!src_server || !dest_server) {
                char error_message[] = "Invalid path or path1 path\n";
                send(client_sock, error_message, strlen(error_message), 0);
                continue;
            }
            // Determine path and path1 storage servers
            int src_ss = src_server->port;
            int dest_ss = dest_server->port;
            if (src_ss == -1 || dest_ss == -1) {
                char error_message[] = "Failed to locate path or path1 server\n";
                send(client_sock, error_message, strlen(error_message), 0);
                continue;
            }
            if (src_ss == dest_ss) {
                // Same storage server
                if (perform_copy_between_servers1(src_ss, dest_ss, path, path1) != 0) {
                    char error_message[] = "Error copying within the same storage server\n";
                    send(client_sock, error_message, strlen(error_message), 0);
                    continue;
                }
            } else {
                if (perform_copy_between_servers1(src_ss, dest_ss, path, path1) != 0) {
                    char error_message[] = "Error copying between storage servers\n";
                    send(client_sock, error_message, strlen(error_message), 0);
                    continue;
                }
            }
            StorageServer *ss = NULL;
            for (int i = 0; i < server_count; i++) {
                if (storage_servers[i].port == dest_ss) {
                    ss = &storage_servers[i];
                }
            }
            if (ss != NULL) {
                if (ss->backup_ss[0] != (-1) && ss->backup_ss[1] != (-1)) {
                    perform_copy_between_servers(ss->port, storage_servers[ss->backup_ss[0]].port, path, path);
                    perform_copy_between_servers(ss->port, storage_servers[ss->backup_ss[1]].port, path, path);
                } else if (ss->backup_ss[0] == -1) {
                    perform_copy_between_servers(ss->port, storage_servers[ss->backup_ss[1]].port, path, path);
                } else if (ss->backup_ss[1] == -1) {
                    perform_copy_between_servers(ss->port, storage_servers[ss->backup_ss[0]].port, path, path);
                }
            }
            char success_message[] = "COPY operation successful\n";
            send(client_sock, success_message, strlen(success_message), 0);
        }
        // Skip the initial client connection message
        if (strncmp(buffer, "CLIENT CONNECTING", 17) == 0) {
            continue;
        }
        if (sscanf(buffer, "%s %s", command, path) < 2) {
            fprintf(stderr, "Invalid command format: %s\n", buffer);
            continue;
        }
        if (strcmp(command, "READ") == 0 || strcmp(command, "WRITE") == 0 || strcmp(command, "INFO") == 0 || strcmp(command, "STREAM") == 0) {
            int found = -1;
            StorageServer *tempo;
            StorageServer **asd;
            if (cache_lookup(path) == -1) {
                tempo = path_exists(path, asd);
                if (tempo == NULL) {
                    found = -1;
                } else {
                    for (int i = 0; i < server_count; i++) {
                        if (tempo->socket_fd == storage_servers[i].socket_fd && !storage_servers[i].is_server_down) {
                            found = i;
                            break;
                        }
                    }
                    if (tempo)
                        cache_insert(path, found);
                }
            } else {
                tempo = NULL;
                found = cache_lookup(path);
                if (found != -1 && storage_servers[found].is_server_down) {
                    tempo = path_exists(path, asd);
                }
            }
            if (tempo == NULL)
                tempo = &storage_servers[found];
            if (found == -1 || tempo == NULL) {
                char response[BUFFER_SIZE] = "File not found in any storage server";
                send(client_sock, response, strlen(response), 0);
                log_message("File not found in any storage server\n");
                printf("File not found in any storage server\n");
                continue;
            }
            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response), "IP: %s Port: %d\n", (tempo)->ip, (tempo)->port);
            send(client_sock, response, strlen(response), 0);
            log_message("Sent Storage Server details to client\n");
            printf("Sent Storage Server details to client\n");
        } else if (strcmp(command, "LIST") == 0) {
            print_all_trie_paths1(client_sock, path);
        } else if (strcmp(command, "CREATE_DIC") == 0 || strcmp(command, "CREATE_F") == 0) {
            int len = strlen(path);
            char file_name[BUFFER_SIZE];
            strcpy(file_name, path);
            for (int i = len - 1; i >= 0; i--) {
                if (path[i] == '/') {
                    len = i;
                    break;
                }
            }
            file_name[len] = '\0';
            int found = -1;
            StorageServer **tempo;
            StorageServer *real = NULL;
            if (cache_lookup(path) == -1) {
                real = path_exists(path, tempo);
            } else {
                found = cache_lookup(path);
            }
            if (real != NULL || found != -1) {
                printf("File or Directory already exists\n");
                log_message("File or Directory already exists\n");
                sleep(1);
                send(client_sock, "File or Directory already exists", strlen("File or Directory already exists"), 0);
                continue;
            }
            if (cache_lookup(file_name) == -1) {
                real = path_exists(file_name, tempo);
                if (real == NULL) {
                    found = -1;
                }
                if (real) {
                    for (int i = 0; i < server_count; i++) {
                        if (real->socket_fd == storage_servers[i].socket_fd) {
                            found = i;
                            break;
                        }
                    }
                    cache_insert(file_name, found);
                }
            } else {
                found = cache_lookup(file_name);
            }
            if (found != -1) {
                if (strcmp(command, "CREATE_DIC") == 0) {
                    if (real) {
                        send_command_to_storage(real, "CREATE_DIC", path);
                        if (real->backup_ss[0] != -1)
                            send_command_to_storage(&storage_servers[real->backup_ss[0]], "CREATE_DIC", path);
                        if (real->backup_ss[1] != -1)
                            send_command_to_storage(&storage_servers[real->backup_ss[1]], "CREATE_DIC", path);
                    } else if (found != -1) {
                        send_command_to_storage(&storage_servers[found], "CREATE_DIC", path);
                        if (storage_servers[found].backup_ss[0] != -1)
                            send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[0]], "CREATE_DIC", path);
                        if (storage_servers[found].backup_ss[1] != -1)
                            send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[1]], "CREATE_DIC", path);
                    }
                } else if (strcmp(command, "CREATE_F") == 0) {
                    if (real) {
                        send_command_to_storage(real, "CREATE_F", path);
                        if (real->backup_ss[0] != -1)
                            send_command_to_storage(&storage_servers[real->backup_ss[0]], "CREATE_F", path);
                        if (real->backup_ss[1] != -1)
                            send_command_to_storage(&storage_servers[real->backup_ss[1]], "CREATE_F", path);
                    } else if (found != -1) {
                        send_command_to_storage(&storage_servers[found], "CREATE_F", path);
                        if (storage_servers[found].backup_ss[0] != -1)
                            send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[0]], "CREATE_F", path);
                        if (storage_servers[found].backup_ss[1] != -1)
                            send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[1]], "CREATE_F", path);
                    }
                }
                if (strcmp(command, "CREATE_F") == 0) {
                    char buffer[BUFFER_SIZE];
                    snprintf(buffer, sizeof(buffer), "File: %s\n", path);
                    strcpy(path, buffer);
                    if (real)
                        parse_and_store_files(real, path);
                    else
                        parse_and_store_files(&storage_servers[found], path);
                } else {
                    char buffer[BUFFER_SIZE];
                    snprintf(buffer, sizeof(buffer), "Directory: %s\n", path);
                    strcpy(path, buffer);
                    if (real)
                        parse_and_store_files(real, path);
                    else
                        parse_and_store_files(&storage_servers[found], path);
                }
            } else {
                char response[BUFFER_SIZE] = "Directory Not Found";
                sleep(1);
                send(client_sock, response, strlen(response), 0);
                log_message("Directory Not found\n");
                printf("Directory Not found\n");
            }
        } else if (strcmp(command, "DELETE") == 0) {
            int found = -1;
            StorageServer **tempo;
            StorageServer *real = NULL;
            if (cache_lookup(path) == -1) {
                real = path_exists(path, tempo);
                if (real == NULL) {
                    found = -1;
                }
                if (real) {
                    for (int i = 0; i < server_count; i++) {
                        if (real->socket_fd == storage_servers[i].socket_fd) {
                            found = i;
                            break;
                        }
                    }
                    cache_insert(path, found);
                }
            } else {
                found = cache_lookup(path);
            }
            char temppp[BUFFER_SIZE];
            if (real) {
                send_command_to_storage(real, "DELETE", path);
                search_path(global_trie_root, path, 1);
                if (real->backup_ss[0] != -1) {
                    send_command_to_storage(&storage_servers[real->backup_ss[0]], "DELETE", path);
                    strcpy(temppp, "Backup1");
                    strcat(temppp, path);
                    search_path(global_trie_root, temppp, 1);
                }
                if (real->backup_ss[1] != -1) {
                    send_command_to_storage(&storage_servers[real->backup_ss[1]], "DELETE", path);
                    strcpy(temppp, "Backup2");
                    strcat(temppp, path);
                    search_path(global_trie_root, temppp, 1);
                }
            } else if (found != -1) {
                send_command_to_storage(&storage_servers[found], "DELETE", path);
                search_path(global_trie_root, path, 1);
                if (storage_servers[found].backup_ss[0] != -1) {
                    send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[0]], "DELETE", path);
                    strcpy(temppp, "Backup1");
                    strcat(temppp, path);
                    search_path(global_trie_root, temppp, 1);
                }
                if (storage_servers[found].backup_ss[1] != -1) {
                    send_command_to_storage(&storage_servers[storage_servers[found].backup_ss[1]], "DELETE", path);
                    strcpy(temppp, "Backup2");
                    strcat(temppp, path);
                    search_path(global_trie_root, temppp, 1);
                }
            } else {
                char response[BUFFER_SIZE] = "File not found in any storage server";
                sleep(1);
                send(client_sock, response, strlen(response), 0);
                log_message("File not found in any storage server\n");
                printf("File not found in any storage server\n");
            }
            remove_paths_from_cache(path);
        } else if (strcmp(command, "STOP") == 0) {
            log_message("Received STOP command from client\n");
            printf("Received STOP command from client\n");
            close(client_sock);
            pthread_exit(NULL);
        } else {
            fprintf(stderr, "Unknown command received\n");
            send(client_sock, "Unknown command", strlen("Unknown command"), 0);
        }
    }
    close(client_sock);
    pthread_exit(NULL);
}

void *main_server_thread(void *arg)
{
    const char *ip = (const char *)arg;
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("Socket failed");
        pthread_exit(NULL);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        pthread_exit(NULL);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(ip);
    address.sin_port = htons(PORT);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        pthread_exit(NULL);
    }
    if (listen(server_fd, MAX_CLIENTS) < 0)
    {
        perror("listen");
        pthread_exit(NULL);
    }
    log_message("Main server thread running, waiting for connections...\n");
    printf("Main server thread running, waiting for connections...\n");

    printf("Naming server started in IP AND PORT %s:%d\n", ip, PORT);
    log_message("Naming server started in IP AND PORT %s:%d\n", ip, PORT);
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        new_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (new_socket < 0)
        {
            perror("accept");
            continue;
        }
        char buffer[BUFFER_SIZE] = {0};
        read(new_socket, buffer, BUFFER_SIZE);
        log_message("Received from client: %s\n", buffer);
        printf("BUFFER: %s\n", buffer);

        if (strncmp(buffer, "CLIENT", 6) == 0)
        {
            log_message("Client connection detected\n");
            printf("Client connection detected.\n");

            send(new_socket, "Connected to Naming Server\n", strlen("Connected to Naming Server\n"), 0);

            // Launch a new thread to handle the client request using handle_client
            int *client_sock = malloc(sizeof(int));
            *client_sock = new_socket;
            pthread_t client_thread;
            pthread_create(&client_thread, NULL, (void *)handle_client, (void *)client_sock);
            pthread_detach(client_thread); // Detach to free resources after finishing
        }
        else if (strncmp(buffer, "STORAGE", 7) == 0)
        {
            log_message("Storage Server connection detected\n");
            printf("Storage Server connection detected.\n");

            // Create a separate thread for handling storage server registration
            int *storage_sock = malloc(sizeof(int));
            *storage_sock = new_socket;

            pthread_t storage_thread;
            pthread_create(&storage_thread, NULL, handle_storage_connection_thread, (void *)storage_sock);
            pthread_detach(storage_thread);
        }
    }

    return NULL;
}

void send_command_to_storage(const StorageServer *server, const char *command, const char *path)
{
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s %s\n", command, path);

    // Use the stored socket_fd to send command directly
    // printf("socket_fd is %d\n", server->socket_fd);
    send(server->socket_fd, buffer, strlen(buffer), 0);
    log_message("Sent command '%s' for path '%s' to Storage Server\n", command, path);
    printf("Sent command '%s' for path '%s' to Storage Server\n", command, path);
}

int main()
{
    global_trie_root = create_trie_node(NULL);

    log_message("Starting Naming Server...\n");
    printf("Starting Naming Server...\n");

    // Get the IP address using hostname -I
    char ip_buffer[BUFFER_SIZE];
    FILE *fp = popen("hostname -I", "r");
    if (fp == NULL)
    {
        perror("Failed to run hostname -I");
        exit(EXIT_FAILURE);
    }
    if (fgets(ip_buffer, sizeof(ip_buffer), fp) != NULL)
    {
        // Remove any trailing newline character
        ip_buffer[strcspn(ip_buffer, "\n")] = '\0';
    }
    pclose(fp);

    char my_correect_short_ip[INET_ADDRSTRLEN];
    sscanf(ip_buffer, "%s", my_correect_short_ip);

    strcpy(my_ip, my_correect_short_ip);

    // printf("My IP is %s\n", my_ip);

    pthread_t server_thread;
    pthread_mutex_init(&lock, NULL);

    // Create the main server thread
    pthread_create(&server_thread, NULL, main_server_thread, my_correect_short_ip);

    // Wait for threads to finish
    pthread_join(server_thread, NULL);

    free_trie(global_trie_root);

    pthread_mutex_destroy(&lock);

    return 0;
}