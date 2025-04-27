#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>

#define MAXLINE 1024
#define MAXCONTENT 5242880 // 5MB for tar files
#define MAXPATH 512
#define MAX_FILES 1000
#define TARFILE_SIZE 5242880

// Global variable for S1 directory
char s1_dir[256];

// Global variables for server ports
int S2_PORT;
int S3_PORT;
int S4_PORT;

// Signal handling
void handle_sigpipe(int signum) {
    printf("S1: Caught SIGPIPE signal\n");
}

// Set socket timeout
int set_socket_timeout(int sockfd, int seconds) {
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    return setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Clean path to remove double slashes
void clean_path(char *path) {
    if (!path) return;
    char *src = path, *dst = path;
    while (*src) {
        if (*src == '/' && *(src + 1) == '/') {
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// Function to recursively search for a file
int find_file(const char *dirname, const char *filename, char *found_path, size_t path_size, const char *base_dir) {
    printf("S1: find_file: Searching %s for %s\n", dirname, filename);
    if (!dirname || !filename || !found_path || path_size == 0) {
        printf("S1: find_file: Invalid arguments\n");
        return -1;
    }
    
    DIR *dir = opendir(dirname);
    if (!dir) {
        printf("S1: find_file: opendir %s failed: %s\n", dirname, strerror(errno));
        return -1;
    }
    
    struct dirent *entry;
    struct stat statbuf;
    int status = 0;
    
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        char path[MAXPATH];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);
        clean_path(path);
        
        if (stat(path, &statbuf) == -1) {
            printf("S1: find_file: stat %s failed: %s\n", path, strerror(errno));
            continue;
        }
        
        if (S_ISREG(statbuf.st_mode)) {
            const char *cmp_name = strrchr(filename, '/') ? strrchr(filename, '/') + 1 : filename;
            if (strcmp(entry->d_name, cmp_name) == 0) {
                if (base_dir && strncmp(path, base_dir, strlen(base_dir)) == 0) {
                    char rel_path[MAXPATH];
                    snprintf(rel_path, sizeof(rel_path), "%s", path + strlen(base_dir) + 1);
                    clean_path(rel_path);
                    if (strlen(rel_path) < path_size) {
                        strncpy(found_path, rel_path, path_size - 1);
                        found_path[path_size - 1] = '\0';
                        printf("S1: find_file: Found %s (relative)\n", found_path);
                        closedir(dir);
                        return 1;
                    }
                } else {
                    if (strlen(path) < path_size) {
                        strncpy(found_path, path, path_size - 1);
                        found_path[path_size - 1] = '\0';
                        printf("S1: find_file: Found %s (absolute)\n", found_path);
                        closedir(dir);
                        return 1;
                    }
                }
            }
        }
        
        if (S_ISDIR(statbuf.st_mode)) {
            status = find_file(path, filename, found_path, path_size, base_dir);
            if (status == 1) {
                closedir(dir);
                return 1;
            }
        }
    }
    
    closedir(dir);
    printf("S1: find_file: %s not found in %s\n", filename, dirname);
    return status;
}

// Create directories recursively
int create_dirs(const char *path) {
    printf("S1: create_dirs: Creating %s\n", path);
    if (!path || strlen(path) == 0) {
        printf("S1: create_dirs: Invalid path\n");
        return -1;
    }
    
    char tmp[MAXPATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    clean_path(tmp);
    size_t len = strlen(tmp);
    
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                printf("S1: create_dirs: mkdir %s failed: %s\n", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        printf("S1: create_dirs: mkdir %s failed: %s\n", tmp, strerror(errno));
        return -1;
    }
    
    printf("S1: create_dirs: %s created\n", tmp);
    return 0;
}

// Collect files with specific extension recursively
int collect_files_recursive(const char *dirname, const char *base_dir, const char *ext, 
                           char files[][512], int *file_count, int max_files) {
    printf("S1: collect_files: Scanning %s for %s\n", dirname, ext);
    if (!dirname || !base_dir || !ext || !files || !file_count) {
        printf("S1: collect_files: Invalid arguments\n");
        return -1;
    }
    
    DIR *dir = opendir(dirname);
    if (!dir) {
        printf("S1: collect_files: opendir %s failed: %s\n", dirname, strerror(errno));
        return -1;
    }
    
    struct dirent *entry;
    struct stat statbuf;
    
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        char path[MAXPATH];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);
        clean_path(path);
        
        if (stat(path, &statbuf) == -1) {
            printf("S1: collect_files: stat %s failed: %s\n", path, strerror(errno));
            continue;
        }
        
        if (S_ISREG(statbuf.st_mode)) {
            char *file_ext = strrchr(entry->d_name, '.');
            if (file_ext && strcasecmp(file_ext, ext) == 0) {
                if (*file_count < max_files) {
                    strncpy(files[*file_count], path, 511);
                    files[*file_count][511] = '\0';
                    printf("S1: collect_files: Added %s\n", path);
                    (*file_count)++;
                } else {
                    printf("S1: collect_files: Max file count reached\n");
                    closedir(dir);
                    return 0;
                }
            }
        } else if (S_ISDIR(statbuf.st_mode)) {
            collect_files_recursive(path, base_dir, ext, files, file_count, max_files);
        }
    }
    
    closedir(dir);
    printf("S1: collect_files: Found %d files in %s\n", *file_count, dirname);
    return 0;
}

// Connect to another server (S2, S3, or S4)
int connect_to_server(int port) {
    printf("S1: connect_to_server: Trying port %d\n", port);
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("S1: connect_to_server: Socket creation failed: %s\n", strerror(errno));
        return -1;
    }
    
    struct sockaddr_in servaddr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = inet_addr("127.0.0.1") };
    
    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        printf("S1: connect_to_server: Connect to port %d failed: %s\n", port, strerror(errno));
        close(sockfd);
        return -1;
    }
    
    if (set_socket_timeout(sockfd, 30) < 0) {
        printf("S1: connect_to_server: Set timeout failed: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }
    
    printf("S1: connect_to_server: Connected to port %d\n", port);
    return sockfd;
}

// Forward command to another server and relay response
int forward_command(int clientfd, char *cmd, char *fname, char *dpath, int port) {
    printf("hi XOXO \n");
    printf("hiiiii %d %d %s %s %s\n", clientfd, port, cmd, fname, dpath);

    fflush(stdout);

    if (!cmd || !fname || !dpath) {
        printf("S1: forward_command: Invalid arguments\n");
        send(clientfd, "ERROR: Invalid command arguments", strlen("ERROR: Invalid command arguments"), 0);
        return -1;
    }
    
    printf("S1: forward_command: %s %s %s to port %d\n", cmd, fname, dpath, port);
    
    int serverfd = connect_to_server(port);
    if (serverfd < 0) {
        printf("S1: forward_command: Connection to port %d failed\n", port);
        send(clientfd, "ERROR: Failed to connect to server", strlen("ERROR: Failed to connect to server"), 0);
        return -1;
    }
    
    char buffer[MAXLINE] = {0};
    snprintf(buffer, sizeof(buffer), "%s %s %s", cmd, fname, dpath);
    printf("S1: forward_command: Sending: %s\n", buffer);
    if (send(serverfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: forward_command: Send failed: %s\n", strerror(errno));
        close(serverfd);
        send(clientfd, "ERROR: Failed to send to server", strlen("ERROR: Failed to send to server"), 0);
        return -1;
    }
    
    if (strcmp(cmd, "uploadf") == 0) {
        char len_str[32] = {0};
        printf("S1: forward_command: Waiting for content length\n");
        int i = 0;
        while (i < sizeof(len_str) - 1) {
            int n = recv(clientfd, &len_str[i], 1, 0);
            if (n <= 0) {
                printf("S1: forward_command: Recv length failed: %s\n", n == 0 ? "closed" : strerror(errno));
                close(serverfd);
                send(clientfd, "ERROR: Failed to receive length", strlen("ERROR: Failed to receive length"), 0);
                return -1;
            }
            if (len_str[i] == '\n') {
                len_str[i] = '\0';
                break;
            }
            i++;
        }
        
        if (i >= sizeof(len_str) - 1) {
            printf("S1: forward_command: Length too long\n");
            close(serverfd);
            send(clientfd, "ERROR: Invalid content length", strlen("ERROR: Invalid content length"), 0);
            return -1;
        }
        
        size_t content_len = atoi(len_str);
        printf("S1: forward_command: Content length: %zu\n", content_len);
        if (content_len == 0 || content_len >= MAXCONTENT) {
            printf("S1: forward_command: Invalid content length: %zu\n", content_len);
            close(serverfd);
            send(clientfd, content_len == 0 ? "ERROR: Invalid content length" : 
                 "ERROR: Content too large", strlen(content_len == 0 ? 
                 "ERROR: Invalid content length" : "ERROR: Content too large"), 0);
            return -1;
        }
        
        char *content = malloc(content_len + 1);
        if (!content) {
            printf("S1: forward_command: Malloc failed\n");
            close(serverfd);
            send(clientfd, "ERROR: Memory allocation failed", strlen("ERROR: Memory allocation failed"), 0);
            return -1;
        }
        
        size_t total = 0;
        printf("S1: forward_command: Receiving %zu bytes\n", content_len);
        while (total < content_len) {
            int n = recv(clientfd, content + total, content_len - total, 0);
            if (n <= 0) {
                printf("S1: forward_command: Recv content failed: %s\n", n == 0 ? "closed" : strerror(errno));
                free(content);
                close(serverfd);
                send(clientfd, "ERROR: Failed to receive content", strlen("ERROR: Failed to receive content"), 0);
                return -1;
            }
            total += n;
        }
        content[total] = '\0';
        printf("S1: forward_command: Received %zu bytes\n", total);
        
        printf("S1: forward_command: Sending length to server: %s\n", len_str);
        if (send(serverfd, len_str, strlen(len_str), 0) < 0) {
            printf("S1: forward_command: Send length failed: %s\n", strerror(errno));
            free(content);
            close(serverfd);
            send(clientfd, "ERROR: Failed to send length to server", strlen("ERROR: Failed to send length to server"), 0);
            return -1;
        }
        
        printf("S1: forward_command: Sending content to server\n");
        if (send(serverfd, content, total, 0) < 0) {
            printf("S1: forward_command: Send content failed: %s\n", strerror(errno));
            free(content);
            close(serverfd);
            send(clientfd, "ERROR: Failed to send content to server", strlen("ERROR: Failed to send content to server"), 0);
            return -1;
        }
        free(content);
    }
    
    char response[MAXCONTENT] = {0};
    int offset = 0;
    printf("S1: forward_command: Waiting for server response\n");
    while (offset < MAXCONTENT - 1) {
        int n = recv(serverfd, response + offset, MAXCONTENT - offset - 1, 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                printf("S1: forward_command: Recv response failed: %s\n", strerror(errno));
            } else if (n == 0) {
                printf("S1: forward_command: Server closed connection\n");
            }
            break;
        }
        offset += n;
        response[offset] = '\0';
        printf("S1: forward_command: Received %d bytes\n", n);
        
        if (strstr(response, "FILE_INFO:") || strstr(response, "TAR_FILE:") || strstr(response, "ERROR:") ||
            strcmp(cmd, "dispfnames") == 0 || strcmp(cmd, "removef") == 0) {
            break;
        }
    }
    
    if (offset > 0) {
        printf("S1: forward_command: Sending response to client (%d bytes)\n", offset);
        if (send(clientfd, response, offset, 0) < 0) {
            printf("S1: forward_command: Send to client failed: %s\n", strerror(errno));
            close(serverfd);
            return -1;
        }
    } else {
        printf("S1: forward_command: No response from server\n");
        send(clientfd, "ERROR: No response from server", strlen("ERROR: No response from server"), 0);
    }
    
    printf("S1: forward_command: Closing server connection\n");
    close(serverfd);
    return 0;
}

// Handle downlf command locally for .c files
int handle_downlf(int connfd, const char *filename) {
    printf("S1: handle_downlf: Starting for %s\n", filename);
    if (!filename || strlen(filename) == 0) {
        printf("S1: handle_downlf: No filename\n");
        send(connfd, "ERROR: Filename not specified", strlen("ERROR: Filename not specified"), 0);
        return -1;
    }
    
    char *ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".c") != 0) {
        printf("S1: handle_downlf: Not a .c file: %s\n", filename);
        send(connfd, "ERROR: Only .c files supported on S1", strlen("ERROR: Only .c files supported on S1"), 0);
        return -1;
    }
    
    char buffer[MAXLINE] = {0};
    char content[MAXCONTENT] = {0};
    char full_path[MAXPATH] = {0};
    
    printf("S1: handle_downlf: Constructing path\n");
    if (filename[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", s1_dir, filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", s1_dir, filename);
    }
    clean_path(full_path);
    printf("S1: handle_downlf: Checking %s\n", full_path);
    
    if (access(full_path, F_OK) != 0) {
        printf("S1: handle_downlf: File not found: %s\n", full_path);
        snprintf(buffer, sizeof(buffer), "ERROR: File not found");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        printf("S1: handle_downlf: Open failed: %s\n", strerror(errno));
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to open file");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size > MAXCONTENT) {
        fclose(fp);
        printf("S1: handle_downlf: File too large: %ld\n", file_size);
        snprintf(buffer, sizeof(buffer), "ERROR: File too large to transfer");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    size_t bytes_read = fread(content, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != file_size) {
        printf("S1: handle_downlf: Read failed: %zu/%ld\n", bytes_read, file_size);
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to read complete file");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "FILE_INFO:%s", filename);
    printf("S1: handle_downlf: Sending info: %s\n", buffer);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: handle_downlf: Send info failed: %s\n", strerror(errno));
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "%zu\n", bytes_read);
    printf("S1: handle_downlf: Sending size: %s\n", buffer);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: handle_downlf: Send size failed: %s\n", strerror(errno));
        return -1;
    }
    
    printf("S1: handle_downlf: Sending %zu bytes\n", bytes_read);
    if (send(connfd, content, bytes_read, 0) < 0) {
        printf("S1: handle_downlf: Send content failed: %s\n", strerror(errno));
        return -1;
    }
    
    printf("S1: handle_downlf: Sent %zu bytes\n", bytes_read);
    return 0;
}

// Handle dispfnames command locally for .c files
int handle_dispfnames(int connfd, const char *pathname) {
    printf("S1: handle_dispfnames: Starting for %s\n", pathname);
    if (!pathname || strlen(pathname) == 0) {
        printf("S1: handle_dispfnames: No path\n");
        send(connfd, "ERROR: Path not specified", strlen("ERROR: Path not specified"), 0);
        return -1;
    }
    
    char buffer[MAXCONTENT] = {0};
    char full_path[MAXPATH] = {0};
    
    if (pathname[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", s1_dir, pathname);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", s1_dir, pathname);
    }
    clean_path(full_path);
    printf("S1: handle_dispfnames: Checking %s\n", full_path);
    
    struct stat st;
    if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        printf("S1: handle_dispfnames: Not a directory: %s\n", full_path);
        snprintf(buffer, sizeof(buffer), "ERROR: Directory %s does not exist", pathname);
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    char c_files[MAX_FILES][512] = {0};
    int c_file_count = 0;
    printf("S1: handle_dispfnames: Collecting .c files\n");
    if (collect_files_recursive(full_path, s1_dir, ".c", c_files, &c_file_count, MAX_FILES) < 0) {
        printf("S1: handle_dispfnames: Collect failed\n");
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to collect files");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    int offset = 0;
    for (int i = 0; i < c_file_count && offset < sizeof(buffer) - 1; i++) {
        char *filename = strrchr(c_files[i], '/') ? strrchr(c_files[i], '/') + 1 : c_files[i];
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s\n", filename);
    }
    
    if (offset == 0) {
        snprintf(buffer, sizeof(buffer), "No .c files found in %s", pathname);
    }
    
    printf("S1: handle_dispfnames: Sending list: %s\n", buffer);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: handle_dispfnames: Send failed: %s\n", strerror(errno));
        return -1;
    }
    
    printf("S1: handle_dispfnames: Done\n");
    return 0;
}

// Handle removef command locally for .c files
int handle_removef(int connfd, const char *filename) {
    printf("S1: handle_removef: Starting for %s\n", filename);
    if (!filename || strlen(filename) == 0) {
        printf("S1: handle_removef: No filename\n");
        send(connfd, "ERROR: Filename not specified", strlen("ERROR: Filename not specified"), 0);
        return -1;
    }
    
    char *ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".c") != 0) {
        printf("S1: handle_removef: Not a .c file: %s\n", filename);
        send(connfd, "ERROR: Only .c files supported on S1", strlen("ERROR: Only .c files supported on S1"), 0);
        return -1;
    }
    
    char buffer[MAXLINE] = {0};
    char full_path[MAXPATH] = {0};
    
    if (filename[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", s1_dir, filename);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", s1_dir, filename);
    }
    clean_path(full_path);
    printf("S1: handle_removef: Checking %s\n", full_path);
    
    struct stat st;
    if (stat(full_path, &st) != 0) {
        printf("S1: handle_removef: File not found: %s\n", full_path);
        snprintf(buffer, sizeof(buffer), "ERROR: File %s does not exist", filename);
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    if (unlink(full_path) != 0) {
        printf("S1: handle_removef: Delete failed: %s\n", strerror(errno));
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to delete file %s: %s", filename, strerror(errno));
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    printf("S1: handle_removef: Deleted %s\n", full_path);
    snprintf(buffer, sizeof(buffer), "File %s deleted from S1", filename);
    send(connfd, buffer, strlen(buffer), 0);
    return 0;
}

// Handle downltar command locally for .c files
int handle_downltar(int connfd, const char *filetype) {
    printf("S1: handle_downltar: Starting for %s\n", filetype);
    if (!filetype || strcmp(filetype, ".c") != 0) {
        printf("S1: handle_downltar: Invalid filetype: %s\n", filetype ? filetype : "null");
        send(connfd, "ERROR: Only .c filetype supported on S1", strlen("ERROR: Only .c filetype supported on S1"), 0);
        return -1;
    }
    
    char buffer[MAXLINE] = {0};
    char content[MAXCONTENT] = {0};
    char c_files[MAX_FILES][512] = {0};
    int file_count = 0;
    
    printf("S1: handle_downltar: Collecting .c files\n");
    if (collect_files_recursive(s1_dir, s1_dir, ".c", c_files, &file_count, MAX_FILES) < 0) {
        printf("S1: handle_downltar: Collect failed\n");
        send(connfd, "ERROR: Failed to collect .c files", strlen("ERROR: Failed to collect .c files"), 0);
        return -1;
    }
    
    if (file_count == 0) {
        printf("S1: handle_downltar: No .c files\n");
        send(connfd, "ERROR: No .c files found in S1", strlen("ERROR: No .c files found in S1"), 0);
        return -1;
    }
    
    char filelist_path[256];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    snprintf(filelist_path, sizeof(filelist_path), "/tmp/c_filelist_%s.txt", timestamp);
    
    FILE *filelist = fopen(filelist_path, "w");
    if (!filelist) {
        printf("S1: handle_downltar: Create filelist failed: %s\n", strerror(errno));
        send(connfd, "ERROR: Failed to prepare tar file", strlen("ERROR: Failed to prepare tar file"), 0);
        return -1;
    }
    
    for (int i = 0; i < file_count; i++) {
        fprintf(filelist, "%s\n", c_files[i]);
    }
    fclose(filelist);
    
    char tar_filename[256];
    snprintf(tar_filename, sizeof(tar_filename), "/tmp/c_files_%s.tar", timestamp);
    char tar_cmd[512];
    snprintf(tar_cmd, sizeof(tar_cmd), "tar -cf %s -T %s", tar_filename, filelist_path);
    
    printf("S1: handle_downltar: Running: %s\n", tar_cmd);
    if (system(tar_cmd) != 0) {
        printf("S1: handle_downltar: Tar failed\n");
        unlink(filelist_path);
        send(connfd, "ERROR: Failed to create tar file", strlen("ERROR: Failed to create tar file"), 0);
        return -1;
    }
    
    unlink(filelist_path);
    
    FILE *tar_fp = fopen(tar_filename, "rb");
    if (!tar_fp) {
        printf("S1: handle_downltar: Open tar failed: %s\n", strerror(errno));
        unlink(tar_filename);
        send(connfd, "ERROR: Failed to read tar file", strlen("ERROR: Failed to read tar file"), 0);
        return -1;
    }
    
    fseek(tar_fp, 0, SEEK_END);
    long tar_size = ftell(tar_fp);
    fseek(tar_fp, 0, SEEK_SET);
    
    if (tar_size > TARFILE_SIZE) {
        fclose(tar_fp);
        unlink(tar_filename);
        printf("S1: handle_downltar: Tar too large: %ld\n", tar_size);
        send(connfd, "ERROR: Tar file too large to transfer", strlen("ERROR: Tar file too large to transfer"), 0);
        return -1;
    }
    
    size_t bytes_read = fread(content, 1, tar_size, tar_fp);
    fclose(tar_fp);
    
    if (bytes_read != tar_size) {
        printf("S1: handle_downltar: Read tar failed: %zu/%ld\n", bytes_read, tar_size);
        unlink(tar_filename);
        send(connfd, "ERROR: Failed to read complete tar file", strlen("ERROR: Failed to read complete tar file"), 0);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "TAR_FILE:c_files.tar");
    printf("S1: handle_downltar: Sending info: %s\n", buffer);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: handle_downltar: Send info failed: %s\n", strerror(errno));
        unlink(tar_filename);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "%zu\n", bytes_read);
    printf("S1: handle_downltar: Sending size: %s\n", buffer);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        printf("S1: handle_downltar: Send size failed: %s\n", strerror(errno));
        unlink(tar_filename);
        return -1;
    }
    
    printf("S1: handle_downltar: Sending %zu bytes\n", bytes_read);
    if (send(connfd, content, bytes_read, 0) < 0) {
        printf("S1: handle_downltar: Send content failed: %s\n", strerror(errno));
        unlink(tar_filename);
        return -1;
    }
    
    printf("S1: handle_downltar: Sent %zu bytes\n", bytes_read);
    unlink(tar_filename);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <S1_port> <S2_port> <S3_port> <S4_port>\n", argv[0]);
        exit(1);
    }

    printf("S1: Parsing arguments\n");
    int port = atoi(argv[1]);
    S2_PORT = atoi(argv[2]);
    S3_PORT = atoi(argv[3]);
    S4_PORT = atoi(argv[4]);

    printf("S1: Ports - S1:%d, S2:%d, S3:%d, S4:%d\n", port, S2_PORT, S3_PORT, S4_PORT);

    if (port <= 0 || port > 65535 || 
        S2_PORT <= 0 || S2_PORT > 65535 ||
        S3_PORT <= 0 || S3_PORT > 65535 ||
        S4_PORT <= 0 || S4_PORT > 65535) {
        fprintf(stderr, "S1: Invalid port number(s)\n");
        exit(1);
    }

    printf("S1: Setting up SIGPIPE handler\n");
    signal(SIGPIPE, handle_sigpipe);

    printf("S1: Getting HOME\n");
    char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "S1: Error: HOME not set\n");
        exit(1);
    }

    printf("S1: HOME=%s\n", home);
    snprintf(s1_dir, sizeof(s1_dir), "%s/S1", home);
    clean_path(s1_dir);
    printf("S1: Creating base directory: %s\n", s1_dir);
    
    if (mkdir(s1_dir, 0755) < 0 && errno != EEXIST) {
        perror("S1: Failed to create S1 directory");
        exit(1);
    }

    printf("S1: Creating socket\n");
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("S1: Socket creation failed");
        exit(1);
    }

    printf("S1: Setting socket options\n");
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("S1: setsockopt(SO_REUSEADDR) failed");
        close(sockfd);
        exit(1);
    }

    printf("S1: Binding to port %d\n", port);
    struct sockaddr_in servaddr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("S1: Bind failed");
        close(sockfd);
        exit(1);
    }

    printf("S1: Listening on port %d\n", port);
    if (listen(sockfd, 5) < 0) {
        perror("S1: Listen failed");
        close(sockfd);
        exit(1);
    }

    printf("S1: Server running, waiting for connections...\n");

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        printf("S1: Waiting for client connection\n");
        int connfd = accept(sockfd, (struct sockaddr*)&cliaddr, &len);
        if (connfd < 0) {
            printf("S1: Accept failed: %s\n", strerror(errno));
            continue;
        }

        printf("S1: Connection from %s:%d\n", inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));

        if (set_socket_timeout(connfd, 30) < 0) {
            printf("S1: Set timeout failed: %s\n", strerror(errno));
            close(connfd);
            continue;
        }

        char buffer[MAXLINE] = {0};
        char content[MAXCONTENT] = {0};
        
        while (1) {
            printf("S1: Waiting for command\n");
            int n = recv(connfd, buffer, MAXLINE - 1, 0);
            if (n <= 0) {
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        printf("S1: Receive timeout\n");
                    } else {
                        printf("S1: Recv failed: %s\n", strerror(errno));
                    }
                } else {
                    printf("S1: Client closed connection\n");
                }
                break;
            }
            
            buffer[n] = '\0';
            printf("S1: Received: %s\n", buffer);

            char cmd[50] = {0}, fname[100] = {0}, dpath[200] = {0};
            sscanf(buffer, "%49s %99s %199s", cmd, fname, dpath);
            printf("S1: Parsed - cmd:%s, fname:%s, dpath:%s\n", cmd, fname, dpath);

            if (strcmp(cmd, "downlf") == 0) {
                if (strlen(fname) == 0) {
                    printf("S1: downlf: No filename\n");
                    send(connfd, "ERROR: Filename not specified", strlen("ERROR: Filename not specified"), 0);
                    continue;
                }
                char *ext = strrchr(fname, '.');
                if (ext && strcmp(ext, ".c") == 0) {
                    handle_downlf(connfd, fname);
                } else if (ext && strcmp(ext, ".pdf") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S2_PORT);
                } else if (ext && strcmp(ext, ".txt") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S3_PORT);
                } else if (ext && strcmp(ext, ".zip") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S4_PORT);
                } else {
                    printf("S1: downlf: Bad file type: %s\n", fname);
                    send(connfd, "ERROR: Unsupported file type", strlen("ERROR: Unsupported file type"), 0);
                }
            } else if (strcmp(cmd, "uploadf") == 0) {
                if (strlen(fname) == 0 || strlen(dpath) == 0) {
                    printf("S1: uploadf: Missing filename or path\n");
                    send(connfd, "ERROR: Filename and path must be specified", 
                         strlen("ERROR: Filename and path must be specified"), 0);
                    continue;
                }
                
                char *ext = strrchr(fname, '.');
                if (ext && strcmp(ext, ".c") == 0) {
                    char len_str[32] = {0};
                    printf("S1: uploadf: Waiting for length\n");
                    int i = 0;
                    while (i < sizeof(len_str) - 1) {
                        n = recv(connfd, &len_str[i], 1, 0);
                        if (n <= 0) {
                            printf("S1: uploadf: Recv length failed: %s\n", n == 0 ? "closed" : strerror(errno));
                            send(connfd, "ERROR: Failed to receive length", 
                                 strlen("ERROR: Failed to receive length"), 0);
                            break;
                        }
                        if (len_str[i] == '\n') {
                            len_str[i] = '\0';
                            break;
                        }
                        i++;
                    }
                    
                    if (n <= 0 || i >= sizeof(len_str) - 1) {
                        printf("S1: uploadf: Bad length\n");
                        continue;
                    }
                    
                    size_t content_len = atoi(len_str);
                    printf("S1: uploadf: Content length: %zu\n", content_len);

                    if (content_len == 0 || content_len >= MAXCONTENT) {
                        printf("S1: uploadf: Invalid length: %zu\n", content_len);
                        send(connfd, content_len == 0 ? "ERROR: Invalid content length" : 
                             "ERROR: Content too large", strlen(content_len == 0 ? 
                             "ERROR: Invalid content length" : "ERROR: Content too large"), 0);
                        continue;
                    }

                    size_t total = 0;
                    printf("S1: uploadf: Receiving %zu bytes\n", content_len);
                    while (total < content_len) {
                        n = recv(connfd, content + total, content_len - total, 0);
                        if (n <= 0) {
                            printf("S1: uploadf: Recv content failed: %s\n", n == 0 ? "closed" : strerror(errno));
                            send(connfd, "ERROR: Failed to receive content", 
                                 strlen("ERROR: Failed to receive content"), 0);
                            break;
                        }
                        total += n;
                    }
                    
                    if (total < content_len) {
                        printf("S1: uploadf: Got %zu/%zu bytes\n", total, content_len);
                        continue;
                    }
                    
                    printf("S1: uploadf: Received %zu bytes\n", total);

                    char dirpath[512] = {0};
                    snprintf(dirpath, sizeof(dirpath), "%s/%s", s1_dir, dpath);
                    clean_path(dirpath);
                    
                    printf("S1: uploadf: Creating %s\n", dirpath);
                    if (create_dirs(dirpath) < 0) {
                        printf("S1: uploadf: Create dir failed\n");
                        send(connfd, "ERROR: Failed to create directories", 
                             strlen("ERROR: Failed to create directories"), 0);
                        continue;
                    }
                    
                    char filepath[512] = {0};
                    snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, fname);
                    clean_path(filepath);
                    printf("S1: uploadf: Saving to %s\n", filepath);
                    
                    FILE *fp = fopen(filepath, "wb");
                    if (!fp) {
                        printf("S1: uploadf: Open failed: %s\n", strerror(errno));
                        send(connfd, "ERROR: Failed to save file", 
                             strlen("ERROR: Failed to save file"), 0);
                        continue;
                    }
                    
                    size_t written = fwrite(content, 1, total, fp);
                    fclose(fp);
                    
                    if (written != total) {
                        printf("S1: uploadf: Write failed: %zu/%zu\n", written, total);
                        send(connfd, "ERROR: Partial file write", 
                             strlen("ERROR: Partial file write"), 0);
                        continue;
                    }
                    
                    printf("S1: uploadf: Saved %s (%zu bytes)\n", filepath, written);
                    send(connfd, "File saved successfully in S1", 
                         strlen("File saved successfully in S1"), 0);
                } else if (ext && strcmp(ext, ".pdf") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S2_PORT);
                } else if (ext && strcmp(ext, ".txt") == 0) {
                    printf("hiiiiiiiiii XOXOXOXO \n");
                    printf("%d %d %s %s %s \n", connfd, S3_PORT, fname, dpath, cmd);
                    forward_command(connfd, cmd, fname, dpath, S3_PORT);
                } else if (ext && strcmp(ext, ".zip") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S4_PORT);
                } else {
                    printf("S1: uploadf: Bad file type: %s\n", fname);
                    send(connfd, "ERROR: Unsupported file type", 
                         strlen("ERROR: Unsupported file type"), 0);
                }
            } else if (strcmp(cmd, "dispfnames") == 0) {
                if (strlen(fname) == 0) {
                    printf("S1: dispfnames: No path\n");
                    send(connfd, "ERROR: Path not specified", 
                         strlen("ERROR: Path not specified"), 0);
                    continue;
                }
                handle_dispfnames(connfd, fname);
                forward_command(connfd, cmd, fname, dpath, S2_PORT);
                forward_command(connfd, cmd, fname, dpath, S3_PORT);
                forward_command(connfd, cmd, fname, dpath, S4_PORT);
            } else if (strcmp(cmd, "removef") == 0) {
                if (strlen(fname) == 0) {
                    printf("S1: removef: No filename\n");
                    send(connfd, "ERROR: Filename not specified", 
                         strlen("ERROR: Filename not specified"), 0);
                    continue;
                }
                char *ext = strrchr(fname, '.');
                if (ext && strcmp(ext, ".c") == 0) {
                    handle_removef(connfd, fname);
                } else if (ext && strcmp(ext, ".pdf") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S2_PORT);
                } else if (ext && strcmp(ext, ".txt") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S3_PORT);
                } else if (ext && strcmp(ext, ".zip") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S4_PORT);
                } else {
                    printf("S1: removef: Bad file type: %s\n", fname);
                    send(connfd, "ERROR: Unsupported file type", 
                         strlen("ERROR: Unsupported file type"), 0);
                }
            } else if (strcmp(cmd, "downltar") == 0) {
                if (strlen(fname) == 0) {
                    printf("S1: downltar: No filetype\n");
                    send(connfd, "ERROR: Filetype not specified", 
                         strlen("ERROR: Filetype not specified"), 0);
                    continue;
                }
                if (strcmp(fname, ".c") == 0) {
                    handle_downltar(connfd, fname);
                } else if (strcmp(fname, ".pdf") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S2_PORT);
                } else if (strcmp(fname, ".txt") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S3_PORT);
                } else if (strcmp(fname, ".zip") == 0) {
                    forward_command(connfd, cmd, fname, dpath, S4_PORT);
                } else {
                    printf("S1: downltar: Bad filetype: %s\n", fname);
                    send(connfd, "ERROR: Unsupported file type", 
                         strlen("ERROR: Unsupported file type"), 0);
                }
            } else {
                printf("S1: Unknown command: %s\n", cmd);
                send(connfd, "ERROR: Unknown command", 
                     strlen("ERROR: Unknown command"), 0);
            }
        }
        
        printf("S1: Closing client connection\n");
        close(connfd);
    }

    close(sockfd);
    return 0;
}