server: chat_server.cpp
	g++ chat_server.cpp -o server
client: chat_client.cpp
	g++ chat_client.cpp -o client
clean:
	rm server client