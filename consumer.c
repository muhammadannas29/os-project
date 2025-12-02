/* consumer.c
 * Scheduler (consumer) process. Dequeues flights, enforces priority rules and assigns runways.
 * It forks a child for each assigned flight to simulate occupying the runway (child frees runway when done).
 *
 * Usage:
 *   ./consumer
 *
 * Open this in a separate terminal (or multiple consumers if you like).
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/time.h>
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

/* helpers */
void die(const char *msg) { perror(msg); exit(1); }

void open_ipc() {
    shm_id = shmget(SHM_KEY, sizeof(shm_state_t), IPC_CREAT | 0666);
    if (shm_id < 0) die("shmget consumer");
    st = (shm_state_t*) shmat(shm_id, NULL, 0);
    if (st == (void*)-1) die("shmat consumer");

    sem_mutex = sem_open(SEM_MUTEX_NAME, O_CREAT, 0666, 1);
    if (sem_mutex == SEM_FAILED) die("sem_open mutex");
    sem_items = sem_open(SEM_ITEMS_NAME, O_CREAT, 0666, 0);
    if (sem_items == SEM_FAILED) die("sem_open items");
    sem_spaces = sem_open(SEM_SPACES_NAME, O_CREAT, 0666, MAX_FLIGHTS);
    if (sem_spaces == SEM_FAILED) die("sem_open spaces");
    sem_runways = sem_open(SEM_RUNWAYS_NAME, O_CREAT, 0666, RUNWAYS);
    if (sem_runways == SEM_FAILED) die("sem_open runways");
}

/* remove item at index idx_in_array (absolute index), shifting later items left */
void remove_at_index(int idx_in_array) {
    int idx = idx_in_array;
    /* shift items: from idx to tail-1 */
    int next = (idx + 1) % MAX_FLIGHTS;
    while (idx != st->q_tail) {
        st->q[idx] = st->q[next];
        idx = next;
        next = (next + 1) % MAX_FLIGHTS;
    }
    /* clear old tail slot */
    int new_tail = (st->q_tail - 1 + MAX_FLIGHTS) % MAX_FLIGHTS;
    st->q[new_tail].used = 0;
    st->q_tail = new_tail;
    st->q_count--;
}

/* find eligible flight index in the array (absolute index) using rules:
   - if no severe weather: pick FIFO (head)
   - if severe weather ON: prefer emergency landings (scan); if none, return -1 (no eligible)
*/
int find_eligible_index() {
    if (st->q_count == 0) return -1;
    if (!st->severe_weather) {
        return st->q_head;
    } else {
        /* find first emergency landing (scan forward) */
        int idx = st->q_head;
        for (int k=0;k<st->q_count;k++) {
            flight_t *f = &st->q[idx];
            if (f->emergency && f->type == FL_LANDING) return idx;
            idx = (idx+1)%MAX_FLIGHTS;
        }
        // no emergency landing found -> nothing eligible during severe weather
        return -1;
    }
}

/* find a free runway index (0..RUNWAYS-1) or -1 if none */
int find_free_runway() {
    for (int i=0;i<RUNWAYS;i++) {
        if (st->runway_in_use[i] == 0) return i;
    }
    return -1;
}

/* child process that occupies runway for duration_ms then frees it */
void child_occupy_runway(int runway_idx, int duration_ms, int flight_id, char *name) {
    /* we are child */
    printf("[child pid=%d] Occupying runway %d for flight id=%d name=%s dur=%dms\n",
           getpid(), runway_idx+1, flight_id, name, duration_ms);

    usleep(duration_ms * 1000);

    /* free runway under mutex */
    sem_wait(sem_mutex);
    if (st->runway_in_use[runway_idx] == getpid()) {
        st->runway_in_use[runway_idx] = 0;
        st->total_assigned++;
        st->total_busy_ms += duration_ms;
        printf("[child pid=%d] Freed runway %d for flight id=%d\n", getpid(), runway_idx+1, flight_id);
    } else {
        printf("[child pid=%d] Warning: runway %d not owned by me\n", getpid(), runway_idx+1);
    }
    sem_post(sem_mutex);

    /* release runway semaphore permit */
    sem_post(sem_runways);

    exit(0);
}

int main() {
    open_ipc();
    printf("Consumer (scheduler) started. Waiting for flights...\n");

    while (1) {
        /* Wait until there's at least one item */
        sem_wait(sem_items);

        /* lock and try to find eligible flight according to weather/emergency */
        sem_wait(sem_mutex);
        int eligible_idx = find_eligible_index();
        if (eligible_idx == -1) {
            /* nothing eligible (e.g., severe weather but no emergency landings).
               Put the semaphore back and wait for future events.
             */
            sem_post(sem_mutex);
            /* re-post the items semaphore for eventual re-evaluation after a toggle or new flight */
            sem_post(sem_items);
            /* small sleep so we don't busy-wait */
            usleep(200000);
            continue;
        }

        /* copy flight details */
        flight_t f = st->q[eligible_idx];

        /* remove it from queue */
        /* To remove we need to compute index position relative to array: eligible_idx is absolute index.
           However our queue is contiguous in the circular buffer; we must shift elements from eligible_idx forward.
        */
        /* To shift, we'll do a manual rotation: starting idx=eligible_idx, repeatedly copy next into idx until tail reached */
        int idx = eligible_idx;
        while (1) {
            int next = (idx + 1) % MAX_FLIGHTS;
            if (next == st->q_tail) {
                /* move last slot into current and break */
                st->q[idx].used = 0;
                break;
            } else {
                st->q[idx] = st->q[next];
            }
            idx = next;
        }
        st->q_tail = (st->q_tail - 1 + MAX_FLIGHTS) % MAX_FLIGHTS;
        st->q_count--;

        printf("[consumer] Dequeued id=%d name=%s type=%s em=%d dur=%dms\n",
               f.id, f.name, (f.type==FL_LANDING?"LAND":"TKOF"), f.emergency, f.duration_ms);

        /* we removed an item so signal space */
        sem_post(sem_mutex);
        sem_post(sem_spaces);

        /* Now we must wait for a runway permit (sem_runways) */
        sem_wait(sem_runways);

        /* got permit: lock and find a free runway and assign it */
        sem_wait(sem_mutex);
        int runway_idx = find_free_runway();
        if (runway_idx < 0) {
            /* This shouldn't happen because sem_runways guaranteed availability, but handle */
            printf("[consumer] no free runway unexpectedly\n");
            sem_post(sem_mutex);
            sem_post(sem_runways);
            continue;
        }
        /* assign runway to this consumer's upcoming child; mark with child's pid after fork */
        /* We'll fork now, child will set the pid in shared memory under mutex */
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            /* restore semaphore and continue */
            st->runway_in_use[runway_idx] = 0;
            sem_post(sem_mutex);
            sem_post(sem_runways);
            continue;
        } else if (pid == 0) {
            /* child */
            /* set its pid into shared memory (already have mutex) */
            st->runway_in_use[runway_idx] = getpid();
            /* copy name to local buffer */
            char name_local[MAX_NAME_LEN];
            strncpy(name_local, f.name, MAX_NAME_LEN-1);
            name_local[MAX_NAME_LEN-1] = 0;
            sem_post(sem_mutex); /* child releases mutex before occupying */
            child_occupy_runway(runway_idx, f.duration_ms, f.id, name_local);
            /* child exits inside function */
        } else {
            /* parent */
            printf("[consumer] Assigned runway %d to flight id=%d (child pid=%d)\n", runway_idx+1, f.id, pid);
            sem_post(sem_mutex);
            /* parent continues to next loop without waiting for child - concurrency handled */
            /* reap any finished children to avoid zombies */
            while (waitpid(-1, NULL, WNOHANG) > 0) {}
        }
    }

    return 0;
}

