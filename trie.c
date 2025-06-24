#include "trie.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TrieNode *create_trie_node(StorageServer *server) {
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    if (!node) {
        printf("Error: Failed to allocate memory for trie node\n");
        return NULL;
    }
    node->is_end_of_path = 0;
    node->is_deleted = 0;
    node->is_directory = 0;
    node->server = server;
    if (server) {
        server->is_server_down = 0;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++)
        node->children[i] = NULL;
    return node;
}

void insert_path(TrieNode *root, const char *path, StorageServer *server, int is_directory) {
    if (!root || !path || !server) {
        printf("Error: Invalid parameters in insert_path\n");
        return;
    }
    TrieNode *crawler = root;
    while (*path) {
        if (!crawler->children[(int)*path]) {
            crawler->children[(int)*path] = create_trie_node(server);
            if (!crawler->children[(int)*path]) {
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
    crawler->server = server;
    if (crawler->server) {
        crawler->server->is_server_down = 0;
    }
}

void mark_subtree_as_deleted(TrieNode *node) {
    if (node == NULL)
        return;
    if (node->is_end_of_path) {
        node->is_deleted = 1;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i] != NULL) {
            mark_subtree_as_deleted(node->children[i]);
        }
    }
}

StorageServer *search_path(TrieNode *root, const char *path, int want_to_delete) {
    TrieNode *crawler = root;
    while (*path) {
        if (!crawler->children[(int)*path] || crawler->is_deleted) {
            return NULL;
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    if (crawler != NULL && crawler->is_end_of_path && crawler->server != NULL) {
        if (want_to_delete) {
            mark_subtree_as_deleted(crawler);
        }
        if (crawler->server->is_server_down || crawler->is_deleted) {
            return NULL;
        }
        return crawler->server;
    }
    return NULL;
}

StorageServer *search_trie(TrieNode *root, const char *path) {
    TrieNode *current = root;
    for (int i = 0; path[i] != '\0'; i++) {
        int index = path[i];
        if (!current->children[index]) {
            return NULL;
        }
        current = current->children[index];
        if (current->is_deleted) {
            return NULL;
        }
    }
    if (current->is_end_of_path && current->server && current->server->is_server_down == 0) {
        return current->server;
    }
    return NULL;
}

void free_trie(TrieNode *root) {
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (root->children[i]) {
            free_trie(root->children[i]);
        }
    }
    free(root);
}

bool remove_path_from_trie(TrieNode *node, const char *path, int depth) {
    int index = path[depth] - 'a';
    if (index < 0 || index >= 26) {
        return false;
    }
    if (node->children[index] != NULL) {
        bool should_delete_child = remove_path_from_trie(node->children[index], path, depth + 1);
        if (should_delete_child) {
            free(node->children[index]);
            node->children[index] = NULL;
            if (!node->is_end_of_path) {
                for (int i = 0; i < ALPHABET_SIZE; i++) {
                    if (node->children[i] != NULL) return false;
                }
                return true;
            }
        }
    }
    return false;
}

void mark_subtree_as_revived(TrieNode *node) {
    if (node == NULL) return;
    if (node->is_end_of_path) {
        node->is_deleted = 0;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i] != NULL) {
            mark_subtree_as_revived(node->children[i]);
        }
    }
}

TrieNode *search_in_trie(TrieNode *root, const char *path) {
    TrieNode *current = root;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') continue;
        int index = *p - 'a';
        if (index < 0 || index >= ALPHABET_SIZE || !current->children[index])
            return NULL;
        current = current->children[index];
    }
    return current;
}

int validate_path(TrieNode *root, const char *path) {
    TrieNode *crawler = root;
    while (*path) {
        if (!crawler->children[(int)*path])
            return 0;
        crawler = crawler->children[(int)*path];
        path++;
    }
    return crawler->is_end_of_path;
}

void print_trie_paths1(TrieNode *root, char *path, int level, int client_sock, char *prefix) {
    if (root->is_deleted) return;
    if (root->is_end_of_path && root->server && !root->server->is_server_down && !root->is_deleted) {
        path[level] = '\0';
        if (root->is_directory) {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "Directory: %s\n", path);
            if (strstr(path, prefix) != NULL)
                send(client_sock, temp, strlen(temp), 0);
        } else if (root->is_directory == 0) {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "File: %s\n", path);
            if (strstr(path, prefix) != NULL)
                send(client_sock, temp, strlen(temp), 0);
        }
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (root->children[i]) {
            path[level] = i;
            print_trie_paths1(root->children[i], path, level + 1, client_sock, prefix);
        }
    }
}

void print_all_trie_paths1(int client_sock, char *prefix) {
    char path[BUFFER_SIZE];
    print_trie_paths1(global_trie_root, path, 0, client_sock, prefix);
    sleep(0.5);
    send(client_sock, "EOF", strlen("EOF"), 0);
}

void print_trie_paths(TrieNode *root, char *path, int level, int client_sock) {
    if (root->is_deleted) return;
    if (root->is_end_of_path && root->server && !root->server->is_server_down && !root->is_deleted) {
        path[level] = '\0';
        if (root->is_directory) {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "Directory: %s\n", path);
            send(client_sock, temp, strlen(temp), 0);
        } else if (root->is_directory == 0) {
            char temp[BUFFER_SIZE];
            snprintf(temp, sizeof(temp), "File: %s\n", path);
            send(client_sock, temp, strlen(temp), 0);
        }
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (root->children[i]) {
            path[level] = i;
            print_trie_paths(root->children[i], path, level + 1, client_sock);
        }
    }
}

void print_all_trie_paths(int client_sock) {
    char path[BUFFER_SIZE];
    print_trie_paths(global_trie_root, path, 0, client_sock);
    sleep(0.5);
    send(client_sock, "EOF", strlen("EOF"), 0);
}

void collect_paths_to_buffer(TrieNode *node, char *current_path, int depth, char *buffer, StorageServer *server) {
    if (node == NULL) return;
    if (node->is_end_of_path && node->server == server) {
        current_path[depth] = '\0';
        strcat(buffer, "File: ");
        strcat(buffer, current_path);
        strcat(buffer, "\n");
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i] != NULL) {
            current_path[depth] = (char)i;
            collect_paths_to_buffer(node->children[i], current_path, depth + 1, buffer, server);
        }
    }
}

void retrieve_paths_to_buffer(TrieNode *root, char *buffer, StorageServer *server) {
    if (root == NULL) return;
    char current_path[BUFFER_SIZE] = {0};
    buffer[0] = '\0';
    collect_paths_to_buffer(root, current_path, 0, buffer, server);
}

void find_paths_with_prefix(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count) {
    if (!node) return;
    if (node->is_end_of_path && !node->is_directory) {
        current_path[depth] = '\0';
        results[*count] = strdup(current_path);
        if (!results[*count]) {
            perror("strdup failed in find_paths_with_prefix");
            return;
        }
        (*count)++;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            current_path[depth] = (char)i;
            find_paths_with_prefix(node->children[i], current_path, depth + 1, prefix, results, count);
        }
    }
}

void find_paths_with_prefix_two(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count) {
    if (!node) return;
    if (node->is_end_of_path) {
        current_path[depth] = '\0';
        results[*count] = strdup(current_path);
        if (!results[*count]) {
            perror("strdup failed in find_paths_with_prefix_two");
            return;
        }
        (*count)++;
    }
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        if (node->children[i]) {
            current_path[depth] = (char)i;
            find_paths_with_prefix_two(node->children[i], current_path, depth + 1, prefix, results, count);
        }
    }
}

char **search_trie_for_prefix(const char *prefix, int *result_count) {
    extern TrieNode *global_trie_root;
    TrieNode *current = global_trie_root;
    for (const char *p = prefix; *p; p++) {
        int index = (unsigned char)*p;
        if (!current->children[index]) {
            *result_count = 0;
            return NULL;
        }
        current = current->children[index];
    }
    char **results = (char **)malloc(100 * sizeof(char *));
    if (!results) {
        perror("malloc failed in search_trie_for_prefix");
        *result_count = 0;
        return NULL;
    }
    char current_path[1024];
    strcpy(current_path, prefix);
    *result_count = 0;
    find_paths_with_prefix(current, current_path, strlen(prefix), prefix, results, result_count);
    return results;
}

char **search_trie_for_prefix_two(const char *prefix, int *result_count) {
    extern TrieNode *global_trie_root;
    TrieNode *current = global_trie_root;
    for (const char *p = prefix; *p; p++) {
        int index = (unsigned char)*p;
        if (!current->children[index]) {
            *result_count = 0;
            return NULL;
        }
        current = current->children[index];
    }
    char **results = (char **)malloc(100 * sizeof(char *));
    if (!results) {
        perror("malloc failed in search_trie_for_prefix_two");
        *result_count = 0;
        return NULL;
    }
    char current_path[1024];
    strcpy(current_path, prefix);
    *result_count = 0;
    find_paths_with_prefix_two(current, current_path, strlen(prefix), prefix, results, result_count);
    return results;
}

int return_one_if_directory(const char *path) {
    extern TrieNode *global_trie_root;
    TrieNode *crawler = global_trie_root;
    while (*path) {
        if (!crawler->children[(int)*path]) {
            return 0;
        }
        crawler = crawler->children[(int)*path];
        path++;
    }
    if (crawler != NULL && crawler->is_end_of_path && crawler->is_directory) {
        return 1;
    }
    return 0;
} 