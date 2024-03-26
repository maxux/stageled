#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <png.h>
#include <pthread.h>

//
// global context
//
typedef struct pixel_t {
	int r;
	int g;
	int b;

} pixel_t;

typedef struct channel_t {
    uint8_t high;
    uint8_t mid;
    uint8_t low;
    uint8_t slider;
    uint8_t mute;
    uint8_t rec;

} channel_t;

typedef struct transform_t {
    int lines;
    channel_t channels[8];
    uint8_t master;
    uint8_t solo;

} transform_t;

typedef struct kntxt_t {
    uint64_t stats_frames;
    pixel_t *pixels;
    uint8_t *bitmap;
    transform_t midi;

} kntxt_t;


//
// helpers
//
void diep(char *str) {
    fprintf(stderr, "[-] %s: %s\n", str, strerror(errno));
    exit(EXIT_FAILURE);
}

void *dieptr(char *str) {
    perror(str);
    exit(EXIT_FAILURE);
}

void diea(char *str, int err) {
    fprintf(stderr, "[-] %s: alsa: %s\n", str, snd_strerror(err));
    exit(EXIT_FAILURE);
}

void *imgerr(char *str) {
    fprintf(stderr, "image: %s\n", str);
    return NULL;
}

//
// image management
//
typedef struct frame_t {
    uint32_t *pixels;
    int width;
    int height;
    size_t length;

} frame_t;

frame_t *loadframe(char *imgfile) {
    FILE *fp;
    png_structp ctx;
    png_infop info;
    frame_t *frame = NULL;

    unsigned char header[8]; // 8 is the maximum size that can be checked

    if(!(fp = fopen(imgfile, "r")))
        dieptr(imgfile);

    if(fread(header, 1, 8, fp) != 8)
        diep("fread");

    if(png_sig_cmp(header, 0, 8))
        return imgerr("unknown file signature (not a png image)");

    if(!(ctx = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL)))
        return imgerr("cannot create png struct");

    if(!(info = png_create_info_struct(ctx)))
        return imgerr("cannot create info struct");

    png_init_io(ctx, fp);
    png_set_sig_bytes(ctx, 8);
    png_read_info(ctx, info);

    int width = png_get_image_width(ctx, info);
    int height = png_get_image_height(ctx, info);

    printf("[+] image dimension: %d x %d px\n", width, height);

    png_bytep *lines = (png_bytep *) malloc(sizeof(png_bytep) * height);
    for(int y = 0; y < height; y++)
        lines[y] = (png_byte *) malloc(png_get_rowbytes(ctx, info));

    png_read_image(ctx, lines);

    fclose(fp);

    if(png_get_color_type(ctx, info) == PNG_COLOR_TYPE_RGB)
        return imgerr("alpha channel required");

    if(png_get_color_type(ctx, info) != PNG_COLOR_TYPE_RGBA)
        return imgerr("only RGBA supported for now");

    // allocate frame
    if(!(frame = malloc(sizeof(frame_t))))
        diep("malloc");

    frame->length = width * height;
    frame->width = width;
    frame->height = height;

    if(!(frame->pixels = (uint32_t *) malloc(sizeof(uint32_t) * frame->length)))
        diep("malloc");

    uint32_t *pixel = frame->pixels;

    for(int y = 0; y < height; y++) {
        png_byte *row = lines[y];

        for(int x = 0; x < width; x++) {
            png_byte *ptr = &(row[x * 4]);
            *pixel = ptr[0] << 16 | ptr[1] << 8 | ptr[2];
            pixel += 1;
        }
    }

    return frame;
}

//
// led controler
//

#define SEGMENTS     24
#define PERSEGMENT   120
#define LEDSTOTAL    (SEGMENTS * PERSEGMENT)
#define BITMAPSIZE   (LEDSTOTAL * 3)
#define BUFSIZE 1024

/*
int animate(frame_t *frame) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    uint8_t map[BITMAPSIZE];

    hostname = "10.241.0.133";
    portno = 1111;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0)
        diep("socket");

    server = gethostbyname(hostname);
    if(server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;

    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    serverlen = sizeof(serveraddr);

    int line = 0;

    while(1) {
        for(int a = 0; a < LEDSTOTAL; a++) {
            printf("%x\n", frame->pixels[(line * frame->width) + a]);

            pixel_t target = {
                .r = (frame->pixels[(line * frame->width) + a] & 0xff0000) >> 16,
                .g = (frame->pixels[(line * frame->width) + a] & 0x00ff00) >> 8,
                .b = (frame->pixels[(line * frame->width) + a] & 0x0000ff),
            };



            map[(a * 3) + 0] = target.r;
            map[(a * 3) + 1] = target.g;
            map[(a * 3) + 2] = target.b;
        }

        printf("sending\n");
        n = sendto(sockfd, map, BITMAPSIZE, 0, (struct sockaddr *) &serveraddr, serverlen);

        if (n < 0)
          diep("sendto");

        usleep(50000);

        line += 1;
        if(line >= frame->height)
            line = 0;
    }

}
*/


//
// midi management
//
int univers_commit(char *univers, size_t unilen) {
    int fd;
    struct sockaddr_un addr;

    if((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0)
        diep("socket");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/dmx.sock");

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        diep("connect");

    printf("[+] network: sending univers frame\n");

    if(send(fd, univers, unilen, 0) < 0)
        diep("send");

    close(fd);

    return 0;
}

int midi_handle_event(const snd_seq_event_t *ev, kntxt_t *kntxt) {
    // printf("[+] type: %d, ", ev->type);

    // value from 0 -> 127
    //
    // channel 1: 16, 17, 18 // 19 -- mute 1, rec 3
    // channel 2: 20, 21, 22 // 23 -- mute 4, rec 6
    // channel 3: 24, 25, 26 // 27 -- mute 7, rec 9
    // channel 4: 28, 29, 30 // 31 -- mute 10, rec 12
    //
    // channel 5: 46, 47, 48 // 49 -- mute 13, rec 15
    // channel 6: 50, 51, 52 // 53 -- mute 16, rec 18
    // channel 7: 54, 55, 56 // 57 -- mute 19, rec 21
    // channel 8: 58, 59, 60 // 61 -- mute 22, rec 24
    //
    // master    : 62 -- solo note 27
    // bank left : note 25
    // bank right: note 26

    unsigned int limlow[] = {16, 20, 24, 28, 46, 50, 54, 58};

    if(ev->type == SND_SEQ_EVENT_NOTEON) {
        // printf("noteon, note: %d", ev->data.note.note);
    }

    if(ev->type == SND_SEQ_EVENT_NOTEOFF) {
        // printf("noteoff, note: %d", ev->data.note.note);
    }

    if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
        for(int i = 0; i < sizeof(limlow) / sizeof(int); i++) {
            if(ev->data.control.param >= limlow[i] && ev->data.control.param < limlow[i] + 4) {
                if(ev->data.control.param == limlow[i])
                    kntxt->midi.channels[i].high = ev->data.control.value * 2;

                if(ev->data.control.param == limlow[i] + 1)
                    kntxt->midi.channels[i].mid = ev->data.control.value * 2;

                if(ev->data.control.param == limlow[i] + 2)
                    kntxt->midi.channels[i].low = ev->data.control.value * 2;

                if(ev->data.control.param == limlow[i] + 3)
                    kntxt->midi.channels[i].slider = ev->data.control.value * 2;
            }

            if(ev->data.control.param == 62)
                kntxt->midi.master = ev->data.control.value * 2;
        }

        // printf("controller, param: %d, value: %d", ev->data.control.param, ev->data.control.value);

        /*
        if(ev->data.control.param == 57)
            univers[2] = ev->data.control.value * 2;
        */

    }

    // printf("\n");

    return 0; // univers_commit(univers, unilen);
}

void netsend_transform_apply(kntxt_t *kntxt) {
    float master = kntxt->midi.master / 255.0;

    for(int i = 0; i < LEDSTOTAL; i++) {
        kntxt->pixels[i].r = (uint8_t) (kntxt->pixels[i].r * master);
        kntxt->pixels[i].g = (uint8_t) (kntxt->pixels[i].g * master);
        kntxt->pixels[i].b = (uint8_t) (kntxt->pixels[i].b * master);
    }
}

void *thread_netsend(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    frame_t *frame = loadframe("/home/maxux/git/stageled/templates/debug.png");

    int line = 0;

    while(1) {
        for(int a = 0; a < LEDSTOTAL; a++) {
            pixel_t target = {
                .r = (frame->pixels[(line * frame->width) + a] & 0xff0000) >> 16,
                .g = (frame->pixels[(line * frame->width) + a] & 0x00ff00) >> 8,
                .b = (frame->pixels[(line * frame->width) + a] & 0x0000ff),
            };

            kntxt->pixels[a] = target;

            /*
            map[(a * 3) + 0] = target.r;
            map[(a * 3) + 1] = target.g;
            map[(a * 3) + 2] = target.b;
            */
        }

        netsend_transform_apply(kntxt);

        usleep(50000);

        line += 1;
        if(line >= frame->height)
            line = 0;
    }


    return NULL;
}

void *thread_feedback(void *extra) {

    return NULL;
}

void *thread_midi(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    snd_seq_t *seq;
    snd_seq_addr_t *ports;
    int err;

    // connect to midi controller
    if((err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0)) < 0)
        diea("open: sequencer", err);

    if((err = snd_seq_set_client_name(seq, "mididmx")) < 0)
        diea("client: set name", err);

    int caps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    int type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION;

    if((err = snd_seq_create_simple_port(seq, "mididmx", caps, type)) < 0)
        diea("create: simple port", err);

    if(!(ports = calloc(sizeof(snd_seq_addr_t), 1)))
        diep("ports: calloc");

    // hardcoded keyboard port
    if((err = snd_seq_parse_address(seq, &ports[0], "24")) < 0)
        diea("parse: address", err);

    if((err = snd_seq_connect_from(seq, 0, ports[0].client, ports[0].port)) < 0)
        diea("ports: connect", err);

    struct pollfd *pfds;
    int npfds;

    npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    pfds = alloca(sizeof(*pfds) * npfds);

    // polling events
    while(1) {
        snd_seq_event_t *event;

        snd_seq_poll_descriptors(seq, pfds, npfds, POLLIN);
        if(poll(pfds, npfds, -1) < 0)
            diep("poll");

        while((err = snd_seq_event_input(seq, &event)) > 0) {
            if(!event)
                continue;

           midi_handle_event(event, kntxt);
        }
    }

    snd_seq_close(seq);

    return NULL;
}

void *thread_console(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;

    printf("\033[2J"); // clean entire screen

    while(1) {
        // printf("\033[2J"); // clean entire screen
        printf("\033[H"); // cursor to home

        for(int line = 0; line < SEGMENTS; line++) {
            for(int pixel = 0; pixel < PERSEGMENT; pixel++) {
                int index = (line * SEGMENTS) + pixel;
                pixel_t *color = &kntxt->pixels[index];

                printf("\033[38;2;%d;%d;%dm█", color->r, color->g, color->b);
            }

            printf("\033[0m\n");
        }

        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].high);

        printf("\n");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].mid);

        printf("\n");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].low);

        printf("\n");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].slider);

        printf("\n\n");
        printf("Master: % 4d -- %f\n", kntxt->midi.master);

        usleep(10000);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    printf("[+] initializing stage-led controle interface\n");
    pthread_t netsend, feedback, midi, console;
    kntxt_t mainctx = {
        .stats_frames = 0,
    };

    // context initializer
    void *kntxt = &mainctx;

    mainctx.pixels = calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.bitmap = calloc(sizeof(uint8_t), BITMAPSIZE);

    memset(&mainctx.midi, 0x00, sizeof(transform_t));
    mainctx.midi.lines = 8; // 8 channels

    printf("[+] starting network dispatcher thread\n");
    if(pthread_create(&netsend, NULL, thread_netsend, kntxt))
        perror("thread: netsend");

    printf("[+] starting network feedback thread\n");
    if(pthread_create(&feedback, NULL, thread_feedback, kntxt))
        perror("thread: feedback");

    printf("[+] starting midi mapping thread\n");
    if(pthread_create(&midi, NULL, thread_midi, kntxt))
        perror("thread: midi");

    printf("[+] starting console monitoring thread\n");
    if(pthread_create(&console, NULL, thread_console, kntxt))
        perror("thread: console");

    pthread_join(netsend, NULL);
    pthread_join(feedback, NULL);
    pthread_join(midi, NULL);
    pthread_join(console, NULL);

    return 0;
}
