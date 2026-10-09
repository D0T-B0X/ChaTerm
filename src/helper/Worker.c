#define _DEFAULT_SOURCE // for usleep()

#include "helper/Worker.h"
#include "helper/ClientTable.h"
#include "data/LocalData.h"
#include "helper/UI/ServerUI.h"

#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

// ###################################
// ## P R I V A T E   M E M B E R S ##
// ###################################

/**
 * @brief Hash a password using SHA-256 with a given salt.
 *        The result is stored as a fixed 32-byte binary hash in the password buffer.
 *
 * @param password Pointer to the password buffer (must be at least 32 bytes).
 * @param salt 16-byte salt.
 * @return char* Returns password on success, NULL on error.
 */
static char*
hashPassword(char* password, uchar* salt) {
    if (!password || !salt) return NULL;

    const size_t SALT_LEN = 16;
    const size_t HASH_LEN = 32;
    size_t passLen = strlen(password); 

    uchar saltedPassword[SALT_LEN + passLen];
    memcpy(saltedPassword, password, passLen);
    memcpy(saltedPassword + passLen, salt, SALT_LEN);

    uchar hashedPass[HASH_LEN];
    SHA256(saltedPassword, passLen + SALT_LEN, hashedPass);

    // Copy the binary hash into the password buffer
    memcpy(password, hashedPass, HASH_LEN);
    return password;
}

/**
 * @brief XOR encrypt data in-place.
 */
static uchar*
encryptData(char* payload, size_t payloadLength, const uchar* key) {
    if (!payload || !key || payloadLength <= 0) return NULL;

    for (size_t byte = 0; byte < payloadLength; ++byte) {
        payload[byte] ^= key[byte % 16];
    }

    return (uchar*)payload;
}

/**
 * @brief XOR decrypt data in-place.
 */
static char*
decryptData(uchar* payload, size_t payloadLength, const uchar* key) {
    if (!payload || !key || payloadLength <= 0) return NULL;

    for (size_t byte = 0; byte < payloadLength; ++byte) {
        payload[byte] ^= key[byte % 16];
    }

    return (char*)payload;
}

/**
 * @brief Send a ServerResponse to a client.
 *        Handles partial writes and non-blocking socket retries.
 *        This version treats responseContent as a null-terminated string.
 */
/**
 * @brief Send a pre-filled ServerResponse struct.
 */
static void
sendServerResponseRaw(ServerResponse* resp, bool allowEncryption) {
    if (!resp) return;

    // The response is encrypted in-place, including its receiver field.
    // Preserve the real descriptor before that field is transformed.
    int receiverFD = (int)resp->receiver;

    if (allowEncryption) {
        lockClientTable();
        UserData* user = getUserByFD(resp->receiver);
        bool shouldEncrypt = user && user->registered;
        uchar key[16];
        if (shouldEncrypt) {
            memcpy(key, user->data.key, sizeof(key));
        }
        unlockClientTable();

        if (shouldEncrypt) {
            encryptData((char*)resp, sizeof(ServerResponse), key);
        }
    }

    size_t written = 0;
    size_t totalSize = sizeof(ServerResponse);
    
    while (written < totalSize) {
        ssize_t bytesSent = write(receiverFD, ((uchar*)resp) + written, totalSize - written);
    
        if (bytesSent > 0) { written += bytesSent; }
        else if (bytesSent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(1000);
            continue;
        } 
        else { break; }
    }
}

static void
sendServerResponse(
    uint32_t receiver,
    ResponseType responseType,
    const char* responseContent
) {
    if (!responseContent) return;

    ServerResponse response;
    memset(&response, 0, sizeof(response));

    response.receiver = receiver;
    response.responseType = responseType;
    response.payloadLength = strlen(responseContent) + 1;
    if (response.payloadLength > sizeof(response.responseContent)) {
        response.payloadLength = sizeof(response.responseContent);
    }
    memcpy(response.responseContent, responseContent, response.payloadLength);

    sendServerResponseRaw(&response, true);
}

/**
 * @brief Send a RES_SUCCESS response that includes the user's encryption key.
 */
static void
sendServerResponseWithKey(uint32_t receiver, const char* message, const uchar* key) {
    ServerResponse resp;
    memset(&resp, 0, sizeof(resp));

    resp.receiver = receiver;
    resp.responseType = RES_SUCCESS;
    resp.payloadLength = strlen(message) + 1;
    memcpy(resp.responseContent, message, resp.payloadLength);
    
    if (key) { memcpy(resp.key, key, 16); }
    // The CONNECT response must remain plaintext. The client needs the key
    // before it can decrypt subsequent responses.
    sendServerResponseRaw(&resp, false);
}

/**
 * @brief Send binary data as a ServerResponse payload.
 *        This is used for encrypted messages and file chunks that may contain null bytes.
 */
static void
sendServerResponseBinary(uint32_t receiver, ResponseType type, const uchar* data, size_t len) {
    if (!data || len == 0) return;

    ServerResponse resp;
    memset(&resp, 0, sizeof(resp));
    resp.receiver = receiver;
    resp.responseType = type;
    resp.payloadLength = len;
    memcpy(resp.responseContent, data, len);
    sendServerResponseRaw(&resp, true);
}

/**
 * @brief Checks whether a user is actually online and registered with the server
 */
static bool
isUserOnline(uint32_t userFD) {
    lockClientTable();

    UserData* user = getUserByFD(userFD);
    if (!user->registered) {
        unlockClientTable();
        
        char responseContent[128];
        snprintf(responseContent, 128, "ERROR: You are not connected", user->data.username);
        sendServerResponse(userFD, RES_ERROR, responseContent);

        return false;
    }
    
    unlockClientTable();
    return true;
}

// #################################
// ## P U B L I C   M E M B E R S ##
// #################################

bool
Worker_registerUser(Task* task) {
    // User is already registered
    if (isUserRegistered(task->command.username)) {
        serverLog("YELLOW", "REGISTER", "Username already exists");
        sendServerResponse(task->sender, RES_ERROR, "ERROR: username already exists");
        return false;
    }

    // 16-byte salt
    uchar salt[16];
    if (RAND_bytes(salt, 16) != 1) {
        serverLog("RED", "REGISTER", "Failed to generate salt");
        sendServerResponse(task->sender, RES_ERROR, "ERROR: could not register user");
        return false;
    }

    // Hash the password
    if (!hashPassword(task->command.password, salt)) {
        serverLog("RED", "REGISTER", "Failed to hash password");
        sendServerResponse(task->sender, RES_ERROR, "ERROR: could not register user");
        return false;
    }

    // Allocate and fill user data
    UserData* user = malloc(sizeof(UserData));
    if (!user) {
        serverLog("RED", "REGISTER", "Memory allocation failed");
        sendServerResponse(task->sender, RES_ERROR, "ERROR: could not register user");
        return false;
    }
    memset(user, 0, sizeof(UserData));

    memcpy(user->data.key, task->command.key, 16);
    memcpy(user->data.username, task->command.username, 32);
    memcpy(user->data.password.value, task->command.password, 32);
    memcpy(user->data.password.salt, salt, 16);

    uint8_t statusCode = saveNewUser(user);
    if (statusCode != FILE_WRITE_SUCCESS) {
        char logBuf[256];
        snprintf(logBuf, sizeof(logBuf), "saveNewUser failed with code %d", statusCode);
        serverLog("RED", "REGISTER", logBuf);
        sendServerResponse(task->sender, RES_ERROR, "ERROR: could not register user");
        free(user);
        return false;
    }

    serverLog("GREEN", "REGISTER", "User registered successfully");
    sendServerResponse(task->sender, RES_SUCCESS, "SUCCESS: user registered successfully");

    free(user);
    return true;
}

bool
Worker_connectUser(Task* task) {
    uchar salt[16];
    if (!getUserSalt(task->command.username, salt)) {
        sendServerResponse(
            task->sender,
            RES_ERROR,
            "ERROR: This user is not registered"
        );
        return false;
    }

    if (!hashPassword(task->command.password, salt)) {
        sendServerResponse(
            task->sender,
            RES_ERROR,
            "ERROR: could not verify user. Try again"
        );
        return false;
    }

    uint8_t verificationStatus = verifyUser(task->command.username, task->command.password);
    if (verificationStatus != USER_VALID) {
        if (verificationStatus == PASS_INVALID) {
            sendServerResponse(
                task->sender,
                RES_ERROR,
                "ERROR: incorrect password"
            );
            return false;
        }
        else {
            sendServerResponse(
                task->sender,
                RES_ERROR,
                "ERROR: could not connect user. Try again"
            );
            return false;
        }
    }

    // Retrieve the user's encryption key from storage
    uchar userKey[16];
    if (!getUserKey(task->command.username, userKey)) {
        sendServerResponse(
            task->sender, 
            RES_ERROR, 
            "ERROR: could not retrieve user key"
        );
        return false;
    }

    lockClientTable();

    UserData* user = getUserByFD(task->sender);
    if (!user) {
        unlockClientTable();
        return false;
    }

    memcpy(user->data.username, task->command.username, 32);
    memcpy(user->data.password.value, task->command.password, 32);
    memcpy(user->data.key, userKey, 16);   // store encryption key in online data
    user->registered = true;

    unlockClientTable();

    sendServerResponseWithKey(
        task->sender,
        "SUCCESS: user connected successfully",
        userKey
    );

    char logBuffer[128];
    snprintf(logBuffer, 128, "%s has connected", user->data.username);
    serverLog("GREEN", "LOGIN", logBuffer);
    return true;
}

bool
Worker_sendMessage(Task* task) {
    if (!isUserOnline(task->sender)) return false;

    lockClientTable();
    UserData* user = getUserByFD(task->sender);
    if (!user->registered) {
        unlockClientTable();
        char responseContent[128];
        snprintf(responseContent, 128, "ERROR: %s is not online", task->command.username);
        sendServerResponse(task->sender, RES_ERROR, responseContent);
        return false;
    }
    unlockClientTable();

    lockClientTable();

    // Get receiver and their key and FD
    UserData* receiverUser = getUserByUsername(task->command.username);
    if (!receiverUser) {
        unlockClientTable();
        char responseContent[128];
        snprintf(responseContent, 128, "ERROR: %s is not online", task->command.username);
        sendServerResponse(task->sender, RES_ERROR, responseContent);
        return false;
    }

    // Copy receiver's key and FD to avoid lock during actual CPU work
    uchar receiverKey[16];
    memcpy(receiverKey, receiverUser->data.key, 16);
    uint32_t receiverFD = receiverUser->FD;

    unlockClientTable();

    // Get sender's username for display on receiver side
    char senderUsername[32] = {0};
    
    lockClientTable();
    
    UserData* senderUser = getUserByFD(task->sender);
    if (senderUser) {
        strncpy(senderUsername, senderUser->data.username, sizeof(senderUsername) - 1);
    }
    
    unlockClientTable();

    // Send the response
    ServerResponse response;
    memset(&response, 0, sizeof(response));

    response.receiver = receiverFD;
    response.responseType = RES_PAYLOAD;
    response.payloadLength = task->command.payloadLength;
    memcpy(response.responseContent, task->command.payload, task->command.payloadLength);
    strncpy(response.senderUsername, senderUsername, sizeof(response.senderUsername) - 1);
    
    sendServerResponseRaw(&response, true);
    return true;
}

bool
Worker_sendFile(Task* task) {
    if (!isUserOnline(task->sender)) return false;

    Command* command = &task->command;
    if (command->commandType == SENDFILE_TO) {
        // Start file transfer
        lockClientTable();

        UserData* receiver = getUserByUsername(command->username);
        if (!receiver) {
            unlockClientTable();
            char msg[128];
            snprintf(msg, sizeof(msg), "ERROR: %s is not online", command->username);
            sendServerResponse(task->sender, RES_ERROR, msg);
            return false;
        }
        uint32_t receiverFD = receiver->FD;
        unlockClientTable();

        // Get the sender's username
        lockClientTable();
        
        UserData* user = getUserByFD(task->sender);
        char username[32];
        memcpy(username, user->data.username, 32);
        
        unlockClientTable();

        // Send RES_FILE_START to receiver
        ServerResponse startResponse;
        memset(&startResponse, 0, sizeof(startResponse));
        startResponse.responseType = RES_FILE_START;
        startResponse.receiver = receiverFD;
        startResponse.fileSize = command->fileSize;
        startResponse.totalChunks = command->totalChunks;
        memcpy(startResponse.senderUsername, username, 32);
        strncpy(startResponse.filename, command->filename, sizeof(startResponse.filename) - 1);
        sendServerResponseRaw(&startResponse, true);
        return true;
    }

    else if (command->commandType == SENDFILE_DATA) {
        // Decrypt with sender's key
        if (!decryptData(command->payload, command->payloadLength, command->key)) {
            sendServerResponse(task->sender, RES_ERROR, "ERROR: Decryption failed");
            return false;
        }

        // Get receiver key and FD
        lockClientTable();
        UserData* receiver = getUserByUsername(command->username);
        if (!receiver) {
            unlockClientTable();
            return false;
        }

        uint32_t receiverFD = receiver->FD;
        uchar receiverKey[16];
        memcpy(receiverKey, receiver->data.key, 16);
        unlockClientTable();

        // Send RES_FILE_DATA (binary safe)
        ServerResponse dataResp;
        memset(&dataResp, 0, sizeof(dataResp));
        dataResp.responseType = RES_FILE_DATA;
        dataResp.receiver = receiverFD;
        dataResp.payloadLength = command->payloadLength;
        memcpy(dataResp.responseContent, command->payload, command->payloadLength);
        sendServerResponseRaw(&dataResp, true);
        return true;
    }

    else if (command->commandType == SENDFILE_END) {
        // Forward end
        lockClientTable();
        UserData* receiver = getUserByUsername(command->username);
        if (!receiver) {
            unlockClientTable();
            return false;
        }
        uint32_t receiverFD = receiver->FD;
        unlockClientTable();

        ServerResponse endResp;
        memset(&endResp, 0, sizeof(endResp));
        endResp.responseType = RES_FILE_END;
        endResp.receiver = receiverFD;
        sendServerResponseRaw(&endResp, true);
        return true;
    }
    
    return false;
}

bool
Worker_listOnlineUsers(Task* task) {
    char* onlineUsers = malloc(2048);

    getListOfOnlineUsers(onlineUsers, 2048);

    // Send the list back to the requesting client
    sendServerResponse(task->sender, RES_SUCCESS, onlineUsers);
    free(onlineUsers);
    
    return true;
}

bool
Worker_sendGoodbye(Task* task) {
    lockClientTable();
    
    UserData* user = getUserByFD(task->sender);
    
    char goodbyeLog[128];
    snprintf(goodbyeLog, sizeof(goodbyeLog), "%s has disconnected", user->data.username);
    
    serverLog("GREEN", "DISCONNECT", goodbyeLog);
    
    unlockClientTable();
}

void
Worker_handleUnrecognizedCommand(uint32_t clientFD) {
    sendServerResponse(clientFD, RES_ERROR, "ERROR: Unrecognized command");
}