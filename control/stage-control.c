#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <png.h>
#include <pthread.h>
#include <stdatomic.h>
#include <hiredis/hiredis.h>

#define LOGGER_SIZE 32
#define SEGMENTS    24
#define PERSEGMENT  120
#define LEDSTOTAL   (SEGMENTS * PERSEGMENT)
#define BITMAPSIZE  (LEDSTOTAL * 3)
#define BUFSIZE     1024
#define TARGET_FPS  30

#define CRST        "\033[0m"
#define CWARN       "\033[1;33m"
#define CGOOD       "\033[1;32m"
#define CSELECTED   "\033[1;37;46m"
#define CEMPTY      "\033[1;37;90m"
#define CWAIT(x)    "\033[1;37;44m" x CRST
#define COK(x)      "\033[1;37;42m" x CRST
#define CBAD(x)     "\033[5m\033[1;37;41m" x CRST
#define CNULL(x)    "\033[1;37;90m" x CRST

#define APC_COLOR_BLACK       0
#define APC_COLOR_WHITE       3
#define APC_COLOR_RED         5
#define APC_COLOR_GREEN       21
#define APC_COLOR_BLUE        45
#define APC_COLOR_YELLOW      13
#define APC_COLOR_LIGHT_BLUE  36
#define APC_COLOR_PURPLE      53

#define APC_BLACKOUT_COLOR    APC_COLOR_WHITE
#define APC_FULLON_COLOR      APC_COLOR_WHITE
#define APC_PRESETS_COLOR     APC_COLOR_YELLOW
#define APC_MASKS_COLOR       APC_COLOR_PURPLE

#define APC_SINGLE_MODE       0x90
#define APC_SINGLE_OFF        0x00
#define APC_SINGLE_ON         0x01
#define APC_SINGLE_BLINK      0x02

#define APC_SOLID_10          0x90
#define APC_SOLID_25          0x91
#define APC_SOLID_50          0x92
#define APC_SOLID_65          0x93
#define APC_SOLID_75          0x94
#define APC_SOLID_90          0x95
#define APC_SOLID_100         0x96

#define APC_PULSE_1_16        0x97
#define APC_PULSE_1_8         0x98
#define APC_PULSE_1_4         0x99
#define APC_PULSE_1_2         0x9A

#define APC_BLINK_1_24        0x9B
#define APC_BLINK_1_16        0x9C
#define APC_BLINK_1_8         0x9D
#define APC_BLINK_1_4         0x9E
#define APC_BLINK_1_2         0x9F

#define TEMPLATE_PREFIX       "/home/maxux/git/stageled/templates"

//
// global context
//
typedef union pixel_t {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    uint32_t raw;

} pixel_t;

typedef struct slider_t {
    uint8_t value;

} slider_t;

typedef struct transform_t {
    int lines;
    slider_t *sliders;
    uint8_t strip_rgb[3]; // strip red, green, blue
    uint8_t master;

    uint8_t *presets;
    uint8_t *masks;

} transform_t;

typedef struct frame_t {
    uint32_t *pixels;
    int width;
    int height;
    size_t length;

} frame_t;

typedef struct controller_stats_t {
    uint64_t state;
    uint64_t old_frames;
    uint64_t frames;
    uint64_t fps;
    uint64_t time_last_frame;

    uint64_t time_current;

    uint16_t main_ac_voltage;

    uint16_t main_core_temperature;
    uint16_t mon_core_temperature;
    uint16_t ext_power_temperature;
    uint16_t ext_compute_temperature;

    uint16_t psu0_volt;
    uint16_t psu0_amps;
    uint16_t psu1_volt;
    uint16_t psu1_amps;
    uint16_t psu2_volt;
    uint16_t psu2_amps;

    uint16_t padding;

} controller_stats_t;

typedef struct control_stats_t {
    uint64_t frames;
    uint64_t ctrl_initial_frames;
    uint64_t ctrl_initial_time;
    uint64_t showframes;
    uint64_t dropped;
    double droprate;
    struct timeval ctrl_last_feedback;

    double time_transform;

} control_stats_t;

typedef struct kntxt_t {
    pixel_t *pixels;
    pixel_t *maskpixels;
    pixel_t *monitor; // monitoring output
    pixel_t *preview; // monitoring without master

    frame_t *frame;
    frame_t *maskframe;
    frame_t *maskreset; // special flag

    transform_t midi;
    useconds_t speed;
    uint8_t blackout;
    uint8_t fullon;
    uint8_t strobe;
    uint8_t strobe_state;
    uint32_t strobe_index;
    uint32_t strobe_duration;

    char **presets;
    int presets_total;
    char **masks;
    int masks_total;

    char *preset;
    char *mask;

    // remote and local stats
    controller_stats_t controller;
    control_stats_t client;
    char *controladdr;

    // flags to monitor interface presence
    uint8_t interface; // not found, found, lost

    // master thread locking (FIXME)
    pthread_mutex_t lock;

    pthread_cond_t cond_presets;
    pthread_mutex_t mutcond_presets; // condition mutex

    pthread_cond_t cond_masks;
    pthread_mutex_t mutcond_masks; // condition mutex

    atomic_char keepgoing;

} kntxt_t;


typedef struct logger_t {
    char **lines;
    int capacity;
    int nextid;

    pthread_mutex_t lock;

} logger_t;

logger_t mainlog;

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

double timediff(struct timeval *n, struct timeval *b) {
    return (double)(n->tv_usec - b->tv_usec) / 1000000 + (double)(n->tv_sec - b->tv_sec);
}

char *uptime_prettify(size_t usec) {
    char buffer[128];
    size_t source = usec / 1000;

    int h = (source / 3600);
    int m = (source - (3600 * h)) / 60;
    int s = (source - (3600 * h) - (m * 60));

    sprintf(buffer, "%02d hrs, %02d min, %02d sec", h, m, s);

    return strdup(buffer);
}

void thread_wait(int ms) {
    struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = ms * 1000,
    };

    nanosleep(&ts, NULL);
}

int list_index_search(char **list, char *entry, int length) {
    for(int i = 0; i < length; i++)
        if(list[i] == entry)
            return i;

    return -1;
}

//
// logging
//
void logger(char *fmt, ...) {
    char buffer[1024], timed[1096];
    logger_t *logs = &mainlog;

    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    // append local time
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    sprintf(timed, "[%02d:%02d:%02d] %s", tm->tm_hour, tm->tm_min, tm->tm_sec, buffer);

    // locking logger and append log line
    pthread_mutex_lock(&logs->lock);

    if(logs->nextid == logs->capacity)
        logs->nextid = 0;

    free(logs->lines[logs->nextid]);
    logs->lines[logs->nextid] = strdup(timed);

    logs->nextid += 1;

    pthread_mutex_unlock(&logs->lock);
}

//
// image and transformation management
//
frame_t *frame_loadfile(char *imgfile) {
    FILE *fp;
    png_structp ctx;
    png_infop info;
    frame_t *frame = NULL;
    char prefixed[256];

    unsigned char header[8]; // 8 is the maximum size that can be checked

    sprintf(prefixed, "%s/%s", TEMPLATE_PREFIX, imgfile);

    if(!(fp = fopen(prefixed, "r")))
        dieptr(prefixed);

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

    if(width != 2880)
        logger("[+] loader: warning: image dimension: %d x %d px", width, height);

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
            *pixel = ptr[0] | ptr[1] << 8 | ptr[2] << 16 | ptr[3] << 24;
            pixel += 1;
        }
    }

    // cleanup working png stuff
    for(int y = 0; y < height; y++)
        free(lines[y]);

    free(lines);

    png_destroy_read_struct(&ctx, &info, NULL);

    return frame;
}

void *thread_animate(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    frame_t *frame, *maskframe;
    int line = 0, maskline = 0;

    // allocate local copy of pixels
    pixel_t *localpixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    pixel_t *localmask = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);

    // fetch initial frame already loaded by loader
    pthread_mutex_lock(&kntxt->lock);

    // remove frame from context, keeping it for us
    frame = kntxt->frame;
    kntxt->frame = NULL;

    maskframe = NULL;

    pthread_mutex_unlock(&kntxt->lock);

    while(kntxt->keepgoing) {
        // checking for changes
        pthread_mutex_lock(&kntxt->lock);

        if(kntxt->frame != NULL) {
            // cleaning frame not used anymore
            free(frame->pixels);
            free(frame);

            // acquiring new frame
            frame = kntxt->frame;
            kntxt->frame = NULL;

            // start from the begining of that frame
            line = 0;
        }

        if(kntxt->maskframe != NULL) {
            // cleaning frame not used anymore
            if(maskframe) {
                free(maskframe->pixels);
                free(maskframe);
            }

            // acquiring new frame
            maskframe = kntxt->maskframe;
            kntxt->maskframe = NULL;

            // start from the begining of that frame
            maskline = 0;

            // special reset flag
            if(maskframe == (frame_t *) &kntxt->maskreset) {
                memset(localmask, 0x00, sizeof(pixel_t) * LEDSTOTAL);
                maskframe = NULL;
            }
        }

        pthread_mutex_unlock(&kntxt->lock);

        // copy line (avoid copy pixel by pixel)
        memcpy(localpixels, &frame->pixels[line * frame->width], LEDSTOTAL * sizeof(pixel_t));

        if(maskframe)
            memcpy(localmask, &maskframe->pixels[maskline * maskframe->width], LEDSTOTAL * sizeof(pixel_t));

        // commit this frame pixel to main context
        pthread_mutex_lock(&kntxt->lock);

        memcpy(kntxt->pixels, localpixels, sizeof(pixel_t) * LEDSTOTAL);
        memcpy(kntxt->maskpixels, localmask, sizeof(pixel_t) * LEDSTOTAL);
        useconds_t waiting = kntxt->speed;

        pthread_mutex_unlock(&kntxt->lock);

        // wait relative to speed for the next frame
        // usleep(waiting);
        thread_wait(waiting);

        line += 1;
        if(line >= frame->height)
            line = 0;

        if(maskframe) {
            maskline += 1;
            if(maskline >= maskframe->height)
                maskline = 0;
        }
    }

    return NULL;
}

//
// presets worker and loader
//
void *thread_presets(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    // int retval;
    frame_t *frame;

    logger("[+] presets: initializing presets, loading default one");

    /*
       struct timespec ts = {
       .tv_sec = 1,
       .tv_nsec = 100000,
       };
       */

    while(kntxt->keepgoing) {
        /*
           if((retval = pthread_cond_timedwait(&kntxt->cond_presets, &kntxt->mutcond_presets, &ts)) != 0) {
           if(retval == ETIMEDOUT) {
           logger("timeout");
           continue;
           }
           }
           */

        pthread_mutex_lock(&kntxt->mutcond_presets);
        pthread_cond_wait(&kntxt->cond_presets, &kntxt->mutcond_presets); // FIXME
        pthread_mutex_unlock(&kntxt->mutcond_presets);

        // loading new preset name
        pthread_mutex_lock(&kntxt->lock);
        char *preset = strdup(kntxt->preset);
        pthread_mutex_unlock(&kntxt->lock);

        // locally load the frame
        logger("[+] presets: loading new presets: %s", preset);
        if(!(frame = frame_loadfile(preset))) {
            // load failed, skipping
            free(preset);
            continue;
        }

        // commit frame
        pthread_mutex_lock(&kntxt->lock);
        // free previous frame not yet acquired by animate thread
        if(kntxt->frame) {
            free(kntxt->frame->pixels);
            free(kntxt->frame);
        }

        kntxt->frame = frame;
        pthread_mutex_unlock(&kntxt->lock);

        free(preset);
    }

    return NULL;
}

void *thread_masks(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    // int retval;
    frame_t *frame;

    logger("[+] masks: initializing masks");

    /*
       struct timespec ts = {
       .tv_sec = 0,
       .tv_nsec = 100000,
       };
       */

    while(kntxt->keepgoing) {
        /*
           if((retval = pthread_cond_timedwait(&kntxt->cond_masks, &kntxt->mutcond_masks, &ts)) != 0) {
           if(retval == ETIMEDOUT)
           continue;
           }
           */

        pthread_mutex_lock(&kntxt->mutcond_masks);
        pthread_cond_wait(&kntxt->cond_masks, &kntxt->mutcond_masks); // FIXME
        pthread_mutex_unlock(&kntxt->mutcond_masks);

        // loading new mask name
        pthread_mutex_lock(&kntxt->lock);
        char *mask = strdup(kntxt->mask);
        pthread_mutex_unlock(&kntxt->lock);

        // locally load the frame
        logger("[+] mask: loading new mask: %s", mask);
        if(!(frame = frame_loadfile(mask))) {
            // load failed, skipping
            free(mask);
            continue;
        }

        // commit frame
        pthread_mutex_lock(&kntxt->lock);
        // free previous frame not yet acquired by animate thread
        if(kntxt->maskframe) {
            free(kntxt->maskframe->pixels);
            free(kntxt->maskframe);
        }

        kntxt->maskframe = frame;
        pthread_mutex_unlock(&kntxt->lock);

        free(mask);
    }

    return NULL;
}

//
// network transmitter management
//
int netsend_transmit_frame(uint8_t *bitmap, char *target) {
    struct sockaddr_in serveraddr;
    struct hostent *hent;
    int sockfd;

    char *hostname = target;
    int portno = 1111;

    if((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        diep("socket");

    if((hent = gethostbyname(hostname)) == NULL) {
        logger("[-] netsend: cannot resolve target host");
        return 1;
    }

    memset(&serveraddr, 0x00, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    memcpy(&serveraddr.sin_addr, hent->h_addr_list[0], hent->h_length);

    int serverlen = sizeof(serveraddr);

    // sending bitmap
    if(sendto(sockfd, bitmap, BITMAPSIZE, 0, (struct sockaddr *) &serveraddr, serverlen) < 0)
        diep("sendto");

    close(sockfd);

    return 0;
}

void netsend_pixels_transform(kntxt_t *kntxt, pixel_t *monitor, pixel_t *preview, uint8_t *localbitmap) {
    // fetch settings from main context
    pthread_mutex_lock(&kntxt->lock);

    uint8_t rawmaster = kntxt->midi.master;
    uint8_t blackout = kntxt->blackout;
    uint8_t fullon = kntxt->fullon;

    uint8_t strobe = kntxt->strobe;
    uint8_t strobe_index = kntxt->strobe_index;
    uint8_t strobe_state = kntxt->strobe_state;

    uint8_t colorize[] = {
        kntxt->midi.strip_rgb[0],
        kntxt->midi.strip_rgb[1],
        kntxt->midi.strip_rgb[2],
    };

    uint8_t segments[] = {
        kntxt->midi.sliders[0].value,
        kntxt->midi.sliders[1].value,
        kntxt->midi.sliders[2].value,
    };

    if(strobe)
        kntxt->strobe_index += 1;

    // applying settings to frame
    if(blackout)
        rawmaster = 0;

    // checking if a strobe mask needs to be applied
    if(strobe && !blackout) {
        int divider = ((256 - strobe) / 20) + 1;
        int ontime = ((256 - kntxt->strobe_duration) / 20) + 1;

        if(strobe_state == 1 && ontime < divider)
            divider = ontime;

        // check if we need to change the state
        if(strobe_index % divider == 0) {
            kntxt->strobe_index = 1;

            strobe_state = (strobe_state) ? 0 : 1;
            kntxt->strobe_state = strobe_state;
        }

        // check if we need to apply something
        if(strobe_state == 0)
            rawmaster = 0;
    }

    pthread_mutex_unlock(&kntxt->lock);

    // copy current state to preview, which is monitor without master applied
    memcpy(preview, monitor, sizeof(pixel_t) * LEDSTOTAL);

    if(fullon)
        memset(monitor, 0xffffffff, LEDSTOTAL * sizeof(pixel_t));

    if(colorize[0] || colorize[1] || colorize[2]) {
        float redmul = (255 - colorize[0]) / 255.0;
        float greenmul = (255 - colorize[1]) / 255.0;
        float bluemul = (255 - colorize[2]) / 255.0;

        for(int i = 0; i < LEDSTOTAL; i++) {
            monitor[i].r = (uint8_t) (monitor[i].r * redmul);
            monitor[i].g = (uint8_t) (monitor[i].g * greenmul);
            monitor[i].b = (uint8_t) (monitor[i].b * bluemul);
        }
    }

    int barperseg[3][8] = {
        {0,  1,  2,  3,  4,  5,  6,  7},
        {8,  9,  10, 11, 12, 13, 14, 15},
        {16, 17, 18, 19, 20, 21, 22, 23},
    };

    for(int segment = 0; segment < 3; segment++) {
        // is segment faded
        if(segments[segment] == 255)
            continue;

        for(int xbar = 0; xbar < 8; xbar++) {
            if(barperseg[segment][xbar] < 0)
                break;

            float mul = segments[segment] / 255.0;

            for(int i = (barperseg[segment][xbar] * PERSEGMENT); i < ((barperseg[segment][xbar] + 1) * PERSEGMENT); i++) {
                monitor[i].r = (uint8_t) (monitor[i].r * mul);
                monitor[i].g = (uint8_t) (monitor[i].g * mul);
                monitor[i].b = (uint8_t) (monitor[i].b * mul);
            }
        }
    }

    // apply mask
    for(int i = 0; i < LEDSTOTAL; i++) {
        if(kntxt->maskpixels[i].raw != 0) {
            float mul = (255 - kntxt->maskpixels[i].a) / 255.0;

            monitor[i].r = (uint8_t) (monitor[i].r * mul);
            monitor[i].g = (uint8_t) (monitor[i].g * mul);
            monitor[i].b = (uint8_t) (monitor[i].b * mul);
        }
    }

    // only compute floating master transformation if needed
    if(rawmaster < 255 && rawmaster > 0) {
        double master = rawmaster / 255.0;

        for(int i = 0; i < LEDSTOTAL; i++) {
            monitor[i].r = (uint8_t) (monitor[i].r * master);
            monitor[i].g = (uint8_t) (monitor[i].g * master);
            monitor[i].b = (uint8_t) (monitor[i].b * master);
        }
    }

    if(rawmaster == 0) {
        // force setting all pixels to zero in a more efficient way
        memset(monitor, 0x00, sizeof(pixel_t) * LEDSTOTAL);
    }

    // building network frame bitmap
    for(int i = 0; i < LEDSTOTAL; i++) {
        localbitmap[(i * 3) + 0] = monitor[i].r;
        localbitmap[(i * 3) + 1] = monitor[i].g;
        localbitmap[(i * 3) + 2] = monitor[i].b;
    }
}

void *thread_netsend(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    char *controladdr;

    logger("[+] netsend: sending frames to controller");

    pixel_t *monitor = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL); // live copy
    pixel_t *preview = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL); // always visible copy

    uint8_t *localbitmap = (uint8_t *) calloc(sizeof(uint8_t), BITMAPSIZE);

    // transform time
    struct timeval before, after;

    while(kntxt->keepgoing) {
        // fetch current frame pixel from animate
        pthread_mutex_lock(&kntxt->lock);

        memcpy(monitor, kntxt->pixels, sizeof(pixel_t) * LEDSTOTAL);
        controladdr = kntxt->controladdr;

        pthread_mutex_unlock(&kntxt->lock);

        // apply transformation
        gettimeofday(&before, NULL);
        netsend_pixels_transform(kntxt, monitor, preview, localbitmap);
        gettimeofday(&after, NULL);

        // commit transformation to monitor to see changes on console
        pthread_mutex_lock(&kntxt->lock);

        kntxt->client.time_transform = timediff(&after, &before);
        memcpy(kntxt->monitor, monitor, sizeof(pixel_t) * LEDSTOTAL);
        memcpy(kntxt->preview, preview, sizeof(pixel_t) * LEDSTOTAL);
        kntxt->client.frames += 1;

        pthread_mutex_unlock(&kntxt->lock);

        // sending the frame to the controller (if alive)
        if(controladdr)
            netsend_transmit_frame(localbitmap, controladdr);

        thread_wait(1000000 / TARGET_FPS);
    }

    free(monitor);
    free(preview);
    free(localbitmap);

    return NULL;
}

//
// feedback management
//
void *thread_feedback(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    char message[1024], ctrladdr[32];
    struct sockaddr_in name;
    struct sockaddr_in client;
    unsigned long clientaddr = 0;
    socklen_t clientlen = sizeof(client);
    int sock;

    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        diep("feedback: socket");

    memset(&name, 0x00, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    name.sin_port = htons(1111);

    if(bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0)
        diep("feedback: bind");

    logger("[+] feedback: waiting for incoming packets");

    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = 10000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &read_timeout, sizeof read_timeout);

    while(kntxt->keepgoing) {
        int bytes = recvfrom(sock, message, sizeof(message), 0, (struct sockaddr *) &client, &clientlen);

        if(bytes <= 0) {
            thread_wait(10000);
            continue;
        }

        pthread_mutex_lock(&kntxt->lock);

        if(kntxt->controller.time_current == 0) {
            logger("[+] feedback: first message received from the controller");

            // save a copy of initial message to be able
            // to compute relative counters
            controller_stats_t initial;
            memcpy(&initial, message, bytes);

            // resetting internal frames counter
            // to get droprate in sync
            kntxt->client.frames = 0;

            kntxt->client.ctrl_initial_frames = initial.frames;
            kntxt->client.ctrl_initial_time = initial.time_current;

            logger("[+] feedback: relative frames: %lu, time: %lu", initial.frames, initial.time_current);
        }

        if(clientaddr != client.sin_addr.s_addr) {
            // save this address as last client address
            clientaddr = client.sin_addr.s_addr;

            inet_ntop(AF_INET, &client.sin_addr, ctrladdr, sizeof(ctrladdr));
            logger("[+] feedback: received from: %s, updating", ctrladdr);

            // linking this client to main network stack
            free(kntxt->controladdr);
            kntxt->controladdr = strdup(ctrladdr);
        }

        // make a lazy binary copy from controller
        memcpy(&kntxt->controller, message, bytes);
        gettimeofday(&kntxt->client.ctrl_last_feedback, NULL);

        kntxt->client.showframes = (kntxt->controller.frames - kntxt->client.ctrl_initial_frames);
        kntxt->client.dropped = kntxt->client.frames - kntxt->client.showframes;
        if(kntxt->client.frames)
            kntxt->client.droprate = (kntxt->client.dropped / (double) kntxt->client.frames) * 100;

        pthread_mutex_unlock(&kntxt->lock);
    }

    close(sock);

    return NULL;
}

//
// midi management
//
inline uint8_t midi_value_parser(uint8_t input) {
    uint8_t parsed = input * 2;

    if(input == 127)
        return 255;

    return parsed;
}

void midi_set_control(snd_seq_t *seq, uint8_t mode, uint8_t button, uint8_t value) {
    snd_seq_event_t ev;

    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, 0);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);

    ev.type = SND_SEQ_EVENT_NOTEON;
    ev.data.note.channel = mode;
    ev.data.note.note = button;
    ev.data.note.velocity = value;

    snd_seq_event_output(seq, &ev);
    snd_seq_drain_output(seq);
}

int midi_handle_event(const snd_seq_event_t *ev, kntxt_t *kntxt, snd_seq_t *seq) {
    // logger("[+] midi type: %d", ev->type);

    // matrix: 0 -> 63
    // from bottom left to top right
    //
    // volume, pan, send, ...
    // 100 -> 107
    //
    // shift: 122
    //
    // clip stop, solo, mute, ...
    // 112 -> 119
    //
    // faders: 48 -> 56

    // logger("midi: event type: %d", ev->type);

    // FIXME: use local copy, not main object
    uint8_t *presets = kntxt->midi.presets;
    uint8_t *masks = kntxt->midi.masks;

    if(ev->type == SND_SEQ_EVENT_NOTEON) {
        // logger("[+] midi: note on, note: %d", ev->data.note.note);

        for(int i = 0; i < kntxt->presets_total; i++) {
            if(ev->data.note.note == presets[i]) {
                // preset not set
                if(!kntxt->presets[i])
                    return 0;

                pthread_mutex_lock(&kntxt->lock);

                // switch button blink
                int oldindex = list_index_search(kntxt->presets, kntxt->preset, kntxt->presets_total);
                if(oldindex >= 0)
                    midi_set_control(seq, APC_SOLID_100, presets[oldindex], APC_PRESETS_COLOR);

                midi_set_control(seq, APC_PULSE_1_4, presets[i], APC_PRESETS_COLOR);

                logger("[+] loading preset %d: %s", i + 1, kntxt->presets[i]);
                kntxt->preset = kntxt->presets[i];
                pthread_cond_signal(&kntxt->cond_presets);

                pthread_mutex_unlock(&kntxt->lock);

                return 0;
            }
        }

        if(ev->data.note.note == 112 && kntxt->mask) {
            pthread_mutex_lock(&kntxt->lock);

            if(kntxt->mask) {
                logger("[+] midi: resetting mask layer");

                int oldindex = list_index_search(kntxt->masks, kntxt->mask, kntxt->masks_total);
                midi_set_control(seq, APC_SOLID_100, masks[oldindex], APC_MASKS_COLOR);
            }

            // disable reset button
            midi_set_control(seq, APC_SINGLE_MODE, 0x70, APC_SINGLE_OFF);

            kntxt->mask = NULL;
            kntxt->maskframe = (frame_t *) &kntxt->maskreset;

            pthread_mutex_unlock(&kntxt->lock);
        }

        for(int i = 0; i < kntxt->masks_total; i++) {
            if(ev->data.note.note == masks[i]) {
                // preset not set
                if(!kntxt->masks[i])
                    return 0;

                pthread_mutex_lock(&kntxt->lock);

                // switch button blink
                if(kntxt->mask) {
                    int oldindex = list_index_search(kntxt->masks, kntxt->mask, kntxt->masks_total);
                    midi_set_control(seq, APC_SOLID_100, masks[oldindex], APC_MASKS_COLOR);
                }

                midi_set_control(seq, APC_PULSE_1_4, masks[i], APC_MASKS_COLOR);

                // enable reset button
                midi_set_control(seq, APC_SINGLE_MODE, 0x70, APC_SINGLE_ON);

                logger("[+] loading mask %d: %s", i + 1, kntxt->masks[i]);
                kntxt->mask = kntxt->masks[i];
                pthread_cond_signal(&kntxt->cond_masks);

                pthread_mutex_unlock(&kntxt->lock);

                return 0;
            }
        }

        pthread_mutex_lock(&kntxt->lock);

        if(ev->data.note.note == 7) {
            if(kntxt->blackout == 0) {
                kntxt->blackout = 1;
                midi_set_control(seq, APC_BLINK_1_24, 0x07, APC_BLACKOUT_COLOR);

            } else {
                kntxt->blackout = 0;
                midi_set_control(seq, APC_SOLID_100, 0x07, APC_BLACKOUT_COLOR);
            }
        }

        // full on enabled
        if(ev->data.note.note == 0x06) {
            kntxt->fullon = 1;
            midi_set_control(seq, APC_SOLID_100, 0x06, APC_FULLON_COLOR);
        }
        // strip color note
        int strip_colors[3] = {APC_COLOR_RED, APC_COLOR_GREEN, APC_COLOR_BLUE};
        if(ev->data.note.note < 3) { // >= 0 not needed, unsigned
            if(kntxt->midi.strip_rgb[ev->data.note.note]) {
                midi_set_control(seq, APC_SOLID_100, ev->data.note.note, strip_colors[ev->data.note.note]);
                kntxt->midi.strip_rgb[ev->data.note.note] = 0;

            } else {
                midi_set_control(seq, APC_BLINK_1_8, ev->data.note.note, strip_colors[ev->data.note.note]);
                kntxt->midi.strip_rgb[ev->data.note.note] = 255;
            }
        }

        // segment configuration
        if(ev->data.note.note == 100)
            logger("[+] midi: configure segments 1");

        if(ev->data.note.note == 101)
            logger("[+] midi: configure segments 2");

        if(ev->data.note.note == 102)
            logger("[+] midi: configure segments 3");

        if(ev->data.note.note == 103)
            logger("[+] midi: configure segments 4");

        pthread_mutex_unlock(&kntxt->lock);

    }

    if(ev->type == SND_SEQ_EVENT_NOTEOFF) {
        // logger("[+] midi: note off, note: %d", ev->data.note.note);

        // full on disabled
        if(ev->data.note.note == 0x06) {
            pthread_mutex_lock(&kntxt->lock);
            kntxt->fullon = 0;
            pthread_mutex_unlock(&kntxt->lock);

            midi_set_control(seq, APC_SOLID_10, 0x06, APC_FULLON_COLOR);
        }
    }

    if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
        // logger("[+] midi: fader: param: %d, value: %d", ev->data.control.param, ev->data.control.value);

        pthread_mutex_lock(&kntxt->lock);

        if(ev->data.control.param > 47 && ev->data.control.param < 56)
            kntxt->midi.sliders[ev->data.control.param - 48].value = midi_value_parser(ev->data.control.value);

        // master channel
        if(ev->data.control.param == 56)
            kntxt->midi.master = midi_value_parser(ev->data.control.value);

        pthread_mutex_unlock(&kntxt->lock);
    }

    pthread_mutex_lock(&kntxt->lock);

    if(kntxt->midi.sliders[7].value > 0) {
        kntxt->speed = (1000000 / kntxt->midi.sliders[7].value);

    } else {
        kntxt->speed = 1000000 / TARGET_FPS;
    }

    // apply strobe value
    kntxt->strobe = kntxt->midi.sliders[6].value;
    kntxt->strobe_duration = kntxt->midi.sliders[5].value;

    if(kntxt->strobe == 0) {
        kntxt->strobe_index = 0;
        kntxt->strobe_state = 0;
    }

    pthread_mutex_unlock(&kntxt->lock);

    return 0;
}

void *midi_no_interface(kntxt_t *kntxt) {
    // force master to full
    kntxt->midi.master = 255;

    // force segments to full
    kntxt->midi.sliders[0].value = 255;
    kntxt->midi.sliders[1].value = 255;
    kntxt->midi.sliders[2].value = 255;

    return NULL;
}

snd_seq_t *midi_initialize_interface(kntxt_t *kntxt) {
    snd_seq_t *seq;
    snd_seq_addr_t *ports;
    int err;

    // prepare device link

    // connect to midi controller
    if((err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0)) < 0)
        diea("open: sequencer", err);

    if((err = snd_seq_set_client_name(seq, "midi-dmx")) < 0)
        diea("client: set name", err);

    int caps = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
    int type = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION;

    if((err = snd_seq_create_simple_port(seq, "midi-dmx", caps, type)) < 0)
        diea("create: simple port", err);

    if(!(ports = calloc(sizeof(snd_seq_addr_t), 1)))
        diep("ports: calloc");

    // hardcoded keyboard port
    if((err = snd_seq_parse_address(seq, &ports[0], "APC mini mk2")) < 0) {
        logger("[-] midi: parse address: %s", snd_strerror(err));
        snd_seq_close(seq);
        // return midi_no_interface(kntxt);
        return NULL;
    }

    if((err = snd_seq_connect_from(seq, 0, ports[0].client, ports[0].port)) < 0) {
        logger("[-] midi: connect from: %s", snd_strerror(err));
        snd_seq_close(seq);
        // return midi_no_interface(kntxt);
        return NULL;
    }

    if((err = snd_seq_connect_to(seq, 0, ports[0].client, ports[0].port)) < 0) {
        logger("[-] midi: connect to: %s", snd_strerror(err));
    }

    kntxt->interface = 1;

    // prepare device light states

    //
    // presets
    //
    uint8_t hardcode_presets[] = {
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };

    memcpy(kntxt->midi.presets, &hardcode_presets, sizeof(hardcode_presets));

    //
    // masks
    //
    uint8_t hardcode_masks[] = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };

    memcpy(kntxt->midi.masks, &hardcode_masks, sizeof(hardcode_masks));

    // reset all leds
    for(int i = 0; i < 64; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    for(int i = 0x64; i < 0x6b; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    for(int i = 0x70; i < 0x7a; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    // set blackout default
    midi_set_control(seq, APC_SOLID_100, 0x07, APC_BLACKOUT_COLOR);

    // set fullon default
    midi_set_control(seq, APC_SOLID_10, 0x06, APC_FULLON_COLOR);

    // set red, green, blue channel cut
    midi_set_control(seq, APC_SOLID_100, 0x00, APC_COLOR_RED);
    midi_set_control(seq, APC_SOLID_100, 0x01, APC_COLOR_GREEN);
    midi_set_control(seq, APC_SOLID_100, 0x02, APC_COLOR_BLUE);

    // set presets pad colors
    for(int i = 0; i < kntxt->presets_total; i++)
        if(kntxt->presets[i])
            midi_set_control(seq, APC_SOLID_100, kntxt->midi.presets[i], APC_PRESETS_COLOR);

    // set masks pad colors
    for(int i = 0; i < kntxt->masks_total; i++)
        if(kntxt->masks[i])
            midi_set_control(seq, APC_SOLID_100, kntxt->midi.masks[i], APC_MASKS_COLOR);

    // set initial preset
    midi_set_control(seq, APC_PULSE_1_4, kntxt->midi.presets[0], APC_PRESETS_COLOR);

    logger("[+] midi: interface initialized");

    return seq;

}

void *thread_midi(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    snd_seq_t *seq = NULL;
    struct pollfd *pfds;
    int err;
    int npfds;

    npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    pfds = calloc(sizeof(*pfds), npfds);

    // polling events
    while(kntxt->keepgoing) {
        snd_seq_event_t *event;

        if(kntxt->interface != 1) {
            if(!(seq = midi_initialize_interface(kntxt))) {
                usleep(20000);
                continue;
            }
        }

        snd_seq_poll_descriptors(seq, pfds, npfds, POLLIN);
        if(poll(pfds, npfds, 100) < 0)
            diep("poll");

        unsigned short evid;
        if(snd_seq_poll_descriptors_revents(seq, pfds, npfds, &evid) == 0) {
            // logger(">> processing %d", evid);
        }

        int pending = snd_seq_event_input_pending(seq, 1);
        if(pending == 0)
            continue;

        if((err = snd_seq_event_input(seq, &event)) > 0) {
            if(event) {
                if(event->type == SND_SEQ_EVENT_PORT_UNSUBSCRIBED) {
                    logger("[-] midi: interface disconnected, closing session");

                    snd_seq_close(seq);
                    kntxt->interface = 2;

                    continue;
                }

                midi_handle_event(event, kntxt, seq);
            }
        }
    }

    snd_seq_close(seq);

    return NULL;
}

//
// console management
//
void console_border_top(char *name) {
    int printed = printf("\033[1;30m┌──┤ \033[34m%s\033[30m ├", name);

    for(int pixel = 0; pixel < PERSEGMENT - printed + 31; pixel++) {
        printf("─");
    }

    printf("┐\033[0m\n");
}

void console_border_bottom() {
    printf("\033[1;30m└");

    for(int pixel = 0; pixel < PERSEGMENT + 3; pixel++) {
        printf("─");
    }

    printf("┘\033[0m");
}

/*
void console_border_single() {
    printf("\033[1;30m│\033[0m");
}

void console_print_line(char *fmt, ...) {
    char buffer[1024];

    // build the string to print
    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    // first write a line with borders
    printf("\033[1;30m│\033[0m%-*s\033[1;30m│\033[0m\r", PERSEGMENT + 3, " ");

    // then write the buffer
    // (this is a workaround for including escape inside buffer)
    printf("\033[1;30m│\033[0m%s\n", buffer);
}
*/

void console_cursor_move(int line, int col) {
    printf("\033[%d;%df", line, col);
}

void console_pane_draw(char *title, int lines, int line, int col) {
    console_cursor_move(line, col);
    console_border_top(title);

    for(int i = 0; i < lines; i++) {
        console_cursor_move(line + i + 1, col);
        printf("\033[1;30m│\033[0m%-*s\033[1;30m│\033[0m\r", PERSEGMENT + 3, " ");
    }

    console_cursor_move(line + lines + 1, col);
    console_border_bottom();
}

void console_panes_refresh() {
    // clean entire screen
    printf("\033[2J\033[H");

    // print panes
    console_pane_draw("Pixel Monitor", 24, 1, 0);
    console_pane_draw("MIDI Channels", 10, 27, 0);
    console_pane_draw("Global Statistics", 8, 39, 0);
    console_pane_draw("System Logger", 15, 49, 0);

    console_pane_draw("Pixel Preview", 24, 1, 127);
    console_pane_draw("Animation Presets", 10, 27, 127);
    console_pane_draw("Animation Masks", 10, 39, 127);
}

void console_pixels_draw(pixel_t *pixels, int shift) {
    for(int line = 0; line < SEGMENTS; line++) {
        console_cursor_move(line + 2, shift);
        printf("\033[0m%2d ", line + 1);

        for(int pixel = 0; pixel < PERSEGMENT; pixel++) {
            int index = (line * PERSEGMENT) + pixel;
            pixel_t *color = &pixels[index];

            printf("\033[38;2;%d;%d;%dm█", color->r, color->g, color->b);
        }
    }
}

void console_list_print(char **list, int total, char *selected, int upper, int left) {
    for(int i = 0; i < total; i++) {
        int colindex = i / 8;
        int lineindex = i % 8;

        console_cursor_move(upper + lineindex, left + (colindex * 40));

        char *color = "";
        char *text = list[i];

        if(list[i] == NULL) {
            color = CEMPTY;
            text = "---";
        }

        if(selected && list[i] == selected)
            color = CSELECTED;

        printf("%02d. %s%s" CRST, (i + 1), color, text);
    }
}

void *thread_console(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    logger_t *logs = &mainlog;
    int upper = 0;

    console_panes_refresh();

    while(kntxt->keepgoing) {
        // preparing relative timing
        struct timeval now;
        gettimeofday(&now, NULL);

        pthread_mutex_lock(&kntxt->lock);

        //
        // pixel dump
        //
        // console_border_top("Pixel Monitoring");
        console_pixels_draw(kntxt->monitor, 2);
        console_pixels_draw(kntxt->preview, 128);

        //
        // midi values
        //
        printf("\033[0m");
        upper = 28;

        console_cursor_move(upper, 2);

        printf("Sliders: ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.sliders[i].value);

        float speedfps = 1000000.0 / kntxt->speed;

        console_cursor_move(upper + 1, 2);
        if(kntxt->blackout) {
            printf("Master: %3d %s", kntxt->midi.master, CBAD(" BLACKOUT ENABLED "));

        } else {
            printf("Master: %3d %-18s", kntxt->midi.master, "");
        }

        console_cursor_move(upper + 2, 2);
        printf("Strobe: %s ", kntxt->strobe ? COK(" on ") : CNULL(" off "));

        console_cursor_move(upper + 2, 14);
        printf(" | refresh %3d / flash %03d / index %3d", kntxt->strobe, kntxt->strobe_duration, kntxt->strobe_index);

        console_cursor_move(upper + 3, 2);
        printf("Speed : % 4d [%.1f fps] %-10s", kntxt->speed, speedfps, "");

        console_cursor_move(upper + 5, 2);
        if(kntxt->interface == 0) {
            printf("Interface: %s %-10s", CBAD(" offline "), "");

        } else if(kntxt->interface == 1) {
            printf("Interface: %s %-10s", COK(" online "), "");

        } else if(kntxt->interface == 2) {
            printf("Interface: %s %-10s", CBAD("  lost  "), "");

        } else {
            printf("Interface: %s %-10s", CWAIT(" unknown "), "");

        }

        // printf("Interface: %s %-10s", kntxt->interface ? COK(" online ") : CBAD(" offline "), "");

        console_cursor_move(upper + 7, 2);
        printf("Preset: %-40s", kntxt->preset);

        console_cursor_move(upper + 8, 2);
        printf("Mask  : %-40s", kntxt->mask ? kntxt->mask : "---");

        //
        // controller and client statistics
        //

        control_stats_t *client = &kntxt->client;
        controller_stats_t *controller = &kntxt->controller;

        double lastping = timediff(&now, &client->ctrl_last_feedback);

        char *sessup = uptime_prettify(controller->time_current - client->ctrl_initial_time);
        char *ctrlup = uptime_prettify(controller->time_current);

        char *state = (controller->state == 0) ? CWAIT(" waiting ") : COK(" online ");
        if(controller->state > 0 && lastping > 2.0)
            state = CBAD(" timed out ");

        upper = 40;
        console_cursor_move(upper, 2);
        printf("Controler: %s %-20s", state, "");

        console_cursor_move(upper + 1, 2);
        if(controller->state == 0) {
            printf("Last seen: %s %-10s", CWAIT(" waiting "), "");

        } else {
            printf("Last seen: %.2f seconds ago %-10s", lastping, "");
        }


        // coloring fps
        char strfps[64];
        sprintf(strfps, CGOOD "%2lu fps" CRST, controller->fps);
        if(controller->fps < 20)
            sprintf(strfps, CWARN "%2lu fps" CRST, controller->fps);

        console_cursor_move(upper + 3, 2);
        printf("Frames displayed: % 6ld, %s", client->showframes, strfps);
        printf(" | Total frames: % 6ld", controller->frames);

        console_cursor_move(upper + 4, 2);
        printf("Frames committed: % 6ld, dropped: %lu [%.1f%%]", client->frames, client->dropped, client->droprate);

        /*
        if(client->frames > 200)
            kntxt->keepgoing = 0;
        */

        console_cursor_move(upper + 6, 2);
        printf("Controler uptime: %s / %.4f ms", ctrlup, client->time_transform * 1000);

        console_cursor_move(upper + 7, 2);
        printf("Interface uptime: %s", sessup);

        free(sessup);
        free(ctrlup);

        // FIXME: stop repeating yourself
        console_cursor_move(upper + 0, 80);

        float psuv = controller->psu0_volt / 100.0;
        float psua = controller->psu0_amps / 100.0;
        int psuw = psuv * psua;

        printf("| PSU 1: % 4.1f v - % 4.2f A - % 4d w", psuv, psua, psuw);

        console_cursor_move(upper + 1, 80);

        psuv = controller->psu1_volt / 100.0;
        psua = controller->psu1_amps / 100.0;
        psuw = psuv * psua;

        printf("| PSU 2: % 4.1f v - % 4.2f A - % 4d w", psuv, psua, psuw);

        console_cursor_move(upper + 2, 80);

        psuv = controller->psu2_volt / 100.0;
        psua = controller->psu2_amps / 100.0;
        psuw = psuv * psua;

        printf("| PSU 3: % 4.1f v - % 4.2f A - % 4d w", psuv, psua, psuw);

        console_cursor_move(upper + 3, 80);
        printf("| Main : % 4.1f v", controller->main_ac_voltage / 100.0);

        console_cursor_move(upper + 4, 80);
        printf("| ");

        console_cursor_move(upper + 5, 80);
        printf("| Core : % 4.1f°C - % 4.1f°C", controller->main_core_temperature / 100.0, controller->mon_core_temperature / 100.0);

        console_cursor_move(upper + 6, 80);
        printf("| Power: % 4.1f°C", controller->ext_power_temperature / 100.0);

        console_cursor_move(upper + 7, 80);
        printf("| Ctrls: % 4.1f°C", controller->ext_compute_temperature / 100.0);




        // we are done with main context
        pthread_mutex_unlock(&kntxt->lock);

        //
        // presets list
        //
        console_list_print(kntxt->presets, kntxt->presets_total, kntxt->preset, 29, 128);

        //
        // masks list
        //
        console_list_print(kntxt->masks, kntxt->masks_total, kntxt->mask, 41, 128);

        //
        // last lines from logger (ring buffer)
        //
        // console_border_top("Logger");
        pthread_mutex_lock(&logs->lock);

        int show = 15;
        int index = logs->nextid - show;
        upper = 50;

        for(int i = 0; i < show; i++) {
            int display = index;

            if(index < 0)
                display = logs->capacity + index;

            console_cursor_move(upper + i, 2);
            printf("%-*s", PERSEGMENT + 3, logs->lines[display] ? logs->lines[display] : "");

            index += 1;
        }

        pthread_mutex_unlock(&logs->lock);

        console_cursor_move(58, 0);
        fflush(stdout);

        thread_wait(40000);
    }

    return NULL;
}

//
// initializer management
//
void cleanup(kntxt_t *kntxt) {
    // master cleaner to check memory sanity (with, eg. valgrind)
    free(kntxt->pixels);
    free(kntxt->monitor);

    // FIXME: preview, midi, ...

    for(int i = 0; i < LOGGER_SIZE; i++)
        free(mainlog.lines[i]);

    free(mainlog.lines);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    printf("[+] initializing stage-led controle interface\n");
    pthread_t netsend, feedback, midi, console, animate, presets, masks;

    // logger initializer
    memset(&mainlog, 0x00, sizeof(logger_t));
    pthread_mutex_init(&mainlog.lock, NULL);
    mainlog.capacity = LOGGER_SIZE;
    mainlog.lines = (char **) calloc(sizeof(char *), mainlog.capacity);

    // create a local context
    kntxt_t mainctx;
    void *kntxt = &mainctx;

    // default initialization
    memset(kntxt, 0x00, sizeof(kntxt_t));

    mainctx.keepgoing = 1;

    mainctx.pixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.maskpixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.monitor = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.preview = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);

    mainctx.midi.lines = 8; // 8 channels
    mainctx.midi.sliders = calloc(sizeof(slider_t), mainctx.midi.lines);
    mainctx.speed = 1000000 / TARGET_FPS;

    mainctx.presets_total = 24;
    mainctx.masks_total = 24;

    mainctx.presets = calloc(sizeof(char *), mainctx.presets_total);
    mainctx.masks = calloc(sizeof(char *), mainctx.masks_total);
    mainctx.midi.presets = calloc(sizeof(uint8_t), mainctx.presets_total);
    mainctx.midi.masks = calloc(sizeof(uint8_t), mainctx.masks_total);

    // mainctx.presets[1] = "/home/maxux/git/stageled/templates/kermesse.png";
    // mainctx.presets[2] = "/home/maxux/git/stageled/templates/spectre3.png";
    // mainctx.presets[3] = "/home/maxux/git/stageled/templates/stan.png";
    // mainctx.presets[4] = "/home/maxux/git/stageled/templates/spectre2.png";
    // mainctx.presets[5] = "/home/maxux/git/stageled/templates/follow1.png";
    // mainctx.presets[6] = "/home/maxux/git/stageled/templates/cedric.png";

    int i = 0;
    mainctx.presets[i++] = "debug.png";
    mainctx.presets[i++] = "thunder-colors-1.png";
    mainctx.presets[i++] = "thunder-colors-2.png";
    mainctx.presets[i++] = "thunder-colors-3.png";
    mainctx.presets[i++] = "thunder-test.png";
    mainctx.presets[i++] = "linear-solid.png";

    mainctx.presets[i++] = "rainbow.png";
    mainctx.presets[i++] = "testku.png";
    mainctx.presets[22] = "full-black.png";
    mainctx.presets[23] = "full.png";

    mainctx.preset = mainctx.presets[0];

    i = 0;
    mainctx.masks[i++] = "mask-diagonal.png";
    mainctx.masks[i++] = "mask-holes.png";
    mainctx.masks[i++] = "mask-thunder.png";
    mainctx.masks[i++] = "mask-thunder-1.png";
    mainctx.masks[i++] = "mask-thunder-2.png";
    mainctx.masks[i++] = "mask-thunder-front-string.png";
    mainctx.masks[i++] = "mask-thunder-full-string.png";
    mainctx.masks[i++] = "mask-thunder-segment-smooth.png";
    mainctx.masks[i++] = "mask-thunder-segment-smooth-2.png";
    mainctx.masks[i++] = "mask-thunder-pattern-1.png";
    mainctx.masks[i++] = "mask-thunder-pattern-2.png";
    mainctx.masks[i++] = "mask-segments-roll.png";
    mainctx.masks[i++] = "mask-smooth-cross.png";

    // loading default frame
    mainctx.frame = frame_loadfile(mainctx.preset);

    pthread_mutex_init(&mainctx.lock, NULL);
    pthread_cond_init(&mainctx.cond_presets, NULL);
    pthread_cond_init(&mainctx.cond_masks, NULL);

    printf("[+] starting network dispatcher thread\n");
    if(pthread_create(&netsend, NULL, thread_netsend, kntxt))
        perror("thread: netsend");

    printf("[+] starting controller feedback thread\n");
    if(pthread_create(&feedback, NULL, thread_feedback, kntxt))
        perror("thread: feedback");

    printf("[+] starting midi mapping thread\n");
    if(pthread_create(&midi, NULL, thread_midi, kntxt))
        perror("thread: midi");

    printf("[+] starting animator thread\n");
    if(pthread_create(&animate, NULL, thread_animate, kntxt))
        perror("thread: animate");

    printf("[+] starting presets thread\n");
    if(pthread_create(&presets, NULL, thread_presets, kntxt))
        perror("thread: presets");

    printf("[+] starting masks thread\n");
    if(pthread_create(&masks, NULL, thread_masks, kntxt))
        perror("thread: masks");

    // starting console at the very end to keep screen clean
    // if some early error appears
    printf("[+] starting console monitoring thread\n");
    if(pthread_create(&console, NULL, thread_console, kntxt))
        perror("thread: console");

    pthread_join(netsend, NULL);
    pthread_join(feedback, NULL);
    pthread_join(midi, NULL);
    pthread_join(animate, NULL);
    pthread_join(presets, NULL);
    pthread_join(masks, NULL);
    pthread_join(console, NULL);

    cleanup(kntxt);

    return 0;
}
