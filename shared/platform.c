#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32

bool platformNetInit(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
void platformNetShutdown(void) { WSACleanup(); }
void platformCloseSocket(SocketFd fd) { if (fd != INVALID_SOCKET_FD) closesocket(fd); }
void platformSetSocketTimeout(SocketFd fd, uint32_t milliseconds) {
    DWORD ms = milliseconds;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
}
int platformLastSocketError(void) { return WSAGetLastError(); }

typedef struct { ThreadFn fn; void* arg; } ThreadTrampoline;
static DWORD WINAPI threadTrampoline(LPVOID p) {
    ThreadTrampoline t = *(ThreadTrampoline*)p;
    free(p);
    t.fn(t.arg);
    return 0;
}
bool platformThreadCreate(ThreadHandle* out, ThreadFn fn, void* arg) {
    ThreadTrampoline* t = malloc(sizeof(*t));
    t->fn = fn; t->arg = arg;
    *out = CreateThread(NULL, 0, threadTrampoline, t, 0, NULL);
    return *out != NULL;
}
void platformThreadJoin(ThreadHandle h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
void platformMutexInit(Mutex* m) { InitializeCriticalSection(m); }
void platformMutexDestroy(Mutex* m) { DeleteCriticalSection(m); }
void platformMutexLock(Mutex* m) { EnterCriticalSection(m); }
void platformMutexUnlock(Mutex* m) { LeaveCriticalSection(m); }
void platformCondInit(CondVar* c) { InitializeConditionVariable(c); }
void platformCondDestroy(CondVar* c) { (void)c; }
bool platformCondWait(CondVar* c, Mutex* m, uint32_t ms) { return SleepConditionVariableCS(c, m, ms) != 0; }
void platformCondSignal(CondVar* c) { WakeConditionVariable(c); }
void platformSleepMs(uint32_t ms) { Sleep(ms); }
uint64_t platformMonotonicMs(void) { return GetTickCount64(); }
uint64_t platformWallClockSeconds(void) { return (uint64_t)time(NULL); }
bool platformMkdir(const char* path) {
    if (_mkdir(path) == 0) return true;
    struct _stat st;
    return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR);
}
bool platformRenameReplace(const char* from, const char* to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

#else // POSIX

#include <stdlib.h>
#include <sys/time.h>

bool platformNetInit(void) { return true; }
void platformNetShutdown(void) {}
void platformCloseSocket(SocketFd fd) { if (fd >= 0) close(fd); }
void platformSetSocketTimeout(SocketFd fd, uint32_t milliseconds) {
    struct timeval tv = { .tv_sec = milliseconds / 1000, .tv_usec = (milliseconds % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
int platformLastSocketError(void) { return errno; }

bool platformThreadCreate(ThreadHandle* out, ThreadFn fn, void* arg) { return pthread_create(out, NULL, fn, arg) == 0; }
void platformThreadJoin(ThreadHandle h) { pthread_join(h, NULL); }
void platformMutexInit(Mutex* m) { pthread_mutex_init(m, NULL); }
void platformMutexDestroy(Mutex* m) { pthread_mutex_destroy(m); }
void platformMutexLock(Mutex* m) { pthread_mutex_lock(m); }
void platformMutexUnlock(Mutex* m) { pthread_mutex_unlock(m); }
void platformCondInit(CondVar* c) { pthread_cond_init(c, NULL); }
void platformCondDestroy(CondVar* c) { pthread_cond_destroy(c); }
bool platformCondWait(CondVar* c, Mutex* m, uint32_t ms) {
    struct timeval now;
    gettimeofday(&now, NULL);
    struct timespec deadline;
    deadline.tv_sec = now.tv_sec + ms / 1000;
    deadline.tv_nsec = (now.tv_usec + (ms % 1000) * 1000) * 1000;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &deadline) == 0;
}
void platformCondSignal(CondVar* c) { pthread_cond_signal(c); }
void platformSleepMs(uint32_t ms) { usleep(ms * 1000); }
uint64_t platformMonotonicMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}
uint64_t platformWallClockSeconds(void) { return (uint64_t)time(NULL); }
bool platformMkdir(const char* path) {
    if (mkdir(path, 0755) == 0) return true;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool platformRenameReplace(const char* from, const char* to) { return rename(from, to) == 0; }

#endif
