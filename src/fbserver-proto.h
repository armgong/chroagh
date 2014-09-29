#include <stdint.h>

struct  __attribute__((__packed__)) screen {
    char type; /* 'S' */
    uint8_t shm:1; /* Transfer data through shm */
    uint16_t width;
    uint16_t height;
    uint8_t pad[2]; /* Reserved */
    uint64_t paddr; /* shm: client buffer address */
    uint64_t sig; /* shm: signature at the beginning of buffer */
};

struct  __attribute__((__packed__)) screen_reply {
    char type; /* 'S' */
    uint8_t shm:1; /* Data was transfered through shm */
    uint8_t updated:1; /* Set to 1 if data has been updated */
    uint16_t width;
    uint16_t height;
};

/* Change resolution (query + reply) */
struct  __attribute__((__packed__)) resolution {
    char type; /* 'R' */
    uint16_t width;
    uint16_t height;
};

struct  __attribute__((__packed__)) key {
    char type; /* 'K' */
    uint8_t down:1; /* 1: down, 0: up */
    uint16_t keysym;
};

struct  __attribute__((__packed__)) mousemove {
    char type; /* 'M' */
    uint16_t x;
    uint16_t y;
};

struct  __attribute__((__packed__)) mouseclick {
    char type; /* 'C' */
    uint8_t down:1;
    uint8_t button;
};
