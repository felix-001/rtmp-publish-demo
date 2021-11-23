#include <stdio.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <math.h>
#include <string.h>
#include <arpa/inet.h>

#define log(fmt, args...) printf("%s> "fmt"\n",  __FUNCTION__, ##args)

#define H264_FILE "../media/video.h264"
#define AAC_FILE "../media/audio.aac"
#define NAL_NON_IDR (0x01)
#define VIDEO_FRAME_INTERVAL (40) // 模拟帧率25fps

typedef int (*video_cb_t)(char *h264, int len, int64_t timestamp, int is_key);
typedef int (*audio_cb_t)(char *h264, int len, int64_t timestamp);

static video_cb_t video_cb;
static audio_cb_t audio_cb;

static int read_file_to_buf(char *file, char **buf)
{
        FILE * fp = fopen(file, "r");

        if (!fp) {
		log("open file %s err", file);
                return -1;
        }
        fseek(fp, 0, SEEK_END);
        int len = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        *buf = (char *)malloc(len);
        assert(buf != NULL);
        if (fread(*buf, 1, len, fp) < 0) {
                log("read file %s fail", file);
                fclose(fp);
                free(*buf);
                return -1;
        }
        fclose(fp);
        return len;
}

void *video_capture_thread(void *param)
{
	char *h264 = NULL;
	int h264_len = 0, offset = 0;
	int64_t timestamp = 0;

	log("enter video capture thread");
	if ((h264_len = read_file_to_buf(H264_FILE, &h264)) < 0) {
		return NULL;
	}
	while(offset < h264_len) {
		int nalu_len = ntohl(*(int*)h264);
		assert(nalu_len > 0);
		offset += 4;
		if (offset+nalu_len > h264_len) {
			// 循环读取
			offset = 0;
			continue;
		}
		int nalu_type = h264[offset+8] & 0x1F;
		video_cb(h264+offset, nalu_len, timestamp, !(nalu_type == NAL_NON_IDR));
		timestamp += VIDEO_FRAME_INTERVAL;
		usleep(VIDEO_FRAME_INTERVAL);

	}
	if (h264)
		free(h264);
	return NULL;
}

void *audio_capture_thread(void *param)
{
	return NULL;
}

void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb)
{
	pthread_t tid;

	video_cb = vcb;
	audio_cb = acb;
	pthread_create(&tid, NULL, video_capture_thread, NULL);	
	pthread_create(&tid, NULL, audio_capture_thread, NULL);	
}