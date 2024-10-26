#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

void cleanup(void);

#define SERVER_FIFO "/tmp/server_fifo"
#define MAX_BUF 1024
#define MAX_FILENAME 256
#define MAX_RETRIES 3

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

static char client_fifo[MAX_FILENAME];
static volatile sig_atomic_t running = 1;

// 시그널 핸들러
void handle_signal(int sig) {
    running = 0;
    printf("Signal received. Initiating cleanup...\n");
    cleanup();
}

// 자원 정리 함수
void cleanup(void) {
    if (unlink(client_fifo) == 0) {
        printf("Client FIFO '%s' successfully deleted.\n", client_fifo);
    } else {
        perror("Failed to delete client FIFO");
    }
}

// 입력 버퍼 정리
void clear_input_buffer(void) {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// 안전한 문자열 입력 함수
int safe_input(char *buf, size_t size) {
    if (fgets(buf, size, stdin) == NULL) {
        return -1;
    }
    buf[strcspn(buf, "\n")] = 0; // 개행 문자 제거
    return 0;
}

// 사용자 입력 받는 함수
int get_user_input(Request *req) {
    char input[MAX_BUF];

    // 파일명 입력
    printf("Enter filename: ");
    if (safe_input(input, sizeof(input)) < 0) {
        return -1;
    }
    if (strlen(input) == 0 || strlen(input) >= MAX_FILENAME) {
        printf("Invalid filename length\n");
        return -1;
    }
    strncpy(req->filename, input, MAX_FILENAME - 1);

    // 접근 모드 입력
    while (1) {
        printf("Enter access mode (r for read, w for write): ");
        if (safe_input(input, sizeof(input)) < 0) {
            return -1;
        }
        if (strlen(input) != 1 || (input[0] != 'r' && input[0] != 'w')) {
            printf("Invalid mode. Please enter 'r' or 'w'.\n");
            continue;
        }
        req->mode = input[0];
        break;
    }

    // 모드별 추가 정보
    if (req->mode == 'r') {
        while (1) {
            printf("Enter number of bytes to read (1-%d): ", MAX_BUF - 1);
            if (safe_input(input, sizeof(input)) < 0) {
                return -1;
            }
            req->bytes = atoi(input);
            if (req->bytes <= 0 || req->bytes >= MAX_BUF) {
                printf("Invalid number of bytes\n");
                continue;
            }
            break;
        }
    } else {
        printf("Enter data to write: ");
        if (safe_input(input, sizeof(input)) < 0) {
            return -1;
        }
        req->bytes = strlen(input);
        if (req->bytes >= MAX_BUF) {
            printf("Data too long\n");
            return -1;
        }
        strncpy(req->data, input, MAX_BUF - 1);
    }

    return 0;
}

int main(void) {
    struct sigaction signal_action;
    int retries;

    // 시그널 핸들러 설정
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = handle_signal;
    sigaction(SIGINT, &signal_action, NULL);
    sigaction(SIGTERM, &signal_action, NULL);

    // 클라이언트 FIFO 이름 설정
    snprintf(client_fifo, sizeof(client_fifo), "/tmp/client_%d_fifo", getpid());

    // cleanup 함수 등록
    atexit(cleanup);

    // 클라이언트 FIFO 생성
    unlink(client_fifo); // 기존 FIFO 파일 삭제
    if (mkfifo(client_fifo, 0666) == -1) {
        perror("Failed to create client FIFO");
        exit(1);
    }
    printf("Client FIFO '%s' created successfully.\n", client_fifo);

    printf("Client started (PID: %d)\n", getpid());

    while (running) {
        Request req;
        req.client_pid = getpid();

        printf("\n=== New File Operation ===\n");
        if (get_user_input(&req) < 0) {
            printf("Invalid input, try again\n");
            continue;
        }

        // 서버 연결 시도
        retries = 0;
        int server_fd;
        while (retries < MAX_RETRIES) {
    
            server_fd = open(SERVER_FIFO, O_WRONLY);
            if (server_fd != -1) {
                printf("Connected to server FIFO '%s'.\n", SERVER_FIFO);
                break;
            }

            if (errno != ENOENT) {
                perror("Cannot open server FIFO");
                exit(1);
            }

            printf("Server not available, retrying... (%d/%d)\n", retries + 1, MAX_RETRIES);
            sleep(1);
            retries++;
        }

        if (retries == MAX_RETRIES) {
            printf("Server not responding\n");
            continue;
        }

        // 요청 전송
        if (write(server_fd, &req, sizeof(Request)) != sizeof(Request)) {
            perror("Failed to send request");
            close(server_fd);
            printf("Server FIFO '%s' closed after failed write.\n", SERVER_FIFO);
            continue;
        }
        close(server_fd);
        printf("Request sent and server FIFO '%s' closed.\n", SERVER_FIFO);


        // 응답 대기
        int client_fd = open(client_fifo, O_RDONLY);
        if (client_fd == -1) {
            perror("Cannot open client FIFO");
            continue;
        }
        printf("Client FIFO '%s' opened for reading response.\n", client_fifo);

        Response resp;
        if (read(client_fd, &resp, sizeof(Response)) != sizeof(Response)) {
            perror("Failed to read response");
            close(client_fd);
            printf("Client FIFO '%s' closed after failed read.\n", client_fifo);
            continue;
        }
        close(client_fd);
        printf("Response received and client FIFO '%s' closed.\n", client_fifo);

        // 결과 출력
        printf("\nOperation Result:\n");
        if (resp.status == 0) {
            if (req.mode == 'r') {
                printf("Successfully read %d bytes:\n%.*s\n", resp.bytes, resp.bytes, resp.data);
            } else {
                printf("Successfully wrote %d bytes\n", resp.bytes);
            }
        } else {
            printf("Operation failed: File does not exist or cannot be accessed.\n");  // 파일 없음 알림
        }

        printf("\nDo you want to perform another operation? (y/n): ");
        char answer;
        scanf(" %c", &answer);
        clear_input_buffer();

        if (answer != 'y' && answer != 'Y') {
            break;
        }
    }

    printf("Client terminating...\n");
    return 0;
}
