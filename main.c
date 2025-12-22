#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

int getRandomBinary(){
    return rand()%2;
}







int main(int argc,char* argv[]){
    srand(time(NULL));
    printf("Cislo: %d",getRandomBinary());
    return 0;
}