#ifndef EXT_SEQHEAD_H_
#define EXT_SEQHEAD_H_

#include <libavformat/avformat.h>

int ext_seqhead_search(const uint8_t *buf, int size, int *ext_size);

#endif