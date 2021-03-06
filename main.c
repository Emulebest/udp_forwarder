// http://www.gnu.org/software/libc/manual/html_node/Feature-Test-Macros.html
#define _XOPEN_SOURCE   600
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <time.h>
#include "rs/rs.h"

#define error(fmt, ...)       \
    {fprintf(stderr, "UdpForwarder %s:%d ERROR " fmt "\n", \
    __FILE__, __LINE__, ##__VA_ARGS__); \
    fprintf(stderr, "%s\r\n", strerror( errno ) );}

//#define log_debug(fmt, ...)     \
//    fprintf(stderr, "UdpForwarder %s(%s):%d DEBUG " fmt "\n", \
//    __FILE__, __func__, __LINE__, ##__VA_ARGS__)

#define log_debug(fmt, ...)     \
    ;

//-------------------------------------------------------------------


typedef struct list_s {
    struct list_s *prev;
    struct list_s *next;
} list_t;

void list_new(list_t *l) {
    l->next = l;
    l->prev = l;
}

// Add n after p

void list_add(list_t *n, list_t *p) {
    p->next->prev = n;
    n->next = p->next;
    p->next = n;
    n->prev = p;
}

// Add n before p

void list_add_tail(list_t *n, list_t *p) {
    p->prev->next = n;
    n->prev = p->prev;
    p->prev = n;
    n->next = p;
}

// Remove p

void list_del(list_t *p) {
    if (p->prev)
        p->prev->next = p->next;
    if (p->next)
        p->next->prev = p->prev;
    p->prev = NULL;
    p->next = NULL;
}

#define list_elt(p, t, m)       \
    ((void*)((char*)p - offsetof(t, m)))

//-------------------------------------------------------------------

#define BUFSIZE 2000
#define BUFLISTSIZE 300
#define FRAMEBUFSIFE 200
// 5 ms
#define TOKENINTERVAL 1000000L
// TOKENINTERVAL*MAXTOKENS = MBps
#define MAXTOKENS 5

#define FRAG_UNIT_A 24
#define FRAG_UNIT_B 28
#define NON_IDR_UNIT 1
enum FragmentationUnit {
    A,
    B
};

#define FU_HEADER_START 128
#define FU_HEADER_END 64
enum FuHeader {
    Start,
    End,
    Middle
};

typedef struct buf_s {
    char buf[BUFSIZE];
    int n;
    list_t list;
} buf_t;

typedef struct bo_video_packet_s {
    uint timestamp;
    uint fec_group;
    ushort fec_index;
    ushort data_packets;
    ushort total_packets;
    char payload[1986];

} bo_video_packet_t;

enum FragmentationUnit get_fragmentation_unit_type(buf_t* buf) {
    switch (buf->buf[12] & 31) {
        case FRAG_UNIT_A:
            return A;
        case FRAG_UNIT_B:
            return B;
        case NON_IDR_UNIT:
            return A;
        default:
            return -1;
    }
}

enum FuHeader get_fu_header_type(buf_t* buf) {
    switch (buf->buf[13] & 192) {
        case FU_HEADER_START:
            return Start;
        case FU_HEADER_END:
            return End;
        default:
            return Middle;
    }
}

uint get_timestamp(const char* const buf) {
    return (unsigned char)buf[4] << 24 | (unsigned char)buf[5] << 16 | (unsigned char)buf[6] << 8 | (unsigned char)buf[7];
}

ushort get_sequence_number(const char* const buf) {
    ushort sequence_num = (unsigned char)buf[2] << 8 | (unsigned char)buf[3];
    return sequence_num;
}
void form_bo_video_packet(char* buf, uint fec_group, ushort fec_index, ushort data_packets, ushort total_packets, uint timestamp) {
    bo_video_packet_t* packet = (bo_video_packet_t*)buf;
    packet->timestamp = timestamp;
    packet->fec_group = fec_group;
    packet->fec_index = fec_index;
    packet->data_packets = data_packets;
    packet->total_packets = total_packets;
}

void form_bo_video_packet_group(buf_t* frame_buf, uint* fec_group, int frame_buffer_end) {
    buf_t *header = &frame_buf[0];
    uint timestamp = get_timestamp(header->buf);
    ushort fec_index = 0;
    for (int idx = 0; idx <= frame_buffer_end; idx++) {
        buf_t *buf_elt = &frame_buf[idx];
        form_bo_video_packet(buf_elt->buf, *fec_group, fec_index++, frame_buffer_end, frame_buffer_end, timestamp);
    }
    *fec_group = *fec_group + 1;
}

unsigned char** generate_parity(buf_t* frame_buf, int frame_buffer_end, int parity_amount) {
    buf_t *header = &frame_buf[0];
    int lowest_sequence_number = get_sequence_number(header->buf);
    int total_packets = parity_amount + frame_buffer_end;
    unsigned char** packets = malloc(total_packets * (sizeof(unsigned char*)));
    reed_solomon *rs = reed_solomon_new(frame_buffer_end, parity_amount);
    for (int idx = 0; idx <= frame_buffer_end; idx++) {
        buf_t *buf_elt = &frame_buf[idx];
        int sequence_num = get_sequence_number(buf_elt->buf);
        int sequence_idx = sequence_num - lowest_sequence_number;
        packets[sequence_idx] = (unsigned char*) buf_elt->buf;
    }
    for (int i = frame_buffer_end; i < total_packets; i++) {
        packets[i] = (unsigned char*) malloc(sizeof(char) * BUFSIZE);
    }
    reed_solomon_encode(rs, packets, total_packets, BUFSIZE);
    reed_solomon_release(rs);
    return packets + frame_buffer_end;
}

void queue_parity(unsigned char** parity, int total_parity_length, buf_t* frame_buf, int* frame_buffer_end) {
    for (int i = 0; i < total_parity_length; i++) {
        buf_t parity_packet;
        memcpy(parity_packet.buf, parity[i], 1041);
        parity_packet.n = 1041;
        frame_buf[*frame_buffer_end] = parity_packet;
        *frame_buffer_end = *frame_buffer_end + 1;
        free(parity[i]);
    }
}

int udp_forward(int video_socket, int audio_socket, int public_video_socket, int public_audio_socket, int efd) {
    nfds_t nfds = 5;
    struct pollfd fds[nfds];

    int tokens = 0;
    buf_t buf_slab[BUFLISTSIZE];
    memset(buf_slab, 0, sizeof (buf_slab));
    int buf_slab_end = 0;
    list_t buf_list;
    list_new(&buf_list);
    list_t frame_buf_list;
    list_new(&frame_buf_list);
    buf_t frame_buffer[FRAMEBUFSIFE];
    int frame_buffer_end = 0;
    uint current_fec_group = 1;

    struct sockaddr_in video_clientaddr;
    int video_clientaddrlen = -1;
    struct sockaddr_in audio_clientaddr;
    int audio_clientaddrlen = -1;

    memset(fds, 0, sizeof (fds));
    fds[0].fd = video_socket;
    fds[0].events = POLLIN;
    fds[1].fd = audio_socket;
    fds[1].events = POLLIN;
    fds[2].fd = public_video_socket;
    fds[2].events = POLLIN;
    fds[3].fd = public_audio_socket;
    fds[3].events = POLLIN;
    fds[4].fd = efd;
    fds[4].events = POLLIN;

    while (1) {
        if (poll(fds, nfds, -1) < 0) {
            error("poll");
            goto err;
        }

        for (int i = 0; i < nfds; i++) {
            char *buf = buf_slab[buf_slab_end].buf;
            int *n = &(buf_slab[buf_slab_end].n);

            if (fds[i].revents == 0 || fds[i].revents != POLLIN)
                continue;
            if (fds[i].fd == efd) {
                *n = read(efd, buf, sizeof (uint64_t));
                if (*n != sizeof (uint64_t)) {
                    error("read efd");
                    continue;
                }
                tokens = MAXTOKENS;
                for (list_t *elt = buf_list.next; elt != &buf_list && tokens > 0;) {
                    buf_t *buf_elt = list_elt(elt, buf_t, list);
                    elt = elt->next;
                    list_del(&buf_elt->list);
                    tokens--;
                    if (sendto(public_video_socket, buf_elt->buf, buf_elt->n, 0, (struct sockaddr *) &video_clientaddr, video_clientaddrlen) < 0)
                    error("video sendto");
                }
                if (tokens == 0) {
                    log_debug("still has 0 tokens");
                } else if (tokens < MAXTOKENS) {
                    log_debug("sent %d packets from the queue", (MAXTOKENS - tokens));
                }
                continue;
            }
            struct sockaddr_in clientaddr;
            int clientaddrlen = sizeof (clientaddr);
            *n = recvfrom(fds[i].fd, buf, BUFSIZE, 0, (struct sockaddr *) &clientaddr, &clientaddrlen);
            if (*n < 0)
                continue;
            if (fds[i].fd == public_video_socket) {
                video_clientaddrlen = clientaddrlen;
                memcpy(&video_clientaddr, &clientaddr, clientaddrlen);
            } else if (fds[i].fd == public_audio_socket) {
                audio_clientaddrlen = clientaddrlen;
                memcpy(&audio_clientaddr, &clientaddr, clientaddrlen);
            } else if (fds[i].fd == video_socket) {
                if (video_clientaddrlen == -1)
                    continue;
                if (get_fragmentation_unit_type(&(buf_slab[buf_slab_end])) == B) {
                    memcpy(&(frame_buffer[frame_buffer_end]), &(buf_slab[buf_slab_end]), sizeof(buf_t));
                    enum FuHeader header = get_fu_header_type(&(frame_buffer[frame_buffer_end]));
                    if (header == Start) {
                        frame_buffer_end = 0;
                    } else if (header == Middle) {
                        frame_buffer_end++;
                        if (frame_buffer_end == FRAMEBUFSIFE) {
                            frame_buffer_end = 0;
                        }
                    } else if (header == End) {
                        frame_buffer_end++;
                        int parity_amount = ((frame_buffer_end + 1) / 10) * 2;
                        unsigned char** parity_packets = generate_parity((buf_t*)&frame_buffer, frame_buffer_end, parity_amount);
                        queue_parity(parity_packets, parity_amount,(buf_t*) &frame_buffer, &frame_buffer_end);
                        form_bo_video_packet_group((buf_t*)&frame_buffer, &current_fec_group, frame_buffer_end);
                        for (int idx = 0; idx <= frame_buffer_end; idx++) {
                            list_add_tail(&(frame_buffer[idx].list), &buf_list);
                        }
                        frame_buffer_end = 0;
                    } else {
                        continue;
                    }
                }
                if (tokens > 0) {
                    tokens--;
                    form_bo_video_packet(buf, current_fec_group, 1, 1, 1, get_timestamp(buf));
                    if (sendto(public_video_socket, buf, *n, 0, (struct sockaddr *) &video_clientaddr, video_clientaddrlen) < 0)
                    error("video sendto");
                    if (tokens == 0) {
                        log_debug("reached 0 tokens");
                    }
                } else {
                    form_bo_video_packet(buf, current_fec_group, 1, 1, 1, get_timestamp(buf));
                    list_add_tail(&(buf_slab[buf_slab_end].list), &buf_list);
                    buf_slab_end++;
                    if (buf_slab_end == BUFLISTSIZE) {
                        buf_slab_end = 0;
                    }
                    if (&(buf_slab[buf_slab_end].list) == buf_list.next) {
                        log_debug("reached max list length, prune all members of a list");
                        list_new(&buf_list);
                    }
                }
            } else if (fds[i].fd == audio_socket) {
                if (audio_clientaddrlen == -1)
                    continue;
                if (sendto(public_audio_socket, buf, *n, 0, (struct sockaddr *) &audio_clientaddr, audio_clientaddrlen) < 0)
                error("audio sendto");
            }
        }
    }

    return 0;
    err:
    return -1;
}

void *token_proc(void *efdp) {
    int efd = *(int *) efdp;
    struct timespec req;
    uint64_t u = 1;
    ssize_t s;

    req.tv_sec = 0;
    req.tv_nsec = TOKENINTERVAL;

    while (1) {
        if (nanosleep(&req, NULL) == -1) {
            error("nanosleep");
            break;
        }
        s = write(efd, &u, sizeof (uint64_t));
        if (s != sizeof (uint64_t)) {
            error("write eventfd");
            continue;
        }
    }

    return NULL;
}

int create_udp_server_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        error("opening socket");
        goto err;
    }

    int optval;
    optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *) &optval, sizeof (int)) == -1) {
        error("setsockopt SO_REUSEADDR");
        goto err;
    }
    optval = 512000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (const void *) &optval, sizeof (int)) == -1) {
        error("setsockopt SO_RCVBUF");
        goto err;
    }
    optval = 512000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (const void *) &optval, sizeof (int)) == -1) {
        error("setsockopt SO_SNDBUF");
        goto err;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        error("fcntl F_GETFL");
        goto err;
    }
    flags |= O_NONBLOCK;
    if (fcntl(sockfd, F_SETFL, flags) == -1) {
        error("fcntl F_SETFL");
        goto err;
    }

    struct sockaddr_in serveraddr;
    memset((char *) &serveraddr, 0, sizeof (serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short) port);

    if (bind(sockfd, (struct sockaddr *) &serveraddr, sizeof (serveraddr)) < 0) {
        error("bind");
        goto err;
    }

    return sockfd;
    err:
    if (sockfd > 0) {
        close(sockfd);
    }

    return -1;
}

int main_(int video_port, int audio_port, int public_video_port, int public_audio_port) {
    reed_solomon_init();
    int video_socket = 0;
    int audio_socket = 0;
    int public_video_socket = 0;
    int public_audio_socket = 0;

    int efd = 0;
    efd = eventfd(0, EFD_NONBLOCK);
    if (efd == -1) {
        error("eventfd");
        goto err;
    }
    pthread_t token_thread = 0;
    if (pthread_create(&token_thread, NULL, token_proc, &efd)) {
        error("pthread_create");
        goto err;
    }

    video_socket = create_udp_server_socket(video_port);
    if (video_socket == -1)
        goto err;
    audio_socket = create_udp_server_socket(audio_port);
    if (audio_socket == -1)
        goto err;
    public_video_socket = create_udp_server_socket(public_video_port);
    if (public_video_socket == -1)
        goto err;
    public_audio_socket = create_udp_server_socket(public_audio_port);
    if (public_audio_socket == -1)
        goto err;

    if (udp_forward(video_socket, audio_socket, public_video_socket, public_audio_socket, efd) == -1)
        goto err;

    close(video_socket);
    close(audio_socket);
    close(public_video_socket);
    close(public_audio_socket);
    close(efd);

    return 0;
    err:
    if (video_socket > 0) {
        close(video_socket);
    }
    if (audio_socket > 0) {
        close(audio_socket);
    }
    if (public_video_socket > 0) {
        close(public_video_socket);
    }
    if (public_audio_socket > 0) {
        close(public_audio_socket);
    }
    if (efd > 0) {
        close(efd);
    }

    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 5) {
        exit(1);
    }

    int video_port = atoi(argv[1]);
    int audio_port = atoi(argv[2]);
    int public_video_port = atoi(argv[3]);
    int public_audio_port = atoi(argv[4]);

    if (main_(video_port, audio_port, public_video_port, public_audio_port) == -1) {
        exit(1);
    }

    return 0;
}