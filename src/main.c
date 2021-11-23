#include <stdint.h>

typedef int (*video_cb_t)(char *h264, int len, int64_t timestamp, int is_key);
typedef int (*audio_cb_t)(char *h264, int len, int64_t timestamp, int is_key);
void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb);

int main()
{
	return 0;
}