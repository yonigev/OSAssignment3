#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#define PGSIZE 4096
#define ARR_SIZE_FORK PGSIZE*17
#define ARR_SIZE_TEST PGSIZE*17

//tests forking
//Right after forking, 
// the child process should have the same number of allocated pages, and number of paged-out pages.

void forkPageTest(){
  int i;
  char * arr;
  int pid;
  arr = sbrk(ARR_SIZE_FORK); //allocates 20 pages,  so 16 in RAM and 4 in the swapFile
  for(i=0; i<ARR_SIZE_FORK; i++)    {
    arr[i]='M';
  }
  printf(1,"\n");
  printf(1,"-----------------------------------\nNow forking - press Control + P Quickly\n-----------------------------------\n");
  sleep(20);
  sleep(20);
  printf(1,"FORK\n");
  //Child - change array to C's and print some
  if((pid=fork()) == 0){  
    printf(1,"-----------------------------------\nChild  - press Control+ P Quickly\n-----------------------------------\n");
    sleep(300);
    sleep(20);
    sleep(20);
    int j;
    for(j=0; j<ARR_SIZE_FORK; j++){
      arr[j]='C'; //change the whole array for the child   
      if(j==ARR_SIZE_FORK-1)
        arr[j]='x';    
    }
    printf(1,"Child -\n-------\n");
    for(j=0; j<ARR_SIZE_FORK; j++){
      if(j % 1000 == 0 || j==ARR_SIZE_FORK-1)
        printf(1,"%c",arr[j]);
    }
    printf(1,"\n\n");
    exit();
  }
  //Parent- change array to 'P's and print some.
  else{
    //parent changing all chars to P
    for(i=0; i<ARR_SIZE_FORK; i++){
      arr[i]='P'; //change the whole array for the Parent       
      if(i==ARR_SIZE_TEST-1)
        arr[i]='X';    
    }
    printf(1,"Parent -\n-------\n");
    for(i=0; i<ARR_SIZE_FORK; i++){
      if(i % 1000 == 0|| i==ARR_SIZE_TEST-1)
        printf(1,"%c",arr[i]);
    }
    printf(1,"\n\n");
    sleep(30);
    wait();
    printf(1,"parent exiting.\n");
    exit();
  }

}


//Does a linear iteration over a 17 pages sized array.
//NFUA- allocation  -  when allocating the last (17th) page, the 1st page should be swapped out into the SwapFile
//      iteration   -  the 1st page is in the swap file. to reach it, page #2 would be swapped out,then #1 swapped in.
//                                                                    then to reach page#2, page #1 would be again swapped out and #1 swapped in.
//                                                                    this continues the same way, or swapping ends here - depends on aging (ticks)
void linear_test(){
	char * arr;
	int i;
  printf(1,"allocation\n");
	arr = sbrk(ARR_SIZE_TEST); //allocates 17 pages - 1 must be in the swapfile

  printf(1,"iteration\n");
  for(i=0; i<ARR_SIZE_TEST; i++){
    sleep(0);
    arr[i]='A';
    if(i % PGSIZE/2 == 0){  //print every 2048 digits
      printf(1,"%c",arr[i]);
    }
  }
  printf(1,"\n\n");
	free(arr);
}


int main(int argc, char *argv[]){
  linear_test();    
  //forkTest();			//for testing swapping machanism in fork.
  printf(1,"memtest done\n");
  exit();
}