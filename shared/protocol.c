//
// Created by MacbookPro on 28/8/2026.
//
#include <protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// allocates memory for a serialized msg:
void* serializeMessage(const MessageType type, uint32_t dataLength, const uint32_t dataItems, void* data, uint32_t* outputLength) {
    void* serializedMessage = NULL;

    // all msgs will have the main header:
    ProtocolHeader newHeader = {
        .type = type,
        .length = 0     // to fill in.
    };

    switch (type) {
        case QUICK_CHECK_EXCHANGE:
            // data will be the item headers:
            QuickCheckHeader newQuickCheckHeader = {
                .totalFiles = dataItems,
                .totalBytes = dataLength
            };
            *outputLength = sizeof(newHeader) + sizeof(newQuickCheckHeader) + dataLength;
            serializedMessage = malloc(*outputLength);
            // Pack data into the buffer:
            void* offset = serializedMessage;
            newHeader.length = sizeof(QuickCheckHeader) + dataLength;
            memcpy(offset, &newHeader, sizeof(newHeader));
            offset+= sizeof(newHeader);
            memcpy(offset, &newQuickCheckHeader, sizeof(newQuickCheckHeader));
            offset += sizeof(newQuickCheckHeader);
            memcpy(offset, data, dataLength);
            break;

        default:
            printf("error: unknown message type\n"); break;
            goto fail;
    }


    fail:
    return serializedMessage;
}

// outputArray returned
void* deserializeMessage(const MessageType type, void* messageBuffer,
                        uint32_t messageBufferLength,  uint32_t* outputLength) {
    void* deserializedData = NULL;
    switch (type) {
        case QUICK_CHECK_EXCHANGE:
            // we are going to get an array of all the files or directories,
            QuickCheckHeader* receievedHdr = (QuickCheckHeader*)messageBuffer;
            *outputLength = receievedHdr->totalFiles;
            deserializedData = messageBuffer += sizeof(QuickCheckHeader);


            break;

        default:
        printf("error: unknown message type\n"); break;
        goto fail;
    }

    fail:
    return deserializedData;
}