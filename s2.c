#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <signal.h>

#define MAXLINE 1024
#define MAXCONTENT 5242880 // 5MB for tar files
#define MAXPATH 512
#define MAX_FILES 1000
#define TARFILE_SIZE 5242880

// Global variable for S2 directory
char s2_dir[256];

// Signal handling
void handle_sigpipe(int signum) {
    printf("S2: Caught SIGPIPE signal. S1 likely disconnected unexpectedly.\n");
}

// Set socket timeout
int set_socket_timeout(int sockfd, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Function to recursively search for a file
int find_file(const char *dirname, const char *filename, char *found_path, size_t path_size, const char *base_dir) {
    DIR *dir;
    struct dirent *entry;
    struct stat statbuf;
    int status = 0;
    
    if (!dirname || !filename || !found_path) {
        return -1;
    }
    
    if ((dir = opendir(dirname)) == NULL) {
        perror("opendir");
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        char path[MAXPATH];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);
        
        if (stat(path, &statbuf) == -1) {
            perror("stat");
            continue;
        }
        
        if (S_ISREG(statbuf.st_mode)) {
            const char *last_slash = strrchr(filename, '/');
            const char *cmp_name = last_slash ? last_slash + 1 : filename;
            if (strcmp(entry->d_name, cmp_name) == 0) {
                if (base_dir && strncmp(path, base_dir, strlen(base_dir)) == 0) {
                    char rel_path[MAXPATH];
                    snprintf(rel_path, sizeof(rel_path), "%s", path + strlen(base_dir) + 1);
                    if (strlen(rel_path) < path_size) {
                        strncpy(found_path, rel_path, path_size - 1);
                        found_path[path_size - 1] = '\0';
                        closedir(dir);
                        return 1;
                    }
                } else {
                    if (strlen(path) < path_size) {
                        strncpy(found_path, path, path_size - 1);
                        found_path[path_size - 1] = '\0';
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
    return status;
}

// Create directories recursively
int create_dirs(const char *path) {
    if (!path || strlen(path) == 0) {
        return -1;
    }
    
    char tmp[MAXPATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    
    if (strlen(tmp) == 0) {
        return 0;
    }
    
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
                perror("mkdir failed");
                return -1;
            }
            *p = '/';
        }
    }
    
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
        perror("mkdir failed");
        return -1;
    }
    
    return 0;
}

// Collect files with specific extension recursively
int collect_files_recursive(const char *dirname, const char *base_dir, const char *ext, 
                           char files[][512], int *file_count, int max_files) {
    if (!dirname || !base_dir || !ext || !files || !file_count) {
        return -1;
    }
    
    DIR *dir;
    struct dirent *entry;
    struct stat statbuf;
    
    if ((dir = opendir(dirname)) == NULL) {
        perror("opendir");
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        
        char path[MAXPATH];
        snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);
        
        if (stat(path, &statbuf) == -1) {
            perror("stat");
            continue;
        }
        
        if (S_ISREG(statbuf.st_mode)) {
            char *file_ext = strrchr(entry->d_name, '.');
            if (file_ext && strcasecmp(file_ext, ext) == 0) {
                if (*file_count < max_files) {
                    strncpy(files[*file_count], path, 511);
                    files[*file_count][511] = '\0';
                    (*file_count)++;
                } else {
                    fprintf(stderr, "Warning: Maximum file count reached\n");
                    closedir(dir);
                    return 0;
                }
            }
        } else if (S_ISDIR(statbuf.st_mode)) {
            collect_files_recursive(path, base_dir, ext, files, file_count, max_files);
        }
    }
    
    closedir(dir);
    return 0;
}

// Handle downlf command
int handle_downlf(int connfd, const char *filename) {
    if (!filename || strlen(filename) == 0) {
        send(connfd, "ERROR: Filename not specified", strlen("ERROR: Filename not specified"), 0);
        return -1;
    }
    
    char *ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".pdf") != 0) {
        send(connfd, "ERROR: Only .pdf files supported", strlen("ERROR: Only .pdf files supported"), 0);
        return -1;
    }
    
    char buffer[MAXLINE];
    char content[MAXCONTENT];
    char found_path[MAXPATH] = {0};
    char full_path[MAXPATH];
    int found = 0;
    
    printf("S2: Processing downlf for file %s\n", filename);
    
    if (strncmp(filename, "~/S2/", 5) == 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", getenv("HOME"), filename + 2);
        if (access(full_path, F_OK) == 0) {
            found = 1;
            strncpy(found_path, filename, sizeof(found_path)-1);
        }
    } else if (filename[0] == '/') {
        strncpy(full_path, filename, sizeof(full_path)-1);
        if (access(full_path, F_OK) == 0) {
            found = 1;
            strncpy(found_path, filename, sizeof(found_path)-1);
        }
    } else {
        found = find_file(s2_dir, filename, found_path, sizeof(found_path), s2_dir);
        if (found == 1) {
            snprintf(full_path, sizeof(full_path), "%s/%s", s2_dir, found_path);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", s2_dir, filename);
            if (access(full_path, F_OK) == 0) {
                found = 1;
                strncpy(found_path, filename, sizeof(found_path)-1);
            }
        }
    }
    
    if (!found || access(full_path, F_OK) != 0) {
        snprintf(buffer, sizeof(buffer), "ERROR: File not found");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    printf("S2: Found file at: %s\n", full_path);
    
    FILE *fp = fopen(full_path, "rb");
    if (!fp) {
        perror("File open failed");
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to open file");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size > MAXCONTENT) {
        fclose(fp);
        snprintf(buffer, sizeof(buffer), "ERROR: File too large to transfer");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    size_t bytes_read = fread(content, 1, file_size, fp);
    fclose(fp);
    
    if (bytes_read != file_size) {
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to read complete file");
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "FILE_INFO:%s", found_path);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        perror("send file info failed");
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "%zu\n", bytes_read);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        perror("send file size failed");
        return -1;
    }
    
    if (send(connfd, content, bytes_read, 0) < 0) {
        perror("send file content failed");
        return -1;
    }
    
    printf("S2: Sent file to S1 (%zu bytes)\n", bytes_read);
    return 0;
}

// Handle dispfnames command
int handle_dispfnames(int connfd, const char *pathname) {
    if (!pathname || strlen(pathname) == 0) {
        send(connfd, "ERROR: Path not specified", strlen("ERROR: Path not specified"), 0);
        return -1;
    }
    
    char buffer[MAXCONTENT] = {0};
    char full_path[MAXPATH];
    
    printf("S2: Processing dispfnames for path %s\n", pathname);
    
    // Construct the full path for S2
    if (strncmp(pathname, "~/S2/", 5) == 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", getenv("HOME"), pathname + 2);
    } else if (pathname[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", pathname);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", s2_dir, pathname);
    }
    
    printf("S2: Searching in directory %s\n", full_path);
    
    // Check if the directory exists
    struct stat st;
    if (stat(full_path, &st) == -1 || !S_ISDIR(st.st_mode)) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "ERROR: Directory %s does not exist", pathname);
        send(connfd, error_msg, strlen(error_msg), 0);
        return -1;
    }
    
    // Collect .pdf files
    char pdf_files[MAX_FILES][512];
    int pdf_file_count = 0;
    collect_files_recursive(full_path, s2_dir, ".pdf", pdf_files, &pdf_file_count, MAX_FILES);
    
    // Prepare output
    int offset = 0;
    for (int i = 0; i < pdf_file_count && offset < sizeof(buffer) - 1; i++) {
        char *filename = strrchr(pdf_files[i], '/') ? strrchr(pdf_files[i], '/') + 1 : pdf_files[i];
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s\n", filename);
    }
    
    if (offset == 0) {
        snprintf(buffer, sizeof(buffer), "No .pdf files found in %s", pathname);
    }
    
    // Send result
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        perror("send failed");
        return -1;
    }
    
    return 0;
}

// Handle removef command
int handle_removef(int connfd, const char *filename) {
    if (!filename || strlen(filename) == 0) {
        send(connfd, "ERROR: Filename not specified", strlen("ERROR: Filename not specified"), 0);
        return -1;
    }
    
    char *ext = strrchr(filename, '.');
    if (!ext || strcmp(ext, ".pdf") != 0) {
        send(connfd, "ERROR: Only .pdf files supported", strlen("ERROR: Only .pdf files supported"), 0);
        return -1;
    }
    
    char buffer[MAXLINE];
    char full_path[MAXPATH];
    char found_path[MAXPATH] = {0};
    
    printf("S2: Processing removef for file %s\n", filename);
    
    if (strncmp(filename, "~/S2/", 5) == 0) {
        snprintf(full_path, sizeof(full_path), "%s/%s", getenv("HOME"), filename + 2);
    } else if (filename[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", filename);
    } else {
        if (find_file(s2_dir, filename, found_path, sizeof(found_path), s2_dir) == 1) {
            snprintf(full_path, sizeof(full_path), "%s/%s", s2_dir, found_path);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", s2_dir, filename);
        }
    }
    
    printf("S2: Full path for removal: %s\n", full_path);
    
    struct stat st;
    if (stat(full_path, &st) == -1) {
        char error_msg[100];
        snprintf(error_msg, sizeof(error_msg), "ERROR: File %s does not exist", filename);
        send(connfd, error_msg, strlen(error_msg), 0);
        return -1;
    }
    
    if (unlink(full_path) == 0) {
        snprintf(buffer, sizeof(buffer), "File %s deleted from S2", filename);
        send(connfd, buffer, strlen(buffer), 0);
        return 0;
    } else {
        snprintf(buffer, sizeof(buffer), "ERROR: Failed to delete file %s: %s", 
                 filename, strerror(errno));
        send(connfd, buffer, strlen(buffer), 0);
        return -1;
    }
}

// Handle downltar command
int handle_downltar(int connfd, const char *filetype) {
    if (!filetype || strcmp(filetype, ".pdf") != 0) {
        send(connfd, "ERROR: Only .pdf filetype supported", strlen("ERROR: Only .pdf filetype supported"), 0);
        return -1;
    }
    
    char buffer[MAXLINE];
    char content[MAXCONTENT];
    char pdf_files[MAX_FILES][512];
    int file_count = 0;
    
    printf("S2: Processing downltar for filetype %s\n", filetype);
    
    if (collect_files_recursive(s2_dir, s2_dir, ".pdf", pdf_files, &file_count, MAX_FILES) < 0) {
        send(connfd, "ERROR: Failed to collect .pdf files", 
             strlen("ERROR: Failed to collect .pdf files"), 0);
        return -1;
    }
    
    if (file_count == 0) {
        send(connfd, "ERROR: No .pdf files found in S2", 
             strlen("ERROR: No .pdf files found in S2"), 0);
        return -1;
    }
    
    char filelist_path[256];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", t);
    snprintf(filelist_path, sizeof(filelist_path), "/tmp/pdf_filelist_%s.txt", timestamp);
    
    FILE *filelist = fopen(filelist_path, "w");
    if (!filelist) {
        perror("Failed to create file list");
        send(connfd, "ERROR: Failed to prepare tar file", 
             strlen("ERROR: Failed to prepare tar file"), 0);
        return -1;
    }
    
    for (int i = 0; i < file_count; i++) {
        fprintf(filelist, "%s\n", pdf_files[i]);
    }
    fclose(filelist);
    
    char tar_filename[256];
    snprintf(tar_filename, sizeof(tar_filename), "/tmp/pdf_files_%s.tar", timestamp);
    char tar_cmd[512];
    snprintf(tar_cmd, sizeof(tar_cmd), "tar -cf %s -T %s", tar_filename, filelist_path);
    
    int tar_result = system(tar_cmd);
    if (tar_result != 0) {
        unlink(filelist_path);
        send(connfd, "ERROR: Failed to create tar file", 
             strlen("ERROR: Failed to create tar file"), 0);
        return -1;
    }
    
    unlink(filelist_path);
    
    FILE *tar_fp = fopen(tar_filename, "rb");
    if (!tar_fp) {
        perror("Failed to open tar file");
        unlink(tar_filename);
        send(connfd, "ERROR: Failed to read tar file", 
             strlen("ERROR: Failed to read tar file"), 0);
        return -1;
    }
    
    fseek(tar_fp, 0, SEEK_END);
    long tar_size = ftell(tar_fp);
    fseek(tar_fp, 0, SEEK_SET);
    
    if (tar_size > TARFILE_SIZE) {
        fclose(tar_fp);
        unlink(tar_filename);
        send(connfd, "ERROR: Tar file too large to transfer", 
             strlen("ERROR: Tar file too large to transfer"), 0);
        return -1;
    }
    
    const char *client_filename = "pdf_files.tar";
    
    memset(content, 0, sizeof(content));
    size_t bytes_read = fread(content, 1, tar_size, tar_fp);
    fclose(tar_fp);
    
    if (bytes_read != tar_size) {
        unlink(tar_filename);
        send(connfd, "ERROR: Failed to read complete tar file", 
             strlen("ERROR: Failed to read complete tar file"), 0);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "TAR_FILE:%s", client_filename);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        perror("Send tar info failed");
        unlink(tar_filename);
        return -1;
    }
    
    snprintf(buffer, sizeof(buffer), "%zu\n", bytes_read);
    if (send(connfd, buffer, strlen(buffer), 0) < 0) {
        perror("Send tar size failed");
        unlink(tar_filename);
        return -1;
    }
    
    if (send(connfd, content, bytes_read, 0) < 0) {
        perror("Send tar content failed");
        unlink(tar_filename);
        return -1;
    }
    
    printf("S2: Sent tar file %s (%zu bytes) to S1\n", tar_filename, bytes_read);
    
    unlink(tar_filename);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <S2_port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        exit(1);
    }

    // Set up signal handler
    signal(SIGPIPE, handle_sigpipe);

    // Create ~/S2 directory
    snprintf(s2_dir, sizeof(s2_dir), "%s/S2", getenv("HOME"));
    printf("S2: Creating base directory: %s\n", s2_dir);
    
    if (mkdir(s2_dir, 0755) < 0 && errno != EEXIST) {
        perror("Failed to create S2 directory");
        exit(1);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }

    struct sockaddr_in servaddr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed");
        exit(1);
    }

    if (listen(sockfd, 5) < 0) {
        perror("Listen failed");
        exit(1);
    }

    printf("S2: Listening on port %d...\n", port);

    while (1) {
        struct sockaddr_in cliaddr;
        socklen_t len = sizeof(cliaddr);
        int connfd = accept(sockfd, (struct sockaddr*)&cliaddr, &len);
        if (connfd < 0) {
            perror("Accept failed");
            continue;
        }

        printf("S2: Connection from %s:%d (S1)\n", 
               inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));

        // Set client socket timeout
        if (set_socket_timeout(connfd, 30) < 0) {
            perror("set client socket timeout");
            close(connfd);
            continue;
        }

        char buffer[MAXLINE];
        char content[MAXCONTENT];
        
        while (1) {
            memset(buffer, 0, sizeof(buffer));
            int n = recv(connfd, buffer, MAXLINE - 1, 0);
            if (n <= 0) {
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        printf("S2: Receive timeout, closing connection\n");
                    } else {
                        perror("Receive command failed");
                    }
                }
                printf("S2: Connection closed\n");
                break;
            }
            
            buffer[n] = '\0';
            printf("S2: Received: %s\n", buffer);

            char cmd[50], fname[100], dpath[200];
            cmd[0] = fname[0] = dpath[0] = '\0';
            sscanf(buffer, "%49s %99s %199s", cmd, fname, dpath);

            if (strcmp(cmd, "downlf") == 0) {
                if (strlen(fname) == 0) {
                    send(connfd, "ERROR: Filename not specified", 
                         strlen("ERROR: Filename not specified"), 0);
                    continue;
                }
                handle_downlf(connfd, fname);
            } else if (strcmp(cmd, "uploadf") == 0) {
                if (strlen(fname) == 0 || strlen(dpath) == 0) {
                    send(connfd, "ERROR: Filename and path must be specified", 
                         strlen("ERROR: Filename and path must be specified"), 0);
                    continue;
                }
                
                char *ext = strrchr(fname, '.');
                if (!ext || strcmp(ext, ".pdf") != 0) {
                    send(connfd, "ERROR: Only .pdf files supported", 
                         strlen("ERROR: Only .pdf files supported"), 0);
                    continue;
                }
                
                char len_str[32] = {0};
                int i = 0;
                while (i < sizeof(len_str) - 1) {
                    n = recv(connfd, &len_str[i], 1, 0);
                    if (n <= 0) {
                        if (n < 0) perror("Receive length failed");
                        else printf("S2: S1 disconnected\n");
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
                    printf("S2: Invalid content length\n");
                    continue;
                }
                
                size_t content_len = atoi(len_str);
                printf("S2: Content length: %zu\n", content_len);

                if (content_len >= MAXCONTENT) {
                    printf("S2: Content too large\n");
                    send(connfd, "ERROR: Content too large", 
                         strlen("ERROR: Content too large"), 0);
                    continue;
                }

                memset(content, 0, sizeof(content));
                size_t total = 0;
                while (total < content_len) {
                    n = recv(connfd, content + total, content_len - total, 0);
                    if (n <= 0) {
                        if (n < 0) perror("Receive content failed");
                        else printf("S2: S1 disconnected\n");
                        send(connfd, "ERROR: Failed to receive content", 
                             strlen("ERROR: Failed to receive content"), 0);
                        break;
                    }
                    total += n;
                }
                
                if (total < content_len) {
                    printf("S2: Incomplete content received\n");
                    continue;
                }
                
                printf("S2: Received %zu bytes of content\n", total);

                char dirpath[512];
                snprintf(dirpath, sizeof(dirpath), "%s/%s", s2_dir, dpath);
                
                printf("S2: Creating directory: %s\n", dirpath);
                if (create_dirs(dirpath) < 0) {
                    perror("Failed to create directories");
                    send(connfd, "ERROR: Failed to create directories", 
                         strlen("ERROR: Failed to create directories"), 0);
                    continue;
                }
                
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s%s", s2_dir, dpath, fname);
                printf("S2: Saving file to: %s\n", filepath);
                
                FILE *fp = fopen(filepath, "wb");
                if (fp) {
                    size_t written = fwrite(content, 1, total, fp);
                    fclose(fp);
                    
                    if (written == total) {
                        printf("S2: Saved %s (%zu bytes)\n", filepath, written);
                        send(connfd, "File saved successfully in S2", 
                             strlen("File saved successfully in S2"), 0);
                    } else {
                        printf("S2: Partial write: %zu of %zu bytes\n", written, total);
                        send(connfd, "ERROR: Partial file write", 
                             strlen("ERROR: Partial file write"), 0);
                    }
                } else {
                    perror("File save failed");
                    send(connfd, "ERROR: Failed to save file", 
                         strlen("ERROR: Failed to save file"), 0);
                }
            } else if (strcmp(cmd, "dispfnames") == 0) {
                if (strlen(fname) == 0) {
                    send(connfd, "ERROR: Path not specified", 
                         strlen("ERROR: Path not specified"), 0);
                    continue;
                }
                handle_dispfnames(connfd, fname);
            } else if (strcmp(cmd, "removef") == 0) {
                if (strlen(fname) == 0) {
                    send(connfd, "ERROR: Filename not specified", 
                         strlen("ERROR: Filename not specified"), 0);
                    continue;
                }
                handle_removef(connfd, fname);
            } else if (strcmp(cmd, "downltar") == 0) {
                if (strlen(fname) == 0) {
                    send(connfd, "ERROR: Filetype not specified", 
                         strlen("ERROR: Filetype not specified"), 0);
                    continue;
                }
                handle_downltar(connfd, fname);
            } else {
                printf("S2: Unknown command: %s\n", cmd);
                send(connfd, "ERROR: Unknown command", 
                     strlen("ERROR: Unknown command"), 0);
            }
        }
        
        close(connfd);
        printf("S2: Connection closed\n");
    }

    close(sockfd);
    return 0;
}