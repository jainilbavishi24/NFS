#ifndef TRIE_H
#define TRIE_H

#include <stddef.h>
#include <stdbool.h>

#define ALPHABET_SIZE 128
#define BUFFER_SIZE 40960

// Forward declaration for StorageServer (to avoid circular dependency)
typedef struct StorageServer StorageServer;

typedef struct TrieNode {
    struct TrieNode *children[ALPHABET_SIZE];
    int is_end_of_path;
    StorageServer *server;
    int is_directory;
    int is_deleted;
} TrieNode;

TrieNode *create_trie_node(StorageServer *server);
void insert_path(TrieNode *root, const char *path, StorageServer *server, int is_directory);
void mark_subtree_as_deleted(TrieNode *node);
StorageServer *search_path(TrieNode *root, const char *path, int want_to_delete);
StorageServer *search_trie(TrieNode *root, const char *path);
void free_trie(TrieNode *root);
bool remove_path_from_trie(TrieNode *node, const char *path, int depth);
void mark_subtree_as_revived(TrieNode *node);
TrieNode *search_in_trie(TrieNode *root, const char *path);
int validate_path(TrieNode *root, const char *path);
void print_trie_paths1(TrieNode *root, char *path, int level, int client_sock, char *prefix);
void print_all_trie_paths1(int client_sock, char *prefix);
void print_trie_paths(TrieNode *root, char *path, int level, int client_sock);
void print_all_trie_paths(int client_sock);
void collect_paths_to_buffer(TrieNode *node, char *current_path, int depth, char *buffer, StorageServer *server);
void retrieve_paths_to_buffer(TrieNode *root, char *buffer, StorageServer *server);
void find_paths_with_prefix(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count);
void find_paths_with_prefix_two(TrieNode *node, char *current_path, size_t depth, const char *prefix, char **results, int *count);
char **search_trie_for_prefix(const char *prefix, int *result_count);
char **search_trie_for_prefix_two(const char *prefix, int *result_count);
int return_one_if_directory(const char *path);

#endif // TRIE_H 