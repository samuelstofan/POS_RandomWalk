#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <SDL2/SDL.h>

int getRandomBinary(){
    return rand()%2;
}

static int clampi(int v, int lo, int hi){
    return (v<lo)?lo:(v>hi)?hi:v;
}

void world_to_screen(int wx, int wy, int *sx, int *sy, int W, int H, int maxWorld) {
    *sx = (int)((wx + maxWorld) * (W - 1) / (2.0 * maxWorld));
    *sy = (int)((wy + maxWorld) * (H - 1) / (2.0 * maxWorld));
    *sy = (H - 1) - *sy;
}

void draw_point_big(SDL_Renderer *r, int x, int y, int size) {
    SDL_Rect rect = { x - size / 2, y - size / 2, size, size};
    SDL_RenderFillRect(r, &rect);
}

typedef struct {
    int x,y;
}Point;


int main(int argc,char* argv[]){
    srand(time(NULL));
    const int W = 900, H = 700;
    const int step = 50;
    const int maxWorld=2000;
    const int delay_ms = 10;
    int aktX = 0;
    int aktY = 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow("Random Walk (X,Y)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,W, H, SDL_WINDOW_SHOWN);
    if (!win) {
        SDL_Log("CreateWindow error: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        SDL_Log("CreateRenderer error: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderDrawColor(ren, 10, 10, 14, 255);
    SDL_RenderClear(ren);
    SDL_RenderPresent(ren);

    int sx,sy;
    world_to_screen(0,0,&sx,&sy,W,H,maxWorld);
    SDL_SetRenderDrawColor(ren,255,0,0,255);
    draw_point_big(ren,sx,sy,6);
    SDL_RenderPresent(ren);
    

    Point p = {0, 0};
    Point prev = p;

    int running = 1;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = 0;
        }

        prev = p;
        if(getRandomBinary()==0){
            int krok = getRandomBinary();
            p.x += step*((krok==0)?-1:1);
            printf("Krok X %d (%d,%d)\n",(krok==0)?-1:1,p.x,p.y);
        } else {
            int krok = getRandomBinary();
            p.y += step*((krok==0)?-1:1);
            printf("Krok X %d (%d,%d)\n",(krok==0)?-1:1,p.x,p.y);
        }

        p.x = clampi(p.x, -maxWorld, maxWorld);
        p.y = clampi(p.y, -maxWorld, maxWorld);
        
        int x1,y1;
        world_to_screen(prev.x,prev.y,&x1,&y1,W,H,maxWorld);
        int x2,y2;
        world_to_screen(p.x,p.y,&x2,&y2,W,H,maxWorld);

        SDL_SetRenderDrawColor(ren, 230, 230, 240, 255);
        SDL_RenderDrawLine(ren, x1, y1, x2, y2);

        SDL_SetRenderDrawColor(ren, 0, 255, 0, 255);
        SDL_RenderDrawPoint(ren,x2,y2);

        SDL_RenderPresent(ren);
        SDL_Delay(delay_ms);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}