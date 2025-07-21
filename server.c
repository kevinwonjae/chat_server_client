// server.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>

// 서버 설정 상수
#define MAX_CLIENTS 20
#define MAX_CHATROOMS 50
#define MAX_ROOM_USERS 10

// 버퍼크기 상수
#define SMALL_BUFF_SIZE 64
#define MEDIUM_BUFF_SIZE 128
#define MEDIUM_LARGE_BUFF_SIZE 512
#define LARGE_BUFF_SIZE 1024

// 모드 관련 상수
#define POLLSIZE 100
#define CHAT_MODE 0
#define GAME_MODE 1
#define POLL_MODE 2
#define MAX_POLL 10

// 클라이언트 상태 정의
typedef enum
{
    STATE_LOBBY,
    STATE_IN_CHATROOM
} ClientState;

// 클라이언트 정보 구조체
typedef struct
{
    int fd;
    char user_name[SMALL_BUFF_SIZE];
    ClientState state;
    int room_id;
} ClientInfo;

// 채팅방 정보 구조체
typedef struct
{
    int id;
    char title[MEDIUM_BUFF_SIZE];
    int user_fds[MAX_ROOM_USERS];
    char *user_names[MAX_ROOM_USERS];
    int user_count;
    pthread_mutex_t lock;
    
    // 숫자 야구 게임 관련
    int mode;         // CHAT_MODE or GAME_MODE
    int game_host_fd; // 게임을 시작한 유저의 fd
    char game_host_name[SMALL_BUFF_SIZE]; // 게임을 시작한 유저의 이름 
    char game_answer[4]; // 정답 (3자리 숫자, 문자열 형태)

    // 투표 시스템 관련
    int poll_mode_stage;               // 0: 항목 수 입력 중, 1: 항목 이름 입력 중, 2: 투표 중
    int poll_count;                    // 항목 개수
    int poll_index;                    // 현재 몇 번째 항목 입력 중
    char *poll_list[MAX_POLL];         // 항목 저장
    int poll_votes[MAX_POLL];          // 득표수
    int vote_received[MAX_ROOM_USERS]; // 사용자별 투표 여부
} ChatRoom;


// 전역 변수 선언

ChatRoom chatrooms[MAX_CHATROOMS];
ClientInfo clients[MAX_CLIENTS];

int room_count = 0;
int client_count = 0;
int server_sock;

pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

// 서버 소켓을 설정하고 바인딩하는 함수
void init_server(char port[]);

// 기본 채팅방 3개를 초기화하고 각각의 스레드를 생성하는 함수
void default_rooms();

// 로비 상태의 클라이언트를 처리하는 메인 루프 스레드 함수
void *main_loop();

// 채팅방 내 사용자 메시지를 처리하는 스레드 함수
void *chatroom_thread(void *arg);

// 로비에 있는 클라이언트 목록을 로그 출력
void print_log_lobby();

// 특정 채팅방의 유저 목록을 로그 출력
void print_log_room(ChatRoom *room);

// 게임 모드 상태를 로그 출력
void print_log_game(ChatRoom *room);

// 투표 모드 상태를 로그 출력
void print_log_poll(ChatRoom *room);

// 현재 서버 접속자 수 출력
void server_state();

// 현재 시간을 출력하는 함수
void print_time();

// 클라이언트에게 메인 메뉴 전송
void send_menu(int client_fd);

// 클라이언트에게 채팅방 목록 전송
void send_room_list(int client_fd);

// 채팅방 정보를 클라이언트에게 전송
void send_chatroom_info(ChatRoom *room, int idx);

// 파일 디스크립터에 해당하는 클라이언트 인덱스 반환
int find_client_index(int fd);

// 채팅방에서 주어진 fd의 사용자 인덱스 반환
int get_user_index(ChatRoom *room, int fd);

// 클라이언트 배열에서 클라이언트를 제거
void remove_client(int index);

// 채팅방에서 사용자를 제거
void remove_user(ChatRoom *room, int index);

// 문자열 양 끝 공백 제거
char *trim(char *str);

// 문자열을 정수로 안전하게 파싱
bool parse_valid_int(const char *input, int *result);

// 세 자리 숫자인지 유효성 검사
int is_valid_number(const char *num);

// 숫자 야구 결과 계산 (스트라이크, 볼)
void evaluate_guess(const char *guess, const char *answer, int *strikes, int *balls);

// 채팅방 전체 사용자에게 메시지를 전송 (예외 fd 제외 가능)
void broadcast_to_room(ChatRoom *room, const char *msg, int except_fd);

// 투표를 시작하며 초기 상태 설정
void start_poll(ChatRoom *room, int host_fd, const char *host_name);

// 투표 상태 초기화
void reset_poll_state(ChatRoom *room);

// SIGINT 수신 시 서버 종료 및 자원 해제 처리
void sigint_handler(int signo);

// 메인 함수
int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf(" Usage : %s <port>\n", argv[0]);
        exit(1);
    }

    signal(SIGINT, sigint_handler);
    default_rooms();
    init_server(argv[1]);

    main_loop(NULL);

    return EXIT_SUCCESS;
}


void init_server(char port[])
{
    struct sockaddr_in serv_addr;

    // 서버 소켓 생성
    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    // 주소 정보 설정
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(atoi(port));

    if (server_sock < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }
 
    // 소켓에 주소 바인딩
    if (bind(server_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("bind");
        close(server_sock);
        exit(EXIT_FAILURE);
    }
    
    // 클라이언트 연결 대기 시작
    if (listen(server_sock, 10) < 0)
    {
        perror("listen");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    // 서버 초기 정보 출력
    system("clear");
    printf("<<<< Chat server >>>>\n");
    printf("Server Port : %s\n", port);
    printf("Max Client : %d\n", MAX_CLIENTS);
    printf(" <<<<          Log         >>>>\n\n");
}


void default_rooms()
{
    for (int i = 0; i < 3; i++)
    {
        // 채팅방 락 초기화
        pthread_mutex_init(&chatrooms[i].lock, NULL);

        // 채팅방 관리 스레드 생성
        pthread_mutex_lock(&chatrooms[i].lock);
        chatrooms[i].id = i;
        snprintf(chatrooms[i].title, sizeof(chatrooms[i].title), "Chatroom-%d", i);
        chatrooms[i].user_count = 0;
        chatrooms[i].mode = CHAT_MODE;
        chatrooms[i].game_host_fd = -1;
        memset(chatrooms[i].game_answer, 0, sizeof(chatrooms[i].game_answer));
        pthread_mutex_unlock(&chatrooms[i].lock);

        pthread_t tid;
        if (pthread_create(&tid, NULL, chatroom_thread, (void *)&chatrooms[i]) != 0)
        {
            perror("pthread_create");
            pthread_mutex_lock(&chatrooms[i].lock);
            chatrooms[i].title[0] = '\0';
            chatrooms[i].user_count = 0;
            chatrooms[i].mode = CHAT_MODE;
            chatrooms[i].game_host_fd = -1;
            pthread_mutex_unlock(&chatrooms[i].lock);
            continue;
        }
        pthread_detach(tid); // 쓰레드 리소스 자동 반환
        room_count++;
    }
}

void *main_loop()
{
    fd_set read_fds;
    char buffer[MEDIUM_BUFF_SIZE];

    while (1)
    {
        FD_ZERO(&read_fds);
        FD_SET(server_sock, &read_fds);
        int max_fd = server_sock;
        
        // 현재 접속한 클라이언트들의 소켓을 select 대상으로 등록
        pthread_mutex_lock(&client_lock);
        for (int i = 0; i < client_count; i++)
        {
            FD_SET(clients[i].fd, &read_fds);
            if (clients[i].fd > max_fd)
            {
                max_fd = clients[i].fd;
            }
        }
        pthread_mutex_unlock(&client_lock);

        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0)
        {
            perror("select");
            continue;
        }

        // 신규 클라이언트 접속 처리
        if (FD_ISSET(server_sock, &read_fds))
        {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cli_fd = accept(server_sock, (struct sockaddr *)&cli_addr, &cli_len);
            if (cli_fd >= 0)
            {
                memset(buffer, 0, sizeof(buffer));
                int n = recv(cli_fd, buffer, sizeof(buffer) - 1, 0);
                if (n < 0)
                {
                    perror("recv");
                    continue;
                }
                if (n == 0)
                {
                    print_log_lobby();
                    printf("연결 종료됨 (%d)", cli_fd);
                    print_time();
                    close(cli_fd);
                    server_state();
                    print_time();
                    continue;
                }

                buffer[strcspn(buffer, "\r\n")] = 0;
                char *name = trim(buffer);

                pthread_mutex_lock(&client_lock);
                if (client_count < MAX_CLIENTS)
                {
                    int idx = client_count;
                    clients[idx].fd = cli_fd;
                    if (strlen(name) == 0)
                        snprintf(clients[idx].user_name, sizeof(clients[idx].user_name), "User%d", idx + 1);
                    else
                        snprintf(clients[idx].user_name, sizeof(clients[idx].user_name), "%s", name);
                    clients[idx].state = STATE_LOBBY;
                    clients[idx].room_id = -1;
                    client_count++;

                    print_log_lobby();
                    printf("새로운 사용자 %s 접속 - Connceted client IP : %s ", clients[idx].user_name, inet_ntoa(cli_addr.sin_addr));
                    print_time();
                    server_state();
                    print_time();

                    send_menu(cli_fd);
                }
                else
                {
                    const char *msg = "서버에 인원이 가득 찼습니다.\n";
                    send(cli_fd, msg, strlen(msg), 0);
                    close(cli_fd);
                }

                pthread_mutex_unlock(&client_lock);
            }
        }

        // 클라이언트 명령 처리
        pthread_mutex_lock(&client_lock);
        for (int i = 0; i < client_count; i++)
        {
            int fd = clients[i].fd;
            char *user_name = clients[i].user_name;
            if (FD_ISSET(fd, &read_fds))
            {
                // 로비 상태 클라이언트만 처리
                if (clients[i].state == STATE_LOBBY)
                {
                    memset(buffer, 0, sizeof(buffer));
                    int n = recv(fd, buffer, sizeof(buffer) - 1, 0);
                    if (n < 0)
                    {
                        perror("recv");
                        continue;
                    }
                    if (n == 0)
                    {
                        print_log_lobby();
                        printf("사용자 %s - 접속이 끊어졌습니다.", user_name);
                        remove_client(i);
                        i--;
                        server_state();
                        print_time();
                        continue;
                    }

                    buffer[strcspn(buffer, "\r\n")] = 0;
                    const char *menu = trim(buffer);

                    if (strlen(menu) == 0)
                    {
                        const char *msg = " 메뉴를 비워둘 수 없습니다.\n";
                        send(fd, msg, strlen(msg), 0);
                        send_menu(fd);
                        continue;
                    }



                    if (strcmp(menu, "0") == 0)
                    { // 메뉴 재전송
                        send_menu(fd);
                    }
                    else if (strcmp(menu, "1") == 0)
                    { // 사용자 이름 변경 처리
                        print_log_lobby();
                        printf("사용자 %s - 메뉴1 선택", user_name);
                        print_time();

                        int done = 0;

                        while (!done)
                        {
                            const char *msg = "새로운 이름을 입력하세요.\n";
                            send(fd, msg, strlen(msg), 0);

                            memset(buffer, 0, sizeof(buffer));
                            int n = recv(fd, buffer, sizeof(buffer) - 1, 0);
                            if (n <= 0)
                            {
                                print_log_lobby();
                                printf("사용자 %s - 접속이 끊어졌습니다.", user_name);
                                print_time();
                                remove_client(i);
                                i--;
                                server_state();
                                print_time();
                                continue;
                            }

                            buffer[strcspn(buffer, "\r\n")] = 0;
                            char *name = trim(buffer);

                            if (strlen(name) == 0)
                            {
                                const char *msg = "이름은 비워둘 수 없습니다. 다시 입력해주세요.\n";
                                send(fd, msg, strlen(msg), 0);
                                continue;
                            }

                            // 이름 저장
                            snprintf(clients[i].user_name, sizeof(clients[i].user_name), "%.31s", name);
                            send(fd, "이름이 성공적으로 변경되었습니다.\n", strlen("이름이 성공적으로 변경되었습니다."), 0);
                            print_log_lobby();
                            printf("사용자 %.31s로 변경", user_name);
                            print_time();
                            done = 1;
                        }

                        send_menu(fd);
                    }
                    else if (strcmp(menu, "2") == 0)
                    { // 채팅방 입장 처리

                        print_log_lobby();
                        printf("사용자 %s - 메뉴2 선택", user_name);
                        print_time();

                        if (clients[i].state != STATE_LOBBY)
                        {
                            const char *msg = "<WARN!> 현재 상태에서는 채팅방에 입장할 수 없습니다.\n";
                            send(fd, msg, strlen(msg), 0);
                            send_menu(fd);
                            continue;
                        }
                        int done = 0;
                        while (!done)
                        {
                            send_room_list(fd);

                            memset(buffer, 0, sizeof(buffer));
                            int n = recv(fd, buffer, sizeof(buffer) - 1, 0);

                            if (n <= 0)
                            {
                                print_log_lobby();
                                printf("사용자 %s - 접속이 끊어졌습니다.", user_name);
                                print_time();
                                remove_client(i);
                                i--;
                                server_state();
                                print_time();
                                continue;
                            }

                            buffer[strcspn(buffer, "\r\n")] = 0;
                            char *rnum = trim(buffer);

                            if (strlen(rnum) == 0)
                            {
                                const char *msg = "입장할 채팅방 번호를 입력하세요.\n";
                                send(fd, msg, strlen(msg), 0);
                                continue;
                            }

                            if (strcasecmp(rnum, "b") == 0)
                            {
                                send_menu(fd);
                                print_log_lobby();
                                done = 1;
                                continue;
                            }

                            int room_id;

                            if (!parse_valid_int(rnum, &room_id))
                            {
                                const char *msg = "유효한 숫자를 입력해주세요.\n";
                                send(fd, msg, strlen(msg), 0);
                                continue;
                            }

                            if (room_id >= 0 && room_id < room_count)
                            {
                                pthread_mutex_lock(&chatrooms[room_id].lock);
                                if (chatrooms[room_id].user_count < MAX_ROOM_USERS)
                                {
                                    chatrooms[room_id].user_fds[chatrooms[room_id].user_count++] = fd;
                                    clients[i].state = STATE_IN_CHATROOM;
                                    clients[i].room_id = room_id;
                                    pthread_mutex_unlock(&chatrooms[room_id].lock);

                                    print_log_lobby();
                                    printf("사용자 %s - 채팅방 %d에 참여합니다.", user_name, room_id);
                                    print_time();

                                    char msg[MEDIUM_LARGE_BUFF_SIZE];
                                    snprintf(msg, sizeof(msg), "채팅방 %s (%d)에 입장했습니다.\n", chatrooms[room_id].title, room_id);
                                    send(fd, msg, strlen(msg), 0);

                                    done = 1;
                                }
                                else
                                {
                                    pthread_mutex_unlock(&chatrooms[room_id].lock);
                                    const char *msg = "해당 채팅방은 인원이 가득 찼습니다.\n";
                                    send(fd, msg, strlen(msg), 0);
                                }
                            }
                            else
                            {
                                const char *msg = "존재하지 않는 채팅방입니다.\n";
                                send(fd, msg, strlen(msg), 0);
                            }
                        }
                    }

                    else if (strcmp(menu, "3") == 0)
                    { // 채팅방 개설 처리
                        print_log_lobby();
                        printf("사용자 %s - 메뉴3 선택", user_name);
                        print_time();

                        int done = 0;
                        char *cname;
                        while (!done)
                        {
                            if (room_count >= MAX_CHATROOMS)
                            {
                                const char *msg = "더 이상 채팅방을 개설할 수 없습니다.\n";
                                send(fd, msg, strlen(msg), 0);
                                send_menu(fd);
                                done = 1;
                            }

                            const char *msg = "개설할 채팅방 이름을 입력하세요.\n";
                            send(fd, msg, strlen(msg), 0);
                            memset(buffer, 0, sizeof(buffer));

                            int n = recv(fd, buffer, sizeof(buffer) - 1, 0);
                            if (n <= 0)
                            {
                                print_log_lobby();
                                printf("사용자 %s - 접속이 끊어졌습니다.", user_name);
                                print_time();
                                remove_client(i);
                                i--;
                                server_state();
                                print_time();
                                continue;
                            }

                            buffer[strcspn(buffer, "\r\n")] = 0;
                            cname = trim(buffer);

                            if (strlen(cname) == 0)
                            {
                                const char *msg = "채팅방 이름은 비워둘 수 없습니다.\n";
                                send(fd, msg, strlen(msg), 0);
                                send_menu(fd);
                                continue;
                            }
                            done = 1;
                        }
                       
                        for (int j = 0; j < MAX_CHATROOMS; j++)
                        {
                            if (chatrooms[j].title[0] == '\0')
                            {
                                pthread_mutex_lock(&chatrooms[j].lock);
                                chatrooms[j].id = j;
                                snprintf(chatrooms[j].title, MEDIUM_BUFF_SIZE, "%.31s", cname);
                                chatrooms[j].user_count = 0;
                                chatrooms[j].mode = CHAT_MODE;
                                chatrooms[j].game_host_fd = -1;
                                memset(chatrooms[j].game_answer, 0, sizeof(chatrooms[j].game_answer));
                                pthread_mutex_unlock(&chatrooms[j].lock);

                                pthread_t tid;
                                if (pthread_create(&tid, NULL, chatroom_thread, (void *)&chatrooms[j]) != 0)
                                {
                                    perror("pthread_create");
                                    pthread_mutex_lock(&chatrooms[j].lock);
                                    chatrooms[j].title[0] = '\0'; // 방 비활성화 표시
                                    chatrooms[j].user_count = 0;
                                    chatrooms[j].mode = CHAT_MODE;
                                    chatrooms[j].game_host_fd = -1;
                                    pthread_mutex_unlock(&chatrooms[j].lock);
                                    continue;
                                }
                                pthread_detach(tid);

                                char msg[MEDIUM_BUFF_SIZE];
                                snprintf(msg, sizeof(msg), "채팅방 %.31s이 개설되었습니다.", cname);

                                send(fd, msg, strlen(msg), 0);
                                print_log_lobby();
                                printf("사용자 %s - 채팅방 %.31s 개설", user_name, cname);
                                print_time();
                                room_count++;
                                break;
                            }
                        }

                        send_menu(fd);
                    }
                    else if (strcmp(menu, "4") == 0)
                    { // 접속 종료

                        print_log_lobby();
                        printf("사용자 %s - 메뉴4 선택", user_name);
                        print_time();

                        print_log_lobby();
                        printf("사용자 %s - 접속을 헤제합니다.", user_name);
                        print_time();
                        remove_client(i);
                        i--;
                        server_state();
                        print_time();
                    }
                    else
                    {
                        const char *msg = "잘못된 명령입니다.\n";
                        send(fd, msg, strlen(msg), 0);
                    }
                }
                else
                { // 예외 상황: 채팅방 클라이언트가 로비 루프로 메시지 전송
                    printf("<Warn!> 채팅방 클라이언트가 main_loop로 메시지 보냄 : %d", fd);
                    print_time();
                }
            }
        }
        pthread_mutex_unlock(&client_lock);
    }
    return NULL;
}


// 채팅방의 개별 스레드 함수
// 각 채팅방마다 독립적으로 클라이언트 메시지를 받고 처리함
void *chatroom_thread(void *arg)
{
    ChatRoom *room = (ChatRoom *)arg;         // 인자로 받은 채팅방 정보
    fd_set read_fds;                          // select()용 파일 디스크립터 집합
    char buffer[MEDIUM_BUFF_SIZE];            // 메시지 수신 버퍼

    while (1)
    {
        FD_ZERO(&read_fds);
        int max_fd = -1;

        // 채팅방 사용자들의 소켓을 read_fds에 추가
        pthread_mutex_lock(&room->lock);
        for (int i = 0; i < room->user_count; i++)
        {
            FD_SET(room->user_fds[i], &read_fds);  // select 대상에 추가
            int client_idx = find_client_index(room->user_fds[i]);

            // 클라이언트 인덱스를 통해 사용자 이름 갱신
            if (client_idx != -1)
                room->user_names[i] = clients[client_idx].user_name;
            else
                room->user_names[i] = "Unknown";

            // select를 위한 최대 fd 추적
            if (room->user_fds[i] > max_fd)
                max_fd = room->user_fds[i];
        }
        pthread_mutex_unlock(&room->lock);

        // 유효한 fd가 없으면 잠깐 대기 후 반복
        if (max_fd < 0)
        {
            sleep(1);
            continue;
        }

        // select로 사용자 입력 대기 (논블로킹)
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (activity < 0)
        {
            perror("select");
            continue;
        }

        // 사용자 입력 처리
        pthread_mutex_lock(&room->lock);
        for (int i = 0; i < room->user_count; i++)
        {
            int user_fd = room->user_fds[i];

            // 해당 클라이언트가 데이터를 보냈다면
            if (FD_ISSET(user_fd, &read_fds))
            {
                memset(buffer, 0, sizeof(buffer));
                int n = recv(user_fd, buffer, sizeof(buffer) - 1, 0);

                // recv 에러
                if (n < 0)
                {
                    perror("recv");
                    continue;
                }

                // 클라이언트 연결 종료 (EOF)
                if (n == 0)
                {
                    print_log_room(room);
                    printf("%s 연결 종료", room->user_names[i]);
                    print_time();

                    // 나머지 사용자에게 알림 메시지 전송
                    for (int j = 0; j < room->user_count; j++)
                    {
                        if (room->user_fds[j] != user_fd)
                        {
                            char msg[SMALL_BUFF_SIZE];
                            snprintf(msg, sizeof(msg), "[NOTICE] 사용자 %s님이 채팅방을 나갔습니다.\n", room->user_names[i]);
                            send(room->user_fds[j], msg, strlen(msg), 0);
                        }
                    }

                    // 클라이언트 소켓 종료 및 제거
                    close(user_fd);
                    remove_user(room, i);
                    i--;

                    server_state();
                    print_time();
                }
                else
                {
                    // 줄바꿈 제거
                    buffer[strcspn(buffer, "\r\n")] = 0;

                    // 로그 출력
                    print_log_room(room);
                    printf("%s의 메시지 : %s", room->user_names[i], buffer);
                    print_time();

                    // "quit" 명령어 처리: 채팅방 나가기
                    if (strcmp(buffer, "quit") == 0)
                    {
                        print_log_room(room);
                        printf("%s가 채팅방에서 나감", room->user_names[i]);
                        print_time();

                        // 다른 사용자에게 알림 전송
                        for (int j = 0; j < room->user_count; j++)
                        {
                            if (room->user_fds[j] != user_fd)
                            {
                                char msg[SMALL_BUFF_SIZE];
                                snprintf(msg, sizeof(msg), "[NOTICE] %s님이 채팅방에서 나갔습니다.\n", room->user_names[i]);
                                send(room->user_fds[j], msg, strlen(msg), 0);
                            }
                        }

                        remove_user(room, i);
                        i--;

                        // 해당 사용자에게 메뉴 전송 (로비로 돌아감)
                        send_menu(user_fd);
                        continue;
                    }

                    // "info" 명령어 처리: 채팅방 정보 제공
                    if (strcmp(buffer, "info") == 0)
                    {
                        send_chatroom_info(room, i);
                        print_log_room(room);
                        printf("%s 채팅방 정보 조회.", room->user_names[i]);
                        print_time();
                        continue;
                    }

                    // "game" 명령어 처리: 숫자 야구 게임 시작 요청
                    if (strcmp(buffer, "game") == 0 && room->mode == CHAT_MODE)
                    {
                        room->mode = GAME_MODE;
                        room->game_host_fd = user_fd;
                        strncpy(room->game_host_name, room->user_names[i], SMALL_BUFF_SIZE - 1);
                        room->game_host_name[SMALL_BUFF_SIZE - 1] = '\0';
                        memset(room->game_answer, 0, sizeof(room->game_answer));
                        const char *msg = "[GAME] 호스트는 3자리 숫자를 입력하세요 (중복 없음):\n";
                        send(user_fd, msg, strlen(msg), 0);
                        print_log_game(room);
                        printf("숫자 야구 게임 호스트: %s", room->game_host_name);
                        print_time();
                        continue;
                    }

                    // 숫자 야구 게임 로직
                    if (room->mode == GAME_MODE)
                    {
                        // 호스트가 정답 입력 전
                        if (user_fd == room->game_host_fd && strlen(room->game_answer) == 0)
                        {
                            buffer[strcspn(buffer, "\r\n")] = '\0';
                            if (is_valid_number(buffer))
                            {
                                strncpy(room->game_answer, buffer, 3);
                                room->game_answer[3] = '\0';

                                print_log_game(room);
                                printf("숫자 야구 정답: %s", room->game_answer);
                                print_time();

                                char msg[MEDIUM_LARGE_BUFF_SIZE];
                                snprintf(msg, sizeof(msg), "====== 숫자 야구 게임이 시작되었습니다! ======\n===== HOST : %s =====\n", room->game_host_name);
                                broadcast_to_room(room, msg, -1);
                            }
                            else
                            {
                                const char *msg = "[GAME] 유효하지 않은 숫자입니다. 다시 입력하세요.\n";
                                send(user_fd, msg, strlen(msg), 0);
                            }
                        }
                        // 참가자가 추측 입력
                        else if (user_fd != room->game_host_fd && strlen(room->game_answer) == 3)
                        {
                            if (!is_valid_number(buffer))
                            {
                                const char *msg = "[GAME] 3자리 숫자를 입력하세요. (중복 없음)\n";
                                send(user_fd, msg, strlen(msg), 0);
                                continue;
                            }

                            int s, b;
                            evaluate_guess(buffer, room->game_answer, &s, &b);

                            char msg[MEDIUM_LARGE_BUFF_SIZE];
                            snprintf(msg, sizeof(msg), "[%s] %s의 결과: %d 스트라이크, %d 볼\n", room->user_names[i], buffer, s, b);
                            broadcast_to_room(room, msg, -1);

                            print_log_game(room);
                            printf("%s -> %s의 결과: %d 스트라이크, %d 볼", room->user_names[i], buffer, s, b);
                            print_time();

                            if (s == 3)
                            {
                                snprintf(msg, sizeof(msg), "[GAME] %s님이 정답을 맞췄습니다! 게임 종료.\n", room->user_names[i]);
                                broadcast_to_room(room, msg, -1);
                                room->mode = CHAT_MODE;
                                memset(room->game_answer, 0, sizeof(room->game_answer));
                                print_log_game(room);
                                printf("%s님 정답 게임 종료.", room->user_names[i]);
                                print_time();
                            }
                        }
                        continue;
                    }

                    // "poll" 명령어 처리: 투표 시작 요청
                    if (strcmp(buffer, "poll") == 0 && room->mode == CHAT_MODE)
                    {
                        print_log_poll(room);
                        printf("사용자 %s - 투표 시작 요청", room->user_names[i]);
                        print_time();
                        start_poll(room, user_fd, room->user_names[i]);

                        const char *msg = "[POLL] 호스트는 항목개수를 입력하세요 (1 ~ 10)\n";
                        send(user_fd, msg, strlen(msg), 0);
                        continue;
                    }

                    // 투표 진행 중 처리
                    if (room->mode == POLL_MODE)
                    {
                        // 1단계: 항목 개수 입력
                        if (user_fd == room->game_host_fd && room->poll_mode_stage == 0)
                        {
                            buffer[strcspn(buffer, "\r\n")] = 0;
                            int poll_count;
                            if (parse_valid_int(buffer, &poll_count) && poll_count > 0 && poll_count <= MAX_POLL)
                            {
                                room->poll_count = poll_count;
                                room->poll_index = 0;
                                room->poll_mode_stage = 1;

                                const char *msg = "[POLL] 항목 1을 입력하세요.\n";
                                send(user_fd, msg, strlen(msg), 0);
                            }
                            else
                            {
                                const char *msg = "[POLL] 유효한 숫자를 입력하세요 (1~10)\n";
                                send(user_fd, msg, strlen(msg), 0);
                            }
                            continue;
                        }

                        // 2단계: 항목 내용 입력
                        if (user_fd == room->game_host_fd && room->poll_mode_stage == 1)
                        {
                            buffer[strcspn(buffer, "\r\n")] = 0;
                            room->poll_list[room->poll_index] = malloc(strlen(buffer) + 1);
                            strcpy(room->poll_list[room->poll_index], buffer);
                            room->poll_votes[room->poll_index] = 0;
                            room->poll_index++;

                            if (room->poll_index < room->poll_count)
                            {
                                char msg[SMALL_BUFF_SIZE];
                                snprintf(msg, sizeof(msg), "[POLL] 항목 %d을 입력하세요\n", room->poll_index + 1);
                                send(user_fd, msg, strlen(msg), 0);
                            }
                            else
                            {
                                room->poll_mode_stage = 2;

                                print_log_poll(room);
                                printf("투표 시작");
                                print_time();

                                // 항목 목록 전체 사용자에게 전송
                                char list[LARGE_BUFF_SIZE] = "===== [POLL_LIST] =====\n";
                                for (int i = 0; i < room->poll_count; i++)
                                {
                                    char line[MEDIUM_BUFF_SIZE];
                                    snprintf(line, sizeof(line), "%d. %s\n", i + 1, room->poll_list[i]);
                                    if (strlen(list) + strlen(line) < sizeof(list))
                                    {
                                        strcat(list, line);
                                    }
                                }

                                for (int i = 0; i < room->user_count; i++)
                                {
                                    send(room->user_fds[i], list, strlen(list), 0);
                                    room->vote_received[i] = -1;
                                }
                            }
                            continue;
                        }

                        // 3단계: 사용자 투표 입력
                        if (room->poll_mode_stage == 2)
                        {
                            int user_idx = get_user_index(room, user_fd);
                            if (user_idx == -1 || room->vote_received[user_idx] != -1)
                                continue;

                            int vote = atoi(buffer) - 1;
                            if (vote < 0 || vote >= room->poll_count)
                            {
                                const char *msg = "[POLL] 올바른 번호를 입력하세요\n";
                                send(user_fd, msg, strlen(msg), 0);
                                continue;
                            }

                            room->poll_votes[vote]++;
                            room->vote_received[user_idx] = vote;

                            const char *msg = "선택 완료!\n";
                            send(user_fd, msg, strlen(msg), 0);

                            // 모든 사용자 투표 완료 확인
                            int all_voted = 1;
                            for (int i = 0; i < room->user_count; i++)
                            {
                                if (room->vote_received[i] == -1)
                                {
                                    all_voted = 0;
                                    break;
                                }
                            }

                            if (all_voted)
                            {
                                char list[LARGE_BUFF_SIZE] = "====== [POLL_RESULT] ======\n";
                                for (int i = 0; i < room->poll_count; i++)
                                {
                                    char line[MEDIUM_BUFF_SIZE];
                                    snprintf(line, sizeof(line), "%s : %d 표\n", room->poll_list[i], room->poll_votes[i]);
                                    if (strlen(list) + strlen(line) < sizeof(list))
                                    {
                                        strcat(list, line);
                                    }
                                }
                                broadcast_to_room(room, list, -1);
                                print_log_poll(room);
                                printf("모든 사용자가 투표를 완료했습니다. 투표 종료");
                                print_time();

                                room->mode = CHAT_MODE;
                                reset_poll_state(room);
                            }
                            continue;
                        }
                    }

                    // 일반 메시지 전송 처리
                    if (room->user_count == 1)
                    {
                        // 혼자 있을 경우 알림
                        const char *msg = "[NOTICE] 현재 채팅방에 혼자 있습니다.\n";
                        send(user_fd, msg, strlen(msg), 0);
                        print_log_room(room);
                        printf("사용자 %s - 혼자여서 메시지를 전달 안 합니다.", room->user_names[i]);
                        print_time();
                        continue;
                    }
                    else if (room->user_count > 1)
                    {
                        // 다수 사용자에게 브로드캐스트
                        for (int j = 0; j < room->user_count; j++)
                        {
                            int target_fd = room->user_fds[j];
                            char msg[LARGE_BUFF_SIZE];

                            if (target_fd == user_fd)
                                snprintf(msg, sizeof(msg), "[ME] %s\n", buffer);
                            else
                                snprintf(msg, sizeof(msg), "[%s] %s\n", room->user_names[i], buffer);

                            send(target_fd, msg, strlen(msg), 0);
                        }
                    }
                    else
                    {
                        // 발생하면 안 되는 비정상 상태
                        print_log_room(room);
                        printf("<WARN!> 비정상 상태...\n");
                        print_time();
                        room->user_count = 0;
                        continue;
                    }
                }
            }
        }
        pthread_mutex_unlock(&room->lock);
    }
    return NULL;
}

// 로그 출력 함수
void print_log_lobby()
{
    printf("[LOBBY] ");
    fflush(stdout);
}

void print_log_room(ChatRoom *room)
{
    printf("[%s] ", room->title);
    fflush(stdout);
}

void print_log_game(ChatRoom *room)
{
    printf("[GAME-%s] ", room->title);
    fflush(stdout);
}

void print_log_poll(ChatRoom *room)
{
    printf("[POLL-%s] ", room->title);
    fflush(stdout);
}

void server_state()
{
    printf("[INFO] All chatters (%d/%d)", client_count, MAX_CLIENTS);
}

void print_time()
{
    time_t timer = time(NULL);
    struct tm *t = localtime(&timer);
    printf("    (%d-%02d-%02d %02d:%02d:%02d)\n", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
}

void send_menu(int client_fd)
{
    const char *menu_text =
        "\n=== MENU ===\n"
        "1: 사용자 이름 설정\n"
        "2: 채팅방 입장\n"
        "3: 채팅방 개설\n"
        "4: 접속 종료\n"
        "0: 메뉴 재표시\n";

    send(client_fd, menu_text, strlen(menu_text), 0);
}


void send_room_list(int client_fd)
{
    char buffer[LARGE_BUFF_SIZE];

    if (room_count <= 0)
    {
        const char *msg = "개설된 채팅방이 없습니다.\n";
        send(client_fd, msg, strlen(msg), 0);
        return;
    }

    int offset = snprintf(buffer, sizeof(buffer), "\n채팅방 번호 입력 (되돌아가기: b)\n\n=== ChatRoom info ===\n");

    for (int i = 0; i < room_count; i++)
    {
        char line[MEDIUM_BUFF_SIZE];
        int len = snprintf(line, sizeof(line), "%d: %s (%d/%d)\n",
                           chatrooms[i].id, chatrooms[i].title,
                           chatrooms[i].user_count, MAX_ROOM_USERS);

        if (offset + len < sizeof(buffer))
        {
            memcpy(buffer + offset, line, len);
            offset += len;
            buffer[offset] = '\0'; // 안전하게 null-terminate
        }
        else
        {
            break; // 더 이상 공간이 없으면 중단
        }
    }
    send(client_fd, buffer, offset, 0);
}

void send_chatroom_info(ChatRoom *room, int idx)
{
    char status[SMALL_BUFF_SIZE];

    if (room->mode == CHAT_MODE)
        strcpy(status, "Chat");
    else if (room->mode == GAME_MODE)
        strcpy(status, "Game");
    else if (room->mode == POLL_MODE)
        strcpy(status, "Poll");
    else
        strcpy(status, "????");

    char info[LARGE_BUFF_SIZE];
    snprintf(info, sizeof(info), "<<<<< %s >>>>>\n", room->title);

    char line[MEDIUM_BUFF_SIZE];
    snprintf(line, sizeof(line), "참여인원: %d\n", room->user_count);

    if (strlen(info) + strlen(line) < sizeof(info))
    {
        strcat(info, line);
    }


    snprintf(line, sizeof(line), "모드: %s\n", status);
    if (strlen(info) + strlen(line) < sizeof(info))
    {
        strcat(info, line);
    }

    send(room->user_fds[idx], info, strlen(info), 0);
}

int find_client_index(int fd)
{
    for (int i = 0; i < client_count; i++)
    {
        if (clients[i].fd == fd)
            return i;
    }
    return -1;
}

int get_user_index(ChatRoom *room, int fd)
{
    for (int i = 0; i < room->user_count; i++)
    {
        if (room->user_fds[i] == fd)
            return i;
    }
    return -1;
}

void remove_client(int index)
{
    close(clients[index].fd);
    clients[index] = clients[--client_count];
}

void remove_user(ChatRoom *room, int index)
{

    int fd = room->user_fds[index];

    // 게임 모드일 때 호스트가 나간 경우
    if (room->mode == GAME_MODE && fd == room->game_host_fd)
    {
        room->mode = CHAT_MODE;
        room->game_host_fd = -1;
        memset(room->game_answer, 0, sizeof(room->game_answer));
        broadcast_to_room(room, "[GAME] 호스트가 나가 게임이 종료되었습니다.\n", -1);
    }

    // 투표 모드에서 호스트가 나간 경우
    if (room->mode == POLL_MODE && room->poll_mode_stage != 2 && fd == room->game_host_fd)
    {
        room->mode = CHAT_MODE;
        reset_poll_state(room);
    }

    pthread_mutex_lock(&client_lock);
    int idx = find_client_index(fd);
    if (idx != -1)
    {
        clients[idx].state = STATE_LOBBY;
        clients[idx].room_id = -1;
    }
    pthread_mutex_unlock(&client_lock);

    --room->user_count;
    room->user_fds[index] = room->user_fds[room->user_count];
    room->user_names[index] = room->user_names[room->user_count];
}

char *trim(char *str)
{
    // 앞쪽 공백 제거
    while (isspace((unsigned char)*str))
        str++;

    if (*str == 0) // 모두 공백이면 빈 문자열
        return str;

    // 뒤쪽 공백 제거
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    // 문자열 끝에 널 문자 추가
    *(end + 1) = '\0';

    return str;
}

bool parse_valid_int(const char *input, int *result)
{
    if (input == NULL || result == NULL)
        return false;

    char *endptr;
    long val = strtol(input, &endptr, 10);

    // 문자열 끝까지 숫자가 아닌 문자가 있으면 실패
    if (*endptr != '\0')
        return false;

    // 유효한 정수 범위 확인 (선택 사항)
    if (val < INT_MIN || val > INT_MAX)
        return false;

    *result = (int)val;
    return true;
}

// --- 게임 유틸 함수 ---
int is_valid_number(const char *num)
{
    if (strlen(num) != 3)
        return 0;
    if (!isdigit(num[0]) || !isdigit(num[1]) || !isdigit(num[2]))
        return 0;
    return num[0] != num[1] && num[1] != num[2] && num[0] != num[2];
}

void evaluate_guess(const char *guess, const char *answer, int *strikes, int *balls)
{
    *strikes = *balls = 0;
    for (int i = 0; i < 3; i++)
    {
        if (guess[i] == answer[i])
            (*strikes)++;
        else if (strchr(answer, guess[i]))
            (*balls)++;
    }
}

void broadcast_to_room(ChatRoom *room, const char *msg, int except_fd)
{
    for (int i = 0; i < room->user_count; i++)
    {
        int fd = room->user_fds[i];
        if (fd != except_fd)
        {
            send(fd, msg, strlen(msg), 0);
        }
    }
}

void start_poll(ChatRoom *room, int host_fd, const char *host_name)
{
    room->mode = POLL_MODE;
    room->game_host_fd = host_fd;

    // 호스트 이름 복사
    strncpy(room->game_host_name, host_name, SMALL_BUFF_SIZE - 1);
    room->game_host_name[SMALL_BUFF_SIZE - 1] = '\0';

    // 상태 초기화
    room->poll_mode_stage = 0;
    room->poll_count = 0;
    room->poll_index = 0;

    // 배열 초기화
    for (int i = 0; i < MAX_POLL; i++)
    {
        if (room->poll_list[i] != NULL)
        {
            free(room->poll_list[i]);
            room->poll_list[i] = NULL;
        }
        room->poll_votes[i] = 0;
    }

    for (int i = 0; i < MAX_ROOM_USERS; i++)
    {
        room->vote_received[i] = -1;
    }
}

void reset_poll_state(ChatRoom *room)
{
    // 메모리 해제
    for (int i = 0; i < MAX_POLL; i++)
    {
        if (room->poll_list[i] != NULL)
        {
            free(room->poll_list[i]);
            room->poll_list[i] = NULL;
        }
    }

    // 기본 상태 초기화
    room->poll_count = 0;
    room->poll_index = 0;
    room->poll_mode_stage = 0;
    room->game_host_fd = -1;

    // 호스트 이름 초기화
    room->game_host_name[0] = '\0';

    // 투표 결과 및 수신 여부 초기화
    for (int i = 0; i < MAX_ROOM_USERS; i++)
    {
        room->poll_votes[i] = 0;
        room->vote_received[i] = -1;
    }
}


void sigint_handler(int signo)
{
    printf("\n[NOTICE] 시그널 핸들러 시작");
    print_time();

    if (server_sock != -1)
        close(server_sock);

    pthread_mutex_lock(&client_lock);
    for (int i = 0; i < client_count; i++)
        close(clients[i].fd);
    pthread_mutex_unlock(&client_lock);

    printf("[NOTICE] 서버 종료");
    print_time();
    exit(0);
}