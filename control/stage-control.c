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

#define CRST        "\033[0m"
#define CWAIT(x)    "\033[1;37;44m" x CRST
#define COK(x)      "\033[1;37;42m" x CRST
#define CBAD(x)     "\033[1;37;41m" x CRST

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
    struct timeval ctrl_last_feedback;

} control_stats_t;

typedef struct kntxt_t {
    pixel_t *pixels;
    frame_t *frame;
    uint8_t *bitmap;
    transform_t midi;
    char *presets[8];
    useconds_t speed;
    char blackout;

    // remote and local stats
    controler_stats_t controler;
    control_stats_t client;

    // flags to monitor interface presence
    uint8_t interface;

    // thread locking
    pthread_mutex_t lock;

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
// image management
//
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

    return frame;
}

//
// network frame transmiter
//
void animate_transform_apply(kntxt_t *kntxt) {
    double master = kntxt->midi.master / 255.0;

    if(kntxt->blackout)
        master = 0;

    for(int i = 0; i < LEDSTOTAL; i++) {
        kntxt->pixels[i].r = (uint8_t) (kntxt->pixels[i].r * master);
        kntxt->pixels[i].g = (uint8_t) (kntxt->pixels[i].g * master);
        kntxt->pixels[i].b = (uint8_t) (kntxt->pixels[i].b * master);
    }

    if(kntxt->midi.channels[7].slider > 0) {
        kntxt->speed = (1000 * kntxt->midi.channels[7].slider);

    } else {
        kntxt->speed = 50000;
    }

    // building network frame bitmap
    for(int i = 0; i < LEDSTOTAL; i++) {
        kntxt->bitmap[(i * 3) + 0] = kntxt->pixels[i].r;
        kntxt->bitmap[(i * 3) + 1] = kntxt->pixels[i].g;
        kntxt->bitmap[(i * 3) + 2] = kntxt->pixels[i].b;
    }
}

void *thread_animate(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    kntxt->frame = loadframe(kntxt->presets[0]);

    int line = 0;

    while(1) {
        pthread_mutex_lock(&kntxt->lock);

        // link frame to main frame
        frame_t *frame = kntxt->frame;

        for(int a = 0; a < LEDSTOTAL; a++) {
            pixel_t target = {
                .r = (frame->pixels[(line * frame->width) + a] & 0xff0000) >> 16,
                .g = (frame->pixels[(line * frame->width) + a] & 0x00ff00) >> 8,
                .b = (frame->pixels[(line * frame->width) + a] & 0x0000ff),
            };

            kntxt->pixels[a] = target;
        }

        animate_transform_apply(kntxt);
        useconds_t waiting = kntxt->speed;

        pthread_mutex_unlock(&kntxt->lock);

        usleep(waiting);

        line += 1;
        if(line >= frame->height)
            line = 0;
    }

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
    unsigned int presets[] = {3, 6, 9, 12, 15, 18, 21, 24};


    if(ev->type == SND_SEQ_EVENT_NOTEON) {
        // printf("noteon, note: %d", ev->data.note.note);

        for(unsigned int i = 0; i < sizeof(presets) / sizeof(unsigned int); i++) {
            if(presets[i] == ev->data.note.note) {
                logger("[+] loading preset %d: %s", i + 1, kntxt->presets[i]);
                // printf("\033[K\n");

                pthread_mutex_lock(&kntxt->lock);

                frame_t *frame = loadframe(kntxt->presets[i]);
                kntxt->frame = frame;

                pthread_mutex_unlock(&kntxt->lock);

                // FIXME: cleanup, concurrence...

                return 0;
            }
        }

        pthread_mutex_lock(&kntxt->lock);

        if(ev->data.note.note == 27) {
            kntxt->blackout = kntxt->blackout ? 0 : 1;
        }

        pthread_mutex_unlock(&kntxt->lock);

    }

    if(ev->type == SND_SEQ_EVENT_NOTEOFF) {
        // printf("noteoff, note: %d", ev->data.note.note);
    }

    if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
        pthread_mutex_lock(&kntxt->lock);

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

            if(ev->data.control.param == 62)
                kntxt->midi.master = midi_value_parser(ev->data.control.value);
        }

        pthread_mutex_unlock(&kntxt->lock);

        // printf("controller, param: %d, value: %d", ev->data.control.param, ev->data.control.value);

        /*
        if(ev->data.control.param == 57)
            univers[2] = ev->data.control.value * 2;
        */

    }

    pthread_mutex_lock(&kntxt->lock);

    // animate_transform_apply(kntxt);

    pthread_mutex_unlock(&kntxt->lock);

    // printf("\n");

    return 0;
}

int netsend_transmit_frame(kntxt_t *kntxt) {
    struct sockaddr_in serveraddr;

    char *hostname = "10.241.0.133";
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

    int n = sendto(sockfd, kntxt->bitmap, BITMAPSIZE, 0, (struct sockaddr *) &serveraddr, serverlen);
    if (n < 0)
      diep("sendto");

    close(sockfd);

    return 0;
}

void *thread_netsend(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;

    logger("[+] netsend: sending frames to controler");

    while(1) {
        pthread_mutex_lock(&kntxt->lock);

        netsend_transmit_frame(kntxt);

        pthread_mutex_unlock(&kntxt->lock);

        usleep(50000);
    }

    return NULL;
}

void *thread_feedback(void *extra) {
    kntxt_t *kntxt = (kntxt_t *) extra;
    char message[1024];
    struct sockaddr_in name;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0)
        diep("feedback: socket");

    bzero((char *) &name, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    name.sin_port = htons(1111);

    if(bind(sock, (struct sockaddr *) &name, sizeof(name)))
        diep("feedback: bind");

    logger("[+] feedback: waiting for incoming packets");

    while(1) {
        int bytes = read(sock, message, sizeof(message));
        if(bytes <= 0) {
            usleep(10000);
            continue;
        }

        pthread_mutex_lock(&kntxt->lock);

        if(kntxt->controler.time_current == 0) {
            logger("[+] feedback: first message received from the controler");

            // save a copy of initial message to be able
            // to compute relative counters
            controler_stats_t initial;
            memcpy(&initial, message, bytes);

            kntxt->client.ctrl_initial_frames = initial.frames;
            kntxt->client.ctrl_initial_time = initial.time_current;

            logger("[+] feedback: relative frames: %lu, time: %lu", initial.frames, initial.time_current);
        }

        // make a lazy binary copy from controler
        memcpy(&kntxt->controler, message, bytes);
        gettimeofday(&kntxt->client.ctrl_last_feedback, NULL);

        pthread_mutex_unlock(&kntxt->lock);
    }

    close(sock);

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
    if((err = snd_seq_parse_address(seq, &ports[0], "MIDI Mix")) < 0) {
        kntxt->midi.master = 255;
        logger("[-] midi: parse address: %s", snd_strerror(err));
        return NULL;
    }

    if((err = snd_seq_connect_from(seq, 0, ports[0].client, ports[0].port)) < 0) {
        kntxt->midi.master = 255;
        logger("[-] midi: connect: %s", snd_strerror(err));
        return NULL;
    }

    struct pollfd *pfds;
    int npfds;

    npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    pfds = alloca(sizeof(*pfds) * npfds);

    kntxt->interface = 1;
    logger("[+] midi: interface connected");

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

void console_print_line(char *fmt, ...) {
    char buffer[1024];

    // build the string to print
    va_list va;
    va_start(va, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, va);
    va_end(va);

    // enclose the line into borders
    printf("\033[1;30m│\033[0m%-*s\033[1;30m│\n", PERSEGMENT + 3, buffer);
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
                pixel_t *color = &kntxt->pixels[index];

                printf("\033[38;2;%d;%d;%dm█", color->r, color->g, color->b);
            }

            printf("\033[1;30m│\n");
        }

        console_border_bottom();

        //
        // midi values
        //
        console_border_top("MIDI Channels");

        printf("│ ");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].high);

        printf("│\n│");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].mid);

        printf("│\n│");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].low);

        printf("│\n│");
        for(int i = 0; i < kntxt->midi.lines; i++)
            printf("% 4d ", kntxt->midi.channels[i].slider);

        console_print_line(" ");
        console_print_line("Master: % 4d", kntxt->midi.master);
        console_print_line("%s", kntxt->interface ? COK(" interface online ") : CBAD(" interface offline "));

        console_border_bottom();

        //
        // controler and client statistics
        //
        console_border_top("Statistics");

        double lastping = timediff(&now, &kntxt->client.ctrl_last_feedback);

        size_t showframes = kntxt->controler.frames - kntxt->client.ctrl_initial_frames;
        size_t dropped = kntxt->client.frames - showframes;
        float droprate = (dropped / (double) kntxt->client.frames) * 100;

        char *sessup = uptime_prettify(kntxt->controler.time_current - kntxt->client.ctrl_initial_time);
        char *ctrlup = uptime_prettify(kntxt->controler.time_current);

        char *state = (kntxt->controler.state == 0) ? CWAIT(" waiting ") : COK(" online ");
        if(kntxt->controler.state > 0 && lastping > 2.0)
            state = CBAD(" timed out ");

        console_print_line("Controler: %s", state);

        if(kntxt->controler.state == 0) {
            console_print_line("Last seen: %s", CWAIT(" waiting "));

        } else {
            console_print_line("Last seen: %.1f seconds ago", lastping);
        }

        console_print_line("");

        console_print_line("Frames displayed: % 6lu [%2d fps]", showframes, kntxt->controler.fps);
        console_print_line("Frames committed: %lu, dropped: %lu [%.1f%%]", kntxt->client.frames, dropped, droprate);

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


        usleep(10000);
    }

    return NULL;
}

void cleanup(kntxt_t *kntxt) {
    // master cleaner to check memory sanity (with, eg. valgrind)
    free(kntxt->pixels);
    free(kntxt->bitmap);

    for(int i = 0; i < LOGGER_SIZE; i++)
        free(mainlog.lines[i]);

    free(mainlog.lines);
}

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;

    printf("[+] initializing stage-led controle interface\n");
    pthread_t netsend, feedback, midi, console, animate;

    // logger initializer
    memset(&mainlog, 0x00, sizeof(logger_t));
    pthread_mutex_init(&mainlog.lock, NULL);
    mainlog.capacity = LOGGER_SIZE;
    mainlog.lines = (char **) calloc(sizeof(char *), mainlog.capacity);

    // create a local context
    kntxt_t mainctx;
    void *kntxt = &mainctx;

    mainctx.pixels = (pixel_t *) calloc(sizeof(pixel_t), LEDSTOTAL);
    mainctx.bitmap = (uint8_t *) calloc(sizeof(uint8_t), BITMAPSIZE);

    memset(&mainctx.midi, 0x00, sizeof(transform_t));
    mainctx.midi.lines = 8; // 8 channels
    mainctx.speed = 50000;

    mainctx.presets[1] = "/home/maxux/git/stageled/templates/segments.png";
    mainctx.presets[0] = "/home/maxux/git/stageled/templates/debug.png";
    mainctx.presets[2] = "/home/maxux/git/stageled/templates/linear-solid.png";
    mainctx.presets[3] = "/home/maxux/git/stageled/templates/stan.png";
    mainctx.presets[4] = "/home/maxux/git/stageled/templates/spectre2.png";
    mainctx.presets[5] = "/home/maxux/git/stageled/templates/follow1.png";
    mainctx.presets[6] = "/home/maxux/git/stageled/templates/cedric.png";
    mainctx.presets[7] = "/home/maxux/git/stageled/templates/strobe.png";

    pthread_mutex_init(&mainctx.lock, NULL);

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
