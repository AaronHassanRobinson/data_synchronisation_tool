//
// Created by MacbookPro on 22/8/2026.
//
#include <stdio.h>
#include <unistd.h>
#include <sys/syslimits.h>

#define SJ_IMPL 1
#include <math.h>
#include <sj.h> // json library I pulled in - source mentioned in shared/sj.h
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <protocol.h>
#define CLIENT_CONFIG "../../client/clientConfig.json"
#define MAX_IP_SIZE 16

// Returns the string of the file contents, or null on failure.
// `fileSize` is also returned as we dynamically allocate memory for this
// todo: look into making this static and move to shared lib
// todo: add more compiler warnings
// todo: Upload to git
// todo: consider making a src folder
char* readFileToString(const char* fileName, long* fileSize) {
    char* result = nullptr;
    FILE* file = fopen(fileName, "rb");
    if (file == NULL) goto fail;

    // Read to the end of the file to get the total amount of bytes we need to allocate
    if (fseek(file, 0, SEEK_END) != 0) {
        printf("error seeking to end of file\n");
        fclose(file);
        goto fail;
    }
    *fileSize = ftell(file);
    if (*fileSize < 0) {
        printf("error using ftell to get file size\n");
        fclose(file);
        goto fail;
    }

    result = malloc(*fileSize + 1);
    if (result == NULL) {
        printf("error allocating memory\n");
        fclose(file);
        goto fail;
    }
    // return to start of file:
    rewind(file);

    // read contents:
    const size_t bytesRead = fread(result, 1, *fileSize, file);
    if (bytesRead < (size_t)fileSize && ferror(file)) {
        perror("Error reading file");
        free(result);
        fclose(file);
        goto fail;
    }

    result[bytesRead] = '\0';
    fclose(file);

    fail:
    return result;
}

// note: this is borrowed from the sj.h demo file "object.c"
bool sj_eq(sj_Value val, char *s) {
    size_t len = val.end - val.start;
    return strlen(s) == len && !memcmp(s, val.start, len);
}
// also borrowed:
void sj_print(sj_Value val) {
    printf("%.*s\n", (int)(val.end-val.start), val.start);
}

void sj_copyStringToValue(char* dest, const sj_Value source) {
    memcpy(dest, source.start, source.end-source.start);
}

int main() {
    int error = 0;
    // configuration options:
    char serverIp[MAX_IP_SIZE] = {0};
    uint16_t serverPort = 0;
    uint32_t scanInterval = 0;
    char directoryPath[PATH_MAX] = {0};

    // todo: cmdline args
    printf("Data synchronisation tool:\n");

    // first check if our local config file exists and get a pointer to the data:
    long fileSize = 0;
    char* localConfig = readFileToString(CLIENT_CONFIG, &fileSize);
    if (localConfig == NULL) {
            printf("error reading config :(\n");
            error = -1;
            goto fail;
    }

    printf("Config loaded - total size: %ld\n", fileSize);

    // load our config:
    sj_Reader newReader = sj_reader(localConfig, fileSize);
    sj_Value newJsonObject = sj_read(&newReader);
    sj_Value key, val;
    while (sj_iter_object(&newReader, newJsonObject, &key, &val)) {
        // todo: make this a switch
        if (sj_eq(key, "server_ip_address")) {
            sj_copyStringToValue(serverIp, val);
            printf("Server IP address: %s\n", serverIp);
        }
        if (sj_eq(key, "server_port")) {
            char x[6];
            sj_copyStringToValue(x, val);
            printf("Server port: %s\n", x);
            serverPort = atoi(x);
        }
        if (sj_eq(key, "scan_interval_seconds")) {
            char x[32];
            sj_copyStringToValue(x, val);
            printf("Scan interval (seconds): %s\n", x);
            scanInterval = atoi(x); //todo: change to strtol or custom
        }
        if (sj_eq(key, "directory_paths")) {
            sj_copyStringToValue(directoryPath, val);
            printf("Directory to watch: %s\n", directoryPath);
        }
    }
    free(localConfig);

    // begin rsync algorithm:

    // 1. Divide a file up into 'S' blocks: Where 'S' is the sqrt of file size (or 700) whichever is bigger
    // 1.5 get filesize:
    struct stat sb;
    long long testTxtFileSize = 0;
    if (stat("../../client/test.txt", &sb) == 0) {
        testTxtFileSize = sb.st_size;
    }
    else {
        printf("error reading file\n");
        goto fail;
    }
    printf("File size: %lld\n", testTxtFileSize);

    printf("Calculating block size...\n");
    double sqrtFileSize = floor(sqrt((double)testTxtFileSize));
    printf("sqrtFileSize: %f\n", sqrtFileSize);

    double blockSize = (sqrtFileSize > 700) ? sqrtFileSize : 700;

    printf("Block size: %f\n", blockSize);


    // Connect to server:
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET; // IPv4 protocol family
    serv_addr.sin_port = htons(serverPort);

    if (inet_pton(AF_INET, serverIp, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address or Address not supported");
        close(client_fd);
        goto fail;
    }

    printf("Connecting to server at %s:%d...\n", serverIp, serverPort);

    if (connect(client_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection Failed");
        close(client_fd);
        goto fail;
    }

    printf("Connected!\n");
    // Exchange 1 [SEND]: Versioning (not rly necessary atm lol)
    printf("Sending version number...\n");
    const InitialExchangeHeader initialHeader = {RSYNC_ALGORITHM};
    int sendResult = send(client_fd, &initialHeader, sizeof(InitialExchangeHeader), 0);
    if (sendResult < 0) {
        perror("send error");
        goto fail;
    }

    // Exchange 1 [RECV]: Versioning, awaiting confirmation from server on a version
    printf("Awaiting Versioning response...\n");
    InitialExchangeHeader initialResponse = {0};
    int recvResult = recv(client_fd, &initialResponse, sizeof(initialResponse), 0);
    if (recvResult < 0) {
        printf("recv failed\n");
        goto fail;
    }
    if (initialResponse.VERSION != initialHeader.VERSION) {
        printf("Version's do not match\n");
        goto fail;
    }
    else {
        printf("Version matches. beginning initial information exchange\n");
    }
    switch (initialHeader.VERSION) {
        case 1:
            // Exchange 2 [SEND]: File list exchange. At the moment just going to do a "quick check",
            // where we send the file name, the file size, and the last time it was modified.
            // U could run the checksum here too but for now lets just get something working.

            // Format will be:
            // [main header][QuickCheckHeader: total files, total size of msg]List[QuickCheckItemHeader][ItemNameString\0]...
            // Interestingly, rsync skips the main header, and goes for an approach where it streams items in,
            // where the motivation is that you don't know how long a scan will take, so you send items one by one
            char* fileName = "test.txt";
            char* msg = malloc (sizeof(QuickCheckItemHeader) + strlen(fileName) + 1);
            QuickCheckItemHeader* newQuickCheckItemHeader = (QuickCheckItemHeader*)msg;
            *newQuickCheckItemHeader = (QuickCheckItemHeader){
                .fileSize = sb.st_size,
                .timeModified = sb.st_mtimespec.tv_nsec,    // st_mtimespec is platform specific to apple and freeBSD...
            };
            strcpy(msg + sizeof(QuickCheckItemHeader), fileName);   //todo: insecure but whatever
            uint32_t dataLength = strlen(fileName) + 1 + sizeof(QuickCheckItemHeader);
            uint32_t bufferLength = 0;
            void* messageBuffer = serializeMessage(QUICK_CHECK_EXCHANGE, dataLength, 1, msg, &bufferLength);
            printf("Serialized data: %d\n", bufferLength);

            // Ideally here u would apply some sort of compression, but for this example, just going to send over tcp

            printf("Sending file list...\n");
            sendResult = send(client_fd, messageBuffer, bufferLength, 0);
            free(messageBuffer);
            free(msg);
        break;

    }


    printf("Terminating...\n");
    fail:
    return error;
}
