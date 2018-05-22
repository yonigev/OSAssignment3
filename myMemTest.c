#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"

#define PGSIZE 4096
#define ARR_SIZE PGSIZE*20

/*
	Test used to check the swapping machanism in fork.
	Best tested when LIFO is used (for more swaps)
*/
void forkTest(){
  int i;
  char * arr;
  int pid;
  arr = malloc (ARR_SIZE); //allocates 20 pages,  so 16 in RAM and 4 in the swapFile
  for(i=0; i<ARR_SIZE; i++){
    if(i >= 16 * PGSIZE)      //last 4 pages will contain 'B's.
      arr[i]='B';
    else
      arr[i]='A';
  }
  //child
  printf(1,"Now forking - press Control + P \n");
  sleep(10);
  if((pid=fork()) == 0){  
    printf(1,"Child  - press Control+ P\n");
    sleep(10);
    int j;
    print(1,"Child-");
    for(j=0; j<20; j++){
      printf(1,"%s",arr[j+PGSIZE*16]);  //print some characters of the B pages

    }
    exit();
  }
  else{
    sleep(5);
    printf(1,"Parent - ");
    for(i=0; i<20; i++){
      printf(1,"%s",arr[i+PGSIZE*16]);  //print some characters of the B pages
    }
  }

}


static unsigned long int next = 1;
int getRandNum() {
  next = next * 1103515245 + 12341;
  return (unsigned int)(next/65536) % ARR_SIZE;
}

#define PAGE_NUM(addr) ((uint)(addr) & ~0xFFF)
#define TEST_POOL 500
/*
Global Test:
Allocates 17 pages (1 code, 1 space, 1 stack, 14 malloc)
Using pseudoRNG to access a single cell in the array and put a number in it.
Idea behind the algorithm:
	Space page will be swapped out sooner or later with scfifo or lap.
	Since no one calls the space page, an extra page is needed to play with swapping (hence the #17).
	We selected a single page and reduced its page calls to see if scfifo and lap will become more efficient.
Results (for TEST_POOL = 500):
LIFO: 42 Page faults
LAP: 18 Page faults
SCFIFO: 35 Page faults
*/
void globalTest(){
	char * arr;
	int i;
	int randNum;
	arr = malloc(ARR_SIZE); //allocates 14 pages (sums to 17 - to allow more then one swapping in scfifo)
  
	for (i = 0; i < TEST_POOL; i++) {
		randNum = getRandNum();	//generates a pseudo random number between 0 and ARR_SIZE
		while (PGSIZE*10-8 < randNum && randNum < PGSIZE*10+PGSIZE/2-8)
			randNum = getRandNum(); //gives page #13 50% less chance of being selected
															//(redraw number if randNum is in the first half of page #13)
		arr[randNum] = 'X';				//write to memory
	printf(1,"test  i= %d\n",i);
  }
	free(arr);
}


int main(int argc, char *argv[]){
  //globalTest();			//for testing each policy efficiency
    
  forkTest();			//for testing swapping machanism in fork.
  printf(1,"memtest done\n");
  exit();
}