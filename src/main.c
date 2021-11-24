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

typedef int (*video_cb_t)(char *h264, int len, int64_t pts, int is_key);
typedef int (*audio_cb_t)(char *aac, int len, int64_t pts);
void start_ipc_simulator(video_cb_t vcb, audio_cb_t acb);

static RtmpPubContext *rtmp_ctx;
static int aac_config_has_been_sent = 0;

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

// 模拟ipc的h264回调，模拟ipc编码一帧h264之后，回调此函数，将h264丢给应用层
int on_video(char *h264, int len, int64_t pts, int is_key)
{
	int ret = 0, offset = 0;
	// 考虑到有的startcode为3个字节的情况, 这里增加10字节防止溢出
	uint8_t *avcc = (uint8_t *)malloc(len+10);
	if (!avcc) {
		return -1;
	}
	annexB2avcc(h264, len, avcc);
	uint8_t * avcc_end = avcc + len;

	while(avcc+offset < avcc_end) {
		uint8_t nalu_type = (avcc+offset)[4]&0x1F;
		int nalu_size = ntohl(*(int *)(avcc+offset));
		assert(nalu_size > 0);
		offset += 4;
		switch(nalu_type) {
		case  NALU_TYPE_SPS:
			/* 3. 将sps数据传递给推流sdk */
			// codec将关键帧丢给应用层，一般sps/pps是随关键帧一起过来的
			RtmpPubSetVideoTimebase(rtmp_ctx, pts);
                	RtmpPubSetSps(rtmp_ctx, avcc+offset, nalu_size);
			break;
		case NALU_TYPE_PPS:
			/* 4. 将pps数据传递给推流sdk */
			// codec将关键帧丢给应用层，一般sps/pps是随关键帧一起过来的
			RtmpPubSetPps(rtmp_ctx, avcc+offset, nalu_size);
			break;
		case NALU_TYPE_IDR:
			/* 5. 发送关键帧数据 */
		 	if (RtmpPubSendVideoKeyframe(rtmp_ctx, avcc+offset, nalu_size, pts)) {
            			log("RtmpPubSendVideoKeyframe() error, errno = %d\n", errno);
				ret = -1;
				goto err;
			}
			break;
        	case NALU_TYPE_SLICE:
			/* 6. 发送非关键帧数据 */
			if (RtmpPubSendVideoInterframe(rtmp_ctx, avcc+offset, nalu_size, pts)) {
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

int on_audio(char *aac, int len, int64_t pts)
{
	if (!aac_config_has_been_sent) {
		char audioSpecCfg[] = { 0x14, 0x10 };
        	RtmpPubSetAudioTimebase(rtmp_ctx, pts);
        	RtmpPubSetAac(rtmp_ctx, audioSpecCfg, sizeof(audioSpecCfg) );
		aac_config_has_been_sent = 1;
	}
	// rtmp推流不需要adts，所以需要把adts从aac中移除
	int protection_absent = aac[1] & 0x01;
	int adts_len = protection_absent ? 7 : 9;
	/* 7. 发送aac音频 */
	if (RtmpPubSendAudioFrame(rtmp_ctx, aac+adts_len, len-adts_len, pts) < 0) {
		log("RtmpPubSendAudioFrame err");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	if (!argv[1]) {
		log("./rtmp-publish-demo <rtmp publish url>");
		return 0;
	}
	/* 1. 创建推流实例化对象 */
	rtmp_ctx = RtmpPubNew(argv[1], 10, RTMP_PUB_AUDIO_NONE, RTMP_PUB_AUDIO_NONE, RTMP_PUB_TIMESTAMP_ABSOLUTE);
    	if (RtmpPubInit(rtmp_ctx)) {
        	RtmpPubDel(rtmp_ctx);
		return 0;
	}
	/* 2. 连接流媒体服务器 */
	if (RtmpPubConnect(rtmp_ctx)) {
		log("rtmp connect err, errno:%d", errno);
		return 0;
	}
	log("rtmp connect %s success", argv[1]);
	// h264文件模拟ipc相关代码，相关代码不需要关注
	// 真实的ipc是codec编码一帧h264之后，丢给应用
	// 层, on_video/on_audio是注册到模拟ipc的
	// 回调函数，模拟的ipc采集一帧h264/aac之后，
	// 会调用这个函数，讲h264/aac丢给应用层
	start_ipc_simulator(on_video, on_audio);
	for(;;) {
		sleep(3);
	} 
	return 0;
}