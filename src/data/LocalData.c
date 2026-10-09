#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/file.h>

#include "data/LocalData.h"
#include "helper/UI/ServerUI.h"   

#define MAX_USERS 1280

static char* localDataPath = NULL;

void
getLocalData() {
    char* path = getenv("CHATERM_PATH");
    if (path) {
        localDataPath = strdup(path);
    } else {
        const char* home = getenv("HOME");
        if (!home) home = "/tmp";
        size_t len = strlen(home) + strlen("/.local/ChaTerm/users.dat") + 1;
        localDataPath = malloc(len);
        snprintf(localDataPath, len, "%s/.local/ChaTerm/users.dat", home);
    }

    // Ensure the directory exists (create full path)
    char* lastSlash = strrchr(localDataPath, '/');
    if (lastSlash) {
        *lastSlash = '\0';
        char mkdirCmd[512];
        snprintf(mkdirCmd, sizeof(mkdirCmd), "mkdir -p \"%s\"", localDataPath);
        system(mkdirCmd);   // create directory tree
        *lastSlash = '/';
    }

    char logBuf[512];
    snprintf(logBuf, sizeof(logBuf), "Using data file: %s", localDataPath);
    serverLog("GREEN", "LOCAL DATA", logBuf);
}

bool
isUserRegistered(const char* username) {
    FILE* localDataFile = fopen(localDataPath, "rb");
    if (!localDataFile) return false;
    
    if (flock(fileno(localDataFile), LOCK_SH) != 0) {
        fclose(localDataFile);
        return false;
    }

    UserRecord record;

    while(fread(&record, sizeof(UserRecord), 1, localDataFile) == 1) {
        if (strcmp(record.username, username) == 0) { 
            flock(fileno(localDataFile), LOCK_UN);
            fclose(localDataFile);
            return true; 
        }
    }

    flock(fileno(localDataFile), LOCK_UN);
    fclose(localDataFile);
    return false;
}

uint8_t
verifyUser(const char* username, const char* password) {
    FILE* localData = fopen(localDataPath, "rb");
    if (!localData) return FILE_NO_OPEN;

    if (flock(fileno(localData), LOCK_SH) != 0) {
        fclose(localData);
        return FILE_IN_USE;
    }

    UserRecord record;

    while(fread(&record, sizeof(UserRecord), 1, localData) == 1) {
        if (strcmp(record.username, username) == 0) {
            // Compare 32-byte hashed password
            if (memcmp(record.password.value, password, 32) == 0) {
                flock(fileno(localData), LOCK_UN);
                fclose(localData);
                return USER_VALID;
            } else {
                flock(fileno(localData), LOCK_UN);
                fclose(localData);
                return PASS_INVALID;
            }
        }
    }

    flock(fileno(localData), LOCK_UN);
    fclose(localData);
    return USER_INVALID;   // username not found
}

uint8_t
saveNewUser(UserData* user) {
    FILE* localDataFile = fopen(localDataPath, "a+b");
    if (!localDataFile) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Cannot open %s: %s", localDataPath, strerror(errno));
        serverLog("RED", "LOCAL DATA", buf);
        return FILE_NO_OPEN;
    }

    if (flock(fileno(localDataFile), LOCK_EX)) {
        fclose(localDataFile);
        serverLog("RED", "LOCAL DATA", "Failed to lock file");
        return FILE_IN_USE;
    }

    UserRecord record;
    rewind(localDataFile);

    while(fread(&record, sizeof(UserRecord), 1, localDataFile)) {
        if (strncmp(record.username, user->data.username, 32) == 0) {
            flock(fileno(localDataFile), LOCK_UN);
            fclose(localDataFile);
            serverLog("YELLOW", "LOCAL DATA", "Username already exists in file");
            return USER_ALREADY_EXISTS;
        }
    }

    if (fwrite(&(user->data), sizeof(UserRecord), 1, localDataFile) != 1) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Write failed: %s", strerror(errno));
        serverLog("RED", "LOCAL DATA", buf);
        flock(fileno(localDataFile), LOCK_UN);
        fclose(localDataFile);
        return FILE_WRITE_ERROR;
    }

    flock(fileno(localDataFile), LOCK_UN);
    fclose(localDataFile);
    return FILE_WRITE_SUCCESS;
}

uchar*
getUserSalt(const char* username, uchar* salt) {
    FILE* localDataFile = fopen(localDataPath, "rb");
    if (!localDataFile) return NULL;

    if (flock(fileno(localDataFile), LOCK_SH) != 0) { 
        fclose(localDataFile); 
        return NULL;
    }

    UserRecord record;
    while(fread(&record, sizeof(UserRecord), 1, localDataFile)) {
        if (strncmp(record.username, username, 32) == 0) {
            flock(fileno(localDataFile), LOCK_UN);
            fclose(localDataFile);
            memcpy(salt, record.password.salt, 16);   
            return salt;
        }
    }

    flock(fileno(localDataFile), LOCK_UN);
    fclose(localDataFile);
    return NULL;
}

uchar*
getUserKey(const char* username, uchar* key) {
    FILE* localDataFile = fopen(localDataPath, "rb");
    if (!localDataFile) return NULL;

    if (flock(fileno(localDataFile), LOCK_SH) != 0) { 
        fclose(localDataFile); 
        return NULL;
    }

    UserRecord record;
    while(fread(&record, sizeof(UserRecord), 1, localDataFile)) {
        if (strncmp(record.username, username, 32) == 0) {
            flock(fileno(localDataFile), LOCK_UN);
            fclose(localDataFile);
            memcpy(key, record.key, 16);   // copy 16‑byte key
            return key;
        }
    }

    flock(fileno(localDataFile), LOCK_UN);
    fclose(localDataFile);
    return NULL;
}