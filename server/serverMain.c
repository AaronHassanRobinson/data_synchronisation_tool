//
// Created by MacbookPro on 22/8/2026.
//
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <protocol.h>
#include <stdlib.h>
#include <list.h>
#define PORT 9001


// note: For doing full directory copies we should use a tree structure, but for simplicity just doing a linked list





typedef struct {
    FileItem* knownRemoteState;
    FileItem* ourCurrentState;
} GlobalState;


int main() {
    printf("Starting server\n");
    GlobalState serverState = {
        .knownRemoteState = nullptr,
        .ourCurrentState = nullptr,
    };

    // todo: check output directory and map our state.

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed\n");
        goto fail;
    }

    int opt =1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed\n");
        close(server_fd);
        goto fail;
    }
    struct sockaddr_in address; // todo: clean this up
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);


    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed\n");
        close(server_fd);
        goto fail;
    }

    if (listen(server_fd, 1) < 0) {
        perror("Listen failed\n");
        close(server_fd);
        goto fail;
    }
    printf("Listening...\n");

    socklen_t address_len = sizeof(address);
    int client_fd = accept(server_fd, (struct sockaddr *)&address, &address_len);
    if (client_fd < 0) {
        perror("Accept failed\n");
        close(server_fd);
        goto fail;
    }

    // Exchange 1 [RECV]: Versioning, awaiting a version from the client.
    InitialExchangeHeader initialHeader = {0};
    int recvResult = recv(client_fd, &initialHeader, sizeof(initialHeader), 0);
    if (recvResult < 0) {
        printf("recv failed\n");
        goto fail;
    }
    printf("Version: %d\n", initialHeader.VERSION);
    VERSION confirmedVersion = initialHeader.VERSION;
    //
    // Exchange 1 [SEND]: Versioning, confirm version with client. //todo: if we do multiple versions, confirm if we support this
    int sendResult = send(client_fd, &initialHeader, sizeof(InitialExchangeHeader), 0);

    // if we haven't got the file, i.e. we just stream the file in over TCP
    switch (confirmedVersion) {
        case RSYNC_ALGORITHM:
            // Exchange 2 [RECV]: Get the initial file list from the client and match with our target directory
            ProtocolHeader newResponse = {0};
            int hdr = recv(client_fd, &newResponse, sizeof(newResponse), 0);
            printf("Message received, type: %d, length: %d\n", newResponse.type, newResponse.length);

            char* receivedMessageBuffer = malloc(newResponse.length);
            recvResult = recv(client_fd, receivedMessageBuffer, newResponse.length, 0);

            // todo: Deserialize
            uint32_t deserializedDataLength = 0;
            void* deserializedData = deserializeMessage(newResponse.type, receivedMessageBuffer, newResponse.length, &deserializedDataLength);   // todo: last param useless atm
            // check if we have any of the files, if we need a file  completely or just a partial since the last update time
            switch (newResponse.type) {
                case QUICK_CHECK_EXCHANGE:
                    // we got a list of the clients items, need to update our global state. We can completely disregard everything in the list.
                    for (uint32_t i = 0; i < deserializedDataLength; i++) {
                        addItemToFileList(serverState.knownRemoteState, deserializedData[i]);
                    }

                    break;
                default:
                    break;
            }
            // todo: put this into a loop
        break;

    }

    // client initiates exchange:

    fail:
    return 1;
}
