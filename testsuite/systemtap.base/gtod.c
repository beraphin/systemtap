#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

int main (int argc, char *argv[])
{
	int i;
	struct timeval tv[100][2];
	int us = 0;
	if (argc == 2) us = atoi(argv[1]);
	for (i=0; i<100; i++) {
		gettimeofday(&tv[i][0], NULL);
		setsid();
		gettimeofday(&tv[i][1], NULL);
		if (us) usleep(us);
	}
	for (i=0; i<100; i++) {
		// change last 4 chars for correctly sorting even if the
		// time stamps are completely same.
	    	printf(":%02d %8ld%06ld appl\n", i, tv[i][0].tv_sec,
		       tv[i][0].tv_usec);
		printf(":%02d %8ld%06ld prog\n", i, tv[i][1].tv_sec,
		       tv[i][1].tv_usec);
	}
	return 0;
}

