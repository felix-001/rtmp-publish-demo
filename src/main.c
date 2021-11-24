#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <assert.h>
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

typedef int (*video_cb_t)(char *h264, int len, int64_t timestamp, int is_key);
typedef int (*audio_cb_t)(char *, int len, int64_t timestamp);
void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb);

static RtmpPubContext *rtmp_ctx;

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

int on_video(char *h264, int len, int64_t pts, int is_key)
{
	int ret = 0, offset = 0;
	uint8_t *avcc = (uint8_t *)malloc(len);
	if (!avcc) {
		return -1;
	}
	annexB2avcc(h264, len, avcc);
	uint8_t * avcc_end = avcc + len;

	while(avcc+offset < avcc_end) {
		uint8_t nalu_type = avcc[4]&0x1F;
		int nalu_size = ntohl(*(int *)avcc);
		assert(nalu_size > 0);
		log("nalu_size:%d", nalu_size);
		offset += 4;
		switch(nalu_type) {
		case  NALU_TYPE_SPS:
			log("set sps");
			RtmpPubSetVideoTimebase(rtmp_ctx, pts);
                	RtmpPubSetSps(rtmp_ctx, avcc, nalu_size);
			break;
		case NALU_TYPE_PPS:
			log("set pps");
			RtmpPubSetPps(rtmp_ctx, avcc, nalu_size);
			break;
		case NALU_TYPE_IDR:
			log("send idr");
		 	if (RtmpPubSendVideoKeyframe(rtmp_ctx, avcc, nalu_size, pts)) {
            			log("RtmpPubSendVideoKeyframe() error, errno = %d\n", errno);
				ret = -1;
				goto err;
			}
			break;
        	case NALU_TYPE_SLICE:
			log("send slice");
			if (RtmpPubSendVideoInterframe(rtmp_ctx, avcc, nalu_size, pts)) {
            			log("RtmpPubSendVideoInterframe() error, errno = %d\n", errno);
				ret = -1;
				goto err;
			}
			break;
		default:
			break;
		}
		offset += nalu_size;
	}

err:
	if (avcc)
		free(avcc);
	return ret;
}

int on_audio(char *h264, int len, int64_t timestamp)
{
	return 0;
}

int main(int argc, char *argv[])
{
	if (!argv[1]) {
		log("./rtmp-publish-demo <rtmp publish url>");
		return 0;
	}
	rtmp_ctx = RtmpPubNew(argv[1], 10, RTMP_PUB_AUDIO_AAC, RTMP_PUB_AUDIO_AAC, RTMP_PUB_TIMESTAMP_ABSOLUTE);
    	if (RtmpPubInit(rtmp_ctx)) {
        	RtmpPubDel(rtmp_ctx);
		return 0;
	}
	if (RtmpPubConnect(rtmp_ctx)) {
		log("rtmp connect err, errno:%d", errno);
		return 0;
	}
	log("rtmp connect %s success", argv[1]);
	start_ipc_simulator(on_video, on_audio);
	for(;;) {
		sleep(3);
	} 
	return 0;
}