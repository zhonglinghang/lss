#include "ext_seqhead.h"
#include <libavutil/intreadwrite.h>

static const uint8_t PATTERN[] = { 0xcd, 0xcd, 0x20, 0x19, 0x03, 0xdc, 0xdc, 0x00};

static const int PINLEN = sizeof(PATTERN);

int ext_seqhead_search(const uint8_t *buf, int size, int *ext_psize)
{
    int i;
    for(i = 0; i< size - PINLEN; i++) {
        if(0 == memcmp(buf + i, PATTERN, PINLEN)) {
            int ext_size = AV_RB24(buf + i + PINLEN);
            int pos = i + PINLEN + 3;
            if(pos + ext_size <= size) {
                *ext_psize = ext_size;
                return pos;
            }
        }
    }
    return -1;
}