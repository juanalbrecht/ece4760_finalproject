/* ============== Libraries ================ */
#include "config.h"
#include "pt_cornell_1_2_1.h"
#include "tft_master.h"
#include "tft_gfx.h"
#include "math.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "i2c_helper.h"
/* ========================================== */

/* ======= General Thread Structure ========= */
static struct pt pt_joystick, pt_accel, pt_button1, pt_button2, 
        pt_pregame, pt_game, pt_postgame, pt_dma_sound, pt_timer;
/* ========================================== */

/* ======= Timing ========= */
volatile int init_time;
/* ======================== */

/* ======== Joystick Thread ========= */
volatile int joy_x;
volatile int joy_y;

typedef enum {
    NoPush,
    MaybePush,
    Pushed, 
    MaybeNoPush
} state;

state PushStateBut1 = NoPush;
state PushStateBut2 = NoPush;
state PushStateJoy  = NoPush;

/* ============================= */

/* ======== Post-Game Thread ========= */
static int post_game = 0; // initialized to 0

#define NOP asm("nop");
// 20 cycles 
#define wait20 NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;NOP;
// 40 cycles
#define wait40 wait20;wait20;
/* =================================== */

/* ======== Pre-Game Thread ========= */
static int pre_game = 1; // initialized to 1
static int pre_game_count = 0;
/* =================================== */

/* ======== Actual Game Thread ========= */
#define bricks_max 100
#define time 30
#define bullets 50

char buffer[60];
volatile int num_bullets = bullets;
volatile int raw_gun_x, gun_x, prev_gun_x;
volatile int xPos_avg[8] = {512, 512, 512, 512, 512, 512, 512, 512};
volatile int avg_idx = 0;
volatile int prev_fire;
volatile int pressed = 0;
volatile int hit = 0, hit_place = 0, brick_hit = 0;
volatile int laser_top;
volatile int obj_speed, speed_accum = 0;
volatile int game_mode, prev_game_mode = 4;
volatile int press_but1, press_but2, press_joy, button1, button2, button_joy;
volatile int prev_pos;
volatile int restart_game = 1; // game is restarted at the beginning
volatile int play_time = time + 2;   // initial playing time
volatile int destroyer = 0;
volatile float rand_num;
volatile int game_result = 0;
volatile int temp_x;

/* Difficulty: Easy(0), Medium (1), Difficult (2)*/
volatile int difficulty = 0;
volatile int diff_array_x[3] = {135, 123, 107};
volatile int diff_array_y[3] = {100, 140, 180};
volatile float diff_array_brick[3] = {0.9, 0.8, 0.7};

void drawField(){
    // Draw the actual structure of the field
    tft_drawRect(100, 5, 215, 230, ILI9340_YELLOW);// x,y,w,h,radius,color
    
    tft_fillRect(15, 10, 70, 100, ILI9340_WHITE); 
    tft_fillRect(30, 20, 40, 30, ILI9340_BLACK);
    tft_fillCircle(35, 80, 9, ILI9340_BLACK);
    tft_fillRect(60, 84, 6, 6, ILI9340_BLUE);
    tft_fillRect(70, 73, 6, 6, ILI9340_RED);
    
    tft_setCursor(5, 140);
    tft_setTextColor(ILI9340_WHITE);
    tft_setTextSize(1);  
    sprintf(buffer, "DIFFICULTY:");                 
    tft_writeString(buffer);
    
    tft_setCursor(5, 160);
    tft_setTextColor(ILI9340_WHITE);
    tft_setTextSize(1); 
    
    if(difficulty == 0){
        sprintf(buffer, "EASY");                 
    }
    else if(difficulty == 1){
        sprintf(buffer, "MEDIUM");
    }
    else{
        sprintf(buffer, "DIFFICULT");
    }
    
    tft_writeString(buffer);
    
    tft_setCursor(5, 190);
    tft_setTextColor(ILI9340_WHITE);
    tft_setTextSize(1);  
    sprintf(buffer, "MODE:");                 
    tft_writeString(buffer);
    
    tft_setCursor(5, 220);
    tft_setTextColor(ILI9340_WHITE);
    tft_setTextSize(1);  
    sprintf(buffer, "DESTROYER:");                 
    tft_writeString(buffer);
}

int running_avg(){
    int i = 0;
    int total = 0;
    for(i = 0; i < 8; i++){
        total += xPos_avg[i];
    }
    return total >> 3;
}

int abs(int x){
    return (x > 0) ? x : -x;
}

void drawPlane(int xPos){
    if(xPos > 100){
        tft_fillRect(prev_pos, 185, 20, 45, ILI9340_BLACK); /* clean screen */
        tft_fillRect(xPos, 190, 20, 4, ILI9340_WHITE); /* wings */
        tft_fillRect(xPos+7, 185, 6, 25, ILI9340_WHITE); /* body */
        tft_fillRect(xPos+4, 210, 12, 4, ILI9340_WHITE); /* tail */

        if(num_bullets < 10){
            tft_setCursor(xPos + 8, 220);                 
        }
        else{
            tft_setCursor(xPos + 5, 220);                             
        }

        if(num_bullets == 0){
            tft_setTextColor(ILI9340_RED);
        }
        else{
            tft_setTextColor(ILI9340_GREEN);
        }

        tft_setTextSize(1);  
        sprintf(buffer, "%d", num_bullets);                 
        tft_writeString(buffer); 

        prev_pos = xPos;
    }
}

float random_gen(){
    return (float)(rand()) / (RAND_MAX);
}

typedef struct brick{
    int x;
    int y;
    int valid;
    int weight;
    int first;
    int type;
} brick;

brick bricks[bricks_max];

void initBricks(){
    int i;
   
    for(i = 0; i < bricks_max; i++){
        bricks[i].x      = 0;
        bricks[i].y      = 0;
        bricks[i].valid  = 0;
        bricks[i].weight = 0;
        bricks[i].first  = 1;
        bricks[i].type   = 0; // brick
    }
}

void clearBricks(){
    int i;
   
    for(i = 0; i < bricks_max; i++){
        if(bricks[i].valid){
            if(bricks[i].type == 2){
                tft_fillCircle(bricks[i].x, bricks[i].y, 9, ILI9340_BLACK);
            }
            else{
                tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_BLACK);
            }

            bricks[i].x      = 0;
            bricks[i].y      = 0;
            bricks[i].valid  = 0;
            bricks[i].weight = 0;
            bricks[i].first  = 1;
            bricks[i].type   = 0; // brick
        }
    }
}

void updateBricks(int planePos){
    
    brick_hit = 0; // no hit
    
    int i;
    for(i = 0; i < bricks_max; i++){
        if(bricks[i].valid){
            if(bricks[i].first){
                switch(bricks[i].type){
                    case 0:
                        tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_YELLOW);
                    
                        tft_setCursor(bricks[i].x + 5, bricks[i].y + 6);
                        tft_setTextColor(ILI9340_BLACK);
                        tft_setTextSize(1);  
                        sprintf(buffer, "%d", bricks[i].weight);                 
                        tft_writeString(buffer);

                        bricks[i].first = 0; // not first anymore
                        break;
                        
                    case 1:
                        tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_GREEN);
                    
                        tft_setCursor(bricks[i].x + 5, bricks[i].y + 6);
                        tft_setTextColor(ILI9340_BLACK);
                        tft_setTextSize(1);  
                        sprintf(buffer, "%d", bricks[i].weight);                 
                        tft_writeString(buffer);

                        bricks[i].first = 0; // not first anymore
                        break;
                        
                    case 2:
                        tft_fillCircle(bricks[i].x, bricks[i].y, 9, ILI9340_RED);
                        bricks[i].first = 0; // not first anymore
                        break;
                        
                    default:
                        break;
                }
            }
            else{
                switch(bricks[i].type){
                    
                    case 0:
                        tft_fillRect(bricks[i].x, bricks[i].y, 20, obj_speed, ILI9340_BLACK);
             
                        bricks[i].y = bricks[i].y + obj_speed; 

                        if((bricks[i].y > 163) && (bricks[i].y < 190) && (((bricks[i].x > planePos) && (bricks[i].x < planePos + 20)) 
                                || ((bricks[i].x + 20 > planePos) && (bricks[i].x + 20 < planePos + 20)))){
                            // Hit the airplane. Should lose the game
                            brick_hit = 1; // hit the plane
                            bricks[i].valid = 0; // not valid anymore
                        }
                        else if (bricks[i].y < 213){
                            tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_YELLOW);

                            tft_setCursor(bricks[i].x + 5, bricks[i].y + 6 - obj_speed);
                            tft_setTextColor(ILI9340_YELLOW);
                            tft_setTextSize(1);  
                            sprintf(buffer, "%d", bricks[i].weight);                 
                            tft_writeString(buffer); 

                            tft_setCursor(bricks[i].x + 5, bricks[i].y + 6);
                            tft_setTextColor(ILI9340_BLACK);
                            tft_setTextSize(1);  
                            sprintf(buffer, "%d", bricks[i].weight);                 
                            tft_writeString(buffer);
                        } 
                        else if ((bricks[i].y + obj_speed) > 233){
                            bricks[i].valid = 0; // not valid anymore
                        }
                        
                        break;
                        
                    case 1:
                        tft_fillRect(bricks[i].x, bricks[i].y, 20, obj_speed, ILI9340_BLACK);
             
                        bricks[i].y = bricks[i].y + obj_speed; 

                        if(bricks[i].y > 163 && (((bricks[i].x > planePos) && (bricks[i].x < planePos + 20)) 
                                || ((bricks[i].x + 20 > planePos) && (bricks[i].x + 20 < planePos + 20)))){
                            // Obtains bullets
                            num_bullets += bricks[i].weight; // hit the plane
                            tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_BLACK);
                            bricks[i].valid = 0; // not valid anymore
                        }
                        else if (bricks[i].y < 213){
                            tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_GREEN);

                            tft_setCursor(bricks[i].x + 5, bricks[i].y + 6 - obj_speed);
                            tft_setTextColor(ILI9340_GREEN);
                            tft_setTextSize(1);  
                            sprintf(buffer, "%d", bricks[i].weight);                 
                            tft_writeString(buffer); 

                            tft_setCursor(bricks[i].x + 5, bricks[i].y + 6);
                            tft_setTextColor(ILI9340_BLACK);
                            tft_setTextSize(1);  
                            sprintf(buffer, "%d", bricks[i].weight);                 
                            tft_writeString(buffer);
                        } 
                        else if ((bricks[i].y + obj_speed) > 233){
                            bricks[i].valid = 0; // not valid anymore
                        }
                        
                        break;
                        
                    case 2:
                        tft_fillCircle(bricks[i].x, bricks[i].y, 9, ILI9340_BLACK);
                        
                        if(bricks[i].y > 163 && (((bricks[i].x - 10 > planePos) && (bricks[i].x - 10 < planePos+20)) 
                            || ((bricks[i].x + 10 > planePos) && (bricks[i].x + 10 < planePos+20)))){
                            // Obtains destroyer
                            destroyer = 1;
                            bricks[i].valid = 0; // not valid anymore
                        }
                        else if (bricks[i].y < 213){
                            bricks[i].y = bricks[i].y + obj_speed;
                            tft_fillCircle(bricks[i].x, bricks[i].y, 9, ILI9340_BLUE);
                        }
                        else if ((bricks[i].y + obj_speed) > 235){
                            bricks[i].valid = 0; // not valid anymore
                        }
                        break;
                        
                    default:
                        break;
                }
                
            }
        }
    } 
}

int detectHit(int planePos){
    int i;
    for(i = 0; i < bricks_max; i++){
        if(bricks[i].valid && (bricks[i].type == 0) && 
        (planePos > bricks[i].x) && (planePos < bricks[i].x + 20)){
            if(bricks[i].weight < num_bullets){
                bricks[i].valid = 0; // brick disappears
                tft_fillRect(bricks[i].x, bricks[i].y, 20, 20, ILI9340_BLACK); 
                num_bullets -= bricks[i].weight;
            }
            else{
                bricks[i].weight -= num_bullets;
                num_bullets = 0;
            }            
            return (bricks[i].y + 20);
        }
    }
    return 25; // no hit detected
}
/* ============================= */

/* =========== I2C Configuration ============= */
float i2c_reads[6];
/* =========================================== */

/* ========= DAC Configuration ========== */
#define DAC_config_chan_A 0b0011000000000000 // A-channel, 1x, active
#define DAC_config_chan_B 0b1011000000000000 // B-channel, 1x, active
/* =================================================== */

//=========== Timer 2 interrupt handler ===============
volatile SpiChannel spiChn = SPI_CHANNEL2 ;	// the SPI channel to use
volatile int spiClkDiv = 2 ; // 20 MHz max speed for this DAC
volatile int generate_period = 40000;
/* =================================================== */

/* ================ Vibration Motor ===================*/
volatile int vibmotor_pwm = 30000;
/* ============================================== */

/* ================ DMA Thread ===================*/
#define sine_table_size 256
#define dmaChn 0
#define SYS_FREQ 40000000

static int sound_flag = 0;
static unsigned char sine_table[sine_table_size];
/* ============================================== */

/* ================ Pre-Game THREAD ===================*/
static PT_THREAD (protothread_pregame(struct pt *pt)){
    PT_BEGIN(pt);
    
    tft_fillScreen(ILI9340_BLACK); // cleaning the screen
        
    while(1){
        
        PT_YIELD_TIME_msec(1);
        
        tft_setCursor(100, 10);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "WELCOME TO");                 
        tft_writeString(buffer);

        tft_setCursor(87, 50);                 
        tft_setTextColor(ILI9340_GREEN); 
        tft_setTextSize(5);                 
        sprintf(buffer, "PGC32");                 
        tft_writeString(buffer);

        tft_setCursor(70, 200);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "Press");                 
        tft_writeString(buffer);

        tft_setCursor(140, 200);                 
        tft_setTextColor(ILI9340_BLUE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "any");                 
        tft_writeString(buffer);

        tft_setCursor(190, 200);                 
        tft_setTextColor(ILI9340_RED); 
        tft_setTextSize(2);                 
        sprintf(buffer, "button");                 
        tft_writeString(buffer);

        tft_setCursor(70, 220);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "to start playing");                 
        tft_writeString(buffer);
                
        PT_YIELD_UNTIL(pt, button1 || button2);
        
        while(button1 || button2){
            tft_drawCircle(160, 140, 1, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 1, ILI9340_BLACK);

            tft_drawCircle(160, 140, 3, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 3, ILI9340_BLACK);

            tft_drawCircle(160, 140, 5, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 5, ILI9340_BLACK);

            tft_drawCircle(160, 140, 7, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 7, ILI9340_BLACK);

            tft_drawCircle(160, 140, 9, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 9, ILI9340_BLACK);

            tft_drawCircle(160, 140, 11, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 11, ILI9340_BLACK);

            tft_drawCircle(160, 140, 13, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 13, ILI9340_BLACK);

            tft_drawCircle(160, 140, 15, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 15, ILI9340_BLACK);

            tft_drawCircle(160, 140, 17, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 17, ILI9340_BLACK);

            tft_drawCircle(160, 140, 19, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 19, ILI9340_BLACK);

            tft_drawCircle(160, 140, 21, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 21, ILI9340_BLACK);

            tft_drawCircle(160, 140, 23, ILI9340_GREEN);
            PT_YIELD_TIME_msec(10);
            tft_drawCircle(160, 140, 23, ILI9340_BLACK); 
        }
        
        /* =============== Difficulty selection ============== */
        tft_fillScreen(ILI9340_BLACK);
        
        difficulty = 0;

        tft_setCursor(70, 35);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(3);                 
        sprintf(buffer, "Difficulty");                 
        tft_writeString(buffer);
        
        tft_setCursor(135, 100);                 
        tft_setTextColor(ILI9340_GREEN); 
        tft_setTextSize(2);                 
        sprintf(buffer, "Easy");                 
        tft_writeString(buffer);

        tft_setCursor(123, 140);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "Medium");                 
        tft_writeString(buffer);

        tft_setCursor(107, 180);                 
        tft_setTextColor(ILI9340_WHITE); 
        tft_setTextSize(2);                 
        sprintf(buffer, "Difficult");                 
        tft_writeString(buffer);
        
        int joy_movement;
        
        while(!button1 && !button2){
            
            PT_YIELD_TIME_msec(10);
            
            joy_movement = ReadADC10(0);
            
            if(joy_movement > 1020){
                                
                tft_setCursor(diff_array_x[difficulty], diff_array_y[difficulty]);                 
                tft_setTextColor(ILI9340_WHITE); 
                tft_setTextSize(2);  
                
                if(difficulty == 0){
                    sprintf(buffer, "Easy");                 
                }
                else if(difficulty == 1){
                    sprintf(buffer, "Medium");
                }
                else{
                    sprintf(buffer, "Difficult");
                }
                
                tft_writeString(buffer);
                
                difficulty = ((difficulty - 1) == -1) ? 2 : difficulty - 1; 

                tft_setCursor(diff_array_x[difficulty], diff_array_y[difficulty]);                 
                tft_setTextColor(ILI9340_GREEN); 
                tft_setTextSize(2);  
                
                if(difficulty == 0){
                    sprintf(buffer, "Easy");                 
                }
                else if(difficulty == 1){
                    sprintf(buffer, "Medium");
                }
                else{
                    sprintf(buffer, "Difficult");
                }
                
                tft_writeString(buffer);
                
                while(joy_movement > 600) // wait for joystick to get down
                {
                    joy_movement = ReadADC10(0);
                }
            }
            else if(joy_movement < 5){
                
                joy_movement = ReadADC10(0);
                
                tft_setCursor(diff_array_x[difficulty], diff_array_y[difficulty]);                 
                tft_setTextColor(ILI9340_WHITE); 
                tft_setTextSize(2);  
                
                if(difficulty == 0){
                    sprintf(buffer, "Easy");                 
                }
                else if(difficulty == 1){
                    sprintf(buffer, "Medium");
                }
                else{
                    sprintf(buffer, "Difficult");
                }
                
                tft_writeString(buffer);
                
                difficulty = ((difficulty + 1) == 3) ? 0 : difficulty + 1; 

                tft_setCursor(diff_array_x[difficulty], diff_array_y[difficulty]);                 
                tft_setTextColor(ILI9340_GREEN); 
                tft_setTextSize(2);  
                
                if(difficulty == 0){
                    sprintf(buffer, "Easy");                 
                }
                else if(difficulty == 1){
                    sprintf(buffer, "Medium");
                }
                else{
                    sprintf(buffer, "Difficult");
                }
                
                tft_writeString(buffer);
                
                while(joy_movement < 400){ // wait for joystick to get down
                    joy_movement = ReadADC10(0);
                } 
            }
            
        }
        
        /* ================ Countdown sequence ================== */

        tft_fillScreen(ILI9340_BLACK);
        PT_YIELD_TIME_msec(500);
        
        tft_fillCircle(165, 110, 70, ILI9340_WHITE); 
        tft_setCursor(143, 80);                 
        tft_setTextColor(ILI9340_BLACK); 
        tft_setTextSize(9);                 
        sprintf(buffer, "3");                 
        tft_writeString(buffer);

        pre_game_count = 1;
        WritePeriod2(400);
        sound_flag = 1;
        PT_YIELD_TIME_msec(800);
        pre_game_count = 0;

        tft_fillScreen(ILI9340_BLACK);
        PT_YIELD_TIME_msec(500);
        
        tft_fillCircle(165, 110, 70, ILI9340_WHITE);
        tft_setCursor(145, 80);                 
        tft_setTextColor(ILI9340_BLACK); 
        tft_setTextSize(9);                 
        sprintf(buffer, "2");                 
        tft_writeString(buffer);

        pre_game_count = 1;
        WritePeriod2(400);
        sound_flag = 1;
        PT_YIELD_TIME_msec(800);
        pre_game_count = 0;

        tft_fillScreen(ILI9340_BLACK);
        PT_YIELD_TIME_msec(500);
        
        tft_fillCircle(165, 110, 70, ILI9340_WHITE);
        tft_setCursor(144, 80);                 
        tft_setTextColor(ILI9340_BLACK); 
        tft_setTextSize(9);                 
        sprintf(buffer, "1");                 
        tft_writeString(buffer);

        pre_game_count = 1;
        WritePeriod2(400);
        sound_flag = 1;
        PT_YIELD_TIME_msec(800);
        pre_game_count = 0;

        tft_fillScreen(ILI9340_BLACK);
        PT_YIELD_TIME_msec(500);
        
        /* Restarting the game parameters */
        play_time = time + 1;
        pre_game = 0; // user started the game
        restart_game = 1;
        num_bullets = bullets;
        destroyer = 0;
        prev_game_mode = 4;
        speed_accum = 0;
        initBricks();
    }
    PT_END(pt);
}
/* =================================================== */

/* ================ Actual Game THREAD ===================*/
static PT_THREAD (protothread_game(struct pt *pt)){
    PT_BEGIN(pt);
    
    tft_fillScreen(ILI9340_BLACK); // cleaning the screen
                
    while(1){
        
        init_time = PT_GET_TIME();

        if(play_time == 0){
            // Won the game
            tft_fillScreen(ILI9340_BLACK); // cleaning the screen
            game_result = 1; // victory
            post_game = 1;
        }
        else{
            game_mode = !mPORTBReadBits(BIT_4); // setting the game mode

            if(game_mode != prev_game_mode){ // detect only on change
                
                tft_fillRect(40, 190, 50, 20, ILI9340_BLACK);// x,y,w,h,radius,color                 
                tft_setCursor(40, 190);                 
                tft_setTextColor(ILI9340_WHITE); 
                tft_setTextSize(1); 
                
                if(game_mode){
                    sprintf(buffer, "Joystick");                 
                }
                else{
                    sprintf(buffer, "Tilt");                 
                }
                tft_writeString(buffer);
            }
            
            if(destroyer){
                tft_fillCircle(70, 223, 4, ILI9340_GREEN);// x,y,w,h,radius,color  
            }
            else{
                tft_fillCircle(70, 223, 4, ILI9340_RED);// x,y,w,h,radius,color    
            }
            
            if(button2 && destroyer){ // used the destroyer
                clearBricks();
                destroyer = 0;
            }
                        
            if(button1 && !pressed && (num_bullets > 0)){
                temp_x = gun_x;
                laser_top = detectHit(temp_x + 10);
                tft_drawLine(temp_x + 10, 180, temp_x + 10, laser_top, ILI9340_GREEN);
                pressed = 1;
            }
            else if(pressed){
                tft_drawLine(temp_x + 10, 180, temp_x + 10, laser_top, ILI9340_BLACK);
                pressed = 0;
            }

            drawPlane(gun_x);
            updateBricks(gun_x);
            
            if(brick_hit){
                game_result = 0; // loss
                post_game = 1;
            }

            prev_gun_x = gun_x;
            
            if(restart_game && gun_x > 100){
                drawField();
                initBricks();
                restart_game = 0;
            }
            
            speed_accum += obj_speed;
            
            if(speed_accum > 70){
                // Time to bring in more bricks
                int i, brick_count = 0;
                for(i = 0; i < bricks_max; i++){
                    if(!bricks[i].valid){
                        bricks[i].valid  = (random_gen() > diff_array_brick[difficulty]) ? 1 : 0;
                        bricks[i].weight = (int)((random_gen() * 3 + 1) * 10);
                        bricks[i].first  = 1;
                        
                        float btype = random_gen();
                        
                        if(btype < 0.15){
                            bricks[i].type = 1; // bullets
                            bricks[i].x      = 115 + 23 * brick_count;
                            bricks[i].y      = 10;
                        }
                        else if(btype > 0.90 && !destroyer){
                            bricks[i].type = 2; // destroyer
                            bricks[i].x      = 125 + 23 * brick_count;
                            bricks[i].y      = 20;
                        }
                        else{
                            bricks[i].type = 0; // brick
                            bricks[i].x      = 115 + 23 * brick_count;
                            bricks[i].y      = 10;
                        }
                        
                        if((brick_count++) == 7){
                            break;
                        }
                    }
                }
                speed_accum = 0;
            }
        }
        
        prev_game_mode = game_mode;
        tft_drawRect(100, 5, 215, 230, ILI9340_YELLOW);// x,y,w,h,radius,color
        
        PT_YIELD_TIME_msec(30 - (PT_GET_TIME() - init_time));
    }
    PT_END(pt);
}
/* =================================================== */

/* ================ Post-Game THREAD ===================*/
static PT_THREAD (protothread_postgame(struct pt *pt)){
    PT_BEGIN(pt);
        
    while(1){
                        
        if(game_result){ // WIN
            tft_fillScreen(ILI9340_BLACK); // cleaning the screen
            tft_setCursor(70, 50);                 
            tft_setTextColor(ILI9340_GREEN); 
            tft_setTextSize(5);                 
            sprintf(buffer, "VICTORY");                 
            tft_writeString(buffer);
            
            int i, j;
            int colors[6] = {ILI9340_WHITE, ILI9340_BLUE, ILI9340_GREEN, 
                             ILI9340_RED, ILI9340_CYAN, ILI9340_MAGENTA};
            int fwX[6] = {50, 275, 160, 124, 68, 241};
            int fwY[6] = {175, 150, 200, 160, 190, 175};
            
            for(i = 0; i < 6; i++){
                
                for(j = 0; j < 150000; j++){
                   wait40;
                }
                
                if(i > 0){
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-25, fwY[i-1], ILI9340_BLACK); 
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1], fwY[i-1]-25, ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1], fwY[i-1]+25, ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+25, fwY[i-1], ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-20, fwY[i-1]-20, ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+20, fwY[i-1]+20, ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+20, fwY[i-1]-20, ILI9340_BLACK);
                    tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-20, fwY[i-1]+20, ILI9340_BLACK);
                }
                
                tft_drawLine(fwX[i], fwY[i], fwX[i]-25, fwY[i], colors[i]); 
                tft_drawLine(fwX[i], fwY[i], fwX[i], fwY[i]-25, colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i], fwY[i]+25, colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i]+25, fwY[i], colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i]-20, fwY[i]-20, colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i]+20, fwY[i]+20, colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i]+20, fwY[i]-20, colors[i]);
                tft_drawLine(fwX[i], fwY[i], fwX[i]-20, fwY[i]+20, colors[i]);
            }
                        
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-25, fwY[i-1], ILI9340_BLACK); 
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1], fwY[i-1]-25, ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1], fwY[i-1]+25, ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+25, fwY[i-1], ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-20, fwY[i-1]-20, ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+20, fwY[i-1]+20, ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]+20, fwY[i-1]-20, ILI9340_BLACK);
            tft_drawLine(fwX[i-1], fwY[i-1], fwX[i-1]-20, fwY[i-1]+20, ILI9340_BLACK);
            
        }
        else{ // LOSS
            
            PT_YIELD_TIME_msec(2000);
            tft_fillScreen(ILI9340_BLACK); // cleaning the screen
                    
            tft_setCursor(75, 50);                 
            tft_setTextColor(ILI9340_RED); 
            tft_setTextSize(5);                 
            sprintf(buffer, "DEFEAT");                 
            tft_writeString(buffer);

            tft_fillRect(130, 150, 70, 10, ILI9340_WHITE); /* wings */
            tft_fillRect(160, 130, 10, 70, ILI9340_WHITE); /* body */
            tft_fillRect(150, 200, 30, 10, ILI9340_WHITE); /* tail */
            
            PT_YIELD_TIME_msec(1000);
            
            // Starting the bleeding process
            int i, j;
            for(i = 130; i < 211; i++){
                if(i < 151){
                    tft_fillRect(160, 130, 10, i - 130, ILI9340_RED); 
                }
                else if(i < 161){
                    tft_fillRect(130, 150, 70, i - 150, ILI9340_RED); 
                }
                else if(i < 201){
                    tft_fillRect(160, 160, 10, i - 160, ILI9340_RED);  
                }
                else{
                    tft_fillRect(150, 200, 30, i - 200, ILI9340_RED);
                }
                
                for(j = 0; j < 10000; j++){
                   wait40;
                }
            }
        }
                
        tft_fillScreen(ILI9340_BLACK); // cleaning the screen
        post_game = 0;
        pre_game  = 1;
        
        PT_YIELD_TIME_msec(1000);
    }
    
    PT_END(pt);
}
/* =================================================== */

/* ================ Joystick THREAD ===================*/
static PT_THREAD (protothread_joystick(struct pt *pt))
{
    PT_BEGIN(pt);
    while(1){
        
        PT_YIELD_TIME_msec(1);
        
        joy_x = ReadADC10(1);        
        joy_y = ReadADC10(0);

        raw_gun_x = ((193 * joy_x) >> 10) + 101;
        xPos_avg[avg_idx] = raw_gun_x;
        avg_idx = (avg_idx+1 == 8) ? 0 : avg_idx+1;
        gun_x = running_avg();
        
        obj_speed = ((2 * joy_y) >> 10) + 1;  
    }
    
    PT_END(pt);  
}
/* ============================================== */

/* ================ Pushbutton1 THREAD ===================*/
static PT_THREAD (protothread_button1(struct pt *pt))
{
    PT_BEGIN(pt);
    while(1){      
        PT_YIELD_TIME_msec(1);

        press_but1 = !mPORTAReadBits(BIT_4);
        
        switch (PushStateBut1) {

            case NoPush: 
                if (press_but1) PushStateBut1 = MaybePush;
                else{
                    PushStateBut1 = NoPush;
                    button1 = 0;
                }
                break;

            case MaybePush:
                if (press_but1) {
                    PushStateBut1 = Pushed;
                    
                    if(!pre_game){
                        WritePeriod2(800);
                        sound_flag = 1;
                    }
                    else if(num_bullets > 0){
                        WritePeriod2(300);
                        sound_flag = 1;
                    }
                    button1 = 1;
                }
               else PushStateBut1 = NoPush;
               break;

            case Pushed:  
                if (press_but1) PushStateBut1 = Pushed; 
                else PushStateBut1 = MaybeNoPush;    
                break;

            case MaybeNoPush:
                if (press_but1) PushStateBut1=Pushed; 
                else PushStateBut1 = NoPush;    
                break;
        }
        
    }
    
    PT_END(pt);  
}
/* ============================================== */

/* ================ Pushbutton2 THREAD ===================*/
static PT_THREAD (protothread_button2(struct pt *pt))
{
    PT_BEGIN(pt);
    while(1){      
        PT_YIELD_TIME_msec(1);

        press_but2 = !mPORTAReadBits(BIT_2);
        
        switch (PushStateBut2) {

            case NoPush: 
                if (press_but2) PushStateBut2 = MaybePush;
                else{
                    PushStateBut2 = NoPush;
                    button2 = 0;
                }
                break;

            case MaybePush:
                if (press_but2) {
                    PushStateBut2 = Pushed;
                    
                    if(!pre_game){
                        WritePeriod2(800);
                        sound_flag = 1;
                    }
                    else if(destroyer){
                        WritePeriod2(300);
                        sound_flag = 1;
                    }
                    button2 = 1;                   
                }
               else PushStateBut2 = NoPush;
               break;

            case Pushed:  
                if (press_but2) PushStateBut2 = Pushed; 
                else PushStateBut2 = MaybeNoPush;    
                break;

            case MaybeNoPush:
                if (press_but2) PushStateBut2=Pushed; 
                else PushStateBut2 = NoPush;    
                break;
        }
        
    }
    
    PT_END(pt);  
}
/* ============================================== */

/* ================ Accelerometer THREAD ===================*/
static PT_THREAD (protothread_accel(struct pt *pt))
{
    PT_BEGIN(pt);
    while(1){
        PT_YIELD_TIME_msec(1);
                
        readImuValues(i2c_reads); 
        
        raw_gun_x = ((193 * (int)(i2c_reads[1] + 16000)) >> 15) + 101;
        xPos_avg[avg_idx] = raw_gun_x;
        avg_idx = (avg_idx+1 == 8) ? 0 : avg_idx+1;
        gun_x = running_avg();
        
        obj_speed = ((2 * (int)(i2c_reads[0] + 16000)) >> 15) + 1;
    }
    PT_END(pt);  
}
/* ============================================== */

/* ================ DMA Thread =================== */
static PT_THREAD (protothread_dma_sound(struct pt *pt)){
    PT_BEGIN(pt);
    while(1)
    {
        PT_YIELD_UNTIL(pt, sound_flag);
        sound_flag = 0;
        
        DmaChnEnable(dmaChn);
        
        PT_YIELD_UNTIL(pt, ((!button1 && !button2 && !pre_game_count) || post_game));

        DmaChnDisable(dmaChn);
    }
    PT_END(pt);
}
/* ============================================== */

/* ================ Timer Thread ================ */
static PT_THREAD (protothread_timer(struct pt *pt))
{
    PT_BEGIN(pt);
  
        while(1) {
            // yield time 1 second
            if(play_time > 0 && !post_game) { 
                play_time--;
            }
            
            if(!pre_game && !post_game){
                tft_fillRect(35, 25, 30, 20, ILI9340_BLACK);
                
                if(play_time > 9){
                    tft_setCursor(38, 27);
                }
                else{
                    tft_setCursor(46, 27);
                }
                
                tft_setTextColor(ILI9340_WHITE);
                tft_setTextSize(2);  
                sprintf(buffer, "%d", play_time);                 
                tft_writeString(buffer);
            }
            
            PT_YIELD_TIME_msec(1000);
        } 
    PT_END(pt);
}
/* ================================================== */

/* ================ Timer ISR =================== */
void __ISR(_TIMER_3_VECTOR, ipl3) Timer3Handler(void)
{
    mT3ClearIntFlag();
    
    if(!post_game && ((button1 && (num_bullets > 0)) 
    || (button2) || pre_game_count)){
        SetDCOC3PWM(vibmotor_pwm);
    }
    else{
        SetDCOC3PWM(0);
    }
}
/* ============================================== */

/* ================== MAIN ===================== */
void main(void) {
      
    ANSELA = 0; ANSELB = 0; 
    
    /* ============== DMA Sine Table ==================== */
    int timer_limit ;
    short s;
  
    timer_limit = SYS_FREQ/(sine_table_size * 1000);
  
    int m;
    for (m = 0; m < sine_table_size; m++){
        s =(short)(2047*sin((float)m*6.283/(float)256));
        sine_table[m] = DAC_config_chan_A | s;
    }
    
    /* =================== Configure timer 3 and DMA =================== */
    OpenTimer2(T2_ON | T2_SOURCE_INT | T2_PS_1_1, timer_limit);

	DmaChnOpen(dmaChn, 0, DMA_OPEN_AUTO);

    DmaChnSetTxfer(dmaChn, sine_table, (void*)&SPI2BUF, sine_table_size, 2, 2);

	DmaChnSetEventControl(dmaChn, DMA_EV_START_IRQ(_TIMER_2_IRQ));
    
    /* ============== SPI Channel Configuration ============= */
    SpiChnOpen(SPI_CHANNEL2, SPI_OPEN_ON | SPI_OPEN_MODE16 | SPI_OPEN_MSTEN | SPI_OPEN_CKE_REV | SPICON_FRMEN | SPICON_FRMPOL, 2);
    PPSOutput(2, RPB5, SDO2); // SDO2 (MOSI) is in PPS output group 2
    PPSOutput(4, RPB10, SS2);

    /* ======== Config timer and output compare to make PWM =========== */ 
    OpenTimer3(T3_ON | T3_SOURCE_INT | T3_PS_1_1, generate_period);
    ConfigIntTimer3(T3_INT_ON | T3_INT_PRIOR_2); 
    mT2ClearIntFlag(); // and clear the interrupt flag 
    
    /* ======== Set up compare3 for PWM mode =========== */  
    OpenOC3(OC_ON | OC_TIMER3_SRC | OC_PWM_FAULT_PIN_DISABLE , vibmotor_pwm, vibmotor_pwm); // 
    // OC3 is PPS group 4, map to RPB9 (pin 18) 
    PPSOutput(4, RPA3, OC3); 
    
    /* ========= Setting input pin for pushbutton1 ========== */
    mPORTASetPinsDigitalIn(BIT_4);
    
    /* ========= Setting input pin for pushbutton2 ========== */
    mPORTASetPinsDigitalIn(BIT_2);
    
    /* ========= Setting input pin for game switch ========== */
    mPORTBSetPinsDigitalIn(BIT_4);
    
    /* ===================== I2C Set up ==================== */
    OpenI2C1(I2C_ON, 48); // open I2C communication
    
    // Take the MPU 6050 out of sleep mode
    char data[] = {0};
    i2c_write(0x6b, data, 1);
        
    /* ========== Configuration of threads ========== */
    PT_setup();

    /* ========= Setup system wide interrupts  ======== */
    INTEnableSystemMultiVectoredInt();

    /* ========== Initializing TFT display =========== */
    tft_init_hw();
    tft_begin();
    tft_fillScreen(ILI9340_BLACK);
    tft_setRotation(1); // use tft_setRotation(1) for 320x240

    /* ================ Configure and enable the ADC ================*/ 
    CloseADC10(); // ensure the ADC is off before setting the configuration

    // define setup parameters for OpenADC10
    // Turn module on | ouput in integer | trigger mode auto | enable autosample
    // ADC_CLK_AUTO -- Internal counter ends sampling and starts conversion (Auto convert)
    // ADC_AUTO_SAMPLING_ON -- Sampling begins immediately after last conversion completes; SAMP bit is automatically set
    // ADC_AUTO_SAMPLING_OFF -- Sampling begins with AcquireADC10();
    #define PARAM1  ADC_FORMAT_INTG16 | ADC_CLK_AUTO | ADC_AUTO_SAMPLING_ON 

    // define setup parameters for OpenADC10
    // ADC ref external  | disable offset test | disable scan mode | do 2 sample | use single buf | alternate mode on
    #define PARAM2  ADC_VREF_AVDD_AVSS | ADC_OFFSET_CAL_DISABLE | ADC_SCAN_OFF | ADC_SAMPLES_PER_INT_2 | ADC_ALT_BUF_OFF | ADC_ALT_INPUT_ON
            //
    // Define setup parameters for OpenADC10
    // use peripherial bus clock | set sample time | set ADC clock divider
    // ADC_CONV_CLK_Tcy2 means divide CLK_PB by 2 (max speed)
    // ADC_SAMPLE_TIME_5 seems to work with a source resistance < 1kohm
    // SLOW it down a little
    #define PARAM3 ADC_CONV_CLK_PB | ADC_SAMPLE_TIME_15 | ADC_CONV_CLK_Tcy 

    // define setup parameters for OpenADC10
    // set AN11 and  as analog inputs
    #define PARAM4 ENABLE_AN5_ANA | ENABLE_AN11_ANA 

    // define setup parameters for OpenADC10
    // do not assign channels to scan
    #define PARAM5 SKIP_SCAN_ALL

    /* ======== Configure ADC to sample AN5 and AN11 on MUX A and B ========= */
    SetChanADC10( ADC_CH0_NEG_SAMPLEA_NVREF | ADC_CH0_POS_SAMPLEA_AN5 | ADC_CH0_NEG_SAMPLEB_NVREF | ADC_CH0_POS_SAMPLEB_AN11 );
    OpenADC10( PARAM1, PARAM2, PARAM3, PARAM4, PARAM5 ); 
    EnableADC10();
    
    /* ========== Initialization of game-related stuff ============= */
    
    /* ========== Initialization of threads ============= */
    PT_INIT(&pt_joystick);
    PT_INIT(&pt_button1);
    PT_INIT(&pt_button2);
    PT_INIT(&pt_accel);
    PT_INIT(&pt_dma_sound);
    PT_INIT(&pt_pregame);
    PT_INIT(&pt_game);  
    PT_INIT(&pt_postgame);
    PT_INIT(&pt_timer);

    /* =========== Control CS for DAC ============= */
    mPORTBSetPinsDigitalOut(BIT_4);
    mPORTBSetBits(BIT_4);
    
    /* =========== Setting up random generator ============ */
    srand(7);
    
    /* ============ Executing Scheduler of Threads ============= */
    while (1){

        if(pre_game){
            PT_SCHEDULE(protothread_pregame(&pt_pregame));
            PT_SCHEDULE(protothread_dma_sound(&pt_dma_sound));
            PT_SCHEDULE(protothread_button1(&pt_button1));
            PT_SCHEDULE(protothread_button2(&pt_button2));
            PT_SCHEDULE(protothread_joystick(&pt_joystick));
        }
        else if(post_game){
            PT_SCHEDULE(protothread_postgame(&pt_postgame));
        }
        else{
            PT_SCHEDULE(protothread_game(&pt_game));
            PT_SCHEDULE(protothread_dma_sound(&pt_dma_sound));
            PT_SCHEDULE(protothread_button1(&pt_button1));
            PT_SCHEDULE(protothread_button2(&pt_button2));
            
            if(game_mode){
                PT_SCHEDULE(protothread_joystick(&pt_joystick));
            }
            else{
               PT_SCHEDULE(protothread_accel(&pt_accel));
            }
        }
        
        PT_SCHEDULE(protothread_timer(&pt_timer));
    }  
  
}
// === end  ======================================================
