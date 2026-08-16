/*
** sd_port.h -- the daemon's portability floor (PLAN-DAEMON, Linux
** shim 2026-08-16).  One small vocabulary, two implementations:
** Win32 (the daemon's birthplace) and POSIX (pthreads + BSD sockets).
**
** The rule for what belongs here: a primitive appears exactly when
** sprocketd.c needs it, with the semantics sprocketd.c relies on --
** this is not a general-purpose layer, and growing it beyond the
** daemon's needs would be dishonest about how well-tested it is.
**
** Semantics relied on (and preserved on both sides):
**   - SdEvent: manual-reset (stays signaled) or one-shot; sdEventWait
**     blocks forever; created signaled or not.
**   - SdMutex/SdCond: plain mutex + condvar; sdCondWaitMs returns
**     after the timeout OR a wake (spurious wakes allowed -- every
**     caller loops on its predicate).
**   - SdThread: create-joinable + join, or create-detached (the
**     per-connection pattern).
**   - Counters: sdAtomic* over volatile long, full-barrier.
**   - sdSockClose on a LISTENING socket must wake a blocked accept()
**     (Win32 does this on closesocket; POSIX needs shutdown first).
**   - Timeouts on data sockets wake blocked recv/send (SO_RCVTIMEO /
**     SO_SNDTIMEO on both sides).
*/
#ifndef SD_PORT_H
#define SD_PORT_H

#include <stdio.h>
#include <stdlib.h>

#define SD_PATHMAX 1024

#ifdef _WIN32
/* ===================================================== Win32 ========= */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include <io.h>

typedef SOCKET SdSocket;
typedef int SdSockLen;
#define SD_INVALID_SOCKET INVALID_SOCKET
static int sdFsync(FILE *f){ fflush(f); return _commit(_fileno(f)); }
static void sdSockInit(void){ WSADATA w; WSAStartup(MAKEWORD(2,2), &w); }
static void sdSockShutdown(void){ WSACleanup(); }
static void sdSockClose(SdSocket s){ closesocket(s); }
static void sdSockTimeouts(SdSocket s, int ms){
  DWORD v = (DWORD)ms;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
}

typedef CRITICAL_SECTION SdMutex;
static void sdMutexInit(SdMutex *m){ InitializeCriticalSection(m); }
static void sdMutexEnter(SdMutex *m){ EnterCriticalSection(m); }
static void sdMutexLeave(SdMutex *m){ LeaveCriticalSection(m); }
static void sdMutexDestroy(SdMutex *m){ DeleteCriticalSection(m); }

typedef CONDITION_VARIABLE SdCond;
static void sdCondInit(SdCond *c){ InitializeConditionVariable(c); }
static void sdCondWaitMs(SdCond *c, SdMutex *m, int ms){
  SleepConditionVariableCS(c, m, ms<0 ? INFINITE : (DWORD)ms);
}
static void sdCondWakeOne(SdCond *c){ WakeConditionVariable(c); }
static void sdCondWakeAll(SdCond *c){ WakeAllConditionVariable(c); }

typedef HANDLE SdEvent;   /* manual-reset or one-shot, per creation */
static SdEvent sdEventCreate(int bManual, int bSignaled){
  return CreateEvent(0, bManual, bSignaled, 0);
}
static void sdEventSet(SdEvent e){ SetEvent(e); }
static void sdEventWait(SdEvent e){ WaitForSingleObject(e, INFINITE); }
static void sdEventWaitMs(SdEvent e, int ms){ WaitForSingleObject(e, (DWORD)ms); }
static void sdEventDestroy(SdEvent e){ if( e ) CloseHandle(e); }
#define SD_EVENT_OK(e) ((e)!=0)

typedef HANDLE SdThread;
typedef DWORD SdThreadRet;
#define SD_THREAD_CALL WINAPI
typedef LPVOID SdThreadArg;
static SdThread sdThreadCreate(SdThreadRet (SD_THREAD_CALL *fn)(SdThreadArg),
                               void *pArg){
  return CreateThread(0, 0, fn, pArg, 0, 0);
}
static int sdThreadCreateDetached(SdThreadRet (SD_THREAD_CALL *fn)(SdThreadArg),
                                  void *pArg){
  HANDLE h = CreateThread(0, 0, fn, pArg, 0, 0);
  if( h==0 ) return 1;
  CloseHandle(h);
  return 0;
}
static void sdThreadJoin(SdThread t, int msMax){
  WaitForSingleObject(t, msMax<0 ? INFINITE : (DWORD)msMax);
  CloseHandle(t);
}
#define SD_THREAD_OK(t) ((t)!=0)

typedef volatile LONG SdCount;
static long sdAtomicInc(SdCount *p){ return InterlockedIncrement(p); }
static long sdAtomicDec(SdCount *p){ return InterlockedDecrement(p); }
static long sdAtomicAdd(SdCount *p, long v){ return InterlockedAdd(p, v); }
static long sdAtomicExchange(SdCount *p, long v){
  return InterlockedExchange(p, v);
}

static long long sdMonotonicMs(void){ return (long long)GetTickCount64(); }
static void sdSleepMs(int ms){ Sleep((DWORD)ms); }

static int sdFileExists(const char *zPath){
  return GetFileAttributesA(zPath)!=INVALID_FILE_ATTRIBUTES;
}
static int sdFileDelete(const char *zPath){ return !DeleteFileA(zPath); }
static int sdFileRename(const char *zFrom, const char *zTo){
  return !MoveFileExA(zFrom, zTo, MOVEFILE_REPLACE_EXISTING);
}
static void sdDirCreate(const char *zDir){ CreateDirectoryA(zDir, 0); }
static void sdDirRemove(const char *zDir){ RemoveDirectoryA(zDir); }

typedef struct SdDir { HANDLE h; WIN32_FIND_DATAA fd; int bFirst; } SdDir;
static int sdDirOpen(SdDir *d, const char *zDir){
  char zGlob[SD_PATHMAX];
  snprintf(zGlob, sizeof(zGlob), "%s\\*", zDir);
  d->h = FindFirstFileA(zGlob, &d->fd);
  d->bFirst = 1;
  return d->h==INVALID_HANDLE_VALUE;
}
static const char *sdDirNext(SdDir *d){
  for(;;){
    if( d->bFirst ){
      d->bFirst = 0;
    }else if( !FindNextFileA(d->h, &d->fd) ){
      return 0;
    }
    if( d->fd.cFileName[0]!='.' ) return d->fd.cFileName;
  }
}
static void sdDirClose(SdDir *d){
  if( d->h!=INVALID_HANDLE_VALUE ) FindClose(d->h);
}

#else
/* ===================================================== POSIX ========= */
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>

#include <signal.h>

typedef int SdSocket;
typedef socklen_t SdSockLen;
#define SD_INVALID_SOCKET (-1)
static int sdFsync(FILE *f){ fflush(f); return fsync(fileno(f)); }
static void sdSockInit(void){
  /* a peer vanishing mid-send must be a send() error, not a fatal
  ** SIGPIPE -- the subscriber-disconnect path relies on it */
  signal(SIGPIPE, SIG_IGN);
}
static void sdSockShutdown(void){}
static void sdSockClose(SdSocket s){
  /* a blocked accept() on this fd must wake: POSIX close() alone does
  ** not promise that, shutdown() first does (Linux: accept returns
  ** EINVAL).  Harmless on data sockets. */
  shutdown(s, SHUT_RDWR);
  close(s);
}
static void sdSockTimeouts(SdSocket s, int ms){
  struct timeval tv;
  tv.tv_sec = ms/1000;
  tv.tv_usec = (ms%1000)*1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

typedef pthread_mutex_t SdMutex;
static void sdMutexInit(SdMutex *m){ pthread_mutex_init(m, 0); }
static void sdMutexEnter(SdMutex *m){ pthread_mutex_lock(m); }
static void sdMutexLeave(SdMutex *m){ pthread_mutex_unlock(m); }
static void sdMutexDestroy(SdMutex *m){ pthread_mutex_destroy(m); }

typedef pthread_cond_t SdCond;
static void sdCondInit(SdCond *c){ pthread_cond_init(c, 0); }
static void sdCondWaitMs(SdCond *c, SdMutex *m, int ms){
  if( ms<0 ){
    pthread_cond_wait(c, m);
  }else{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms/1000;
    ts.tv_nsec += (long)(ms%1000)*1000000L;
    if( ts.tv_nsec>=1000000000L ){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &ts);
  }
}
static void sdCondWakeOne(SdCond *c){ pthread_cond_signal(c); }
static void sdCondWakeAll(SdCond *c){ pthread_cond_broadcast(c); }

typedef struct SdEventObj {
  pthread_mutex_t mx;
  pthread_cond_t cv;
  int bSignaled;
  int bManual;
} SdEventObj;
typedef SdEventObj *SdEvent;
static SdEvent sdEventCreate(int bManual, int bSignaled){
  SdEvent e = (SdEvent)malloc(sizeof(*e));
  if( e==0 ) return 0;
  pthread_mutex_init(&e->mx, 0);
  pthread_cond_init(&e->cv, 0);
  e->bSignaled = bSignaled;
  e->bManual = bManual;
  return e;
}
static void sdEventSet(SdEvent e){
  pthread_mutex_lock(&e->mx);
  e->bSignaled = 1;
  pthread_cond_broadcast(&e->cv);
  pthread_mutex_unlock(&e->mx);
}
static void sdEventWait(SdEvent e){
  pthread_mutex_lock(&e->mx);
  while( !e->bSignaled ) pthread_cond_wait(&e->cv, &e->mx);
  if( !e->bManual ) e->bSignaled = 0;
  pthread_mutex_unlock(&e->mx);
}
static void sdEventWaitMs(SdEvent e, int ms){
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms/1000;
  ts.tv_nsec += (long)(ms%1000)*1000000L;
  if( ts.tv_nsec>=1000000000L ){ ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&e->mx);
  while( !e->bSignaled ){
    if( pthread_cond_timedwait(&e->cv, &e->mx, &ts)==ETIMEDOUT ) break;
  }
  if( e->bSignaled && !e->bManual ) e->bSignaled = 0;
  pthread_mutex_unlock(&e->mx);
}
static void sdEventDestroy(SdEvent e){
  if( e==0 ) return;
  pthread_mutex_destroy(&e->mx);
  pthread_cond_destroy(&e->cv);
  free(e);
}
#define SD_EVENT_OK(e) ((e)!=0)

typedef pthread_t SdThread;
typedef void *SdThreadRet;
#define SD_THREAD_CALL
typedef void *SdThreadArg;
static SdThread sdThreadCreate(SdThreadRet (*fn)(SdThreadArg), void *pArg){
  pthread_t t;
  if( pthread_create(&t, 0, fn, pArg)!=0 ) return (pthread_t)0;
  return t;
}
static int sdThreadCreateDetached(SdThreadRet (*fn)(SdThreadArg), void *pArg){
  pthread_t t;
  pthread_attr_t a;
  int rc;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&t, &a, fn, pArg);
  pthread_attr_destroy(&a);
  return rc!=0;
}
static void sdThreadJoin(SdThread t, int msMax){
  (void)msMax;              /* POSIX join has no timeout; callers only
                            ** join threads that are known to exit */
  pthread_join(t, 0);
}
#define SD_THREAD_OK(t) ((t)!=(pthread_t)0)

typedef volatile long SdCount;
static long sdAtomicInc(SdCount *p){ return __sync_add_and_fetch(p, 1); }
static long sdAtomicDec(SdCount *p){ return __sync_sub_and_fetch(p, 1); }
static long sdAtomicAdd(SdCount *p, long v){ return __sync_add_and_fetch(p, v); }
static long sdAtomicExchange(SdCount *p, long v){
  return __sync_lock_test_and_set(p, v);
}

static long long sdMonotonicMs(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static void sdSleepMs(int ms){ usleep((useconds_t)ms*1000); }

static int sdFileExists(const char *zPath){
  struct stat st;
  return stat(zPath, &st)==0;
}
static int sdFileDelete(const char *zPath){ return unlink(zPath)!=0; }
static int sdFileRename(const char *zFrom, const char *zTo){
  return rename(zFrom, zTo)!=0;
}
static void sdDirCreate(const char *zDir){ mkdir(zDir, 0755); }
static void sdDirRemove(const char *zDir){ rmdir(zDir); }

typedef struct SdDir { DIR *d; } SdDir;
static int sdDirOpen(SdDir *d, const char *zDir){
  d->d = opendir(zDir);
  return d->d==0;
}
static const char *sdDirNext(SdDir *d){
  struct dirent *e;
  while( (e = readdir(d->d))!=0 ){
    if( e->d_name[0]!='.' ) return e->d_name;
  }
  return 0;
}
static void sdDirClose(SdDir *d){ if( d->d ) closedir(d->d); }

#endif /* _WIN32 / POSIX */
#endif /* SD_PORT_H */
