#define PGSIZE 4096
#include "types.h"
#include "user.h"
#include "stat.h"
int main(){

    char arr1[10]={1};
    char arr2[20]={2};

    int i;
    for(i=0; i<10; i++){
        printf(1, "%c",arr1[i]);
    }
    
    for(i=0; i<20 ; i++){
        printf(1, "%c",arr2[i]);
    }













}