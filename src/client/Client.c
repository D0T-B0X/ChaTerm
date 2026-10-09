#define _POSIX_C_SOURCE 200809L // For strtok_r()

#include "client/Client.h"
#include "helper/UI/ClientUI.h"
#include "Common.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <wordexp.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <netinet/in.h>

#define MAX_EVENTS 16
#define INF_TIMEOUT -1
#define CHUNK_SIZE 1024

typedef struct sockaddr_in sockaddr_in;
typedef struct epoll_event epoll_event;

// Client specific data
static int clientSocket;
static int epollFD;
static char clientKey[16];
static bool sessionEncrypted = false;
static bool awaitingConnectResponse = false;

// Terminal display
static ClientTerm* term;

// File transfer
static FILE* incomingFile = NULL;
static char incomingFilename[256];
static uint32_t expectedChunks = 0;
static uint32_t receivedChunks = 0;

// Buffer for accumulating incoming server data
static uchar recvBuffer[sizeof(ServerResponse) * 4];
static size_t recvBufferLen = 0;

// ###################################
// ## P R I V A T E   M E M B E R S ##
// ###################################

/**
 * @brief XOR encrypt data in-place.
 */
static uchar*
encryptData(char* payload, size_t payloadLength, const char* key) {
    if (!payload || payloadLength == 0 || !key) { return NULL; }

    for(size_t byte = 0; byte < payloadLength; ++byte) {
        payload[byte] ^= key[byte % 16];
    }

    return (uchar*)payload;
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
 * @brief Parse user input into a Command struct
 * 
 * @param msg The input string from the user
 * @return Command* Pointer to allocated Command (must be freed)
 */
static Command*
parseUserInput(const char* msg) {
    Command* command = malloc(sizeof(Command));
    memset(command, 0, sizeof(Command)); 
    command->statusCode = INVALID_COMMAND; 

    if (!msg || strlen(msg) == 0) { 
        command->statusCode = EMPTY_INPUT;
        return command; 
    } 

    // Make a local copy because strtok_r modifies the string in-place
    char buffer[2048];
    strncpy(buffer, msg, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char* saveptr;
    char* cmdToken = strtok_r(buffer, " ", &saveptr);

    if (!cmdToken) return command;

    // REGISTER <username> <password> KEY <key>
    if (strcmp(cmdToken, "REGISTER") == 0) {
        command->commandType = REGISTER;
        
        char* user = strtok_r(NULL, " ", &saveptr);
        char* pass = strtok_r(NULL, " ", &saveptr);
        char* keyCmd = strtok_r(NULL, " ", &saveptr);
        char* key = strtok_r(NULL, " ", &saveptr);

        if (user) strncpy(command->username, user, sizeof(command->username) - 1);
        if (pass) strncpy(command->password, pass, sizeof(command->password) - 1);
        
        // Only copy the key if the KEY keyword was present and key is not NULL
        if (keyCmd && strcmp(keyCmd, "KEY") == 0 && key) {
            strncpy(clientKey, key, sizeof(clientKey));
            strncpy(command->key, key, sizeof(command->key) - 1);
        } else {
            command->statusCode = NO_KEY;
        }

        // Success only if user, pass, keyCmd == "KEY", and key are present
        if (user && pass && keyCmd && strcmp(keyCmd, "KEY") == 0 && key) { 
            command->statusCode = VALID_COMMAND; 
        }
    } 

    // CONNECT <username> <password>
    else if (strcmp(cmdToken, "CONNECT") == 0) {
        command->commandType = CONNECT;
        
        char* user = strtok_r(NULL, " ", &saveptr);
        char* pass = strtok_r(NULL, " ", &saveptr);
        
        if (user) strncpy(command->username, user, sizeof(command->username) - 1);
        if (pass) strncpy(command->password, pass, sizeof(command->password) - 1);

        if (user && pass) { command->statusCode = VALID_COMMAND; }
    } 

    // SEND TO <username> <message>
    else if (strcmp(cmdToken, "SEND") == 0) {
        char* toToken = strtok_r(NULL, " ", &saveptr);
        
        if (toToken && strcmp(toToken, "TO") == 0) {
            command->commandType = SEND_TO;
            char* user = strtok_r(NULL, " ", &saveptr);

            if (user) {
                strncpy(command->username, user, sizeof(command->username) - 1);
                command->statusCode = VALID_COMMAND;

                // Copy the client's own encryption key for this command
                memcpy(command->key, clientKey, sizeof(command->key));

                // Extract the message: skip leading spaces, then copy rest
                if (saveptr && *saveptr != '\0') {
                    // Skip leading spaces
                    while (*saveptr == ' ') saveptr++;
                    if (*saveptr != '\0') {
                        strncpy(command->payload, saveptr, sizeof(command->payload) - 1);
                        command->payloadLength = strlen(command->payload);
                    }
                }
            }
        } else {
            command->statusCode = INVALID_FORMAT;
        }
    } 

    // SENDFILE TO <username> <filename>
    else if (strcmp(cmdToken, "SENDFILE") == 0) {
        char* toToken = strtok_r(NULL, " ", &saveptr);

        if (toToken && strcmp(toToken, "TO") == 0) {
            command->commandType = SENDFILE_TO;
        
            char* user = strtok_r(NULL, " ", &saveptr);
            char* file = strtok_r(NULL, " ", &saveptr);
        
            if (user) strncpy(command->username, user, sizeof(command->username) - 1);
            if (file) {
                strncpy(command->filename, file, sizeof(command->filename) - 1);
                command->filename[sizeof(command->filename) - 1] = '\0';
                command->statusCode = VALID_COMMAND;
                memcpy(command->key, clientKey, sizeof(command->key));
            } 

            else { command->statusCode = INVALID_FORMAT; }
        } 
        
        else { command->statusCode = INVALID_FORMAT; }
    }

    // LIST 
    else if (strcmp(cmdToken, "LIST") == 0) {
        command->statusCode = VALID_COMMAND;
        command->commandType = LIST;
    }

    // HELP
    else if (strcmp(cmdToken, "HELP") == 0) {
        command->commandType = HELP;
        command->statusCode = VALID_COMMAND; 
    }

    // QUIT
    else if (strcmp(cmdToken, "QUIT") == 0) {
        command->commandType = QUIT;
        command->statusCode = VALID_COMMAND; 
    }

    // Invalid input if unregnized tokens are found
    if (command->commandType != SEND_TO) {
        char* trail = strtok_r(NULL, " ", &saveptr);
        if (trail) { command->statusCode = INVALID_FORMAT; }
    }

    return command;
}

/**
 * @brief Expand a filename containing tilde, environment variables, and
 *        relative/absolute path. Backslashes are treated as forward slashes.
 * @param filename Input path (may include ~, $VAR, etc.)
 * @return Newly allocated expanded string, or NULL on failure.
 */
static char*
expandFilename(const char* filename) {
    if (!filename) return NULL;

    wordexp_t p;
    int ret = wordexp(filename, &p, WRDE_NOCMD);  // no command substitution
    if (ret != 0 || p.we_wordc == 0) {
        if (ret == 0) wordfree(&p);
        return strdup(filename);  // failed return original string
    }

    // Use the first expanded word 
    char* expanded = strdup(p.we_wordv[0]);
    wordfree(&p);

    return expanded;
}

/**
 * @brief Send a Command structure to the server
 * 
 * @param command Pointer to the command to send
 * @return true on success, false on error
 */
static bool
transmitToServer(Command* command) {
    if (!command) return false;

    bool isConnect = command->commandType == CONNECT;

    // If this is not a registration or connect command, encrypt the whole struct
    if (command->commandType != REGISTER && command->commandType != CONNECT) {
        encryptData((char*)command, sizeof(Command), clientKey);
    }

    // retry latency
    struct timespec ts = {0, 1000000}; // 1ms
    size_t written = 0;
    size_t totalSize = sizeof(Command);

    while(written < totalSize) {
        ssize_t bytesSent = write(
            clientSocket, 
            ((uchar*)command) + written, 
            totalSize - written
        );
        if (bytesSent > 0) { written += bytesSent; }
        else if (bytesSent == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                nanosleep(&ts, NULL);
                continue;
            }
            return false;
        }
    }
    if (isConnect) {
        awaitingConnectResponse = true;
    }

    return true;
}

/**
 * @brief Send a file to a receiver by splitting it into chunks.
 * 
 * @param filename Path to the file to send.
 * @param receiver Username of the receiver.
 * @param key Sender's encryption key.
 * @return true on success, false on error.
 */
static bool
sendFile(const char* filename, const char* receiver, const char* key) {
    // Expand path 
    char* expandedPath = expandFilename(filename);
    if (!expandedPath) {
        ClientUI_TermSetPopup(term, "ERROR: Invalid file path", 1);
        return false;
    }

    const char* dot = strrchr(expandedPath, '.');
    if (!dot || dot == expandedPath) { 
        free(expandedPath);    
        ClientUI_TermSetPopup(term, "ERROR: only .txt files are supported", 1);
        return false; 
    }

    const char* ext = dot + 1;
    if (strlen(ext) != 3) { 
        free(expandedPath);
        return false; 
    }

    if (tolower((unsigned char)ext[0]) != 't' ||
        tolower((unsigned char)ext[1]) != 'x' ||
        tolower((unsigned char)ext[2]) != 't') { 
            free(expandedPath);
            ClientUI_TermSetPopup(term, "ERROR: only .txt files are supported", 1);
            return false; 
        }

    FILE* fp = fopen(expandedPath, "rb");
    if (!fp) {
        ClientUI_TermSetPopup(term, "ERROR: Cannot open file", 1);
        free(expandedPath);
        return false;
    }

    // Check file size
    fseek(fp, 0, SEEK_END);
    long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0 || fileSize > 1024 * 1024) {
        fclose(fp);
        ClientUI_TermSetPopup(term, "ERROR: File must be 1 MB or less", 1);
        free(expandedPath);
        return false;
    }

    // Extract base name (handle both '/' and '\')
    const char* baseName = expandedPath;
    for (const char* p = expandedPath; *p; ++p) {
        if (*p == '/' || *p == '\\') baseName = p + 1;
    }

    // Send start command
    Command startCommand;
    memset(&startCommand, 0, sizeof(startCommand));
    
    startCommand.commandType = SENDFILE_TO;
    strncpy(startCommand.username, receiver, sizeof(startCommand.username) - 1);
    strncpy(startCommand.filename, baseName, sizeof(startCommand.filename) - 1);
    startCommand.fileSize = fileSize;
    startCommand.totalChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    memcpy(startCommand.key, key, sizeof(startCommand.key));
    
    startCommand.statusCode = VALID_COMMAND;
    if (!transmitToServer(&startCommand)) {
        fclose(fp);
        free(expandedPath);
        return false;
    }

    // Send data chunks
    uchar buffer[CHUNK_SIZE];
    uint32_t chunkIndex = 0;
    
    size_t bytesRead;
    while ((bytesRead = fread(buffer, 1, CHUNK_SIZE, fp)) > 0) {
        Command dataCommand;
        memset(&dataCommand, 0, sizeof(dataCommand));
        
        dataCommand.commandType = SENDFILE_DATA;
        strncpy(dataCommand.username, receiver, sizeof(dataCommand.username) - 1);
        dataCommand.chunkIndex = chunkIndex;
        dataCommand.payloadLength = bytesRead;
        memcpy(dataCommand.payload, buffer, bytesRead);
        memcpy(dataCommand.key, key, sizeof(dataCommand.key));
        
        dataCommand.statusCode = VALID_COMMAND;

        // Encrypt payload
        encryptData(dataCommand.payload, dataCommand.payloadLength, dataCommand.key);

        if (!transmitToServer(&dataCommand)) {
            fclose(fp);
            return false;
        }
        chunkIndex++;
    }

    // Send end command
    Command endCommand;
    memset(&endCommand, 0, sizeof(endCommand));
    
    endCommand.commandType = SENDFILE_END;
    strncpy(endCommand.username, receiver, sizeof(endCommand.username) - 1);
    memcpy(endCommand.key, key, sizeof(endCommand.key));
    
    endCommand.statusCode = VALID_COMMAND;
    if (!transmitToServer(&endCommand)) {
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

/**
 * @brief Display an error message for invalid commands
 * 
 * @param command The command that was invalid
 */
static void
processInvalidCommand(Command* command) {
    if (command->statusCode == INVALID_COMMAND) {
        ClientUI_TermSetPopup(term, "ERROR: Command not recognized", 1);
    }
    else if (command->statusCode == EMPTY_INPUT) {
        return;
    }
    else if (command->statusCode == NO_KEY) {
        ClientUI_TermSetPopup(term, "ERROR: Provide a key to register", 1);
    }
    else if (command->statusCode == INVALID_FORMAT) {
        ClientUI_TermSetPopup(term, "ERROR: Invalid command format", 1);
    }
}

/**
 * @brief Handle keyboard input from the user
 * 
 * @return true to continue the client loop, false to exit
 */
static bool
handleKeyboardInput() {
    char keyboardBuffer[512];
    ssize_t bytesRead = read(STDIN_FILENO, keyboardBuffer, sizeof(keyboardBuffer));

    if (bytesRead > 0) {
        int status = ClientUI_TermHandleInput(term, keyboardBuffer, bytesRead);

        if (status == 1) {
            const char* msg = ClientUI_TermTakeSubmittedLine(term);
            Command* command = parseUserInput(msg);
            
            // Print local message with the ">" indicator
            if (command->statusCode != EMPTY_INPUT) {
                char localMsg[2200];
                snprintf(localMsg, sizeof(localMsg), C_YOU ">" C_RST " %s", msg);
                ClientUI_TermAppendLine(term, localMsg);
            }
            
            if (command->statusCode == VALID_COMMAND) {
                
                // Exit the client
                if (command->commandType == QUIT) { 
                    transmitToServer(command);
                    free(command);
                    return false;
                }

                // List all available commands to the user
                else if (command->commandType == HELP) {
                    ClientUI_ShowHelp(term);
                    free(command);
                    return true;
                }

                else {
                    // Encrypt the payload if necessary

                    if (command->commandType == SENDFILE_TO) {

                        // Send file using the filename stored in command->filename
                        if (!sendFile(command->filename, command->username, command->key)) {
                            // Error already shown via popup
                        }
                        free(command);
                        return true;
                    }

                    else {
                        // all other 
                        transmitToServer(command);
                    }
                }
            }
            else { 
                processInvalidCommand(command); 
            }

            free(command);
        }

        // User exits the client
        else if (status == -1) { return false; }
    }

    return true;
}

/**
 * @brief Process a complete ServerResponse received from the server
 * 
 * @param response Pointer to the response
 */
static void
processServerResponse(ServerResponse* response) {
    if (!response) return;

    if (sessionEncrypted) {
        decryptData((uchar*)response, sizeof(ServerResponse), clientKey);
    }

    char displayMsg[1024];
    memset(displayMsg, 0, sizeof(displayMsg));

    switch (response->responseType) {
        case RES_SUCCESS:
            if (awaitingConnectResponse) {
                memcpy(clientKey, response->key, sizeof(clientKey));
                sessionEncrypted = true;
                awaitingConnectResponse = false;
            }

            snprintf(displayMsg, sizeof(displayMsg), C_GRN "%s" C_RST, response->responseContent);
            ClientUI_TermAppendLine(term, displayMsg);
            break;
        
        case RES_ERROR:
            snprintf(displayMsg, sizeof(displayMsg), C_RED "%s" C_RST, response->responseContent);
            ClientUI_TermAppendLine(term, displayMsg);
            break;
        
        case RES_PAYLOAD:
            snprintf(displayMsg, sizeof(displayMsg), C_CYN "%s: %s" C_RST,
                     response->senderUsername, response->responseContent);
            ClientUI_TermAppendLine(term, displayMsg);
            break;
        
        case RES_FILE_START:
            if (incomingFile) {
                fclose(incomingFile);
                incomingFile = NULL;
            }
            strncpy(incomingFilename, response->filename, sizeof(incomingFilename) - 1);
            incomingFile = fopen(incomingFilename, "wb");
            if (!incomingFile) {
                ClientUI_TermAppendLine(term, C_RED "ERROR: Cannot create file" C_RST);
            } else {
                expectedChunks = response->totalChunks;
                receivedChunks = 0;
                char startMsg[512];
                snprintf(startMsg, sizeof(startMsg), C_SYS "Receiving file: %s from %s" C_RST, incomingFilename, response->senderUsername);
                ClientUI_TermAppendLine(term, startMsg);
            }
            break;

        case RES_FILE_DATA:
            if (incomingFile) {
                fwrite(response->responseContent, 1, response->payloadLength, incomingFile);
                receivedChunks++;
            }
            break;

        case RES_FILE_END:
            if (incomingFile) {
                fclose(incomingFile);
                incomingFile = NULL;
                ClientUI_TermAppendLine(term, C_GRN "File received successfully" C_RST);
            }
            break;
    }
}

/**
 * @brief Handle incoming data from the server socket
 * 
 * @return true to continue the client loop, false to exit
 */
static bool
handleServerResponse() {
    uchar tempBuffer[1024];
    ssize_t bytesRead;

    // Read all available data from the socket
    while (1) {
        bytesRead = read(clientSocket, tempBuffer, sizeof(tempBuffer));

        if (bytesRead > 0) {
            // Ensure we don't overflow recvBuffer
            if (recvBufferLen + bytesRead > sizeof(recvBuffer)) {
                // This should not happen normally; treat as fatal
                close(clientSocket);
                return false;
            }

            // Append to our accumulation buffer
            memcpy(recvBuffer + recvBufferLen, tempBuffer, bytesRead);
            recvBufferLen += bytesRead;

            // Process complete ServerResponse structs
            while (recvBufferLen >= sizeof(ServerResponse)) {
                ServerResponse response;
                memcpy(&response, recvBuffer, sizeof(ServerResponse));
                processServerResponse(&response);

                // Remove the processed bytes from the buffer
                size_t remaining = recvBufferLen - sizeof(ServerResponse);
                if (remaining > 0) {
                    memmove(recvBuffer, recvBuffer + sizeof(ServerResponse), remaining);
                }
                recvBufferLen = remaining;
            }
        }
        else if (bytesRead == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data to read right now
                break;
            }
            // Other error
            close(clientSocket);
            return false;
        }
        else {
            // bytesRead == 0: server closed connection
            close(clientSocket);
            return false;
        }
    }

    return true;
}

// #################################
// ## P U B L I C   M E M B E R S ##
// #################################

void
enableClient() {
    initClientUI();

    char host[128];
    char port[6];

    // User writes the server address and port
    if (!ClientUI_PromptConnect(host, sizeof(host), port, sizeof(port))) {
        // User pressed Esc
        endClientUI();
        exit(0);
    }

    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    // In case of socket failure end the UI
    if (clientSocket == -1) {
        ClientUI_ShowMessage(
            "ERROR: Could not create a socket", 
            "Press any key to exit", 
            1
        );
        ClientUI_WaitKey();

        endClientUI();
        exit(EXIT_FAILURE);
    }

    sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));

    serverAddress = (sockaddr_in) {
        .sin_family = AF_INET,
        .sin_addr.s_addr = inet_addr(host),
        .sin_port = htons(atoi(port))
    };

    // set a timeout interval so that the screen isnt stuck on a connecting screen for eternity (3 seconds)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    ClientUI_ShowConnecting(host, port);
    if (connect(
            clientSocket, 
            (struct sockaddr*)&serverAddress, 
            sizeof(serverAddress)) == -1) 
    {
        ClientUI_ShowMessage(
            "ERROR: Could not connect to the server", 
            "Press any key to exit",
            1
        );
        ClientUI_WaitKey();

        endClientUI();
        exit(EXIT_FAILURE);
    }

    setNonBlocking(clientSocket);

    term = ClientUI_TermCreate(host, port);
    ClientUI_TermDraw(term);
}

void
clientLoop() {
    epollFD = epoll_create1(0);
    if (epollFD == -1) {
        ClientUI_TermDestroy(term);
        endClientUI();
        exit(EXIT_FAILURE);
    }

    epoll_event event;
    epoll_event events[MAX_EVENTS];

    event.events = EPOLLIN;
    event.data.fd = STDIN_FILENO;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, STDIN_FILENO, &event);

    event.events = EPOLLIN | EPOLLET;
    event.data.fd = clientSocket;
    epoll_ctl(epollFD, EPOLL_CTL_ADD, clientSocket, &event);

    bool running = true;

    while(running) {
        int activeCount = epoll_wait(epollFD, events, MAX_EVENTS, INF_TIMEOUT);

        for(int e = 0; e < activeCount; ++e) {
            int activeFD = events[e].data.fd;

            // Keyboard input
            if (activeFD == STDIN_FILENO) { running = handleKeyboardInput(); }

            // Response from the server
            else if (activeFD == clientSocket) { running = handleServerResponse(); }
        }

        ClientUI_TermDraw(term);
    }

    destroyClient();
}

void
destroyClient() {
    close(clientSocket);
    close(epollFD);
    ClientUI_TermDestroy(term);
    endClientUI();
}