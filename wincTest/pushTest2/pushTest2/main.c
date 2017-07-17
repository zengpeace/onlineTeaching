#include "common.h"
#include "createStream.h"
#include "pushTest.h"

//static const char url[] = "rtmp://incastyun.cn/live/test";
static const char url[] = "rtmp://113.194.201.52/live/test";
static const unsigned int frameRate = 30;

extern int g_enc_start;

int main()
{
	WSADATA wsaData;
	int nRet;
	if ((nRet = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
		udi_erro_log("WSAStartup failed");
		return -6;
	}

	int needToPush = 1;
	int ret;
	ret = rtmp_module_init(&needToPush);
	if (ret < 0)
	{
		udi_erro_log("rtmp_module_init fail ! ret = %d\n", ret);
		system("pause");
		return -1;
	}

	void *rtmpObj = NULL;

	push_rtmp_init(url, frameRate, &rtmpObj);
	if (rtmpObj == NULL)
	{
		udi_erro_log("push_rtmp_init fail ! \n");
		return -2;
	}

	pthread_t createVideoDataThreadPid;
	ret = create_thread_normal(&createVideoDataThreadPid, read_h264_file_thr, (void *)&rtmpObj);
	if (ret < 0)
	{
		udi_erro_log("create_thread_normal fail ! ret = %d\n", ret);
		return -5;
	}

	char c;
	while (1)
	{
		scanf("%c", &c);
		if (c == 'q')
		{
			udi_info_log("catch input q !\n");
			break;
		}

		sleep(2);
	}

	g_enc_start = 0;
	needToPush = 0;
	ret = push_rtmp_uninit(&rtmpObj);
	if (ret < 0)
	{
		udi_erro_log("push_rtmp_uninit fail ! ret = %d\n", ret);
		return -3;
	}

	ret = rtmp_module_destroy();
	if (ret < 0)
	{
		udi_erro_log("rtmp_module_destroy fail ! ret = %d\n", ret);
		return -4;
	}
	
	return 0;
}


