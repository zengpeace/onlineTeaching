#ifndef _common_h_
#define _common_h_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <corecrt_io.h>
#include <winsock.h>

#include "pthread.h"
#include "semaphore.h"

#ifndef sleep
#define sleep(s) Sleep(s * 1000)
#endif
#define usleep(us) Sleep(us / 1000)
#define close(fd) closesocket(fd)

#define LOGD udi_erro_log
#define udi_erro_log printf
#define udi_info_log printf


#define MIN_THREAD_STACK_SIZE		(16 * 1024)
#define MIDDLE_THREAD_STACK_SIZE	(1024 * 1024)
#define NORMAL_THREAD_STACK_SIZE	(8 * 1024 * 1024)


typedef struct _BufferInfo
{
	unsigned char *pData;
	int nSize;
	int nFlag;
	struct _BufferInfo *pNext;
}BufferInfo, *lpBufferInfo;

typedef struct _BlockInfo
{
	sem_t m_Semaphore;
	pthread_mutex_t m_Lock;
	BufferInfo *m_Push;
	BufferInfo *m_Pop;
	BufferInfo *m_pInfo;
	unsigned char *m_pData;
	unsigned char *m_pCurr;
	int m_nInfoSize;
	int m_nDataSize;
}BlockInfo, *lpBlockInfo;


#define MAIN_VIDEO_STREAM_BIT_RATE_M	2
static const unsigned int udpAudioBlockSize = 800 * 1024;
static const unsigned int tcpVideoAudioBufMaxNumber = 1024;
static const unsigned int tcpVideoAudioBlockSize = MAIN_VIDEO_STREAM_BIT_RATE_M * 1024 * 1024;
//static const unsigned int tcpVideoAudioBlockSize = 1024;

int InitBlockInfoBuffer(BlockInfo *pBlockInfo, int nInfoSize, int nDataSize, unsigned char *pBuf, int nBufCount);
int pushBlock(BlockInfo *pBlockInfo, unsigned char *pData, int iSize);
int pushBlockEx(BlockInfo *pBlockInfo, const unsigned char *pData, const int iSize, const unsigned char type);
int pushBlo(BlockInfo *pBlockInfo, const unsigned char *pData, const int iSize, const unsigned char *pf, const int ifSize);
int PopBlockInfoData(BlockInfo *pBlockInfo, void(*pFunc)(const unsigned char*, const int, void*), void* arg);
int PopBlockInfoDataNotConst(BlockInfo *pBlockInfo, void(*pFunc)(unsigned char*, const int, void*), void* arg);
int PopBlockInfoDataEx(BlockInfo *pBlockInfo, void(*pFunc)(const unsigned char*, const int, void*), void* arg, int(*loopConditionFunc)(void), int timeOut);
int PopBlockInfoDataExNotConst(BlockInfo *pBlockInfo, void(*pFunc)(unsigned char*, const int, void*), void* arg, int(*loopConditionFunc)(void *arg), int timeOut);
void releaseBlock(BlockInfo *pBlockInfo);

int create_thread_small(pthread_t *pid, void*(*thread_callback)(void*), void *arg);
int create_thread_middle(pthread_t *pid, void*(*thread_callback)(void*), void *arg);
int create_thread_normal(pthread_t *pid, void*(*thread_callback)(void*), void *arg);

int initVideoAudioBlock(BlockInfo *pBlockInfo);

int gettimeofday(struct timeval *tp, void *tzp);


#endif
