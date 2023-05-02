

#include "def.h"

int bytes_track[NUM_DBLOCKS];
pthread_mutex_t bytes_track_mutex; 


//function to initialize bytes_track
int init_bt(){
	for(int i = 0; i < NUM_DBLOCKS; i++) 
		bytes_track[i] = 0;
	return 0;
}

//kick out of the system, sort of unnessecary, used for concurrency
void free_bytes_loc(int loc){
	pthread_mutex_lock(&bytes_track_mutex);
	bytes_track[loc] = 0;
	pthread_mutex_unlock(&bytes_track_mutex);
}
