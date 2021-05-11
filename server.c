#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<string.h>
#include<time.h>
#include<dirent.h>
#include<pthread.h>
#include<unistd.h>
#include<signal.h>
#include<sys/socket.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/times.h>
#include<netinet/in.h>
#include<netinet/tcp.h>
#include<arpa/inet.h>
#include<netdb.h>

// must be long enough to hold entire HTTP request and response headers.
#define BUFFER_LEN 4096
#define KEEP_ALIVE true

// on MacOS, TCP_NOPUSH stands in for TCP_CORK
#ifndef TCP_CORK
#define TCP_CORK TCP_NOPUSH
#endif

static int verbose = 2;

int make_socket(uint16_t port) {
    int sock;
    sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }
    int state = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &state, sizeof state) < 0) {
        perror("setsockopt");
    }
    struct sockaddr_in name = {
        .sin_family = AF_INET,
        .sin_addr = (struct in_addr) {
            .s_addr = htonl(INADDR_ANY),
        },
    };
    printf("port: %d\n", port);
    name.sin_port = htons(port);
    if (bind(sock, (struct sockaddr*) &name, sizeof name) < 0) {
        perror("bind");
        exit(1);
    }
    return sock;
}

static struct timespec time0;
static struct timespec time1;
void myprint(char *msg) {
    clock_gettime(CLOCK_REALTIME, &time1);
    long ns_elapsed = time1.tv_nsec - time0.tv_nsec;
    long us_elapsed = 1000000 * (time1.tv_sec - time0.tv_sec) + ns_elapsed/1000;
    printf("%ldms: %s\n", us_elapsed/1000, msg);
}

int send_fully(int fd, char* buf, int len) {
    char msg[50];
    int total = 0;
    while (total < len) {
        int nWrote = write(fd, buf+total, len);
        if (verbose >= 2) {
            sprintf(msg, "wrote %d", nWrote);
            myprint(msg);
        }
        if (nWrote < 0) {
            perror("write");
            return -1;
        }
        total += nWrote;
    }
    return 0;
}

// read fd into buffer until getting double-break indicating end of header
int recv_header_fully(int fd, char* buf) {
    int totalRead = 0;
    int nRead;
    while (1) {
        nRead = read(fd, buf+totalRead, BUFFER_LEN-totalRead);
        if (verbose >= 2) {
            printf("read %d\n", nRead);
        }
        if (nRead < 0) {
            perror("read");
            return -1;
        }
        totalRead += nRead;
        // if the socket is closed or we reach the end of the request, exit
        if (nRead == 0 || strstr(buf, "\r\n\r\n") != NULL) {
            break;
        }
        // only == should occur in below
        if (totalRead >= BUFFER_LEN) {
            printf("request exceeded buffer space\n");
            return -1;
        }
    }
    buf[totalRead] = '\0';
    return 0;
}

int send_http_response(int fd, char *buf, char *path) {
    if (verbose) {
        printf("send_http_response() with path=%s\n", path);
    }

    // PROCESS PATH
    char *printPath = path;
    FILE *fp;
    DIR *dir;
    if (strlen(path) < 1 || strlen(path) > 512 || path[0] != '/') {
        fprintf(stderr, "bad path in call to send_http_response()\n");
        return -1;
    }
    // add leading . for wd
    char newPath[500];
    strcpy(newPath, ".");
    strcat(newPath, path);
    path = newPath;
    // check if is a directory
    struct stat path_stat;
    stat(path, &path_stat);
    bool is_dir = S_ISDIR(path_stat.st_mode);
    bool path_exists = true;
    if (is_dir) {
        // append / to the path if missing from end
        if (path[strlen(path)-1] != '/') {
            strcat(path, "/");
            strcat(printPath, "/");
        }
        dir = opendir(path);
        if (dir == NULL) {
            if (verbose) {
                perror("opendir");
            }
            path_exists = false;
        } else {
            char pathIndex[500];
            strcpy(pathIndex, path);
            strcat(pathIndex, "index.html");
            fp = fopen(pathIndex, "r");
            if (fp != NULL) {
                is_dir = false;
                path = pathIndex;
                closedir(dir);
            }
        }
    } else {
        fp = fopen(path, "r");
        if (fp == NULL) {
            if (verbose) {
                perror("fopen");
            }
            path_exists = false;
        }
    }
    if (path_exists == false) {
        is_dir = false;
        fp = fopen("404.html", "r");
        if (fp == NULL) {
            perror("fopen");
            fprintf(stderr, "fatal: 404.html does not exists\n");
            return -1;
        }
    }
    if (verbose) {
        printf("got is_dir=%d; path_exists=%d; path=%s\n", is_dir, path_exists, path);
    }

    // IF DIRECTORY, GENERATE CONTENT
    char *dirBuf;
    if (is_dir) {
        dirBuf = malloc(BUFFER_LEN+1);
        sprintf(dirBuf, "");
        sprintf(dirBuf+strlen(dirBuf), "<DOCTYPE html><html>");
        sprintf(dirBuf+strlen(dirBuf), "<head><title>%s</title></head>", printPath);
        sprintf(dirBuf+strlen(dirBuf), "<body>");
        sprintf(dirBuf+strlen(dirBuf), "<h1>%s</h1>", printPath);
        sprintf(dirBuf+strlen(dirBuf), "<ul>");
        for (struct dirent *dent = readdir(dir); dent != NULL; dent = readdir(dir)) {
            // assume that filenames do not exceed 256 characters
            char item[300];
            sprintf(item, "<li><a href=\"%s%s\">%s</a></li>", path, dent->d_name, dent->d_name);
            // if come close to exceeding directory html buffer, just represent the rest with ...
            if (strlen(dirBuf) + strlen(item) > BUFFER_LEN - 100) {
                sprintf(item, "<li>...[omitted]</li>");
                strcat(dirBuf, item);
                break;
            } else {
                strcat(dirBuf, item);
            }
        }
        closedir(dir);
        sprintf(dirBuf+strlen(dirBuf), "</ul>");
        sprintf(dirBuf+strlen(dirBuf), "</body>");
        sprintf(dirBuf+strlen(dirBuf), "</html>");
    }

    // GENERATE AND SEND RESPONSE HEADER
    // start-line
    sprintf(buf, "");
    if (path_exists == true) {
        sprintf(buf+strlen(buf), "HTTP/1.0 200 OK\r\n");
    } else {
        sprintf(buf+strlen(buf), "HTTP/1.0 404 NOT FOUND\r\n");
    }

    // content-type header
    char *contentType;
    if (path_exists == true && is_dir == false) {
        char *ext = path + strlen(path);
        while (1) {
            if (*(ext-1) == '.') {
                break;
            }
            if (ext == path || *(ext-1) == '/') {
                ext = "";
                break;
            }
            ext--;
        }
        if (strcmp(ext, "txt") == 0) {
            contentType = "text/plain";
        } else if (strcmp(ext, "html") == 0) {
            contentType = "text/html";
        } else if (strcmp(ext, "jpg") == 0) {
            contentType = "image/jpeg";
        } else if (strcmp(ext, "gif") == 0) {
            contentType = "image/gif";
        } else if (strcmp(ext, "png") == 0) {
            contentType = "image/png";
        } else if (strcmp(ext, "ico") == 0) {
            contentType = "image/x-icon";
        } else if (strcmp(ext, "pdf") == 0) {
            contentType = "application/pdf";
        } else {
            contentType = "application/octet-stream";
        }
    } else {
        contentType = "text/html";
    }
    sprintf(buf+strlen(buf), "Content-Type: %s\r\n", contentType);

    // content-length header
    int contentLen;
    if (is_dir) {
        contentLen = strlen(dirBuf);
    } else {
        fseek(fp, 0L, SEEK_END);
        contentLen = ftell(fp);
        fseek(fp, 0L, SEEK_SET);
    }
    sprintf(buf+strlen(buf), "Content-Length: %d\r\n", contentLen);

    // keep connection alive to do multiple requests without another TCP handshake
    sprintf(buf+strlen(buf), "Connection: keep-alive\r\n");

    int state;
    // https://baus.net/on-tcp_cork/
    state = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &state, sizeof state) < 0) {
        perror("setsockopt");
    }

    // server header and cap off
    sprintf(buf+strlen(buf), "Server: mycserver/1.0\r\n");
    sprintf(buf+strlen(buf), "\r\n");
    // this should never happen, if it does memory may be corrupted
    if (strlen(buf) > BUFFER_LEN) {
        fprintf(stderr, "response too large for buffer\n");
        return -1;
    }
    printf("generated response: \n%s\n", buf);
    if (send_fully(fd, buf, strlen(buf)) < 0) {
        return -1;
    }
    if (verbose) {
        myprint("sent header\n");
    }

    // SEND CONTENT
    if (is_dir) {
        if (send_fully(fd, dirBuf, strlen(dirBuf)) < 0) {
            return -1;
        }
        free(dirBuf);
    } else {
        int nRead;
        while ((nRead = fread(buf, 1, BUFFER_LEN, fp)) != 0) {
            if (nRead < 0) {
                perror("fread");
                return -1;
            }
            if (send_fully(fd, buf, nRead) < 0) {
                return -1;
            }
        }
        fclose(fp);
    }
    state = 0;
    if (setsockopt(fd, IPPROTO_TCP, TCP_CORK, &state, sizeof state) < 0) {
        perror("setsockopt");
    }

    if (verbose) {
        myprint("sent content\n");
    }

    return 0;
}

void *handle_connection(void *fdstr) {
    int fd = atoi((char*) fdstr);

    myprint("entering handler");
    char *buf = malloc(BUFFER_LEN+1);

    while (1) {
        // READ REQUEST FROM CONNECTION
        // if the client closed the connection, then this will be the empty string.
        if (recv_header_fully(fd, buf) < 0 || strlen(buf) == 0) {
            break;
        }
        printf("\n%s\n", buf);

        // PROCESS REQUEST
        // assume that the path is after the first slash up to the next space
        char *res1, *res2;
        if ((res1 = strstr(buf, "/")) == NULL || (res2 = strstr(res1, " ")) == NULL) {
            fprintf(stderr, "error: bad request\n");
            continue;
        }
        int pathLen = res2 - res1;
        char path[500];
        strncpy(path, res1, pathLen);
        path[pathLen] = '\0';

        // MAKE AND SEND RESPONSE
        if (send_http_response(fd, buf, path) < 0) {
            break;
        }

        fflush(stdout);
    }

    myprint("exiting handler");
    free(buf);
    close(fd);

    pthread_exit(NULL);
}

void need_help() {
    char *help_str = "incorrect usage; ./lab2 PORT SERVE_DIR [--V0|V1|V2]\n";
    fprintf(stderr, "%s", help_str);
    exit(1);
}

int main(int argc, char* argv[]) {
    char *res;
    if (argc < 3 || argc > 4) {
        need_help();
    }
    // extract port from first arg
    uint16_t port = (uint16_t) atoi(argv[1]);
    if (port < 1024) {
        printf("illegal port: %d\n", port);
        exit(1);
    }
    // get verbose level
    if (argc == 3) {
        verbose = 1;
    } else {
        char *levelStr = argv[3];
        if (sscanf(levelStr, "--V%d", &verbose) != 1 || verbose < 0 || verbose > 2) {
            need_help();
        }
    }
    // change dir to serve root
    if (chdir(argv[2]) < 0) {
        perror("chdir");
        exit(1);
    }
    // salt rand() and get starting time
    srand((unsigned) time(NULL));
    clock_gettime(CLOCK_REALTIME, &time0);
    myprint("start");

    // ignore SIGPIPE for when client suddenly closes connection
    signal(SIGPIPE, SIG_IGN);
    //signal(SIGINT, fsfssfd);

    int sock = make_socket(port);
    myprint("socket()");

    // ENTER ACCEPT LOOP
    if (listen(sock, 5) < 0) {
        perror("listen");
        exit(1);
    }
    myprint("listen()");

    struct sockaddr_in addr;
    socklen_t addrlen;
    int conn_sock;
    while (1) {
        if ((conn_sock = accept(sock, (struct sockaddr*) &addr, &addrlen)) < 0) {
            perror("accept");
            exit(1);
        }
        char pstr[50];
        sprintf(pstr, "accept() on ip=%s", inet_ntoa(addr.sin_addr));
        myprint(pstr);

        char fdstr[50];
        sprintf(fdstr, "%d", conn_sock);
        pthread_t thread0;
        pthread_create(&thread0, NULL, handle_connection, (void*) fdstr);
    }

    close(sock);
    myprint("close()");

    return 0;
}

