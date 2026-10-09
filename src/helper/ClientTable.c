#include "helper/ClientTable.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#define INITIAL_TABLE_SIZE 10
#define MAX_USERS 1280

// The server *SHOULD* (gambling) be able to safely handle all clients using 8k FDs
#define MAX_FDS 8192

typedef struct ClientTable
{
    UserData*   userTable;
    uint16_t    tableSize;
    uint16_t    userCount;
} ClientTable;

// ###################################
// ## P R I V A T E   M E M B E R S ##
// ###################################

static ClientTable          clientTable;
static pthread_mutex_t      tableLock;
static int16_t              fdMap[MAX_FDS];

static uint16_t
getUserIndexByFD(uint32_t FD) {
    if (FD >= MAX_FDS) {
        return USER_NOT_FOUND; // safety check
    }

    int16_t userIndex = fdMap[FD];

    // If the user doesn't exist yet return an error code
    if (userIndex == -1) { return USER_NOT_FOUND; }
    return userIndex;
}

// #################################
// ## P U B L I C   M E M B E R S ##
// #################################

void
clientTableInit() {
    tableLock = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER;

    clientTable.userCount = 0;
    clientTable.tableSize = INITIAL_TABLE_SIZE;
    clientTable.userTable = NULL;

    // Initialize the map with sentinel values (will later change to the actual user index)
    for(int16_t fd = 0; fd < MAX_FDS; ++fd) { fdMap[fd] = -1; }

    uint8_t iter = 0;
    while(!clientTable.userTable) {
        if (iter == 10) {
            printf("ERROR: Could not create Client Table\n");
            exit(-1);
        }

        clientTable.userTable = malloc(sizeof(UserData) * INITIAL_TABLE_SIZE);
        
        // time for the OS to breathe and clean up memory (atleast one of us is getting some sleep)
        if (!clientTable.userTable) {
            sleep(1); 
            iter++;
        }
    }
}

uint16_t
connectUserInClientTable(UserData user) {
    if (user.FD >= MAX_FDS) {
        return NO_CONNECT;
    }

    lockClientTable();

    // Double the size of the "users" array to accomodate more users
    if (clientTable.userCount == clientTable.tableSize) {

        // The program can only handle 1280 users at once
        if (clientTable.userCount == MAX_USERS) { 
            unlockClientTable();
            return TABLE_FULL; 
        }       

        UserData* temp = realloc(
            clientTable.userTable, 
            (clientTable.tableSize * 2) * sizeof(UserData)
        );

        // no space for the new user
        if (!temp) {
            unlockClientTable(); 
            return NO_CONNECT; 
        }

        clientTable.tableSize *= 2;
        clientTable.userTable = temp;
    }

    // Map the user data to its FD for O(1) retrieval later (ong)
    fdMap[user.FD] = clientTable.userCount;

    clientTable.userTable[clientTable.userCount] = user;
    clientTable.userCount++;

    unlockClientTable();
    return 0;
}

uint16_t
disconnectUserFromClientTable(uint32_t userFD) {
    lockClientTable();

    uint16_t userIndex = getUserIndexByFD(userFD);

    // Return an appropriate error if the user was not found
    if (userIndex == USER_NOT_FOUND) {
        unlockClientTable(); 
        return USER_NOT_FOUND; 
    }

    // Save the FD of the user being removed
    uint32_t deletedFD = clientTable.userTable[userIndex].FD;

    // Shift the table back by one to fill the space taken by the deleted entry
    for(uint16_t index = userIndex + 1; index < clientTable.userCount; ++index) {
        clientTable.userTable[index - 1] = clientTable.userTable[index];
        
        // Update fdMap for the moved user
        fdMap[clientTable.userTable[index - 1].FD] = index - 1;
    }

    // Clear the mapping for the deleted FD
    fdMap[deletedFD] = -1;
    clientTable.userCount--;

    // Half the table size if occupancy falls below a quarter
    if (clientTable.tableSize / 4 >= clientTable.userCount &&
        clientTable.tableSize > INITIAL_TABLE_SIZE) {

        UserData* temp = realloc(
            clientTable.userTable, 
            (clientTable.tableSize / 2) * sizeof(UserData)
        );

        // Not really a major error (client table remains untouched)
        if (!temp) { 
            unlockClientTable();
            return NO_SHRINK; 
        }

        clientTable.tableSize /= 2;
        clientTable.userTable = temp;
    }

    unlockClientTable();
    return 0;
}

void
getListOfOnlineUsers(char* users, size_t usersSize) {
    if (!users || usersSize == 0) return;

    lockClientTable();

    // Build a string with all online users
    users[0] = '\0';
    snprintf(users, usersSize, "Online users:\n");

    for (uint16_t i = 0; i < clientTable.userCount; ++i) {
        // Skip users who are not yet registered/connected
        if (!clientTable.userTable[i].registered) continue;

        size_t used = strlen(users);
        if (used >= usersSize) break;

        snprintf(
            users + used,
            usersSize - used,
            "  - %s\n",
            clientTable.userTable[i].data.username
        );
    }

    unlockClientTable();
}

UserData*
getUserByFD(uint32_t FD) {
    uint16_t userIndex = getUserIndexByFD(FD);
    
    // go back empty if you cant get nothin
    if (userIndex == USER_NOT_FOUND) { return NULL; }
    
    return &clientTable.userTable[userIndex];
}

UserData*
getUserByUsername(const char* username) {
    if (!username) return NULL;

    for(uint16_t user = 0; user < clientTable.userCount; ++user) {
        if (strncmp(username, clientTable.userTable[user].data.username, 32) == 0) {
            // return the user's information
            return &clientTable.userTable[user];
        }
    }

    // The user isnt online
    return NULL;
}

void
lockClientTable() {
    pthread_mutex_lock(&tableLock);
}

void
unlockClientTable() {
    pthread_mutex_unlock(&tableLock);
}