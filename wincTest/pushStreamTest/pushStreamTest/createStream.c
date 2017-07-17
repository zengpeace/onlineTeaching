#include "common.h"
#include "pushStream.h"
#include "createStream.h"

int g_enc_start = 1;

static const int test_file_buf_size[200] = { 52724,28610,15016,11643,11498,11718,11520,11928,12233,11862,11770,12363,11947,
12432,13004,13365,12928,12586,11981,11961,12102,11940,12266,12000,11884,11619,11614,11637,11919,12034,12374,
11637,11596,12815,13450,13878,11413,13135,12775,12631,12551,13902,13511,13693,12832,13387,12578,
12449,12478,12823,13466,12379,13760,11928,13159,13314,13283,13291,13420,12882,13552,12053,13661,
13053,13247,12604,13096,12746,12465,12327,13676,12923,13059,12835,13233,13256,12097,13563,12711,
13187,12788,13168,13179,13620,12970,13490,12459,12834,12626,12808,46231,10129,10522,10032,10755,
10394,10364,10183,10843,10779,10602,12092,11741,12858,12303,12583,13325,14856,12018,12209,13239,
13399,12588,12993,13138,14488,13270,13005,12163,13196,12073,13442,13118,12878,12909,12804,12775,
13128,12975,13071,12719,12744,13707,13291,13268,14452,13170,12388,12989,12792,13190,13699,13642,
12748,12924,13104,12596,12744,12940,12787,13750,13823,12424,13339,13076,13017,13036,13193,12914,
13036,12637,13028,13477,12384,13719,14098,12677,12497,13278,13395,12021,13070,13843,13899,13250,
14300,16978,11281,12638,11187,47543,9611,9144,9129,9287,9583,9358,9322,9551,11046,12375,
12621,13048,12913,13209,12743,12066,12458,14071,11929 };

void *read_h264_file_thr(void *arg)
{
	void **rtmpObj = arg;

	FILE *pFile;
	int nFilePosition = 0;
	char data[1024 * 1024] = { 0 };

	pFile = fopen("./1080P60_2.h264", "rb");
	if (NULL == pFile)
	{
		printf("\r\n In read_h264_file_thr fopen failed!\r\n");
		return 0;
	}

	printf("\r\n start read file..........................\r\n");
	int readBytes;
	while (1 == g_enc_start)
	{
		readBytes = fread(data, 1, test_file_buf_size[nFilePosition], pFile);
		if (test_file_buf_size[nFilePosition] == readBytes)
		{
			push_rtmp_video(*rtmpObj, (unsigned char *)data, (unsigned int)readBytes);
		}
		else
		{
			printf("read error!!! readBytes = %d, test_file_buf_size[%d] = %d\n", readBytes, nFilePosition, test_file_buf_size[nFilePosition]);
		}

		nFilePosition++;

		if (nFilePosition >= 200)
		{
			fseek(pFile, 0, SEEK_SET);
			nFilePosition = 0;
		}

		Sleep(33);
	}

	printf("\r\n In read_h264_file_thr exit!\r\n");

	return NULL;
}
