#ifndef _HEVC_PATCH_H_
#define _HEVC_PATCH_H_

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

int hevc_patch_is_annexb(AVPacket *pkt);

int hevc_patch_hvcc_is_keyframe(AVPacket *pkt);

int hevc_patch_hvcc_is_keyframe_ignore_cra(AVPacket *pkt);

#endif