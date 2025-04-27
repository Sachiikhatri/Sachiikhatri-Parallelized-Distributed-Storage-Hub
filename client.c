#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <errno.h>

#define MAXLINE 1024
#define MAXCONTENT 5242880 // 5MB for tar files
#define MAXPATH 512

// Create directories recursively
int create_dirs(const char *path) {
    char tmp[512];
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

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <S1_port>\n", argv[0]);
        exit(1);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        exit(1);
    }

    int sockfd;
    struct sockaddr_in servaddr;
    char buffer[MAXLINE];
    char content[MAXCONTENT];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(1);
    }

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(port);
    servaddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        perror("Connection to S1 failed");
        exit(1);
    }

    printf("Connected to S1 on port %d\n", port);

    while (1) {
        printf("\nAvailable commands:\n");
        printf("  downlf <filename>           - Download a file\n");
        printf("  uploadf <filename> <path>   - Upload a file\n");
        printf("  dispfnames <path>           - Display filenames in path\n");
        printf("  removef <filename>          - Remove a file\n");
        printf("  downltar <.c|.pdf|.txt|.zip> - Download tar of all specified files\n");
        printf("  exit                        - Exit the client\n");
        printf("Enter command: ");

        char input[512];
        if (fgets(input, sizeof(input), stdin) == NULL) {
            printf("Input error\n");
            continue;
        }

        input[strcspn(input, "\n")] = '\0';

        if (strlen(input) == 0) {
            printf("Empty input, please enter a command\n");
            continue;
        }

        // Send command to S1
        if (send(sockfd, input, strlen(input), 0) < 0) {
            perror("Send failed");
            continue;
        }

        char cmd[50], fname[100], dpath[200];
        cmd[0] = fname[0] = dpath[0] = '\0';
        sscanf(input, "%49s %99s %199s", cmd, fname, dpath);

        if (strcmp(cmd, "exit") == 0) {
            printf("Exiting client\n");
            break;
        }
        else if (strcmp(cmd, "downlf") == 0 || strcmp(cmd, "downltar") == 0) {
            // Receive file or tar info
            int n = recv(sockfd, buffer, MAXLINE - 1, 0);
            if (n <= 0) {
                if (n < 0) perror("Receive file info failed");
                else printf("Server disconnected\n");
                break;
            }
            buffer[n] = '\0';
            printf("Received: %s\n", buffer);

            if (strncmp(buffer, "ERROR:", 6) == 0) {
                printf("Server error: %s\n", buffer);
                continue;
            }

            char *filename = NULL;
            if (strncmp(buffer, "FILE_INFO:", 10) == 0) {
                filename = buffer + 10;
            } else if (strncmp(buffer, "TAR_FILE:", 9) == 0) {
                filename = buffer + 9;
            } else {
                printf("Invalid server response: %s\n", buffer);
                continue;
            }

            // Receive content length
            char len_str[32] = {0};
            int i = 0;
            while (i < sizeof(len_str) - 1) {
                n = recv(sockfd, &len_str[i], 1, 0);
                if (n <= 0) {
                    if (n < 0) perror("Receive length failed");
                    else printf("Server disconnected\n");
                    break;
                }
                if (len_str[i] == '\n') {
                    len_str[i] = '\0';
                    break;
                }
                i++;
            }

            if (n <= 0 || i >= sizeof(len_str) - 1) {
                printf("Failed to receive content length\n");
                break;
            }

            size_t content_len = atoi(len_str);
            printf("Content length: %zu bytes\n", content_len);

            if (content_len >= MAXCONTENT) {
                printf("Content too large to receive\n");
                continue;
            }

            // Receive content
            size_t total = 0;
            while (total < content_len) {
                n = recv(sockfd, content + total, content_len - total, 0);
                if (n <= 0) {
                    if (n < 0) perror("Receive content failed");
                    else printf("Server disconnected\n");
                    break;
                }
                total += n;
            }

            if (n <= 0 && total < content_len) {
                printf("Connection closed during content receive\n");
                break;
            }

            printf("Received %zu bytes\n", total);

            // Save the file locally
            char *save_filename = strrchr(filename, '/') ? strrchr(filename, '/') + 1 : filename;
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "./%s", save_filename);
            printf("Saving to: %s\n", filepath);

            FILE *fp = fopen(filepath, "wb");
            if (fp) {
                size_t written = fwrite(content, 1, total, fp);
                fclose(fp);
                if (written == total) {
                    printf("File saved successfully (%zu bytes)\n", written);
                } else {
                    printf("Partial write: %zu of %zu bytes\n", written, total);
                }
            } else {
                perror("File save failed");
            }
        }
        else if (strcmp(cmd, "uploadf") == 0) {
            if (strlen(fname) == 0 || strlen(dpath) == 0) {
                printf("Filename and path must be specified\n");
                continue;
            }

            FILE *fp = fopen(fname, "rb");
            if (!fp) {
                perror("File open failed");
                continue;
            }

            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            if (file_size > MAXCONTENT) {
                printf("File too large to upload\n");
                fclose(fp);
                continue;
            }

            size_t bytes_read = fread(content, 1, file_size, fp);
            fclose(fp);

            if (bytes_read != file_size) {
                printf("Failed to read complete file\n");
                continue;
            }

            char len_str[32];
            snprintf(len_str, sizeof(len_str), "%zu\n", bytes_read);
            if (send(sockfd, len_str, strlen(len_str), 0) < 0) {
                perror("Send length failed");
                continue;
            }

            if (send(sockfd, content, bytes_read, 0) < 0) {
                perror("Send content failed");
                continue;
            }

            // Receive response
            int n = recv(sockfd, buffer, MAXLINE - 1, 0);
            if (n <= 0) {
                if (n < 0) perror("Receive response failed");
                else printf("Server disconnected\n");
                break;
            }
            buffer[n] = '\0';
            printf("Server response: %s\n", buffer);
        }
        else if (strcmp(cmd, "dispfnames") == 0 || strcmp(cmd, "removef") == 0) {
            // Receive response
            int n = recv(sockfd, buffer, MAXCONTENT - 1, 0);
            if (n <= 0) {
                if (n < 0) perror("Receive response failed");
                else printf("Server disconnected\n");
                break;
            }
            buffer[n] = '\0';
            printf("Server response:\n%s\n", buffer);
        }
        else {
            // Receive response for unknown commands
            int n = recv(sockfd, buffer, MAXLINE - 1, 0);
            if (n <= 0) {
                if (n < 0) perror("Receive response failed");
                else printf("Server disconnected\n");
                break;
            }
            buffer[n] = '\0';
            printf("Server response: %s\n", buffer);
        }
    }

    close(sockfd);
    return 0;
}