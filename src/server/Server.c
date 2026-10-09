#define _DEFAULT_SOURCE // for usleep()

#include "helper/ClientTable.h"
#include "helper/UI/ServerUI.h"
#include "helper/TaskQueue.h"
#include "helper/ThreadPool.h"
#include "data/LocalData.h"
#include "server/Server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_EVENTS 64
#define PORT 5713

typedef struct sockaddr_in sockaddr_in;
typedef struct epoll_event epoll_event;

// This is a byte buffer
typedef unsigned char uchar;

// ###################################
// ## P R I V A T E   M E M B E R S ##
// ###################################

// Server specific variables
static int serverFD;
static int epollFD;
static epoll_event event;
static epoll_event events[MAX_EVENTS];

// Server log buffer
static char logBuffer[128];

/**
 * @brief Set the Non Blocking object
 * 
 * @param FD 
 * @return int 
 */
static int
setNonBlocking(int FD) {
    // get the current set flags
    int flags = fcntl(FD, F_GETFL, 0);
    if (flags == -1) return -1;

    // set non blocking
    flags |= O_NONBLOCK;
    return fcntl(FD, F_SETFL, flags);
}

/**
 * @brief Retrieves the primary local IP address of the machine.
 * 
 * @param buffer A character array to store the IP string.
 * @param buflen The size of the buffer.
 */
static void
getLocalIP (char* buffer, size_t buflen) {
    // Assuming the function fails
    strncpy(buffer, "127.0.0.1", buflen);

    // An extra scoket to establish a dummy connection
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) return;

    // Almost always accessible public destination (8.8.8.8 on port 53)
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);

    // Connect the UDP socket. 
    // This is to get the routing table binding from the kernel
    if (connect(sock, (const struct sockaddr*)&serv, sizeof(serv)) == -1) {
        close(sock);
        return;
    }

    // Read the local IP address the kernel assigned to this socket
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    if (getsockname(sock, (struct sockaddr*)&name, &namelen) != -1) {
        // Binary to string so we can display to users
        inet_ntop(AF_INET, &name.sin_addr, buffer, buflen);
    }

    close(sock);
}

/**
 * @brief XOR decrypt data in-place.
 */
static char*
decryptData(uchar* payload, size_t payloadLength, const char* key) {
    if (!payload || payloadLength == 0 || !key) { return NULL; }

    for(size_t byte = 0; byte < payloadLength; ++byte) {
        payload[byte] ^= key[byte % 16];
    }

    return (char*)payload;
}


static uint8_t
formUserCommand(UserData* user, uchar* buffer, size_t bytesRead) {
    // Stop buffer overflow
    if (user->receivedBytes + bytesRead > sizeof(user->buffer)) { return BUFFER_OVERFLOW; }

    // Add received bytes from the client to the reception buffer
    memcpy(
        user->buffer + user->receivedBytes, 
        buffer, 
        bytesRead
    );
    user->receivedBytes += bytesRead;

    // If we have enough bytes to form a Command then 
    // do it and process it
    while(user->receivedBytes >= sizeof(Command)) {
        Command* userCommand = malloc(sizeof(Command));
        memcpy(userCommand, user->buffer, sizeof(Command));

        if (user->registered) { 
            decryptData((uchar*)userCommand, sizeof(Command), user->data.key); 
        }

        // Capture command type before pushTask 
        CommandType cmdType = userCommand->commandType;

        if (pushTask(user->FD, userCommand) == QUEUE_FULL) {
            free(userCommand); 
            return TASK_QUEUE_FULL; 
        }

        snprintf(logBuffer, sizeof(logBuffer), "Command from FD %d (type: %d)", user->FD, cmdType);
        serverLog("CYAN", "COMMAND", logBuffer);

        // Move the remaining bytes to the start of the buffer  
        // so we can accumulate data for the next command
        size_t leftoverBytes = user->receivedBytes - sizeof(Command);
        if (leftoverBytes > 0)
            memmove(
                user->buffer, 
                user->buffer + sizeof(Command), 
                leftoverBytes
            );
    
        user->receivedBytes = leftoverBytes;
    }

    return 0;
}

/**
 * @brief Send a ServerResponse to a client.
 *        Handles partial writes and non-blocking socket retries.
 */
static void
sendServerResponse(
    uint32_t receiver,
    ResponseType responseType,
    const char* responseContent
)
{
    if (!responseContent) return;

    ServerResponse response;
    memset(&response, 0, sizeof(ServerResponse));

    response.receiver = receiver;
    response.responseType = responseType;
    response.payloadLength = strlen(responseContent) + 1;
    memcpy(response.responseContent, responseContent, response.payloadLength);

    size_t written = 0;
    size_t totalSize = sizeof(ServerResponse);

    while (written < totalSize) {
        ssize_t bytesSent = write(
            response.receiver,
            ((uchar*)&response) + written,
            totalSize - written
        );

        if (bytesSent > 0) {
            written += bytesSent;
        }
        else if (bytesSent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            break;
        }
        else {
            // avoid infinite loop
            break;
        }
    }
}

static void
addNewUser() {
    // Handle all incoming connection requests
    while(true) {
        sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFD = 
            accept(
                serverFD, 
                (struct sockaddr*)&clientAddr, 
                &clientLen
            );

        if (clientFD == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { break; } 
            else {
                serverLog("RED", "CONNECTION FAILED", "Could not connect a new client");
                break;
            }
        }

        setNonBlocking(clientFD);

        // Allocate space for the new user's entry
        UserData newUser;
        memset(&newUser, 0, sizeof(UserData));
        newUser.FD = clientFD;

        // Try to add the client to the table first
        uint16_t status = connectUserInClientTable(newUser);
        if (status != 0) {
            // Failed to add, close the FD and continue
            close(clientFD);
            serverLog("YELLOW", "WARNING", "Client table full or allocation failed");
            continue;
        }

        // Now add to epoll
        epoll_event clientEvent;
        clientEvent.events = EPOLLIN | EPOLLET;
        clientEvent.data.fd = clientFD;
        if (epoll_ctl(epollFD, EPOLL_CTL_ADD, clientFD, &clientEvent) == -1) {
            // If epoll_ctl fails, remove from table and close
            disconnectUserFromClientTable(clientFD);
            close(clientFD);
            serverLog("RED", "ERROR", "Failed to add client to epoll");
        } else {
            // New socket connection logged
            snprintf(logBuffer, sizeof(logBuffer), "New socket connection (FD: %d)", clientFD);
            serverLog("GREEN", "SOCKET", logBuffer);
        }
    }
}

/**
 * @brief 
 * 
 * @param clientFD 
 */
static void
handleClientMessage(uint32_t clientFD) {
    lockClientTable();
    UserData* user = getUserByFD(clientFD);
    
    if (!user) {
        unlockClientTable(); 

        disconnectUserFromClientTable(clientFD);
        close(clientFD);
        return;
    }

    while(true) {
        uchar* tempBuffer = malloc(sizeof(Command));
        ssize_t bytesRead = read(clientFD, tempBuffer, sizeof(Command));

        // ###############################################
        // ## AN ERROR OCCURRED WHEN READING USER INPUT ##
        // ###############################################
        if (bytesRead == -1) {
            // No more input to read
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                free(tempBuffer);
                break;
            }

            // an error occurred when accepting the message
            unlockClientTable();
            disconnectUserFromClientTable(clientFD);
            free(tempBuffer); 
            close(clientFD);
            return;
        } 

        // #############################
        // ## CLIENT HAS DISCONNECTED ##
        // #############################
        else if (bytesRead == 0) {
            UserData* user = getUserByFD(clientFD);

            snprintf(logBuffer, sizeof(logBuffer), "%s disconnected (FD: %d)", user->data.username, clientFD);
            serverLog("YELLOW", "DISCONNECT", logBuffer);

            unlockClientTable();

            disconnectUserFromClientTable(clientFD);
            close(clientFD);
            free(tempBuffer);
            return;
        } 

        // ################################################
        // ## FORM USER COMMAND BY READING INPUT BUFFERS ##
        // ################################################
        else { 
            uint8_t parseStatus = formUserCommand(user, tempBuffer, bytesRead);
            
            if (parseStatus != 0) {
                const char* logError = "Unknown parsing error";
                memcpy(logBuffer, logError, sizeof(logError));
                const char* clientResponse = "ERROR: Unknown error. Request dropped.";

                if (parseStatus == BUFFER_OVERFLOW) {
                    const char* logError = "Buffer overflow detected. Clearing user buffer";
                    memcpy(logBuffer, logError, sizeof(logError));
                    clientResponse = "ERROR: Message too large. Request dropped.";
                } 
                else if (parseStatus == TASK_QUEUE_FULL) {
                    const char* logError = "Task queue full. Dropping client request";
                    memcpy(logBuffer, logError, sizeof(128));
                    clientResponse = "ERROR: Server is overloaded. Request dropped.";
                }

                serverLog("YELLOW", "WARNING", logBuffer);

                // Clear the buffer for future use
                user->receivedBytes = 0;
                
                uint32_t targetFD = user->FD;
                unlockClientTable();
                
                sendServerResponse(targetFD, RES_ERROR, clientResponse);
                
                free(tempBuffer);
                return; 
            } 
        }

        free(tempBuffer);
    }

    unlockClientTable();
}

// #################################
// ## P U B L I C   M E M B E R S ##
// #################################

void 
enableServer() {
    // Get the IP address of the server
    char ipBuffer[INET_ADDRSTRLEN];
    getLocalIP(ipBuffer, (size_t)INET_ADDRSTRLEN);

    // Display the public IP and port number to the server terminal
    initServerUI(ipBuffer, PORT);
    sockaddr_in serverAddress;

    getLocalData();

    // Initialize the client table data structure 
    // to hold all online users
    clientTableInit();

    initTaskQueue();
    initThreadPool();

    // create the socket blah blah blah
    serverFD = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFD == -1) {
        cleanupServerUI();
        printf("ERROR: Could not create server\n");
        exit(EXIT_FAILURE);
    }

    // If this is not set, we gotta wait 2 minutes on every hot reload
    int opt = 1;
    setsockopt(serverFD, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(PORT);

    // bind blah blah blah
    if (bind(serverFD, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        cleanupServerUI();
        printf("ERROR: Could not create server\n");
        exit(EXIT_FAILURE);
    }

    // so the server doesnt freeze on every read() or write() call
    setNonBlocking(serverFD);

    // global event poll
    epollFD = epoll_create1(0);
    event = (epoll_event) {
        .events = EPOLLIN | EPOLLET,
        .data.fd = serverFD
    };
    epoll_ctl(epollFD, EPOLL_CTL_ADD, serverFD, &event);
}

void 
serverLoop() {
    // Start listening for any connection requests
    listen(serverFD, SOMAXCONN);

    while(true) {

        // Get all active buffers in the epoll queue
        int activeSocketCount = epoll_wait(epollFD, events, MAX_EVENTS, -1);

        for(int socket = 0; socket < activeSocketCount; ++socket) {
            int activeFD = events[socket].data.fd;

            // ##############
            // ## NEW USER ##
            // ##############
            if (activeFD == serverFD) { addNewUser(); }

            // #####################################
            // ## RECEIVE MESSAGE FROM THE CLIENT ##
            // #####################################
            else { handleClientMessage(activeFD); }
        }
    }
}