
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <errno.h>
#include "shared.h"

#define SEM_MUTEX_NAME "/airport_mutex"
#define SEM_ITEMS_NAME "/airport_items"
#define SEM_SPACES_NAME "/airport_spaces"
#define SEM_RUNWAYS_NAME "/airport_runways"

static shm_state_t *st = NULL;
static int shm_id = -1;
static sem_t *sem_mutex = NULL;
static sem_t *sem_items = NULL;
static sem_t *sem_spaces = NULL;
static sem_t *sem_runways = NULL;

void die(const char *msg) {
    perror(msg);
    exit(1);
}

void open_ipc() {
    shm_id = shmget(SHM_KEY, sizeof(shm_state_t), IPC_CREAT | 0666);
    if (shm_id < 0) die("shmget");
    st = (shm_state_t*) shmat(shm_id, NULL, 0);
    if (st == (void*)-1) die("shmat");

    sem_mutex = sem_open(SEM_MUTEX_NAME, O_CREAT, 0666, 1);
    if (sem_mutex == SEM_FAILED) die("sem_open mutex");
    sem_items = sem_open(SEM_ITEMS_NAME, O_CREAT, 0666, 0);
    if (sem_items == SEM_FAILED) die("sem_open items");
    sem_spaces = sem_open(SEM_SPACES_NAME, O_CREAT, 0666, MAX_FLIGHTS);
    if (sem_spaces == SEM_FAILED) die("sem_open spaces");
    sem_runways = sem_open(SEM_RUNWAYS_NAME, O_CREAT, 0666, RUNWAYS);
    if (sem_runways == SEM_FAILED) die("sem_open runways");
}

void add_flight(const char *name, int type, int duration_ms, int emergency) {
    sem_wait(sem_spaces);
    sem_wait(sem_mutex);
    int idx = st->q_tail;
    st->q[idx].used = 1;
    st->q[idx].id = st->next_id++;
    strncpy(st->q[idx].name, name, MAX_NAME_LEN-1);
    st->q[idx].name[MAX_NAME_LEN-1] = 0;
    st->q[idx].type = type;
    st->q[idx].emergency = emergency ? 1 : 0;
    st->q[idx].duration_ms = duration_ms;
    st->q_tail = (st->q_tail + 1) % MAX_FLIGHTS;
    st->q_count++;
    printf("[producer] Enqueued id=%d name=%s type=%s dur=%dms em=%d\n",
           st->q[idx].id, st->q[idx].name, (type==FL_LANDING?"LAND":"TKOF"),
           duration_ms, emergency);
    sem_post(sem_mutex);
    sem_post(sem_items);
}

void print_status() {
    sem_wait(sem_mutex);
    printf("=== STATUS (producer view) ===\n");
    printf("Severe weather: %s\n", st->severe_weather ? "ON" : "OFF");
    printf("Queue count: %d\n", st->q_count);
    int i = st->q_head;
    for (int k=0;k<st->q_count;k++) {
        flight_t *f = &st->q[i];
        printf("  id=%d name=%s type=%s em=%d dur=%d\n", f->id, f->name,
               (f->type==FL_LANDING?"LAND":"TKOF"), f->emergency, f->duration_ms);
        i = (i+1)%MAX_FLIGHTS;
    }
    for (int r=0;r<RUNWAYS;r++) {
        printf("Runway %d: %s\n", r+1, st->runway_in_use[r] ? "IN USE" : "FREE");
    }
    printf("Total assigned: %d, total busy ms: %ld\n", st->total_assigned, st->total_busy_ms);
    sem_post(sem_mutex);
}

void mark_emergency(int id) {
    sem_wait(sem_mutex);
    int found = 0;
    int idx = st->q_head;
    for (int k=0;k<st->q_count;k++) {
        flight_t *f = &st->q[idx];
        if (f->id == id) {
            f->emergency = 1;
            found = 1;
            printf("[producer] Marked id=%d as EMERGENCY\n", id);
            break;
        }
        idx = (idx+1) % MAX_FLIGHTS;
    }
    if (!found) printf("[producer] id=%d not found in queue\n", id);
    sem_post(sem_mutex);
   
    sem_post(sem_items);
}

int parse_type(const char *s) {
    if (strcasecmp(s,"landing")==0 || strcasecmp(s,"land")==0) return FL_LANDING;
    if (strcasecmp(s,"takeoff")==0 || strcasecmp(s,"tkof")==0 || strcasecmp(s,"take")==0) return FL_TAKEOFF;
    return -1;
}

int main(int argc, char **argv) {
    open_ipc();

    sem_wait(sem_mutex);
    if (st->next_id == 0) {
        st->q_head = st->q_tail = st->q_count = 0;
        for (int i=0;i<MAX_FLIGHTS;i++) st->q[i].used = 0;
        for (int r=0;r<RUNWAYS;r++) st->runway_in_use[r] = 0;
        st->severe_weather = 0;
        st->total_assigned = 0;
        st->total_busy_ms = 0;
        st->next_id = 1;
    }
    sem_post(sem_mutex);

    if (argc >= 2) {
        FILE *f = fopen(argv[1],"r");
        if (!f) {
            perror("open schedule");
           
        } else {
            char name[128], type_s[32];
            int dur, em;
            while (fscanf(f, "%31s %31s %d %d", name, type_s, &dur, &em) == 4) {
                int t = parse_type(type_s);
                if (t<0) continue;
                add_flight(name, t, dur, em);
                usleep(100000); 
            }
            fclose(f);
        }
    }

  
    while (1) {
        printf("\nProducer Menu:\n");
        printf("1) Add flight\n2) Mark queued flight EMERGENCY (by id)\n3) Toggle severe weather (current %s)\n4) Show status\n5) Exit\nChoose: ",
               st->severe_weather ? "ON" : "OFF");
        char line[64];
        if (!fgets(line,sizeof(line),stdin)) break;
        int opt = atoi(line);
        if (opt == 1) {
            char name[64], type_s[16], em_s[8], dur_s[16];
            printf("Name: ");
            if (!fgets(name,sizeof(name),stdin)) continue;
            name[strcspn(name,"\n")] = 0;
            printf("Type (LANDING/TAKEOFF): ");
            if (!fgets(type_s,sizeof(type_s),stdin)) continue;
            type_s[strcspn(type_s,"\n")] = 0;
            printf("Duration ms (e.g. 2000): ");
            if (!fgets(dur_s,sizeof(dur_s),stdin)) continue;
            dur_s[strcspn(dur_s,"\n")] = 0;
            printf("Emergency? (0/1): ");
            if (!fgets(em_s,sizeof(em_s),stdin)) continue;
            em_s[strcspn(em_s,"\n")] = 0;
            int t = parse_type(type_s);
            if (t<0) { printf("Invalid type\n"); continue; }
            int dur = atoi(dur_s);
            int em = atoi(em_s);
            add_flight(name, t, dur>0?dur:2000, em);
        } else if (opt == 2) {
            printf("Enter id to mark emergency: ");
            char ibuf[32];
            if (!fgets(ibuf,sizeof(ibuf),stdin)) continue;
            int id = atoi(ibuf);
            if (id>0) mark_emergency(id);
        } else if (opt == 3) {
            sem_wait(sem_mutex);
            st->severe_weather = !st->severe_weather;
            printf("Severe weather set to %d\n", st->severe_weather);
            sem_post(sem_mutex);
         
            sem_post(sem_items);
        } else if (opt == 4) {
            print_status();
        } else if (opt == 5) {
            printf("Producer exiting\n");
            break;
        } else {
            printf("Invalid\n");
        }
    }

   
    shmdt(st);
  
    return 0;
}

