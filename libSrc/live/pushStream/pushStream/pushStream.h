#ifndef _pushStream_h_
#define _pushStream_h_

int rtmp_module_init(int *isPush);
int rtmp_module_destroy();

void push_rtmp_init(const char *url, const unsigned int frameRate, void **obj);
int push_rtmp_uninit(void **obj);

void push_rtmp_video(void *obj, unsigned char *data, unsigned int size);
void push_rtmp_audio(void *obj, const unsigned char *data, const unsigned int size);

#endif