
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <termios.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <ctype.h>

#include "shared.h"

#define SEM_MUTEX_NAME "/airport_mutex"
#define LOGFILE "airport_log.txt"


#define ANSI_CLEAR_SCREEN()      printf("\x1b[2J")
#define ANSI_CURSOR_HOME()       printf("\x1b[H")
#define ANSI_HIDE_CURSOR()       printf("\x1b[?25l")
#define ANSI_SHOW_CURSOR()       printf("\x1b[?25h")
#define ANSI_BOLD()              printf("\x1b[1m")
#define ANSI_RESET()             printf("\x1b[0m")
#define ANSI_RED()               printf("\x1b[31m")
#define ANSI_GREEN()             printf("\x1b[32m")
#define ANSI_YELLOW()            printf("\x1b[33m")
#define ANSI_BLUE()              printf("\x1b[34m")
#define ANSI_MAGENTA()           printf("\x1b[35m")
#define ANSI_CYAN()              printf("\x1b[36m")
#define ANSI_WHITE()             printf("\x1b[37m")

const char *SPINNER[] = { "|", "/", "-", "\\" };
const int SPINNER_FRAMES = 4;

static shm_state_t *st = NULL;
static int shm_id = -1;
static sem_t *sem_mutex = NULL;

static struct termios orig_term;

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_term);
    struct termios raw = orig_term;
    raw.c_lflag &= ~(ICANON | ECHO); // disable canonical and echo
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}


void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

int open_ipc() {
    shm_id = shmget(SHM_KEY, sizeof(shm_state_t), 0666);
    if (shm_id < 0) {
        return -1;
    }
    st = (shm_state_t*) shmat(shm_id, NULL, SHM_RDONLY);
    if (st == (void*) -1) {
        st = NULL;
        return -1;
    }
    sem_mutex = sem_open(SEM_MUTEX_NAME, 0);
    if (sem_mutex == SEM_FAILED) sem_mutex = NULL;
    return 0;
}

void close_ipc() {
    if (st) {
        shmdt(st);
        st = NULL;
    }
    if (sem_mutex && sem_mutex != SEM_FAILED) {
        sem_close(sem_mutex);
        sem_mutex = NULL;
    }
}

int read_log_tail(char **out_lines, int max_lines) {
    for (int i=0;i<max_lines;i++) out_lines[i] = NULL;
    FILE *f = fopen(LOGFILE, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long pos = ftell(f);
    int lines = 0;
    long cur = pos;
    size_t bufcap = 0;
    char *buf = NULL;

    while (cur > 0 && lines <= max_lines) {
        cur--;
        fseek(f, cur, SEEK_SET);
        int c = fgetc(f);
        if (c == '\n') {
            long start = ftell(f);
            size_t len = pos - start;
            fseek(f, start, SEEK_SET);
            free(buf);
            buf = malloc(len+1);
            if (!buf) break;
            fread(buf, 1, len, f);
            buf[len] = 0;
            out_lines[max_lines - lines - 1] = strdup(buf);
            lines++;
            pos = cur;
        }
    }
  
    if (lines < max_lines) {
        fseek(f, 0, SEEK_SET);
        free(buf);
        buf = NULL;
        long start = 0;
        size_t len = pos - start;
        if (len > 0) {
            buf = malloc(len+1);
            if (buf) {
                fread(buf, 1, len, f);
                buf[len] = 0;
                out_lines[max_lines - lines - 1] = strdup(buf);
                lines++;
            }
        }
    }
    free(buf);
    fclose(f);
    return lines;
}

int term_width() {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) return 80;
    return w.ws_col;
}

void print_header(const char *title) {
    int w = term_width();
    printf("┌");
    for (int i=0;i<w-2;i++) printf("─");
    printf("┐\n");
    printf("│");
    int left = (w - 2 - (int)strlen(title)) / 2;
    for (int i=0;i<left;i++) printf(" ");
    ANSI_BOLD(); ANSI_CYAN();
    printf("%s", title);
    ANSI_RESET();
    for (int i=0;i<w-2-left-(int)strlen(title);i++) printf(" ");
    printf("│\n");
    printf("├");
    for (int i=0;i<w-2;i++) printf("─");
    printf("┤\n");
}


void draw_occupancy_bar(int width, int frame) {
    int fill = (frame % (width)) + 1;
    printf("[");
    for (int i=0;i<width;i++) {
        if (i < fill) printf("■");
        else printf(" ");
    }
    printf("]");
}

int main() {
    if (open_ipc() != 0) {
        fprintf(stderr, "Failed to open shared memory (is producer/consumer running?).\n");
        fprintf(stderr, "Still you can run monitor and it will keep trying.\n");
    }

    enable_raw_mode();
    ANSI_HIDE_CURSOR();

    int spinner_frame = 0;
    int sleep_ms = 300;
    char key = 0;

    while (1) {
     
        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (ch == 'q' || ch == 'Q') {
                break;
            }
        }

   
        shm_state_t snapshot;
        int have_snapshot = 0;
        if (st) {
            if (sem_mutex) sem_wait(sem_mutex);
            snapshot = *st; /* copy whole struct */
            if (sem_mutex) sem_post(sem_mutex);
            have_snapshot = 1;
        }

     
        ANSI_CLEAR_SCREEN();
        ANSI_CURSOR_HOME();

        print_header("✈ AIRPORT RUNWAY SCHEDULER - MONITOR");

        if (have_snapshot && snapshot.severe_weather) {
            ANSI_BOLD(); ANSI_RED();
            printf("!!! SEVERE WEATHER ACTIVE: ONLY EMERGENCY LANDINGS/TKOF ALLOWED !!!\n");
            ANSI_RESET();
        } else {
            ANSI_GREEN();
            printf("Weather: NORMAL (all operations allowed)\n");
            ANSI_RESET();
        }
        printf("\n");

        printf("Active Runways:\n");
        for (int r=0;r<RUNWAYS;r++) {
            printf("  RWY-%d: ", r+1);
            if (have_snapshot && snapshot.runway_in_use[r] != 0) {
                ANSI_YELLOW(); printf("OCCUPIED "); ANSI_RESET();
                printf("(PID %d) ", snapshot.runway_in_use[r]);
              
                printf("%s ", SPINNER[spinner_frame % SPINNER_FRAMES]);
                draw_occupancy_bar(20, spinner_frame);
                printf("\n");
            } else {
                ANSI_GREEN(); printf("FREE"); ANSI_RESET();
                printf(" ");
                draw_occupancy_bar(20, spinner_frame);
                printf("\n");
            }
        }
        printf("\n");

     
        printf("Queued Flights (front -> back):\n");
        if (have_snapshot && snapshot.q_count > 0) {
            int cnt = snapshot.q_count;
            int idx = snapshot.q_head;
            for (int i=0;i<cnt;i++) {
                flight_t *f = &snapshot.q[idx];
                char type_s[16];
                if (f->type == FL_LANDING) strcpy(type_s, "LANDING");
                else strcpy(type_s, "TAKEOFF ");
                if (f->emergency) {
                    ANSI_BOLD(); ANSI_RED();
                    printf("  %2d) %s  %-8s  [EMERGENCY]\n", f->id, f->name, type_s);
                    ANSI_RESET();
                } else {
                    printf("  %2d) %s  %-8s\n", f->id, f->name, type_s);
                }
                idx = (idx + 1) % MAX_FLIGHTS;
            }
        } else {
            printf("  <queue empty>\n");
        }
        printf("\n");

        if (have_snapshot) {
            printf("Metrics: total_assigned=%d  total_busy_ms=%ld  queue_len=%d\n",
                   snapshot.total_assigned, snapshot.total_busy_ms, snapshot.q_count);
        } else {
            printf("Metrics: (no shared memory)\n");
        }
        printf("\n");

        int w = term_width();
        int mid = w / 2;
        /* sky line */
        ANSI_CYAN(); printf(" ");
        for (int i=0;i<w-2;i++) {
            if ((i + spinner_frame) % 20 == 0) printf("✈");
            else printf(" ");
        }
        ANSI_RESET(); printf("\n\n");

     
        printf("Recent Log:\n");
        char *lines[16];
        int read_lines = read_log_tail(lines, 12); 
        if (read_lines > 0) {
            for (int i=0;i<12;i++) {
                if (lines[i]) {
                    printf("  %s\n", lines[i]);
                    free(lines[i]);
                }
            }
        } else {
            printf("  <no log file or empty>\n");
        }

        printf("\n");
        ANSI_BOLD(); printf("Press 'q' to quit. Refresh rate: %d ms\n", sleep_ms); ANSI_RESET();

        fflush(stdout);

        spinner_frame++;
        usleep(sleep_ms * 1000);
    }

    ANSI_SHOW_CURSOR();
    disable_raw_mode();
    close_ipc();
    ANSI_CLEAR_SCREEN();
    ANSI_CURSOR_HOME();
    printf("Monitor exited.\n");
    return 0;
}

