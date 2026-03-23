CC = g++
FLAGS = -std=c++17 -Wall

all: server client

server: server.cpp net_utils.cpp message.h net_utils.h
	$(CC) $(FLAGS) -o server server.cpp net_utils.cpp

client: client.cpp net_utils.cpp message.h net_utils.h
	$(CC) $(FLAGS) -o client client.cpp net_utils.cpp

clean:
	rm -f server client