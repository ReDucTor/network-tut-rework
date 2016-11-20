#include "Server.h"
#include <thread>
#include <WS2tcpip.h>

Server::Server(int PORT, bool BroadcastPublically) //Port = port to broadcast on. BroadcastPublically = false if server is not open to the public (people outside of your router), true = server is open to everyone (assumes that the port is properly forwarded on router settings)
{
	//Winsock Startup
	WSAData wsaData;
	WORD DllVersion = MAKEWORD(2, 1);
	if (WSAStartup(DllVersion, &wsaData) != 0) //If WSAStartup returns anything other than 0, then that means an error has occured in the WinSock Startup.
	{
		MessageBoxA(NULL, "WinSock startup failed", "Error", MB_OK | MB_ICONERROR);
		exit(1);
	}

	if (BroadcastPublically) //If server is open to public
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else //If server is only for our router
		addr.sin_addr.s_addr = inet_addr("127.0.0.1"); //Broadcast locally
	addr.sin_port = htons(PORT); //Port
	addr.sin_family = AF_INET; //IPv4 Socket

	sListen = socket(AF_INET, SOCK_STREAM, NULL); //Create socket to listen for new connections
	if (bind(sListen, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) //Bind the address to the socket, if we fail to bind the address..
	{
		std::string ErrorMsg = "Failed to bind the address to our listening socket. Winsock Error:" + std::to_string(WSAGetLastError());
		MessageBoxA(NULL, ErrorMsg.c_str(), "Error", MB_OK | MB_ICONERROR);
		exit(1);
	}
	if (listen(sListen, SOMAXCONN) == SOCKET_ERROR) //Places sListen socket in a state in which it is listening for an incoming connection. Note:SOMAXCONN = Socket Oustanding Max connections, if we fail to listen on listening socket...
	{
		std::string ErrorMsg = "Failed to listen on listening socket. Winsock Error:" + std::to_string(WSAGetLastError());
		MessageBoxA(NULL, ErrorMsg.c_str(), "Error", MB_OK | MB_ICONERROR);
		exit(1);
	}
	serverptr = this;
	CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)PacketSenderThread, NULL, NULL, NULL); //Create thread that will manage all outgoing packets
}

bool Server::ListenForNewConnection()
{
	SOCKET newConnectionSocket = accept(sListen, (SOCKADDR*)&addr, &addrlen); //Accept a new connection
	if (newConnectionSocket == 0) //If accepting the client connection failed
	{
		std::cout << "Failed to accept the client's connection." << std::endl;
		return false;
	}
	else //If client connection properly accepted
	{
		char ipAddress[32] = {};
		inet_ntop(AF_INET, &addr.sin_addr, ipAddress, sizeof(ipAddress));
		std::unique_lock<std::shared_mutex> lock(connectionMgr_mutex); //Lock connection manager mutex since we are adding an element to connection vector
		std::shared_ptr<Connection> newConnection;
		if (UnusedConnections > 0) //If there is an unused connection that this client can use
		{
			for (size_t i = 0; i < connections.size(); i++) //iterate through the unused connections starting at first connection
			{
				if (connections[i]->ActiveConnection == false) //If connection is not active
				{
					newConnection = connections[i];
					newConnection->socket = newConnectionSocket;
					newConnection->ActiveConnection = true;
					newConnection->ipAddress = ipAddress;
					
					UnusedConnections -= 1;
					break;
				}
			}
		}
		else //If no unused connections available... (add new connection to the socket)
		{
			newConnection = std::make_shared<Connection>(newConnectionSocket);
			newConnection->ipAddress = ipAddress;
			connections.push_back(newConnection); //push new connection into vector of connections
		}
		std::cout << "Client Connected! IP:" << newConnection->ipAddress << std::endl;
		std::thread{ &ClientHandlerThread, newConnection }.detach();
		return true;
	}
}

bool Server::ProcessPacket(Connection & connection, PacketType _packettype)
{
	switch (_packettype)
	{
	case PacketType::ChatMessage: //Packet Type: chat message
	{
		std::string message; //string to store our message we received
		if (!GetString(connection, message)) //Get the chat message and store it in variable: Message
			return false; //If we do not properly get the chat message, return false
						  //Next we need to send the message out to each user
		{
			std::shared_lock<std::shared_mutex> lock{ connectionMgr_mutex };
			for (const auto & otherConnection : connections)
			{
				// Exclude in-active connections
				if (!otherConnection->ActiveConnection)
				{
					continue;
				}

				// Exclude this connection
				if (otherConnection.get() == &connection)
				{
					continue;
				}

				SendString(*otherConnection, message);
			}
		}
		std::cout << "Processed chat message packet from user IP: " << connection.ipAddress << std::endl;
		break;
	}
	case PacketType::FileTransferRequestFile:
	{
		std::string FileName; //string to store file name
		if (!GetString(connection, FileName)) //If issue getting file name
			return false; //Failure to process packet

		connection.file.infileStream.open(FileName, std::ios::binary | std::ios::ate); //Open file to read in binary | ate mode. We use ate so we can use tellg to get file size. We use binary because we need to read bytes as raw data
		if (!connection.file.infileStream.is_open()) //If file is not open? (Error opening file?)
		{
			std::cout << "Client: " << connection.ipAddress << " requested file: " << FileName << ", but that file does not exist." << std::endl;
			std::string ErrMsg = "Requested file: " + FileName + " does not exist or was not found.";
			SendString(connection, ErrMsg); //Send error msg string to client
			return true;
		}

		connection.file.fileName = FileName; //set file name just so we can print it out after done transferring
		connection.file.fileSize = connection.file.infileStream.tellg(); //Get file size
		connection.file.infileStream.seekg(0); //Set cursor position in file back to offset 0 for when we read file
		connection.file.fileOffset = 0; //Update file offset for knowing when we hit end of file
		
		if (!HandleSendFile(connection)) //Attempt to send byte buffer from file. If failure...
			return false;
		break;
	}
	case PacketType::FileTransferRequestNextBuffer:
	{
		if (!HandleSendFile(connection)) //Attempt to send byte buffer from file. If failure...
			return false;
		break;
	}
	default: //If packet type is not accounted for
	{
		std::cout << "Unrecognized packet: " << (int32_t)_packettype << std::endl; //Display that packet was not found
		break;
	}
	}
	return true;
}

bool Server::HandleSendFile(Connection & connection)
{
	if (connection.file.fileOffset >= connection.file.fileSize) //If end of file reached then return true and skip sending any bytes
		return true;
	if (!SendPacketType(connection, PacketType::FileTransferByteBuffer)) //Send packet type for file transfer byte buffer
		return false;
	
	connection.file.remainingBytes = connection.file.fileSize - connection.file.fileOffset; //calculate remaining bytes
	if (connection.file.remainingBytes > connection.file.buffersize) //if remaining bytes > max byte buffer
	{
		connection.file.infileStream.read(connection.file.buffer, connection.file.buffersize); //read in max buffer size bytes
		if (!Sendint32_t(connection, connection.file.buffersize)) //send int of buffer size
			return false;
		if (!sendall(connection, connection.file.buffer, connection.file.buffersize)) //send bytes for buffer
			return false;
		connection.file.fileOffset += connection.file.buffersize; //increment fileoffset by # of bytes written
	}
	else
	{
		connection.file.infileStream.read(connection.file.buffer, connection.file.remainingBytes); //read in remaining bytes
		if (!Sendint32_t(connection, connection.file.remainingBytes)) //send int of buffer size
			return false;
		if (!sendall(connection, connection.file.buffer, connection.file.remainingBytes)) //send bytes for buffer
			return false;
		connection.file.fileOffset += connection.file.remainingBytes; //increment fileoffset by # of bytes written
	}

	if (connection.file.fileOffset == connection.file.fileSize) //If we are at end of file
	{
		if (!SendPacketType(connection, PacketType::FileTransfer_EndOfFile)) //Send end of file packet
			return false;
		//Print out data on server details about file that was sent
		std::cout << std::endl << "File sent: " << connection.file.fileName << std::endl;
		std::cout << "File size(bytes): " << connection.file.fileSize << std::endl << std::endl;
	}
	return true;
}

void Server::ClientHandlerThread(std::shared_ptr<Connection> connection)
{
	PacketType packettype;
	while (true)
	{
		if (!serverptr->GetPacketType(*connection, packettype)) //Get packet type
			break; //If there is an issue getting the packet type, exit this loop
		if (!serverptr->ProcessPacket(*connection, packettype)) //Process packet (packet type)
			break; //If there is an issue processing the packet, exit this loop
	}
	std::cout << "Lost connection to client IP: " << connection->ipAddress << std::endl;
	serverptr->DisconnectClient(*connection); //Disconnect this client and clean up the connection if possible
	return;
}

void Server::PacketSenderThread() //Thread for all outgoing packets
{
	while (true)
	{
		std::shared_lock< std::shared_mutex > lock{ serverptr->connectionMgr_mutex };
		for (size_t i = 0; i < serverptr->connections.size(); i++) //for each connection...
		{
			if (serverptr->connections[i]->pm.HasPendingPackets()) //If there are pending packets for this connection's packet manager
			{
				Packet p = serverptr->connections[i]->pm.Retrieve(); //Retrieve packet from packet manager
				if (!serverptr->sendall(*serverptr->connections[i], p.buffer, p.size)) //send packet to connection
				{
					std::cout << "Failed to send packet to ID: " << i << std::endl; //Print out if failed to send packet
				}
				delete p.buffer; //Clean up buffer from the packet p
			}
		}
		Sleep(5);
	}
}

void Server::DisconnectClient(Connection & connection) //Disconnects a client and cleans up socket if possible
{
	std::unique_lock<std::shared_mutex> lock(connectionMgr_mutex); //Lock connection manager mutex since we are possible removing element(s) from the vector
	if (connection.ActiveConnection == false) //If connection has already been disconnected?
	{
		return; //return - this should never happen, but just in case...
	}
	connection.pm.Clear(); //Clear out all remaining packets in queue for this connection
	connection.ActiveConnection = false; //Update connection's activity status to false since connection is now unused
	closesocket(connection.socket); //Close the socket for this connection
	if (&connection == connections.back().get()) //If last connection in vector.... (we can remove it)
	{
		connections.pop_back(); //Erase last connection from vector
		//After cleaning up that connection, check if there are any more connections that can be erased (only connections at the end of the vector can be erased)

		for (size_t i = connections.size() - 1; i >= 0 && connections.size()>0; i--)
		{
			if (connections[i]->ActiveConnection) //If connection is active we cannot remove any more connections from vector
				break;
			//If we have not broke out of the for loop, we can remove the current indexed connection
			connections.pop_back(); //Erase last connection from vector
			UnusedConnections -= 1;
		}
	}
	else
	{
		UnusedConnections += 1;
	}
}