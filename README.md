# Chat-Room
A chat room server based on process pool. 

## Description

This is a chat room server based on process pool and IO multiplexing. The implementation refers to *High Performance Linux Server Programming*, but many modifications have also been made. It's a good practice for beginners of C++ back-end development.

## Usage

My environment is Ubuntu 20.04. 

To compile and run server, type 

```
make server
./server [IP address] [port]
```

To compile and run client, type

```
make client
./client [IP address] [port] [room number] [user name]
```

