#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>

#define BUF_SIZE 128
#define NORMAL_SIZE 64

void *send_msg(void *arg);
void *recv_msg(void *arg);
void cleanup(int signo);
void menu();

char name[NORMAL_SIZE] = "[DEFALT]"; // 사용자 이름
char serv_time[NORMAL_SIZE];         // 서버 시간 문자열
char serv_port[NORMAL_SIZE];         // 서버 포트 문자열
char clnt_ip[NORMAL_SIZE];           // 클라이언트 IP 문자열
pthread_t snd_thread, rcv_thread;

int sock = -1; // 소켓 디스크립터

int main(int argc, char *argv[])
{
    signal(SIGINT, cleanup); // Ctrl+C 시 종료 처리

    struct sockaddr_in serv_addr;
    void *thread_return;

    if (argc != 4)
    {
        printf(" Usage : %s <ip> <port> <name>\n", argv[0]);
        exit(1);
    }

    // 사용자 정보 설정
    sprintf(clnt_ip, "%s", argv[1]);
    sprintf(serv_port, "%s", argv[2]);
    sprintf(name, "[%s]", argv[3]);

    sock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(argv[1]);
    serv_addr.sin_port = htons(atoi(argv[2]));

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
    {
        perror("connect");
        exit(1);
    }

    // 서버에 사용자 이름 전송
    send(sock, argv[3], strlen(argv[3]), 0);

    // 접속 시간 기록
    time_t timer = time(NULL);
    struct tm *t = localtime(&timer);
    snprintf(serv_time, sizeof(serv_time),
             "(%d-%02d-%02d %02d:%02d:%02d)\n",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    menu();

    // 송신/수신 스레드 생성
    pthread_create(&snd_thread, NULL, send_msg, (void *)&sock);
    pthread_create(&rcv_thread, NULL, recv_msg, (void *)&sock);
    pthread_join(snd_thread, &thread_return);
    pthread_join(rcv_thread, &thread_return);

    cleanup(0);
    return EXIT_SUCCESS;
}

void *send_msg(void *arg)
{
    int sock = *((int *)arg);
    char buffer[BUF_SIZE];

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));
        if (fgets(buffer, sizeof(buffer), stdin) == NULL)
        {
            perror("fgets");
            break;
        }

        buffer[strcspn(buffer, "\r\n")] = '\0';
        if (strlen(buffer) == 0)
            continue;

        if (send(sock, buffer, strlen(buffer), 0) < 0)
        {
            perror("send");
            break;
        }
    }

    return NULL;
}

void *recv_msg(void *arg)
{
    int sock = *((int *)arg);
    char buffer[BUF_SIZE];

    while (1)
    {
        memset(buffer, 0, sizeof(buffer));
        int n = recv(sock, buffer, sizeof(buffer) - 1, 0);

        if (n < 0)
        {
            perror("recv");
            sleep(1);
            continue;
        }
        if (n == 0)
        {
            printf("\n[INFO] 서버와 연결 종료됨\n");
            cleanup(0);
        }

        printf("%s", buffer); 
    }

    return NULL;
}

void menu()
{
    system("clear");
    printf(" <<<< Chat Client >>>>\n");
    printf(" Server Port : %s \n", serv_port);
    printf(" Client IP   : %s \n", clnt_ip);
    printf(" Chat Name   : %s \n", name);
    printf(" Server Time : %s \n", serv_time);
}

void cleanup(int signo)
{
    if (sock != -1) close(sock);

    pthread_cancel(snd_thread);
    pthread_cancel(rcv_thread);

    printf("\n[NOTICE] 클라이언트 종료\n");
    exit(0);
}
