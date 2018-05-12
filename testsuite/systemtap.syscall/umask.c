/* COVERAGE: umask */
#include <sys/types.h>
#include <sys/stat.h>

int main()
{
  umask (0);
  //staptest// umask (00) = NNNN
  umask (7);
  //staptest// umask (07) = 0
  umask (077);
  //staptest// umask (077) = 07
  umask (0666);
  //staptest// umask (0666) = 077
  umask (0777);
  //staptest// umask (0777) = 0666
  umask (01777);
  //staptest// umask (01777) = 0777

  /* Limits testing */

  umask (-1);
  //staptest// umask (037777777777) = NNNN

  return 0;
}
