#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include "rtmp_publish.h"

#define log(fmt, args...) printf("%s $ "fmt"\n", __FUNCTION__, ##args)

#define NALU_TYPE_SLICE     1
#define NALU_TYPE_DPA       2
#define NALU_TYPE_DPB       3
#define NALU_TYPE_DPC       4
#define NALU_TYPE_IDR       5
#define NALU_TYPE_SEI       6
#define NALU_TYPE_SPS       7
#define NALU_TYPE_PPS       8
#define NALU_TYPE_AUD       9
#define NALU_TYPE_EOSEQ     10
#define NALU_TYPE_EOSTREAM  11
#define NALU_TYPE_FILL      12

typedef int (*video_cb_t)(char *h264, int len, int64_t pts, int is_key);
typedef int (*audio_cb_t)(char *aac, int len, int64_t pts);
void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb);

static RtmpPubContext *rtmp_ctx;
static int aac_config_has_been_sent = 0;
static pthread_mutex_t mutex;

const uint8_t *avc_find_startcode_internal(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *a = p + 4 - ((intptr_t)p & 3);

    for (end -= 3; p < a && p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    for (end -= 3; p < end; p += 4) {
        uint32_t x = *(const uint32_t*)p;
//      if ((x - 0x01000100) & (~x) & 0x80008000) // little endian
//      if ((x - 0x00010001) & (~x) & 0x00800080) // big endian
        if ((x - 0x01010101) & (~x) & 0x80808080) { // generic
            if (p[1] == 0) {
                if (p[0] == 0 && p[2] == 1)
                    return p;
                if (p[2] == 0 && p[3] == 1)
                    return p+1;
            }
            if (p[3] == 0) {
                if (p[2] == 0 && p[4] == 1)
                    return p+2;
                if (p[4] == 0 && p[5] == 1)
                    return p+3;
            }
        }
    }

    for (end += 3; p < end; p++) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1)
            return p;
    }

    return end + 3;
}

const uint8_t *avc_find_startcode(const uint8_t *p, const uint8_t *end)
{
    const uint8_t *out = avc_find_startcode_internal(p, end);
    if(p<out && out<end && !out[-1]) out--;
    return out;
}

int rw_int32(uint8_t *buf_in, uint8_t *buf_out)
{
	for (int i=0; i<4; i++) {
		buf_out[i] = buf_in[4-i-1];
	}
	return 0;
}

int write_nalu_size(uint8_t *buf_out, int nalu_size)
{
	return rw_int32((uint8_t *)&nalu_size, buf_out); 
}

// nalu startcode 00000001 to nalu size
int annexB2avcc(const uint8_t *buf_in, int buf_size, uint8_t *buf_out)
{
    const uint8_t *p = buf_in;
    const uint8_t *end = p + buf_size;
    const uint8_t *nal_start, *nal_end;

    nal_start = avc_find_startcode(p, end);
    for (;;) {
        while (nal_start < end && !*(nal_start++));
        if (nal_start == end)
            break;

        nal_end = avc_find_startcode(nal_start, end);
	write_nalu_size(buf_out, nal_end - nal_start);
	buf_out += 4;
	memcpy(buf_out, nal_start, nal_end - nal_start);
	buf_out += nal_end - nal_start;
        nal_start = nal_end;
    }
    return 0;
}

// ??????ipc???h264???????????????ipc????????????h264??????????????????????????????h264???????????????
int on_video(char *h264, int len, int64_t pts, int is_key)
{
	int ret = 0, offset = 0;
	// ???????????????startcode???3??????????????????, ????????????10??????????????????
	uint8_t *avcc = (uint8_t *)malloc(len+10);
	if (!avcc) {
		return -1;
	}
	annexB2avcc(h264, len, avcc);
	uint8_t * avcc_end = avcc + len;

	pthread_mutex_lock(&mutex);
	while(avcc+offset < avcc_end) {
		uint8_t nalu_type = (avcc+offset)[4]&0x1F;
		int nalu_size = ntohl(*(int *)(avcc+offset));
		assert(nalu_size > 0);
		offset += 4;
		switch(nalu_type) {
		case  NALU_TYPE_SPS:
			/* 3. ???sps?????????????????????sdk */
			// codec????????????????????????????????????sps/pps??????????????????????????????
			RtmpPubSetVideoTimebase(rtmp_ctx, pts);
                	RtmpPubSetSps(rtmp_ctx, avcc+offset, nalu_size);
			//log("set sps");
			break;
		case NALU_TYPE_PPS:
			/* 4. ???pps?????????????????????sdk */
			// codec????????????????????????????????????sps/pps??????????????????????????????
			RtmpPubSetPps(rtmp_ctx, avcc+offset, nalu_size);
			//log("set pps");
			break;
		case NALU_TYPE_IDR:
			/* 5. ????????????????????? */
		 	if (RtmpPubSendVideoKeyframe(rtmp_ctx, avcc+offset, nalu_size, pts)) {
            			log("RtmpPubSendVideoKeyframe() error, errno = %d\n", errno);
				ret = -1;
				goto err;
			}
			//log("send idr");
			break;
        	case NALU_TYPE_SLICE:
			/* 6. ???????????????????????? */
			if (RtmpPubSendVideoInterframe(rtmp_ctx, avcc+offset, nalu_size, pts)) {
            			log("RtmpPubSendVideoInterframe() error, errno = %d\n", errno);
				ret = -1;
				goto err;
			}
			//log("send slice");
			break;
		default:
			break;
		}
		offset += nalu_size;
	}

err:
	pthread_mutex_unlock(&mutex);
	if (avcc)
		free(avcc);
	return ret;
}

int on_audio(char *aac, int len, int64_t pts)
{
	pthread_mutex_lock(&mutex);
	if (!aac_config_has_been_sent) {
		char audioSpecCfg[] = { 0x14, 0x10 };
        	RtmpPubSetAudioTimebase(rtmp_ctx, pts);
        	RtmpPubSetAac(rtmp_ctx, audioSpecCfg, sizeof(audioSpecCfg) );
		aac_config_has_been_sent = 1;
	}
	// rtmp???????????????adts??????????????????adts???aac?????????
	int protection_absent = aac[1] & 0x01;
	int adts_len = protection_absent ? 7 : 9;
	/* 7. ??????aac?????? */
	if (RtmpPubSendAudioFrame(rtmp_ctx, aac+adts_len, len-adts_len, pts) < 0) {
		log("RtmpPubSendAudioFrame err, %s", strerror(errno));
		pthread_mutex_unlock(&mutex);
		return -1;
	}
	pthread_mutex_unlock(&mutex);
	//log("send aac");
	return 0;
}

int main(int argc, char *argv[])
{
	if (!argv[1]) {
		log("./rtmp-publish-demo <rtmp publish url>");
		return 0;
	}
	/* 1. ??????????????????????????? */
	rtmp_ctx = RtmpPubNew(argv[1], 30, RTMP_PUB_AUDIO_AAC, RTMP_PUB_AUDIO_AAC, RTMP_PUB_TIMESTAMP_ABSOLUTE);
    	if (RtmpPubInit(rtmp_ctx)) {
        	RtmpPubDel(rtmp_ctx);
		return 0;
	}
	/* 2. ???????????????????????? */
	if (RtmpPubConnect(rtmp_ctx)) {
		log("rtmp connect err, errno:%d", errno);
		return 0;
	}
	pthread_mutex_init(&mutex, NULL);
	log("rtmp connect %s success", argv[1]);
	// h264????????????ipc??????????????????????????????????????????
	// ?????????ipc???codec????????????h264?????????????????????
	// ???, on_video/on_audio??????????????????ipc???
	// ????????????????????????ipc????????????h264/aac?????????
	// ???????????????????????????h264/aac???????????????
	start_ipc_simulator(on_video, on_audio);
	for(;;) {
		sleep(3);
	} 
	return 0;
}