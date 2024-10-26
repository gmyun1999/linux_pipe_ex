#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

void cleanup(void);

#define SERVER_FIFO "/tmp/server_fifo"
#define MAX_BUF 1024
#define MAX_FILENAME 256
#define ERR_FIFO_OPEN -1
#define ERR_FILE_OPEN -2
#define ERR_READ -3
#define ERR_WRITE -4

typedef struct {
    char filename[MAX_FILENAME];
    char mode;
    int bytes;
    char data[MAX_BUF];
    pid_t client_pid;
} Request;

typedef struct {
    int status;
    char data[MAX_BUF];
    int bytes;
} Response;

static volatile sig_atomic_t running = 1;

// 시그널 핸들러
void handle_signal(int sig) {
    running = 0;
}

// 자원 정리 함수 - >FIFO 파일 삭제
void cleanup(void) {
    unlink(SERVER_FIFO);
}

// 에러 처리 함수
void handle_error(const char* msg, int exit_code) {
    perror(msg);
    exit(exit_code);
}

// 좀비 프로세스 처리
void handle_zombie(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

// 파일 작업 처리 함수
int process_file_operation(const char* filename, char mode, char* data, int bytes, Response* resp) {
    int fd;
    ssize_t io_result;

    if (mode == 'r') {
        if ((fd = open(filename, O_RDONLY)) == -1) {
            if (errno == ENOENT) {
                printf("File '%s' does not exist.\n", filename);
            }
            return ERR_FILE_OPEN;
        }

        if ((io_result = read(fd, resp->data, bytes)) == -1) {
            close(fd);
            return ERR_READ;
        }
        resp->bytes = io_result;

    } else if (mode == 'w') {
        if ((fd = open(filename, O_WRONLY)) == -1) {
            if (errno == ENOENT) {
                printf("File '%s' does not exist and will not be created.\n", filename);
            }
            return ERR_FILE_OPEN;
        }

        if ((io_result = write(fd, data, bytes)) == -1) {
            close(fd);
            return ERR_WRITE;
        }
        resp->bytes = io_result;
    }

    close(fd);
    return 0;
}

void handle_client_request(Request *req) {
    char client_fifo[MAX_FILENAME];
    snprintf(client_fifo, sizeof(client_fifo), "/tmp/client_%d_fifo", req->client_pid);

    Response resp = {0};
    int client_fd;
    int result;

    if ((client_fd = open(client_fifo, O_WRONLY)) == -1) {
        printf("Failed to open client FIFO, client may have disconnected\n");
        return;
    }

    result = process_file_operation(req->filename, req->mode, req->data, req->bytes, &resp);
    if (result == ERR_FILE_OPEN) {
        printf("File '%s' does not exist, notifying client.\n", req->filename);
        resp.status = -1;
    } else {
        resp.status = (result == 0) ? 0 : -1;
    }

    if (write(client_fd, &resp, sizeof(Response)) != sizeof(Response)) {
        perror("Failed to send response to client");
    }
    close(client_fd);
}

int main(void) {
    struct sigaction signal_action;
    int server_fd;

    // 시그널 핸들러 설정
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = handle_signal;
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);

    // 좀비 프로세스 처리 핸들러
    signal_action.sa_handler = handle_zombie;
    sigaction(SIGCHLD, &signal_action, NULL);

    // 프로그램 종료 시 cleanup 등록
    atexit(cleanup);

    // 기존 FIFO 제거 및 새로 생성
    unlink(SERVER_FIFO);
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        handle_error("Failed to create server FIFO", 1);
    }
    printf("Server FIFO created.\n");

    while (running) {
        printf("Attempting to open server FIFO...\n");
        if ((server_fd = open(SERVER_FIFO, O_RDONLY)) == -1) {
            if (errno == EINTR) {
                printf("Interrupted by signal.\n");
                continue;
            }
            handle_error("Failed to open server FIFO", 1);
        }

        Request req;
        ssize_t bytes_read = read(server_fd, &req, sizeof(Request));
        
        if (bytes_read == -1) {
            if (errno == EINTR) {
                printf("Read interrupted by signal\n");
                close(server_fd);
                continue;
            }
            handle_error("Failed to read request", 1);
        }

        if (bytes_read == sizeof(Request)) {
            pid_t pid = fork();

            if (pid == -1) {
                handle_error("Failed to fork", 1);
            } else if (pid == 0) {
                // 자식 프로세스
                close(server_fd);
                handle_client_request(&req);
                _exit(0);   // 이거 exit 쓰면 atexit가 catch해서 cleanup 을 호출함. 따라서 _exit
            }
            // 부모 프로세스는 계속 진행
        }

        close(server_fd);
        printf("Server FIFO closed, waiting for next request...\n");
    }

    printf("Server shutting down...\n");
    return 0;
}
