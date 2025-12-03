
#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>

#define SHM_KEY 0xBEEFBEEF
#define MAX_FLIGHTS 256
#define MAX_NAME_LEN 32
#define RUNWAYS 2


#define FL_LANDING  1
#define FL_TAKEOFF  2

typedef struct {
    int used;                     
    int id;                       
    char name[MAX_NAME_LEN];      
    int type;                     
    int emergency;                
    int duration_ms;              
} flight_t;

typedef struct {
    
    flight_t q[MAX_FLIGHTS];
    int q_head;
    int q_tail;
    int q_count;
    pid_t runway_in_use[RUNWAYS];
    int severe_weather;

    int total_assigned;
    long total_busy_ms; 
    int next_id;
} shm_state_t;

#endif

