# Network File System (NFS) Project

## Overview

This project implements a distributed Network File System (NFS) in C, supporting multiple clients, a central Naming Server, and multiple Storage Servers. The system allows clients to perform file operations (read, write, create, delete, list, info, stream audio) transparently across a distributed set of storage servers, with support for replication, asynchronous writes, and fault tolerance.

## Architecture

```
+---------+         +----------------+         +------------------+
| Client  | <-----> |  Naming Server | <-----> | Storage Servers  |
+---------+         +----------------+         +------------------+
```

- **Clients**: User-facing programs that issue file operations.
- **Naming Server (NM)**: Central coordinator that maintains the directory structure, maps files/folders to storage servers, and manages metadata, replication, and fault tolerance.
- **Storage Servers (SS)**: Store actual file data and handle file operations as directed by the Naming Server.

## Features

- **File Operations**: Read, write (sync/async), create, delete, copy, list, info, and audio streaming.
- **Replication**: Each file/folder is replicated to two other storage servers (when more than two are present) for fault tolerance.
- **Asynchronous Writes**: Large writes can be handled asynchronously for better client responsiveness.
- **Concurrency**: Multiple clients can access the system concurrently; only one writer per file at a time.
- **Efficient Search**: Trie-based directory structure with LRU caching for fast lookups.
- **Failure Handling**: Detects storage server failures and serves data from replicas.
- **Logging**: All operations and communications are logged for traceability.

## File Structure

- `naming.c` — Naming Server implementation.
- `storage.c` — Storage Server implementation.
- `client.c` — Client implementation.

## Compilation

You need a C compiler (e.g., `gcc`) and POSIX environment (Linux, WSL, etc.).

```sh
gcc -o naming naming.c -lpthread
gcc -o storage storage.c -lpthread
gcc -o client client.c -lpthread
```

## Running the System

### 1. Start the Naming Server

```sh
./naming
```
- The Naming Server will print its IP and port (default: 8090).
- The IP is auto-detected using `hostname -I`.

### 2. Start Storage Servers

Each storage server must be started with:
```sh
./storage <Naming Server IP> <Naming Server Port> <Storage Server Port> <Accessible Folder>
```
Example:
```sh
./storage 192.168.1.10 8090 9001 ./data1
./storage 192.168.1.10 8090 9002 ./data2
```
- You can run multiple storage servers on different machines or ports.
- Each server registers itself and its accessible folder with the Naming Server.

### 3. Start Clients

Each client connects to the Naming Server:
```sh
./client <Naming Server IP> <Naming Server Port>
```
Example:
```sh
./client 192.168.1.10 8090
```

## Client Commands

After starting, the client will prompt for commands:

- `READ` — Read a file.
- `WRITE` — Write to a file (optionally synchronous).
- `CREATE_DIC` — Create a directory.
- `CREATE_F` — Create a file.
- `DELETE` — Delete a file or directory.
- `LIST` — List files and directories in a folder.
- `INFO` — Get file metadata.
- `COPY` — Copy a file or directory.
- `STREAM` — Stream an audio file (`.mp3` only; requires `mpv` installed).
- `EXIT` — Exit the client.

**Example session:**
```
Enter a command: READ
Enter file path: ./data1/file.txt
...
Enter a command: WRITE
Enter file path: ./data1/file.txt
Do you want to write synchronously irrespective of time overhead? (yes/no): no
Enter data to write: Hello, world!
...
```

## Key Implementation Details

- **Trie Directory Structure**: The Naming Server uses a trie to efficiently map file/directory paths to storage servers.
- **LRU Cache**: Recently accessed paths are cached for faster lookup.
- **Replication**: When more than two storage servers are present, each file is replicated to two others for redundancy.
- **Asynchronous Writes**: Large writes are handled in the background, with immediate acknowledgment to the client.
- **Concurrency**: Mutexes and condition variables ensure safe concurrent access to files.
- **Failure Handling**: If a storage server goes down, the Naming Server marks it and serves data from replicas (read-only).
- **Logging**: All operations are logged in `naming_server.log`.

## Error Codes & Handling

- If a file is not found, or is being written to, or a command is invalid, the client receives a descriptive error message.
- The system handles partial writes, server failures, and concurrent access gracefully.

## Audio Streaming

- The client can stream `.mp3` files using the `STREAM` command.
- Requires `mpv` to be installed on the client machine.

## Assumptions

- All communication uses TCP sockets.
- IP addresses and ports are provided at runtime; do not hardcode `127.0.0.1`.
- The system is designed for demonstration/educational purposes and may not be production-hardened.

## References

- [Project Specification](https://karthikv1392.github.io/cs3301_osn/project/)
- [POSIX C Library](https://pubs.opengroup.org/onlinepubs/9699919799/)
- [Handling Multiple Clients in C](https://www.geeksforgeeks.org/handling-multiple-clients-on-server-with-multithreading-using-socket-programming-in-c-cpp/)
- [mpv Manual](https://mpv.io/manual/master/)

## Authors

- [Your Team Name/Number]
- [Your Names]

---

**Note:** For any issues or questions, please refer to the project specification or contact the course staff. 