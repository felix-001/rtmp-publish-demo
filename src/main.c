#include <stdint.h>
#include <unistd.h>
#include <stdio.h>

#define log(fmt, args...) printf("%s $ "fmt"\n", __FUNCTION__, ##args)

typedef int (*video_cb_t)(char *h264, int len, int64_t timestamp, int is_key);
typedef int (*audio_cb_t)(char *, int len, int64_t timestamp);
void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb);

int on_video(char *h264, int len, int64_t timestamp, int is_key)
{
	return 0;
}

int on_audio(char *h264, int len, int64_t timestamp)
{
	return 0;
}

int main()
{
	start_ipc_simulator(on_video, on_audio);
	for(;;) {
		sleep(3);
	} 
	return 0;
}