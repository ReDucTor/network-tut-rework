#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib,"ws2_32.lib") //Required for WinSock
#include <WinSock2.h> //For win sockets
#include <string> //For std::string
#include <iostream> //For std::cout, std::endl
#include "FileTransferData.h"
#include "PacketManager.h"
#include "PacketStructs.h"
#include <vector> //for std::vector
#include <shared_mutex>

class Connection
{
public:
	Connection(SOCKET socket_)
	{
		socket = socket_;
		ActiveConnection = true; //Default to active connection 
	}
	bool ActiveConnection; //True if connection is active, false if inactive(due to a disconnect)
	SOCKET socket;
	//file transfer data
	FileTransferData file; //Object that contains information about our file that is being sent to the client from this server
	PacketManager pm; //Packet Manager for outgoing data for this connection
	std::string ipAddress;
};

class Server
{
public:
	Server(int PORT, bool BroadcastPublically = false);
	bool ListenForNewConnection();

private:
	
	bool sendall(Connection & connection, char * data, int totalbytes);
	bool recvall(Connection & connection, char * data, int totalbytes);

	bool Sendint32_t(Connection & connection, int32_t _int32_t);
	bool Getint32_t(Connection & connection, int32_t & _int32_t);

	bool SendPacketType(Connection & connection, PacketType _packettype);
	bool GetPacketType(Connection & connection, PacketType & _packettype);

	void SendString(Connection & connection, std::string & _string);
	bool GetString(Connection & connection, std::string & _string);

	bool ProcessPacket(Connection & connection, PacketType _packettype);
	bool HandleSendFile(Connection & connection);

	static void ClientHandlerThread(std::shared_ptr<Connection> connection);
	static void PacketSenderThread();
	
	void DisconnectClient(Connection & connection); //Called to properly disconnect and clean up a client (if possible)
private:
	std::vector<std::shared_ptr<Connection>> connections;
	std::shared_mutex connectionMgr_mutex; //mutex for managing connections (used when a client disconnects)

	SOCKADDR_IN addr; //Address that we will bind our listening socket to
	int addrlen = sizeof(addr);
	SOCKET sListen;
};

static Server * serverptr; //Serverptr is necessary so the static ClientHandler method can access the server instance/functions.
