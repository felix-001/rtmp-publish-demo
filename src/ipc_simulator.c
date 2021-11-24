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

/*
* 本文件用于使用文件模拟ipc采集一帧h264/aac后，丢给应用层
* 与sdk的使用没有关系，sdk用户可以不用关注此文件
* 真实的场景是ipc片上codec采集视音频数据后，丢给应用层
*/

#define log(fmt, args...) printf("%s() "fmt"\n",  __FUNCTION__, ##args)

#define H264_FILE "../media/video.h264"
#define AAC_FILE "../media/audio.aac"
#define NAL_NON_IDR (0x01)
#define VIDEO_FRAME_INTERVAL (40) // 模拟帧率25fps

typedef int (*video_cb_t)(char *h264, int len, int64_t pts, int is_key);
typedef int (*audio_cb_t)(char *aac, int len, int64_t pts);

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

// 使用h264文件模拟ipc codec编码一帧h264，回调给应用层
// 模拟的帧率是25fps,实际的场景是cam sensor采集到yuv/rgb
// 经过codec编码为h264之后，丢给应用层
void *video_capture_simulator_thread(void *param)
{
	char *h264 = NULL;
	int h264_len = 0, offset = 0;
	int64_t pts = 0;

	log("enter video capture simulator thread");
	if ((h264_len = read_file_to_buf(H264_FILE, &h264)) < 0) {
		return NULL;
	}
	assert(h264_len > 0);
	for(;;) {
		int nalu_len = 0;
		memcpy(&nalu_len, h264+offset, 4);
		//log("len:%x", nalu_len);
		assert(nalu_len > 0);
		offset += 4;
		int nalu_type = h264[offset+8] & 0x1F;
		video_cb(h264+offset, nalu_len, pts, !(nalu_type == NAL_NON_IDR));
		offset += nalu_len;
		if (offset >= h264_len) {
			// 循环读取
			offset = 0;
			//log("rewind");
			continue;
		}
		pts += VIDEO_FRAME_INTERVAL;
		usleep(VIDEO_FRAME_INTERVAL*1000);

	}
	if (h264)
		free(h264);
	return NULL;
}

static int aacfreq[13] = {96000, 88200,64000,48000,44100,32000,24000, 22050 , 16000 ,12000,11025,8000,7350};

// 使用aac文件模拟ipc codec编码一帧aac，回调给应用层
// 实际的场景是摄像头采集到一帧pcm，编码为aac，丢给应用层
void *audio_capture_thread(void *param)
{
	FILE *fp = fopen(AAC_FILE, "r");
	if (!fp) {
		log("open file %s err", AAC_FILE);
		return NULL;
	}
	log("enter audio capture simulator thread");
	char *buf = (char *)malloc(7);
	if (!buf) {
		log("malloc err");
		return NULL;
	}
	int nb_buf = 7;
	int64_t pts = 0;
	for (;;) {
		if (fread(buf, 1, 7, fp) < 0) {
			log("fread err");
			goto err;
		}
		uint8_t syncword = (buf[0] << 4) | (buf[1] >> 4);
		if (syncword != 0xFFF) {
			log("check syncword err");
			goto err;
		}
		short frame_length = ((buf[3] & 0x3) << 11) | (buf[4] << 3) | (buf[5] >> 5);
		log("frame_length:%d", frame_length);
		assert(frame_length > 7);
		if (frame_length > nb_buf) {
			buf = (char *)realloc(buf, frame_length);
			nb_buf = frame_length;
		}
		if (fread(buf+7, 1, frame_length-7, fp) < 0) {
			log("fread err");
			goto err;
		}
		int sampling_freq_idx = (buf[2] >> 2) & 0xF;
		pts += ((1024*1000.0)/aacfreq[sampling_freq_idx]);
		audio_cb(buf, frame_length, pts);
		if (feof(fp)) {
			// 循环读取
			fseek(fp, 0, SEEK_SET);
		}
		usleep(((1024*1000.0)/aacfreq[sampling_freq_idx])*1000);

	}
err:
	if (buf)
		free(buf);
	return NULL;
}

void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb)
{
	pthread_t tid;

	video_cb = vcb;
	audio_cb = acb;
	pthread_create(&tid, NULL, video_capture_simulator_thread, NULL);	
	pthread_create(&tid, NULL, audio_capture_thread, NULL);	
	log("ipc simulator started");
}