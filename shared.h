/* shared.h
 * Common definitions for airport producer/consumer
 */
#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>

#define SHM_KEY 0xBEEFBEEF
#define MAX_FLIGHTS 256
#define MAX_NAME_LEN 32
#define RUNWAYS 2

/* Flight types */
#define FL_LANDING  1
#define FL_TAKEOFF  2

typedef struct {
    int used;                     /* 0 = free slot, 1 = occupied */
    int id;                       /* unique id */
    char name[MAX_NAME_LEN];      /* plane name */
    int type;                     /* FL_LANDING or FL_TAKEOFF */
    int emergency;                /* 1 = emergency */
    int duration_ms;              /* simulated runway occupation in ms */
} flight_t;

typedef struct {
    /* circular queue stored as simple array with count + head/tail */
    flight_t q[MAX_FLIGHTS];
    int q_head;
    int q_tail;
    int q_count;

    /* runway status: 0 = free, otherwise PID of occupant */
    pid_t runway_in_use[RUNWAYS];

    /* severe weather flag: 0 = off, 1 = only emergency landings allowed */
    int severe_weather;

    /* metrics */
    int total_assigned;
    long total_busy_ms; /* sum of durations */

    /* next flight id */
    int next_id;
} shm_state_t;

#endif

