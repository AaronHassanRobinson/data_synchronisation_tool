//
// Created by MacbookPro on 25/8/2026.
//

#ifndef DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#define DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
#include <stdint.h>

// Main header
#pragma pack(push, 1)


typedef enum {
    INITIAL_EXCHANGE              = 0,
    QUICK_CHECK_EXCHANGE          = 1,
    QUICK_CHECK_EXCHANGE_RESPONSE = 2,
} MessageType;

typedef struct {
    MessageType type;
    uint32_t length;
} ProtocolHeader;

typedef enum {
    RSYNC_ALGORITHM = 1,
} VERSION;

// 1. first we send is a version number
typedef struct {
    VERSION VERSION;
} InitialExchangeHeader;

typedef struct {
    uint32_t totalFiles;
    uint32_t totalBytes;
} QuickCheckHeader;

typedef struct {
    uint64_t timeModified;
    uint32_t fileSize;
} QuickCheckItemHeader;

#pragma pack(pop)


void* serializeMessage(MessageType type, uint32_t dataLength, uint32_t dataItems, void* data, uint32_t* outputLength);
void* deserializeMessage(MessageType type, void* messageBuffer,
                        uint32_t messageBufferLength,  uint32_t* outputLength);
#endif //DATA_SYNCHRONISATION_TOOL_PROTOCOL_H
