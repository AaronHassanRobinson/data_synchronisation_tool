//
// Thin OS shim so the rest of the code base can be written once against
// POSIX-shaped names. Windows (Winsock2 / Win32 threads / _mkdir) and POSIX
// (BSD sockets / pthreads / mkdir) differ in small, annoying ways; every one
// of those differences lives here and nowhere else.
//
#ifndef DATA_SYNCHRONISATION_TOOL_PLATFORM_H
#define DATA_SYNCHRONISATION_TOOL_PLATFORM_H
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  typedef SOCKET SocketFd;
  #define INVALID_SOCKET_FD INVALID_SOCKET
  #define PATH_SEPARATOR '\\'
  typedef HANDLE ThreadHandle;
  typedef CRITICAL_SECTION Mutex;
  typedef CONDITION_VARIABLE CondVar;
  typedef long long ssize_t_compat;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <errno.h>
  typedef int SocketFd;
  #define INVALID_SOCKET_FD (-1)
  #define PATH_SEPARATOR '/'
  typedef pthread_t ThreadHandle;
  typedef pthread_mutex_t Mutex;
  typedef pthread_cond_t CondVar;
#endif

// --- sockets ---
bool platformNetInit(void);   // WSAStartup on Windows, no-op elsewhere
void platformNetShutdown(void);
void platformCloseSocket(SocketFd fd);
// Sets SO_RCVTIMEO/SO_SNDTIMEO so a dead peer can't hang us forever.
void platformSetSocketTimeout(SocketFd fd, uint32_t milliseconds);
int platformLastSocketError(void);

// --- threads / sync ---
typedef void* (*ThreadFn)(void*);
bool platformThreadCreate(ThreadHandle* out, ThreadFn fn, void* arg);
void platformThreadJoin(ThreadHandle handle);
void platformMutexInit(Mutex* m);
void platformMutexDestroy(Mutex* m);
void platformMutexLock(Mutex* m);
void platformMutexUnlock(Mutex* m);
void platformCondInit(CondVar* c);
void platformCondDestroy(CondVar* c);
// Waits up to `milliseconds`; returns false on timeout. Mutex must be held.
bool platformCondWait(CondVar* c, Mutex* m, uint32_t milliseconds);
void platformCondSignal(CondVar* c);

// --- misc ---
void platformSleepMs(uint32_t milliseconds);
uint64_t platformMonotonicMs(void);
uint64_t platformWallClockSeconds(void);
// mkdir for a single path component; true if it now exists (created or already there).
bool platformMkdir(const char* path);
// rename() with replace-existing semantics on every platform.
bool platformRenameReplace(const char* from, const char* to);

#endif //DATA_SYNCHRONISATION_TOOL_PLATFORM_H
