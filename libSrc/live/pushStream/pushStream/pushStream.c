#include "rtmp_sys.h"
#include "rtmp.h"
#include "log.h"

#include "common.h"
#include "pushStream.h"

enum
{
	NAL_SLICE = 1,
	NAL_DPA,
	NAL_DPB,
	NAL_DPC,
	NAL_IDR_SLICE,
	NAL_SEI,
	NAL_SPS,
	NAL_PPS,
	NAL_AUD,
	NAL_END_SEQUENCE,
	NAL_END_STREAM,
	NAL_FILLER_DATA,
	NAL_SPS_EXT,
	NAL_AUXILIARY_SLICE = 19
};

typedef enum
{
	eVideoHead = 0,
	eAudioHead,
	eVideoData,
	eAudioData,
	eMaxPackageType,
}PackageType;

static const int g_rtmpLinkTimeOut = 5;
static const int64_t g_sendThreadWaitTime = 2;

static const unsigned int g_rtmp_video_package_max_size = 2 * 1024 * 1024;
static const unsigned int g_rtmp_audio_package_max_size = 2 * 1024 * 1024;
static const unsigned int g_clockTicks = 1000;

static const int g_video_channel_number = 0x04;
static const int g_audio_channel_number = 0x05;

static const unsigned int g_minVideoFrameRate = 10;
static const unsigned int g_maxVideoFrameRate = 100;

static const unsigned int g_aacSampleRateFrequency[] = { 96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,8000,8000,8000,8000,8000,8000 };
static const unsigned int g_sampleRate = 44100;
static const unsigned int g_samplePerSecond = 1024;
static const unsigned char g_audioObjectType = 2;			//5bit
															//static const unsigned char g_samplingFrequencyIndex = 4;	//4bit
static const unsigned char g_channelConfiguration = 2;		//4bit
static const unsigned char g_frameLengthFlag = 0;			//1bit	
static const unsigned char g_dependsOnCoreCoder = 0;		//1bit	
static const unsigned char g_extensionFlag = 0;				//1bit	
static const unsigned int g_audioSpecificConfigSize = 2;	//above add together, bytes

static const unsigned int g_maxDisConnectTimes = 3;
static const unsigned int g_reconnectCheckTimeInterval = 2;


typedef struct _push_rtmp_info
{
	char rtmp_url[1024]; //这个没什么卵用，留着以后调试用吧
	RTMP *rtmp_obj;
	unsigned long start_time;
	pthread_t thread_id;
	//pthread_t reconnect_thread_id;
	unsigned int threadIsRunning;
	BlockInfo packageBlock;

	RTMPPacket *video_packet_obj;
	unsigned int videoFrameRate;
	unsigned char sps[128];
	unsigned char pps[128];
	unsigned int sps_size;
	unsigned int pps_size;
	unsigned int videoFrameIndex;
	unsigned int videoFrameTick;
	unsigned char hasSendVideoHead;
	int video_channel_number;
	unsigned int lastVideoTimeStamp;
	unsigned int makeUpVideoTimeStamp;
	unsigned int makeUpVideoInternal;
	unsigned int nowMakeUpVideoTimeStamp;

	RTMPPacket *audio_packet_obj;
	unsigned int audioFrameRate;
	unsigned int audioFrameIndex;
	unsigned int audioFrameTick;	//44100Hz, 1024sample per frame
	unsigned char hasSendAudioHead;
	unsigned char audioSpecificConfig[2];
	unsigned int audioSpecificConfigSize;
	int audio_channel_number;
	unsigned int lastAudioTimeStamp;
	unsigned int makeUpAudioTimeStamp;
	unsigned int makeUpAudioInternal;
	unsigned int nowMakeUpAudioTimeStamp;

	unsigned int disConnectErrTimes;
}PUSH_RTMP_INFO;

static int sg_finish_init = 0;
static pthread_rwlock_t sg_lock;
static char sg_url[1024];
static int sg_frameRate = 30;
static void **sg_obj = NULL;
static int *sg_isPush = NULL;
static pthread_t sg_reconnectPid;
static PUSH_RTMP_INFO *sg_push_info = NULL;

static char *get_cur_time_us();
//static unsigned long GetTickCount();
static int push_rtmp_send_h264_head(PUSH_RTMP_INFO *push_obj);
static int push_rtmp_send_video(PUSH_RTMP_INFO *push_obj, const unsigned char *data, const unsigned int size);
static int GetCode(unsigned char *src, int count, unsigned char **pOut, int *iSize);
static int push_rtmp_send_aac_head(PUSH_RTMP_INFO *obj);
static int push_rtmp_send_audio(PUSH_RTMP_INFO *push_obj, const unsigned char *data, const unsigned int size);
static void createAudioSpecificConfig(unsigned char *pAudioSpecificConfig, unsigned int *pAudioSpecificConfigSize);
static unsigned char getSamplingFrequencyIndex(unsigned int sampleRate);
static void pushDataToBlock(PackageType type, const unsigned char *pData, const int dataSize, BlockInfo *pBlock);
static void* sendPackageThread(void *arg);
static void dealBlockData(const unsigned char *pData, const int dataSize, void *arg);
static void* reconnectThread(void *arg);

int rtmp_module_init(int *isPush)
{
	printf("+%s\n", __FUNCTION__);
	if (sg_finish_init)
	{
		udi_erro_log("%s: reinit !\n", __FUNCTION__);
		return -1;
	}

	if (isPush == NULL)
	{
		udi_erro_log("%s: isPush == NULL !\n", __FUNCTION__);
		return -2;
	}

	int ret = pthread_rwlock_init(&sg_lock, NULL);
	if (ret < 0)
	{
		udi_erro_log("%s:pthread_rwlock_init fail !\n", __FUNCTION__);
		return -3;
	}

	sg_finish_init = 1;
	ret = create_thread_normal(&sg_reconnectPid, reconnectThread, (void *)NULL);
	if (ret < 0)
	{
		udi_erro_log("%s:create_thread_small fail !\n", __FUNCTION__);
		sg_finish_init = 0;
		return -4;
	}

	sg_isPush = isPush;
	printf("-%s\n", __FUNCTION__);
	return 0;
}

int rtmp_module_destroy()
{
	printf("+%s\n", __FUNCTION__);
	if (sg_finish_init == 0)
	{
		udi_erro_log("%s: must init before dstroy !\n", __FUNCTION__);
		return -1;
	}

	sg_push_info = NULL;
	sg_finish_init = 0;
	sg_isPush = NULL;
	pthread_join(sg_reconnectPid, NULL);
	memset(&sg_reconnectPid, 0, sizeof(sg_reconnectPid));
	pthread_rwlock_destroy(&sg_lock);
	printf("-%s\n", __FUNCTION__);
	return 0;
}

void push_rtmp_init(const char *url, const unsigned int frameRate, void **obj)
{
	udi_info_log("!!!!!!+%s!!!!!!!!!!!!\n", __FUNCTION__);
	if (!obj)
	{
		udi_erro_log("%s: obj NULL !\n", __FUNCTION__);
		return;
	}
	pthread_rwlock_wrlock(&sg_lock);
	udi_info_log("%s: get sg_lock success !\n", __FUNCTION__);

	if (NULL == url)
	{
		LOGD("invalid param url ! url == NULL\n");
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}
	if (frameRate < g_minVideoFrameRate || frameRate > g_maxVideoFrameRate)
	{
		LOGD("invalid param frameRate ! frameRate = %u\n", frameRate);
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}

	udi_info_log("push_rtmp_init ! url=%s, frameRate=%u\n", url, frameRate);
	RTMP *rtmp_obj = NULL;
	rtmp_obj = RTMP_Alloc();
	if (NULL == rtmp_obj)
	{
		LOGD("RTMP_Alloc failed!\n");
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}
	udi_info_log("RTMP_Alloc success !\n");

	RTMP_Init(rtmp_obj);
	udi_info_log("RTMP_Init success !\n");
	rtmp_obj->Link.timeout = g_rtmpLinkTimeOut;
	RTMP_SetupURL(rtmp_obj, (char*)url);
	RTMP_EnableWrite(rtmp_obj);
	udi_info_log("prepare to RTMP_Connect !\n");
	//sleep(20);
	//udi_info_log("prepare to RTMP_Connect 2 !\n");
	int ret = RTMP_Connect(rtmp_obj, NULL);
	if (ret == 0)
	{
		LOGD("RTMP_Connect fail ! ret = %d\n", ret);
		RTMP_Close(rtmp_obj);
		RTMP_Free(rtmp_obj);
		pthread_rwlock_unlock(&sg_lock);
		LOGD("RTMP_Connect failed!\n");
		*obj = NULL;
		return;
	}
	udi_info_log("RTMP_Connect success !\n");
	if (!RTMP_ConnectStream(rtmp_obj, 0))
	{
		RTMP_Close(rtmp_obj);
		RTMP_Free(rtmp_obj);
		pthread_rwlock_unlock(&sg_lock);
		LOGD("RTMP_ConnectStream fail !\n");
		*obj = NULL;
		return;
	}
	udi_info_log("RTMP_ConnectStream success !\n");
	RTMPPacket *video_packet_obj = (RTMPPacket*)malloc(sizeof(RTMPPacket));
	if (NULL == video_packet_obj)
	{
		RTMP_Close(rtmp_obj);
		RTMP_Free(rtmp_obj);
		pthread_rwlock_unlock(&sg_lock);
		LOGD("malloc video RTMPPacket failed!\n");
		*obj = NULL;
		return;
	}
	memset(video_packet_obj, 0, sizeof(RTMPPacket));
	RTMPPacket_Alloc(video_packet_obj, g_rtmp_video_package_max_size);
	RTMPPacket_Reset(video_packet_obj);
	udi_info_log("RTMPPacket_Alloc success !\n");
	RTMPPacket *audio_packet_obj = (RTMPPacket*)malloc(sizeof(RTMPPacket));
	if (NULL == audio_packet_obj)
	{
		RTMP_Close(rtmp_obj);
		RTMP_Free(rtmp_obj);
		RTMPPacket_Free(video_packet_obj);
		free(video_packet_obj);
		pthread_rwlock_unlock(&sg_lock);
		LOGD("malloc audio RTMPPacket failed!\n");
		*obj = NULL;
		return;
	}
	memset(audio_packet_obj, 0, sizeof(RTMPPacket));
	RTMPPacket_Alloc(audio_packet_obj, g_rtmp_audio_package_max_size);
	RTMPPacket_Reset(audio_packet_obj);
	udi_info_log("RTMPPacket_Alloc success !\n");
	PUSH_RTMP_INFO *push_rtmp_info = (PUSH_RTMP_INFO*)malloc(sizeof(PUSH_RTMP_INFO));
	if (NULL == push_rtmp_info)
	{
		LOGD("malloc PUSH_RTMP_INFO failed!\n");
		RTMP_Close(rtmp_obj);
		RTMP_Free(rtmp_obj);
		RTMPPacket_Free(video_packet_obj);
		free(video_packet_obj);
		RTMPPacket_Free(audio_packet_obj);
		free(audio_packet_obj);
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}
	udi_info_log("malloc push_rtmp_info success !\n");
	memset(push_rtmp_info, 0, sizeof(PUSH_RTMP_INFO));
	strcpy(push_rtmp_info->rtmp_url, url);
	push_rtmp_info->rtmp_obj = rtmp_obj;

	push_rtmp_info->video_packet_obj = video_packet_obj;
	push_rtmp_info->videoFrameIndex = 1;
	push_rtmp_info->hasSendVideoHead = 0;
	push_rtmp_info->videoFrameTick = g_clockTicks / frameRate;
	push_rtmp_info->video_channel_number = g_video_channel_number;
	push_rtmp_info->lastVideoTimeStamp = 0;
	push_rtmp_info->makeUpVideoTimeStamp = g_clockTicks % frameRate;
	if (push_rtmp_info->makeUpVideoTimeStamp == 0)
	{
		push_rtmp_info->makeUpVideoInternal = 0;
	}
	else
	{
		push_rtmp_info->makeUpVideoInternal = frameRate / push_rtmp_info->makeUpVideoTimeStamp;
	}
	push_rtmp_info->nowMakeUpVideoTimeStamp = 0;
	push_rtmp_info->videoFrameRate = frameRate;

	push_rtmp_info->audio_packet_obj = audio_packet_obj;
	push_rtmp_info->audioFrameIndex = 1;
	push_rtmp_info->hasSendAudioHead = 0;
	push_rtmp_info->audioFrameTick = g_clockTicks * g_samplePerSecond / g_sampleRate;
	createAudioSpecificConfig(push_rtmp_info->audioSpecificConfig, &(push_rtmp_info->audioSpecificConfigSize));
	push_rtmp_info->audio_channel_number = g_audio_channel_number;
	push_rtmp_info->lastAudioTimeStamp = 0;
	push_rtmp_info->makeUpAudioTimeStamp = (g_clockTicks * g_samplePerSecond) % g_sampleRate;
	push_rtmp_info->makeUpAudioInternal = g_sampleRate / push_rtmp_info->makeUpAudioTimeStamp;
	push_rtmp_info->nowMakeUpAudioTimeStamp = 0;
	push_rtmp_info->audioFrameRate = g_sampleRate;

	udi_info_log("orgv:makeUpVideoTimeStamp,makeUpVideoInternal,videoFrameRate=%u,%u,%u\n", \
		push_rtmp_info->makeUpVideoTimeStamp, push_rtmp_info->makeUpVideoInternal, push_rtmp_info->videoFrameRate);
	udi_info_log("orga:makeUpAudioTimeStamp,makeUpAudioInternal,audioFrameRate=%u,%u,%u\n", \
		push_rtmp_info->makeUpAudioTimeStamp, push_rtmp_info->makeUpAudioInternal, push_rtmp_info->audioFrameRate);
	udi_info_log("push_rtmp_init success !\n");

	if (initVideoAudioBlock(&(push_rtmp_info->packageBlock)))
	{
		LOGD("initVideoAudioBlock fail !\n");
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}

	if (create_thread_middle(&(push_rtmp_info->thread_id), sendPackageThread, (void *)push_rtmp_info))
	{
		udi_erro_log("create thread failure {%s(%d)}\n", __FILE__, __LINE__);
		pthread_rwlock_unlock(&sg_lock);
		*obj = NULL;
		return;
	}

	strcpy(sg_url, url);
	sg_frameRate = frameRate;
	sg_obj = obj;

	push_rtmp_info->threadIsRunning = 1;
	push_rtmp_info->disConnectErrTimes = 0;
	push_rtmp_info->start_time = GetTickCount();
	*obj = push_rtmp_info;
	sg_push_info = push_rtmp_info;

	pthread_rwlock_unlock(&sg_lock);
	return;
}

int push_rtmp_uninit(void **obj)
{
	udi_info_log("!!!!!!+%s!!!!!!!!!!!!\n", __FUNCTION__);
	if (!obj)
	{
		udi_erro_log("%s: para error ! obj == NULL !\n", __FUNCTION__);
		return -1;
	}

	pthread_rwlock_wrlock(&sg_lock);
	udi_info_log("%s: get sg_lock success !\n", __FUNCTION__);
	PUSH_RTMP_INFO *push_obj = (PUSH_RTMP_INFO *)*obj;
	if (NULL == push_obj)
	{
		LOGD("invalid params\n");
		pthread_rwlock_unlock(&sg_lock);
		return -2;
	}

	RTMP_Close(push_obj->rtmp_obj);
	RTMP_Free(push_obj->rtmp_obj);
	RTMPPacket_Free(push_obj->video_packet_obj);
	free(push_obj->video_packet_obj);
	RTMPPacket_Free(push_obj->audio_packet_obj);
	free(push_obj->audio_packet_obj);

	push_obj->threadIsRunning = 0;
	pthread_join(push_obj->thread_id, NULL);
	memset(&(push_obj->thread_id), 0, sizeof(push_obj->thread_id));
	LOGD("send rtmp package thread is over !\n");

	usleep(1000 * 1000);
	releaseBlock(&(push_obj->packageBlock));

	free(push_obj);
	push_obj = NULL;
	*obj = NULL;

	pthread_rwlock_unlock(&sg_lock);
	return 0;
}

static void push_rtmp_reinit(void *arg)
{
	udi_info_log("!!!!!!!!!!! +%s !!!!!!!!!!!!!!!!\n", __FUNCTION__);
	push_rtmp_uninit(sg_obj);
	udi_info_log("finish uninit !\n");
	if (!sg_obj)
	{
		udi_erro_log("something error after first push_rtmp_init success ! sg_obj == NULL ! \n");
		return;
	}

	*sg_obj = NULL;
	push_rtmp_init(sg_url, sg_frameRate, sg_obj);
}

static void* reconnectThread(void *arg)
{
	while (sg_finish_init)
	{
		sleep(g_reconnectCheckTimeInterval);

		pthread_rwlock_rdlock(&sg_lock);
		if (sg_push_info && sg_push_info->disConnectErrTimes >= g_maxDisConnectTimes)
		{
			sg_push_info->disConnectErrTimes = 0;
			if (!sg_isPush)
			{
				udi_erro_log("%s: sg_isPush == NULL !\n", __FUNCTION__);
				pthread_rwlock_unlock(&sg_lock);
				continue;
			}

			if (*sg_isPush == 0)
			{
				udi_erro_log("%s:rtmp push disconnect overtime ! but it's not pushing ! status error !\n", __FUNCTION__);
				pthread_rwlock_unlock(&sg_lock);
				continue;
			}

			pthread_rwlock_unlock(&sg_lock);
			push_rtmp_reinit((void *)NULL);
			continue;
		}

		pthread_rwlock_unlock(&sg_lock);
	}

	return NULL;
}

void push_rtmp_video(void *obj, unsigned char *data, unsigned int size)
{
	//udi_info_log("+%s:data=%p, size=%u\n", __FUNCTION__, data, size);
	if (NULL == data || size < 0)
	{
		LOGD("data invalid data, please check!\n");
		return;
	}

	PUSH_RTMP_INFO *push_obj = (PUSH_RTMP_INFO *)obj;
	if (NULL == push_obj)
	{
		LOGD("invalid params\n");
		return;
	}

	int ret = pthread_rwlock_tryrdlock(&sg_lock);
	if (ret != 0)
	{
		udi_erro_log("%s:pthread_rwlock_tryrdlock fail !\n", __FUNCTION__);
		return;
	}

	//udi_info_log("%s get rd lock !\n", __FUNCTION__);
	if (push_obj->threadIsRunning == 0)
	{
		pthread_rwlock_unlock(&sg_lock);
		udi_info_log("%s release rd lock for push_obj->threadIsRunning !\n", __FUNCTION__);
		return;
	}

	unsigned char *pTmp = data;
	unsigned char *pBuf;
	int  ret1, size1;
	unsigned char frameKeyValue;
	for (;;)
	{
		ret1 = GetCode(pTmp, data + size - pTmp, &pBuf, &size1);
		if ((ret1 == 0) || (ret1 == 1))
		{
			if ((pBuf[0] & 0x80) == 0) //forbid bit is 0, the frame is ok to be used
			{
				frameKeyValue = pBuf[0] & 0x1F;
				switch (frameKeyValue)
				{
				case NAL_SPS:
					if (push_obj->hasSendVideoHead == 0)
					{
						memcpy(push_obj->sps, pBuf, size1);
						push_obj->sps_size = size1;
						//printf("nspsSize = %d\n", push_obj->sps_size);
					}
					break;
				case NAL_PPS:
					if (push_obj->hasSendVideoHead == 0)
					{
						memcpy(push_obj->pps, pBuf, size1);
						push_obj->pps_size = size1;
						//printf("nppsSize = %d\n", push_obj->pps_size);
					}
					break;
				case NAL_SEI:
					break;
				default:
					if (push_obj->hasSendVideoHead != 0)
					{
						//push_rtmp_send_video(push_obj, pBuf, size1);
						pushDataToBlock(eVideoData, pBuf, size1, &(push_obj->packageBlock));
					}
					else if (push_obj->sps_size > 0 && push_obj->pps_size > 0)
					{
						unsigned int iii;
						printf("sps_size=%u, sps= ", push_obj->sps_size);
						for (iii = 0; iii < push_obj->sps_size; iii++) printf("%2x ", push_obj->sps[iii]);
						printf("\n");
						printf("pps_size=%u, pps= ", push_obj->pps_size);
						for (iii = 0; iii < push_obj->pps_size; iii++) printf("%2x ", push_obj->pps[iii]);
						printf("\n");
						//push_rtmp_send_h264_head(push_obj);
						//push_rtmp_send_video(push_obj, pBuf, size1);
						pushDataToBlock(eVideoHead, NULL, 0, &(push_obj->packageBlock));
						pushDataToBlock(eVideoData, pBuf, size1, &(push_obj->packageBlock));
						push_obj->hasSendVideoHead = 1;
					}
					break;
				}
			}
		}
		pTmp = pBuf + size1;
		if ((ret1 == 0) || (ret1 == -1))
		{
			break;
		}
	}

	pthread_rwlock_unlock(&sg_lock);
	//udi_info_log("%s release rd lock !\n", __FUNCTION__);
}

void push_rtmp_audio(void *obj, const unsigned char *data, const unsigned int size)
{
	PUSH_RTMP_INFO *push_obj = (PUSH_RTMP_INFO *)obj;
	if (NULL == push_obj)
	{
		LOGD("invalid params\n");
		return;
	}
	if (NULL == data || size < 0)
	{
		LOGD("data invalid data, please check!\n");
		return;
	}

	int ret = pthread_rwlock_tryrdlock(&sg_lock);
	if (ret != 0)
	{
		udi_erro_log("%s:pthread_rwlock_tryrdlock fail !\n", __FUNCTION__);
		return;
	}

	if (push_obj->threadIsRunning == 0)
	{
		pthread_rwlock_unlock(&sg_lock);
		return;
	}

	//udi_info_log("%s get rd lock !\n", __FUNCTION__);
	//LOGD("audio before dataSize = %d\n", size);
	if (push_obj->hasSendAudioHead == 0)
	{
		push_obj->hasSendAudioHead = 1;
		//push_rtmp_send_aac_head(push_obj);
		pushDataToBlock(eAudioHead, NULL, 0, &(push_obj->packageBlock));
	}
	//push_rtmp_send_audio(push_obj, data, size);
	pushDataToBlock(eAudioData, data, size, &(push_obj->packageBlock));

	pthread_rwlock_unlock(&sg_lock);
	//udi_info_log("%s release rd lock !\n", __FUNCTION__);
}

static int push_rtmp_send_h264_head(PUSH_RTMP_INFO *push_obj)
{
	LOGD("+push_rtmp_send_h264_head\n");
	if (push_obj->sps_size == 0 || push_obj->pps_size == 0)
	{
		LOGD("push_obj->sps_size=%u,push_obj->pps_size=%u\n", (unsigned int)push_obj->sps, (unsigned int)push_obj->pps);
		return -1;
	}

	RTMPPacket*packet = NULL;
	RTMP *rtmp = NULL;
	packet = push_obj->video_packet_obj;
	rtmp = push_obj->rtmp_obj;
	if (NULL == packet || NULL == rtmp)
	{
		LOGD("packet_obj or rtmp_obj invalid, please check!\n");
		return -2;
	}
	RTMPPacket_Reset(packet);
	char *body = packet->m_body;

	if (NULL == body)
	{
		LOGD("packet m_body invalid\n");
		return -3;
	}
	int i = 0;
	body[i++] = 0x17;
	body[i++] = 0x00;
	body[i++] = 0x00;
	body[i++] = 0x00;
	body[i++] = 0x00;
	/*AVCDecoderConfigurationRecord*/
	body[i++] = 0x01;
	body[i++] = push_obj->sps[1];
	body[i++] = push_obj->sps[2];
	body[i++] = push_obj->sps[3];
	body[i++] = 0xff;
	/*sps*/
	body[i++] = 0xe1;
	body[i++] = (push_obj->sps_size >> 8) & 0xff;
	body[i++] = push_obj->sps_size & 0xff;
	memcpy(&body[i], push_obj->sps, push_obj->sps_size);
	i += push_obj->sps_size;
	/*pps*/
	body[i++] = 0x01;
	body[i++] = (push_obj->pps_size >> 8) & 0xff;
	body[i++] = (push_obj->pps_size) & 0xff;
	memcpy(&body[i], push_obj->pps, push_obj->pps_size);
	i += push_obj->pps_size;
	//packet info
	packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
	packet->m_nBodySize = i;
	packet->m_nChannel = push_obj->video_channel_number;
	packet->m_nTimeStamp = 0;
	packet->m_hasAbsTimestamp = 0;
	packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
	//packet->m_nInfoField2 = 0;//rtmp->m_stream_id;
	packet->m_nInfoField2 = rtmp->m_stream_id;
	if (!RTMP_IsConnected(rtmp))
	{
		LOGD("rtmp disconnected\n");
		//push_obj->disConnectErrTimes++;
		return -4;
	}
	if (!RTMP_SendPacket(rtmp, packet, 0))
	{
		LOGD("RTMP_SendPacket failed\n");
		return -5;
	}
	//push_obj->start_time = GetTickCount();
	return 0;
}

static int push_rtmp_send_video(PUSH_RTMP_INFO *push_obj, const unsigned char *data, const unsigned int size)
{
	//LOGD("+push_rtmp_send_video\n");
	RTMPPacket*packet = NULL;
	RTMP *rtmp = NULL;
	packet = push_obj->video_packet_obj;
	rtmp = push_obj->rtmp_obj;
	if (NULL == packet || NULL == rtmp)
	{
		LOGD("packet_obj or rtmp_obj invalid, please check!\n");
		return -1;
	}
	RTMPPacket_Reset(packet);
	char *body = packet->m_body;

	if (NULL == body)
	{
		LOGD("packet m_body invalid\n");
		return -2;
	}

	//printf("%u,%u:", push_obj->videoFrameIndex * push_obj->videoFrameTick, size);
	//int ii;
	//for (ii = 0; ii < 10; ii++)
	//{
	//	printf("%2x ", data[ii]);
	//}
	//printf("\n");

	int type = 0;
	const unsigned char *buf = data;
	int len = size;
	type = buf[0] & 0x1f;
	packet->m_nBodySize = len + 9;
	/*send video packet*/
	memset(body, 0, len + 9);
	/*key frame*/
	if (type == NAL_IDR_SLICE)
	{
		body[0] = 0x17;
	}
	else
	{
		body[0] = 0x27;
	}
	body[1] = 0x01;  /*nal unit*/
	memset(&body[2], 0, 3);
	body[5] = (len >> 24) & 0xff;
	body[6] = (len >> 16) & 0xff;
	body[7] = (len >> 8) & 0xff;
	body[8] = (len) & 0xff;
	/*copy data*/
	memcpy(&body[9], buf, len);
	packet->m_hasAbsTimestamp = 0;
	packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
	//packet->m_nInfoField2 = 0;//winsys->rtmp->m_stream_id;
	packet->m_nInfoField2 = rtmp->m_stream_id;
	packet->m_nChannel = push_obj->video_channel_number;
	packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;//RTMP_PACKET_SIZE_LARGE;
#ifdef USE_ABS_TIMESTAMP
	packet->m_nTimeStamp = GetTickCount() - push_obj->start_time;
	//udi_info_log("packet->m_nTimeStamp = %u\n", packet->m_nTimeStamp);
#else
	if (push_obj->makeUpVideoInternal > 0 && (push_obj->videoFrameIndex % push_obj->makeUpVideoInternal == 0))
	{
		if (push_obj->nowMakeUpVideoTimeStamp > push_obj->makeUpVideoTimeStamp)
		{
			LOGD("something error ! now=%u, max=%u\n", push_obj->nowMakeUpVideoTimeStamp, push_obj->makeUpVideoTimeStamp);
			push_obj->nowMakeUpVideoTimeStamp = push_obj->makeUpVideoTimeStamp;
		}
		if (push_obj->nowMakeUpVideoTimeStamp == push_obj->makeUpVideoTimeStamp)
		{
			packet->m_nTimeStamp = push_obj->lastVideoTimeStamp + push_obj->videoFrameTick;
		}
		else
		{
			//LOGD("nowMakeUpVideoTimeStamp ++\n");
			push_obj->nowMakeUpVideoTimeStamp++;
			packet->m_nTimeStamp = push_obj->lastVideoTimeStamp + push_obj->videoFrameTick + 1;
		}
	}
	else
	{
		packet->m_nTimeStamp = push_obj->lastVideoTimeStamp + push_obj->videoFrameTick;
	}
	if (push_obj->videoFrameIndex % push_obj->videoFrameRate == 0)
	{
		push_obj->nowMakeUpVideoTimeStamp = 0;
	}
#endif
	//LOGD("%6u:%6u, diff=%u\n", push_obj->videoFrameIndex, packet->m_nTimeStamp, packet->m_nTimeStamp - push_obj->lastVideoTimeStamp);
	push_obj->videoFrameIndex++;
	push_obj->lastVideoTimeStamp = packet->m_nTimeStamp;
	if (!RTMP_IsConnected(rtmp))
	{
		LOGD("rtmp disconnected\n");
		push_obj->disConnectErrTimes++;
		return -3;
	}
	if (!RTMP_SendPacket(rtmp, packet, 0))
	{
		LOGD("RTMP_SendPacket failed\n");
		return -4;
	}

	//udi_info_log("-push_rtmp_send_video success !\n", __FUNCTION__);
	return 0;
}

static int push_rtmp_send_aac_head(PUSH_RTMP_INFO *push_obj)
{
	LOGD("+push_rtmp_send_aac_head\n");
	RTMPPacket*packet = NULL;
	RTMP *rtmp = NULL;
	packet = push_obj->audio_packet_obj;
	rtmp = push_obj->rtmp_obj;
	if (NULL == packet || NULL == rtmp)
	{
		LOGD("packet_obj or rtmp_obj invalid, please check!\n");
		return -1;
	}
	RTMPPacket_Reset(packet);
	char *body = packet->m_body;
	if (NULL == body)
	{
		LOGD("packet m_body invalid\n");
		return -2;
	}
	int len = push_obj->audioSpecificConfigSize;  /*spec data长度,一般是2*/
												  /*AF 00 + AAC RAW data*/
	body[0] = 0xAF;
	body[1] = 0x00;
	memcpy(&body[2], push_obj->audioSpecificConfig, len); /*spec_buf是AAC sequence header数据*/
	packet->m_packetType = RTMP_PACKET_TYPE_AUDIO;
	packet->m_nBodySize = len + 2;
	packet->m_nChannel = push_obj->audio_channel_number;
	packet->m_nTimeStamp = 0;
	packet->m_hasAbsTimestamp = 0;
	packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
	//packet->m_nInfoField2 = 1;//rtmp->m_stream_id;
	packet->m_nInfoField2 = rtmp->m_stream_id;
	if (!RTMP_IsConnected(rtmp))
	{
		LOGD("rtmp disconnected\n");
		//push_obj->disConnectErrTimes++;
		return -3;
	}
	if (!RTMP_SendPacket(rtmp, packet, 0))
	{
		LOGD("RTMP_SendPacket failed\n");
		return -4;
	}
	return 0;
}

static int push_rtmp_send_audio(PUSH_RTMP_INFO *push_obj, const unsigned char *data, const unsigned int size)
{
	//LOGD("+push_rtmp_send_audio\n");
	if (NULL == data || size == 0)
	{
		LOGD("data invalid data, please check!\n");
		return -1;
	}
	RTMPPacket*packet = NULL;
	RTMP *rtmp = NULL;
	packet = push_obj->audio_packet_obj;
	rtmp = push_obj->rtmp_obj;
	if (NULL == packet || NULL == rtmp)
	{
		LOGD("packet_obj or rtmp_obj invalid, please check!\n");
		return -2;
	}
	RTMPPacket_Reset(packet);
	char *body = packet->m_body;
	if (NULL == body)
	{
		LOGD("packet m_body invalid\n");
		return -3;
	}
	const unsigned char *buf = data + 7;
	int len = size - 7;
	if (len>0)
	{
		/*AF 01 + AAC RAW data*/
		body[0] = 0xAF;
		body[1] = 0x01;
		memcpy(&body[2], buf, len);
		packet->m_packetType = RTMP_PACKET_TYPE_AUDIO;
		packet->m_nBodySize = len + 2;
		packet->m_nChannel = push_obj->audio_channel_number;

#ifdef USE_ABS_TIMESTAMP
		packet->m_nTimeStamp = GetTickCount() - push_obj->start_time;
#else
		if (push_obj->makeUpAudioInternal > 0 && (push_obj->audioFrameIndex % push_obj->makeUpAudioInternal == 0))
		{
			if (push_obj->nowMakeUpAudioTimeStamp > push_obj->makeUpAudioTimeStamp)
			{
				LOGD("something error ! now=%u, max=%u\n", push_obj->nowMakeUpAudioTimeStamp, push_obj->makeUpAudioTimeStamp);
				push_obj->nowMakeUpAudioTimeStamp = push_obj->makeUpAudioTimeStamp;
			}
			if (push_obj->nowMakeUpAudioTimeStamp == push_obj->makeUpAudioTimeStamp)
			{
				packet->m_nTimeStamp = push_obj->lastAudioTimeStamp + push_obj->audioFrameTick;
			}
			else
			{
				push_obj->nowMakeUpAudioTimeStamp++;
				packet->m_nTimeStamp = push_obj->lastAudioTimeStamp + push_obj->audioFrameTick + 1;
			}
		}
		else
		{
			packet->m_nTimeStamp = push_obj->lastAudioTimeStamp + push_obj->audioFrameTick;
		}
		if (push_obj->audioFrameIndex % push_obj->audioFrameRate == 0)
		{
			push_obj->nowMakeUpAudioTimeStamp = 0;
		}
#endif
		//printf("%6u:%6u, diff=%u\n", push_obj->audioFrameIndex, packet->m_nTimeStamp, packet->m_nTimeStamp - push_obj->lastAudioTimeStamp);
		push_obj->audioFrameIndex++;
		push_obj->lastAudioTimeStamp = packet->m_nTimeStamp;
		packet->m_hasAbsTimestamp = 0;
		packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
		//packet->m_nInfoField2 = 1;//rtmp->m_stream_id;
		packet->m_nInfoField2 = rtmp->m_stream_id;
		if (!RTMP_IsConnected(rtmp))
		{
			LOGD("rtmp disconnected\n");
			push_obj->disConnectErrTimes++;
			return -4;
		}
		if (!RTMP_SendPacket(rtmp, packet, 0))
		{
			LOGD("RTMP_SendPacket failed\n");
			return -5;
		}
	}
	else
	{
		LOGD("no data to send\n");
	}
	return 0;
}

static int GetCode(unsigned char *src, int count, unsigned char **pOut, int *iSize)
{
	unsigned char *pTmp = NULL;
	int  start = 0, i = 0, size;
	*pOut = NULL;
	*iSize = 0;
	for (i = 0; i <= (count - 4); i++)
	{
		if (src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 1)
		{
			if (pTmp)
			{
				size = i - start;
				if (i > 0) if (src[i - 1] == 0)  size--;
				*pOut = pTmp;
				*iSize = size;
				return 1;
			}
			start = i + 3;
			pTmp = src + i + 3;
		}
	}
	size = 0;
	if (pTmp != NULL)
	{
		size = src + count - pTmp;
	}
	if (size > 0)
	{
		*pOut = pTmp;
		*iSize = size;
		return 0;
	}
	return -1;
}

static unsigned char getSamplingFrequencyIndex(unsigned int sampleRate)
{
	unsigned char i;
	unsigned char arrayLen = sizeof(g_aacSampleRateFrequency) / sizeof(unsigned int);
	for (i = 0; i < arrayLen; i++)
	{
		if (sampleRate == g_aacSampleRateFrequency[i])
		{
			return i;
		}
	}
	LOGD("are you kidding me ? input sampleRate not found ! sampleRate = %u\n", sampleRate);
	return 4; //default sampleRate = 44100
}

static void createAudioSpecificConfig(unsigned char *pAudioSpecificConfig, unsigned int *pAudioSpecificConfigSize)
{
	if (!pAudioSpecificConfig || !pAudioSpecificConfigSize)
	{
		LOGD("input param error !\n");
		return;
	}

	unsigned char samplingFrequencyIndex = getSamplingFrequencyIndex(g_sampleRate);

	pAudioSpecificConfig[0] = ((g_audioObjectType << 3) & 0xF8) | ((samplingFrequencyIndex >> 1) & 0x07);
	pAudioSpecificConfig[1] = ((samplingFrequencyIndex << 7) & 0x80) | ((g_channelConfiguration << 3) & 0x78) | \
		((g_frameLengthFlag << 2) & 0x04) | ((g_dependsOnCoreCoder << 1) & 0x02) | (g_extensionFlag & 0x01);
	*pAudioSpecificConfigSize = g_audioSpecificConfigSize;

	LOGD("pAudioSpecificConfig:%2x,%2x,size=%u,sfIndex=%u\n", pAudioSpecificConfig[0], pAudioSpecificConfig[1], *pAudioSpecificConfigSize, samplingFrequencyIndex);
}

//static unsigned long GetTickCount()
//{
//	struct timespec ts;
//
//	clock_gettime(CLOCK_MONOTONIC, &ts);
//
//	return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
//}

static void* sendPackageThread(void *arg)
{
	PUSH_RTMP_INFO *push_rtmp_info = (PUSH_RTMP_INFO *)arg;
	BlockInfo *pBlock = &(push_rtmp_info->packageBlock);
	struct timespec ts;
	struct timeval  tv;

	while (push_rtmp_info->threadIsRunning)
	{
		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + g_sendThreadWaitTime;
		ts.tv_nsec = tv.tv_usec * 1000;
		sem_timedwait(&(pBlock->m_Semaphore), &ts);

		if (!push_rtmp_info || !pBlock)
		{
			LOGD("sendPackageThread exit due to param is NULL ! push_rtmp_info=%u, pBlock=%u\n", (unsigned int)push_rtmp_info, (unsigned int)pBlock);
			return NULL;
		}

		PopBlockInfoData(pBlock, dealBlockData, arg);
	}
	return NULL;
}

static void dealBlockData(const unsigned char *pData, const int dataSize, void *arg)
{
	PUSH_RTMP_INFO *push_rtmp_info = (PUSH_RTMP_INFO *)arg;

	//LOGD("%s:pData=%u, dataSize=%d\n", __FUNCTION__, (unsigned int)pData, dataSize);
	if (!pData || dataSize <= 0)
	{
		//LOGD("%s:param error ! pData=%u, dataSize=%d\n", __FUNCTION__, (unsigned int)pData, dataSize);
		return;
	}
	PackageType type = (PackageType)pData[0];
	if (type >= eMaxPackageType)
	{
		LOGD("package type error ! type = %u\n", (unsigned int)type);
		return;
	}

	const unsigned char *pRealData = &pData[1];
	const int realDataSize = dataSize - 1;
	switch (type)
	{
	case eVideoHead:
		push_rtmp_send_h264_head(push_rtmp_info);
		break;
	case eAudioHead:
		push_rtmp_send_aac_head(push_rtmp_info);
		break;
	case eVideoData:
		push_rtmp_send_video(push_rtmp_info, pRealData, realDataSize);
		break;
	case eAudioData:
		//LOGD("audio after dataSize = %d\n", realDataSize);
		push_rtmp_send_audio(push_rtmp_info, pRealData, realDataSize);
		break;
	default:
		LOGD("unknow type %u\n", (unsigned int)type);
		break;
	}
}

static void pushDataToBlock(PackageType type, const unsigned char *pData, const int dataSize, BlockInfo *pBlock)
{
	if (type >= eMaxPackageType || !pBlock)
	{
		LOGD("param error ! type=%u,pBlock=%u\n", (unsigned int)type, (unsigned int)pBlock);
		return;
	}

	if (type != eVideoHead && type != eAudioHead)
	{
		if (!pData || dataSize <= 0)
		{
			LOGD("param2 error ! type=%u, pData=%u, dataSize=%d\n", (unsigned char)type, (unsigned int)pData, dataSize);
			return;
		}
	}

	//udi_info_log("before pushBlockEx: %p, %d, %u\n", pData, dataSize, (unsigned char)type);
	int ret = pushBlockEx(pBlock, pData, dataSize, (unsigned char)type);
	if (ret)
	{
		LOGD("block is full ! ret = %d, type=%u, dataSize=%d\n", ret, (unsigned char)type, dataSize);
		return;
	}
}
