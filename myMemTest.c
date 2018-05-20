#define H_PGSIZE 2048
#include "types.h"
#include "user.h"
#include "stat.h"
int main(){

    char arr1[4000]={1};
    //char arr2[2*PGSIZE]={2};

    int i;
    for(i=0; i<H_PGSIZE; i++){
        printf(1, "%c",arr1[i]);
    }
    
    // for(i=0; i<PGSIZE * 2; i++){
    //     printf(1, "%c",arr2[i]);
    // }






    exit();






}