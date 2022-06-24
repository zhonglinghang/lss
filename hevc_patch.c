#include "hevc_patch.h"

int hevc_patch_is_annexb(AVPacket *pkt)
{
    if(pkt->size > 4) {
        if(pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 0 && pkt->data[3] == 0) {
            return 1;
        }
        if(pkt->data[0] == 0 && pkt->data[1] == 0 && pkt->data[2] == 1 && pkt->data[3] == 1) {
            return 1;
        }
    }
    return 0;
}

#define BUF2INT32(buf) ((((int)(buf)[0]) << 24) | (((int)(buf)[1]) << 16) | (((int)(buf)[2]) << 8) | ((int)(buf)[3]))

int hevc_patch_hvcc_is_keyframe(AVPacket *pkt) 
{
    int is_keyframe = 0;
    uint8_t *data = pkt->data;
    int size = pkt->size;
    for (;;) {
        if(size <= 4) {
            break;
        }
        int nal_size = BUF2INT32(data);
        if(nal_size <= 0 || 4 + nal_size > size) {
            break;
        }
        int nal_type = (data[4] & 0x7E) >> 1;
        if (16 <= nal_type && nal_type <= 23) {
            is_keyframe = 1;
        }
        data += (4 + nal_size);
        size -= (4 + nal_size);
    }
    return is_keyframe;
}

// work around for ngcodec, where cra frame appears within a gop
// segmenting ts at cra for ngcodec would cause first frame not key,
// while it's necessary to segment at cra for standard hevc
int hevc_patch_hvcc_is_keyframe_ignore_cra(AVPacket *pkt)
{
    int is_keyframe = 0;
    uint8_t *data = pkt->data;
    int size = pkt->size;
    for(;;) {
        if(size <= 4) {
            break;
        }
        int nal_size = BUF2INT32(data);
        if(nal_size <= 0 || 4 + nal_size > size) {
            break;
        }
        int nal_type = (data[4] & 0x7E) >> 1;
        if(16 <= nal_type && nal_type <= 23 && nal_type != 21) {
            is_keyframe = 1;
        }
        data += (4 + nal_size);
        size -= (4 + nal_size);
    }
    return is_keyframe;
}
