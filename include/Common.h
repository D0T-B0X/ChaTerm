#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint64_t Key;
typedef unsigned char uchar;

// ------- Server -------

typedef struct {
    uchar           value[32];
    uchar           salt[16];   
} Password;

typedef struct {
    char            key[16];
    Password        password;
    char            username[32];
} UserRecord;

typedef struct {
    UserRecord      data;
    uint32_t        FD;
    bool            registered;

    // Per user buffers to handle server-client communication
    uchar           buffer[10000];
    size_t          receivedBytes;
} UserData;

typedef enum {
    RES_SUCCESS,
    RES_ERROR,
    RES_PAYLOAD,
    RES_FILE_START,     
    RES_FILE_DATA,      
    RES_FILE_END        
} ResponseType;

typedef struct {
    ResponseType responseType;
    uint32_t     receiver;
    uint32_t     fileSize;        
    uint32_t     totalChunks;     
    char         filename[256];   
    uchar        responseContent[2024];
    char         senderUsername[32];
    uint16_t     payloadLength;
    char         key[16];
} ServerResponse;

// ------- Client -------

typedef enum {
    REGISTER,
    CONNECT,
    SEND_TO,
    SENDFILE_TO,
    SENDFILE_DATA,      
    SENDFILE_END,       
    QUIT,
    LIST,
    HELP
} CommandType;

typedef struct {
    CommandType     commandType;
    uint32_t        statusCode;
    uint32_t        payloadLength;
    char            payload[2048];
    char            username[32];
    char            password[32];
    char            key[16];

    // File transfer fields
    uint32_t        fileSize;
    uint32_t        chunkIndex;
    uint32_t        totalChunks;
    char            filename[256];
} Command;


// ------- Synchronization -------

typedef struct Task {
    uint32_t           sender;
    Command            command;
    struct Task*       next;
} Task;

#endif // __COMMON_H__