CC = gcc
CFLAGS = -Wall

# 실행 파일 이름
SERVER = server.out
CLIENTS = client1.out client2.out client3.out client4.out

# 기본 빌드
all: $(SERVER) $(CLIENTS)

# 서버 빌드
$(SERVER): server.c
	$(CC) $(CFLAGS) -o $(SERVER) server.c -lpthread

# 클라이언트 빌드
client1.out: client.c
	$(CC) $(CFLAGS) -o client1.out client.c -DC1

client2.out: client.c
	$(CC) $(CFLAGS) -o client2.out client.c -DC1

client3.out: client.c
	$(CC) $(CFLAGS) -o client3.out client.c -DC1

client4.out: client.c
	$(CC) $(CFLAGS) -o client4.out client.c -DC1

# 정리
clean:
	rm -f *.out *.o