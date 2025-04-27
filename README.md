# 🌐 Parallelized-Distributed-Storage-Hub

A resilient distributed file system with a multi-server architecture that provides type-based file routing and unified client access.

## 📋 Overview

Parallelized-Distributed-Storage-Hub is a network-based storage solution that partitions data across multiple specialized servers while presenting a unified access layer to clients. The system employs a coordinator-worker architecture where the primary server (S1) routes requests to the appropriate specialized server based on file types.

## 🏗️ Architecture

The system consists of five main components:

1. **Client**: Provides a user interface to interact with the distributed file system
2. **Server 1 (S1)**: Primary coordinator that routes requests to the appropriate specialized server
3. **Server 2 (S2)**: Handles PDF files (`.pdf`)
4. **Server 3 (S3)**: Handles text files (`.txt`)
5. **Server 4 (S4)**: Handles compressed files (`.zip`)

Server S1 additionally manages C source files (`.c`) directly.

## ✨ Features

- **Type-Based Routing**: Automatically directs operations to the appropriate specialized server
- **Comprehensive File Operations**:
  - Upload files to specific paths
  - Download individual files
  - Remove files from the system
  - List files in specified directories
  - Create TAR archives of specific file types
- **Recursive Path Resolution**: Enables efficient file lookups across the distributed system
- **Parallel Processing**: Multiple servers can operate simultaneously to improve throughput
- **Fault Tolerance**: Servers operate independently to prevent system-wide failures
- **Transparent Client Interface**: Provides a simple interface that abstracts the underlying distribution

## 🛠️ System Requirements

- Linux/Unix-based operating system
- GCC compiler
- Network connectivity between servers
- Available ports for server communication

## 📦 Installation

1. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/Parallelized-Distributed-Storage-Hub.git
   cd Parallelized-Distributed-Storage-Hub
   ```

2. Compile the servers and client:
   ```bash
   gcc -o client client.c
   gcc -o s1 s1.c
   gcc -o s2 s2.c
   gcc -o s3 s3.c
   gcc -o s4 s4.c
   ```

## 🚀 Usage

1. **Start the specialized servers first**:
   ```bash
   ./s2 8002
   ./s3 8003
   ./s4 8004
   ```

2. **Start the primary server (S1)**:
   ```bash
   ./s1 8001 8002 8003 8004
   ```

3. **Start the client and connect to S1**:
   ```bash
   ./client 8001
   ```

4. **Use client commands**:
   - `downlf <filename>` - Download a file
   - `uploadf <filename> <path>` - Upload a file to specified path
   - `dispfnames <path>` - Display filenames in the specified path
   - `removef <filename>` - Remove a file
   - `downltar <.c|.pdf|.txt|.zip>` - Download a tar archive of all files of the specified type
   - `exit` - Exit the client

## 📁 Directory Structure

The system automatically creates the following directory structure in the user's home directory:

```
~/
├── S1/         # Storage for .c files
├── S2/         # Storage for .pdf files
├── S3/         # Storage for .txt files
└── S4/         # Storage for .zip files
```

## ⚙️ How It Works

1. All client requests are initially sent to S1.
2. S1 examines the file extension and either:
   - Processes the request directly (for .c files)
   - Forwards the request to the appropriate specialized server (S2, S3, or S4)
3. The specialized server processes the request and sends the response back to S1.
4. S1 relays the response to the client.

For operations like `dispfnames`, S1 aggregates results from all servers.

## 🔒 Error Handling

The system includes comprehensive error handling for scenarios such as:
- File not found
- Invalid commands
- Connection issues
- Path resolution failures
- Permission errors
- File size limitations

## 🧩 Implementation Details

### Client
- TCP/IP-based communication with S1
- Command parsing and handling
- File content transmission

### Server 1 (S1)
- Request routing based on file extensions
- Direct handling of .c files
- Connection management to specialized servers
- Response relaying to clients

### Specialized Servers (S2, S3, S4)
- Handling specific file types
- Directory creation and management
- File operations (read, write, delete)
- Archive creation

## 🤝 Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

