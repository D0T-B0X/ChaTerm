#include <signal.h>
#include <stdlib.h>

#include "helper/UI/AppMenu.h"
#include "server/Server.h"
#include "client/Client.h"

void runServer();
void runClient();

int 
main() {
    // Dead sockets dont kill the server/client
    signal(SIGPIPE, SIG_IGN);

    switch (getInitialMode()) {
        case MODE_SERVER:
            runServer();
            break;
        case MODE_CLIENT:
            runClient();
            break;
        case MODE_EXIT:
            exit(0);
    }

    return 0;
}

void
runServer() {
    enableServer();
    serverLoop();
}

void
runClient() {
    enableClient();
    clientLoop();
}