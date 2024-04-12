#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
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
#define CWAIT(x)    "\033[1;37;44m" x CRST
#define COK(x)      "\033[1;37;42m" x CRST
#define CBAD(x)     "\033[1;37;41m" x CRST

#define APC_COLOR_BLACK       0
#define APC_COLOR_WHITE       3
#define APC_COLOR_RED         5
#define APC_COLOR_GREEN       21
#define APC_COLOR_BLUE        45
#define APC_COLOR_YELLOW      13
#define APC_COLOR_LIGHT_BLUE  36
#define APC_COLOR_PURPLE      53

#define APC_BLACKOUT_COLOR    APC_COLOR_WHITE

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

typedef struct frame_t {
    uint32_t *pixels;
    int width;
    int height;
    size_t length;

} frame_t;

typedef struct controler_stats_t {
    uint64_t state;
    uint64_t old_frames;
    uint64_t frames;
    uint64_t fps;
    uint64_t time_last_frame;
    uint64_t time_current;

} controler_stats_t;

typedef struct control_stats_t {
    uint64_t frames;
    uint64_t ctrl_initial_frames;
    uint64_t ctrl_initial_time;
    uint64_t showframes;
    uint64_t dropped;
    double droprate;
    struct timeval ctrl_last_feedback;

} control_stats_t;

typedef struct kntxt_t {
    pixel_t *pixels;
    pixel_t *monitor; // monitoring output

    frame_t *frame;
    transform_t midi;
    useconds_t speed;
    uint8_t blackout;
    uint8_t strobe;
    uint8_t strobe_state;
    uint32_t strobe_index;

    char *presets[8];
    char *preset;

    // remote and local stats
    controler_stats_t controler;
    control_stats_t client;
    char *controladdr;

    // flags to monitor interface presence
    uint8_t interface;

    // master thread locking (FIXME)
    pthread_mutex_t lock;

    pthread_cond_t cond_presets;
    pthread_mutex_t mutcond_presets; // condition mutex

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

    logger("[+] loader: image dimension: %d x %d px", width, height);

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

    // cleanup working png stuff
    for(int y = 0; y < height; y++)
        free(lines[y]);

    free(lines);

    png_destroy_read_struct(&ctx, &info, NULL);

    return frame;
}

void *thread_animate(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    frame_t *frame;
    int line = 0;

    // allocate local copy of pixels
    pixel_t *localpixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);

    // fetch initial frame already loaded by loader
    pthread_mutex_lock(&kntxt->lock);
    // remove frame from context, keeping it for us
    frame = kntxt->frame;
    kntxt->frame = NULL;
    pthread_mutex_unlock(&kntxt->lock);

    while(1) {
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
        pthread_mutex_unlock(&kntxt->lock);

        for(int a = 0; a < LEDSTOTAL; a++) {
            pixel_t target = {
                .r = (frame->pixels[(line * frame->width) + a] & 0xff0000) >> 16,
                .g = (frame->pixels[(line * frame->width) + a] & 0x00ff00) >> 8,
                .b = (frame->pixels[(line * frame->width) + a] & 0x0000ff),
            };

            localpixels[a] = target;
        }

        // commit this frame pixel to main context
        pthread_mutex_lock(&kntxt->lock);
        memcpy(kntxt->pixels, localpixels, sizeof(pixel_t) * LEDSTOTAL);
        useconds_t waiting = kntxt->speed;
        pthread_mutex_unlock(&kntxt->lock);

        // wait relative to speed for the next frame
        usleep(waiting);

        line += 1;
        if(line >= frame->height)
            line = 0;
    }

    return NULL;
}

//
// presets worker and loader
//
void *thread_presets(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    frame_t *frame;

    logger("[+] presets: initializing presets, loading default one");

    while(1) {
        pthread_cond_wait(&kntxt->cond_presets, &kntxt->mutcond_presets);

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
//
// network transmitter management
//
int netsend_transmit_frame(uint8_t *bitmap, char *target) {
    struct sockaddr_in serveraddr;

    char *hostname = target;
    int portno = 1111;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0)
        diep("socket");

    struct hostent *server = gethostbyname(hostname);
    if(server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;

    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);

    int serverlen = sizeof(serveraddr);

    // sending bitmap
    if(sendto(sockfd, bitmap, BITMAPSIZE, 0, (struct sockaddr *) &serveraddr, serverlen) < 0)
      diep("sendto");

    close(sockfd);

    return 0;
}

void netsend_pixels_transform(kntxt_t *kntxt, pixel_t *localpixels, uint8_t *localbitmap) {
    // fetch settings from main context
    pthread_mutex_lock(&kntxt->lock);

    uint8_t rawmaster = kntxt->midi.master;
    uint8_t blackout = kntxt->blackout;

    uint8_t strobe = kntxt->strobe;
    uint8_t strobe_index = kntxt->strobe_index;
    uint8_t strobe_state = kntxt->strobe_state;

    uint8_t colorize[] = {
        kntxt->midi.channels[0].high,
        kntxt->midi.channels[0].mid,
        kntxt->midi.channels[0].low,
    };

    uint8_t segments[] = {
        kntxt->midi.channels[0].slider,
        kntxt->midi.channels[1].slider,
        kntxt->midi.channels[2].slider,
    };

    if(strobe)
        kntxt->strobe_index += 1;

    // applying settings to frame
    if(blackout)
        rawmaster = 0;

    // checking if a strobe mask needs to be applied
    if(strobe && !blackout) {
        int divider = ((256 - strobe) / 20);
        if(divider < 1)
            divider = 1;

        // check if we need to change the state
        if(strobe_index % divider == 0) {
            strobe_state = (strobe_state) ? 0 : 1;
            kntxt->strobe_state = strobe_state;
        }

        // check if we need to apply something
        if(strobe_state == 0)
            rawmaster = 0;
    }

    pthread_mutex_unlock(&kntxt->lock);

    if(colorize[0] || colorize[1] || colorize[2]) {
        float redmul = (255 - colorize[0]) / 255.0;
        float greenmul = (255 - colorize[1]) / 255.0;
        float bluemul = (255 - colorize[2]) / 255.0;

        for(int i = 0; i < LEDSTOTAL; i++) {
            localpixels[i].r = (uint8_t) (localpixels[i].r * redmul);
            localpixels[i].g = (uint8_t) (localpixels[i].g * greenmul);
            localpixels[i].b = (uint8_t) (localpixels[i].b * bluemul);
        }
    }

    if(segments[0] || segments[1] || segments[2]) {
        for(int segment = 0; segment < 3; segment++) {
            float attenuation = (255 - segments[segment]) / 255.0;

            for(int i = (segment * PERSEGMENT); i < ((segment + 1) * PERSEGMENT); i++) {
                localpixels[i].r = (uint8_t) (localpixels[i].r * attenuation);
                localpixels[i].g = (uint8_t) (localpixels[i].g * attenuation);
                localpixels[i].b = (uint8_t) (localpixels[i].b * attenuation);
            }
        }
    }

    // only compute floating master transformation if needed
    if(rawmaster < 255 && rawmaster > 0) {
        double master = rawmaster / 255.0;

        for(int i = 0; i < LEDSTOTAL; i++) {
            localpixels[i].r = (uint8_t) (localpixels[i].r * master);
            localpixels[i].g = (uint8_t) (localpixels[i].g * master);
            localpixels[i].b = (uint8_t) (localpixels[i].b * master);
        }
    }

    if(rawmaster == 0) {
        // force setting all pixels to zero in a more efficient way
        memset(localpixels, 0x00, sizeof(pixel_t) * LEDSTOTAL);
    }

    // building network frame bitmap
    for(int i = 0; i < LEDSTOTAL; i++) {
        localbitmap[(i * 3) + 0] = localpixels[i].r;
        localbitmap[(i * 3) + 1] = localpixels[i].g;
        localbitmap[(i * 3) + 2] = localpixels[i].b;
    }
}

void *thread_netsend(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    char *controladdr;

    logger("[+] netsend: sending frames to controler");

    pixel_t *localpixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    uint8_t *localbitmap = (uint8_t *) calloc(sizeof(uint8_t), BITMAPSIZE);

    while(1) {
        // fetch current frame pixel from animate
        pthread_mutex_lock(&kntxt->lock);

        memcpy(localpixels, kntxt->pixels, sizeof(pixel_t) * LEDSTOTAL);
        controladdr = kntxt->controladdr;

        pthread_mutex_unlock(&kntxt->lock);

        // apply transformation
        netsend_pixels_transform(kntxt, localpixels, localbitmap);

        // commit transformation to monitor to see changes on console
        pthread_mutex_lock(&kntxt->lock);
        memcpy(kntxt->monitor, localpixels, sizeof(pixel_t) * LEDSTOTAL);
        kntxt->client.frames += 1;
        pthread_mutex_unlock(&kntxt->lock);

        // sending the frame to the controler (if alive)
        if(controladdr)
            netsend_transmit_frame(localbitmap, controladdr);

        usleep(1000000 / TARGET_FPS);
    }

    free(localpixels);
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
    unsigned long clientaddr;
    int clientlen;
    int sock;

    if((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        diep("feedback: socket");

    memset(&name, 0x00, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    name.sin_port = htons(1111);

    // no previous client
    // avoid initializing to zero (fake mismatch)
    clientaddr = 1;

    if(bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0)
        diep("feedback: bind");

    logger("[+] feedback: waiting for incoming packets");

    while(1) {
        int bytes = recvfrom(sock, message, sizeof(message), 0, (struct sockaddr *) &client, (socklen_t *) &clientlen);

        if(bytes <= 0) {
            usleep(10000);
            continue;
        }

        // FIXME: why first frame is not set correctly ?
        if(client.sin_addr.s_addr == 0)
            continue;

        pthread_mutex_lock(&kntxt->lock);

        if(kntxt->controler.time_current == 0) {
            logger("[+] feedback: first message received from the controler");

            // save a copy of initial message to be able
            // to compute relative counters
            controler_stats_t initial;
            memcpy(&initial, message, bytes);

            // resetting internal frames counter
            // to get droprate in sync
            kntxt->client.frames = 0;

            kntxt->client.ctrl_initial_frames = initial.frames;
            kntxt->client.ctrl_initial_time = initial.time_current;

            logger("[+] feedback: relative frames: %lu, time: %lu", initial.frames, initial.time_current);
        }

        if(clientaddr != client.sin_addr.s_addr && client.sin_addr.s_addr != 0) {
            // save this address as last client address
            clientaddr = client.sin_addr.s_addr;

            inet_ntop(AF_INET, &client.sin_addr, ctrladdr, sizeof(ctrladdr));
            logger("[+] feedback: received from: %s, updating", ctrladdr);

            // linking this client to main network stack
            free(kntxt->controladdr);
            kntxt->controladdr = strdup(ctrladdr);
        }

        // make a lazy binary copy from controler
        memcpy(&kntxt->controler, message, bytes);
        gettimeofday(&kntxt->client.ctrl_last_feedback, NULL);

        kntxt->client.showframes = (kntxt->controler.frames - kntxt->client.ctrl_initial_frames);
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

    unsigned int limlow[] = {16, 20, 24, 28, 46, 50, 54, 58};
    unsigned int presets[] = {3, 6, 9, 12, 15, 18, 21, 24};

    if(ev->type == SND_SEQ_EVENT_NOTEON) {
        logger("[+] midi: note on, note: %d", ev->data.note.note);

        for(unsigned int i = 0; i < sizeof(presets) / sizeof(unsigned int); i++) {
            if(presets[i] == ev->data.note.note) {
                pthread_mutex_lock(&kntxt->lock);

                logger("[+] loading preset %d: %s", i + 1, kntxt->presets[i]);
                kntxt->preset = kntxt->presets[i];
                pthread_cond_signal(&kntxt->cond_presets);

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

        if(ev->data.note.note == 0) {
            if(kntxt->midi.channels[0].high == 255) {
                midi_set_control(seq, APC_SOLID_100, ev->data.note.note, APC_COLOR_RED);
                kntxt->midi.channels[0].high = 0;

            } else {
                midi_set_control(seq, APC_BLINK_1_8, ev->data.note.note, APC_COLOR_RED);
                kntxt->midi.channels[0].high = 255;
            }
        }

        if(ev->data.note.note == 1) {
            if(kntxt->midi.channels[0].mid == 255) {
                midi_set_control(seq, APC_SOLID_100, ev->data.note.note, APC_COLOR_GREEN);
                kntxt->midi.channels[0].mid = 0;

            } else {
                midi_set_control(seq, APC_BLINK_1_8, ev->data.note.note, APC_COLOR_GREEN);
                kntxt->midi.channels[0].mid = 255;
            }
        }

        if(ev->data.note.note == 2) {
            if(kntxt->midi.channels[0].low == 255) {
                midi_set_control(seq, APC_SOLID_100, ev->data.note.note, APC_COLOR_BLUE);
                kntxt->midi.channels[0].low = 0;

            } else {
                midi_set_control(seq, APC_BLINK_1_8, ev->data.note.note, APC_COLOR_BLUE);
                kntxt->midi.channels[0].low = 255;
            }
        }


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
        // printf("noteoff, note: %d", ev->data.note.note);
    }

    if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
        // logger("[+] midi: fader: param: %d, value: %d", ev->data.control.param, ev->data.control.value);

        pthread_mutex_lock(&kntxt->lock);

        /*
        for(unsigned int i = 0; i < sizeof(limlow) / sizeof(int); i++) {
            if(ev->data.control.param >= limlow[i] && ev->data.control.param < limlow[i] + 4) {
                if(ev->data.control.param == limlow[i])
                    kntxt->midi.channels[i].high = midi_value_parser(ev->data.control.value);

                if(ev->data.control.param == limlow[i] + 1)
                    kntxt->midi.channels[i].mid = midi_value_parser(ev->data.control.value);

                if(ev->data.control.param == limlow[i] + 2)
                    kntxt->midi.channels[i].low = midi_value_parser(ev->data.control.value);

                if(ev->data.control.param == limlow[i] + 3)
                    kntxt->midi.channels[i].slider = midi_value_parser(ev->data.control.value);

            }

        }
        */

        if(ev->data.control.param > 47 && ev->data.control.param < 56)
            kntxt->midi.channels[ev->data.control.param - 48].slider = midi_value_parser(ev->data.control.value);

        // master channel
        if(ev->data.control.param == 56)
            kntxt->midi.master = midi_value_parser(ev->data.control.value);

        pthread_mutex_unlock(&kntxt->lock);
    }

    pthread_mutex_lock(&kntxt->lock);

    if(kntxt->midi.channels[7].slider > 0) {
        kntxt->speed = (1000000 / kntxt->midi.channels[7].slider);

    } else {
        kntxt->speed = 1000000 / TARGET_FPS;
    }

    // apply strobe value
    kntxt->strobe = kntxt->midi.channels[6].slider;
    if(kntxt->strobe == 0) {
        kntxt->strobe_index = 0;
        kntxt->strobe_state = 0;
    }

    pthread_mutex_unlock(&kntxt->lock);

    return 0;
}

void *thread_midi(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    snd_seq_t *seq;
    snd_seq_addr_t *ports;
    int err;

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
        kntxt->midi.master = 255;
        logger("[-] midi: parse address: %s", snd_strerror(err));
        return NULL;
    }

    if((err = snd_seq_connect_from(seq, 0, ports[0].client, ports[0].port)) < 0) {
        kntxt->midi.master = 255;
        logger("[-] midi: connect from: %s", snd_strerror(err));
        return NULL;
    }

    if((err = snd_seq_connect_to(seq, 0, ports[0].client, ports[0].port)) < 0) {
        logger("[-] midi: connect to: %s", snd_strerror(err));
    }

    struct pollfd *pfds;
    int npfds;

    npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    pfds = alloca(sizeof(*pfds) * npfds);

    kntxt->interface = 1;
    logger("[+] midi: interface connected, initializing");

    // reset all leds
    for(int i = 0; i < 64; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    for(int i = 0x64; i < 0x6b; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    for(int i = 0x70; i < 0x7a; i++)
        midi_set_control(seq, APC_SOLID_10, i, APC_COLOR_BLACK);

    // set blackout default
    midi_set_control(seq, APC_SOLID_100, 0x07, APC_BLACKOUT_COLOR);

    // set red, green, blue channel cut
    midi_set_control(seq, APC_SOLID_100, 0x00, APC_COLOR_RED);
    midi_set_control(seq, APC_SOLID_100, 0x01, APC_COLOR_GREEN);
    midi_set_control(seq, APC_SOLID_100, 0x02, APC_COLOR_BLUE);

    // set presets colors
    for(int i = 0x38; i < 0x40; i++)
        midi_set_control(seq, APC_SOLID_100, i, APC_COLOR_YELLOW);

    // set initial preset
    midi_set_control(seq, APC_PULSE_1_4, 0x38, APC_COLOR_YELLOW);

    logger("[+] midi: interface initialized");

    // polling events
    while(1) {
        snd_seq_event_t *event;

        snd_seq_poll_descriptors(seq, pfds, npfds, POLLIN);
        if(poll(pfds, npfds, -1) < 0)
            diep("poll");

        while((err = snd_seq_event_input(seq, &event)) > 0) {
            if(!event)
                continue;

           midi_handle_event(event, kntxt, seq);
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

    printf("┘\033[0m\n");
}

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

void *thread_console(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    logger_t *logs = &mainlog;

    // printf("[+] preparing console for monitoring\n");
    // usleep(5000000);

    printf("\033[2J"); // clean entire screen

    while(1) {
        // preparing relative timing
        struct timeval now;
        gettimeofday(&now, NULL);

        pthread_mutex_lock(&kntxt->lock);

        printf("\033[H"); // cursor to home

        //
        // pixel dump
        //
        console_border_top("Pixel Monitoring");

        for(int line = 0; line < SEGMENTS; line++) {
            printf("\033[1;30m│%2d ", line + 1);

            for(int pixel = 0; pixel < PERSEGMENT; pixel++) {
                int index = (line * PERSEGMENT) + pixel;
                pixel_t *color = &kntxt->monitor[index];

                printf("\033[38;2;%d;%d;%dm█", color->r, color->g, color->b);
            }

            printf("\033[1;30m│\n");
        }

        console_border_bottom();

        //
        // midi values
        //
        console_border_top("MIDI Channels");

        console_border_single();
        printf(" HI ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].high);

        printf("\n");

        console_border_single();
        printf(" MI ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].mid);

        printf("\n");

        console_border_single();
        printf(" LO ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].low);

        printf("\n");

        console_print_line("\033[1;30m ──────────────────────────────────────────── ");

        console_border_single();
        printf(" SL ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].slider);

        printf("\n");

        float speedfps = 1000000.0 / kntxt->speed;

        console_print_line(" ");
        if(kntxt->blackout) {
            console_print_line("Master: %3d %s", kntxt->midi.master, CBAD(" BLACKOUT ENABLED "));

        } else {
            console_print_line("Master: %3d", kntxt->midi.master);
        }

        console_print_line("Strobe: %3d [index %3d]", kntxt->strobe, kntxt->strobe_index);
        console_print_line("Speed : % 4d [%.1f fps]", kntxt->speed, speedfps);
        console_print_line("");
        console_print_line("Interface: %s", kntxt->interface ? COK(" online ") : CBAD(" offline "));
        console_print_line("Animating: %s", kntxt->preset);

        console_border_bottom();

        //
        // controler and client statistics
        //
        console_border_top("Statistics");

        control_stats_t *client = &kntxt->client;
        controler_stats_t *controler = &kntxt->controler;

        double lastping = timediff(&now, &client->ctrl_last_feedback);

        char *sessup = uptime_prettify(controler->time_current - client->ctrl_initial_time);
        char *ctrlup = uptime_prettify(controler->time_current);

        char *state = (controler->state == 0) ? CWAIT(" waiting ") : COK(" online ");
        if(controler->state > 0 && lastping > 2.0)
            state = CBAD(" timed out ");

        console_print_line("Controler: %s", state);

        if(controler->state == 0) {
            console_print_line("Last seen: %s", CWAIT(" waiting "));

        } else {
            console_print_line("Last seen: %.2f seconds ago", lastping);
        }

        console_print_line("");

        // coloring fps
        char strfps[64];
        sprintf(strfps, CGOOD "%2lu fps" CRST, controler->fps);
        if(controler->fps < 20)
            sprintf(strfps, CWARN "%2lu fps" CRST, controler->fps);

        console_print_line("Frames displayed: % 6lu, %s", client->showframes, strfps);
        console_print_line("Frames committed: % 6lu, dropped: %lu [%.1f%%]", client->frames, client->dropped, client->droprate);

        console_print_line("");
        console_print_line("Controler uptime: %s", ctrlup);
        console_print_line("Interface uptime: %s", sessup);

        free(sessup);
        free(ctrlup);

        console_border_bottom();

        // we are done with main context
        pthread_mutex_unlock(&kntxt->lock);

        //
        // last lines from logger (ring buffer)
        //
        console_border_top("Logger");
        pthread_mutex_lock(&logs->lock);

        int show = 6;
        int index = logs->nextid - show;

        for(int i = 0; i < show; i++) {
            int display = index;

            if(index < 0)
                display = logs->capacity + index;

            console_print_line("%s", logs->lines[display] ? logs->lines[display] : "");

            index += 1;
        }

        pthread_mutex_unlock(&logs->lock);
        console_border_bottom();

        usleep(40000);
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

    for(int i = 0; i < LOGGER_SIZE; i++)
        free(mainlog.lines[i]);

    free(mainlog.lines);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    printf("[+] initializing stage-led controle interface\n");
    pthread_t netsend, feedback, midi, console, animate, presets;

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

    mainctx.pixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.monitor = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);

    mainctx.midi.lines = 8; // 8 channels
    mainctx.speed = 1000000 / TARGET_FPS;

    mainctx.presets[0] = "/home/maxux/git/stageled/templates/debug.png";
    mainctx.presets[1] = "/home/maxux/git/stageled/templates/kermesse.png";
    mainctx.presets[2] = "/home/maxux/git/stageled/templates/spectre3.png";
    mainctx.presets[3] = "/home/maxux/git/stageled/templates/stan.png";
    mainctx.presets[4] = "/home/maxux/git/stageled/templates/spectre2.png";
    mainctx.presets[5] = "/home/maxux/git/stageled/templates/follow1.png";
    mainctx.presets[6] = "/home/maxux/git/stageled/templates/cedric.png";
    mainctx.presets[7] = "/home/maxux/git/stageled/templates/full.png";
    mainctx.preset = mainctx.presets[0];

    // loading default frame
    mainctx.frame = frame_loadfile(mainctx.preset);

    pthread_mutex_init(&mainctx.lock, NULL);
    pthread_cond_init(&mainctx.cond_presets, NULL);

    printf("[+] starting network dispatcher thread\n");
    if(pthread_create(&netsend, NULL, thread_netsend, kntxt))
        perror("thread: netsend");

    printf("[+] starting controler feedback thread\n");
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

    // starting console at the very end to keep screen clean
    // if some early error appears
    printf("[+] starting console monitoring thread\n");
    if(pthread_create(&console, NULL, thread_console, kntxt))
        perror("thread: console");

    pthread_join(netsend, NULL);
    pthread_join(feedback, NULL);
    pthread_join(midi, NULL);
    pthread_join(console, NULL);
    pthread_join(animate, NULL);

    cleanup(kntxt);

    return 0;
}
