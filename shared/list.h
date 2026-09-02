//
// Created by MacbookPro on 28/8/2026.
//

#ifndef DATA_SYNCHRONISATION_TOOL_LIST_H
#define DATA_SYNCHRONISATION_TOOL_LIST_H
#include <stdint.h>

typedef enum {
    FILE_TYPE_FILE      = 0,
    FILE_TYPE_DIRECTORY = 1,
} FileType;


typedef struct FileItem {
    struct FileItem* next;
    FileType fileType;
    uint64_t timeModified;
    uint32_t fileSize;
    uint32_t nameLength;
    char nameBuffer[];
} FileItem;

#endif //DATA_SYNCHRONISATION_TOOL_LIST_H
