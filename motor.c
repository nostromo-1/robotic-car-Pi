/**********************************************************************************
C�digo fuente del proyecto de coche rob�tico
Fichero: motor.c
Fecha: 21/2/2017

Realiza el control principal del coche


***********************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <malloc.h>
#include <string.h>
#include <stdbool.h>
  
#include <pigpio.h>
#include <cwiid.h>

extern char *optarg;
extern int optind, opterr, optopt;

#define MI_ENA 17
#define MI_IN1 27
#define MI_IN2 22

#define MD_ENA 16
#define MD_IN1 20
#define MD_IN2 21

#define SONAR_TRIGGER 23
#define SONAR_ECHO    24
#define NUMPOS 2     /* N�mero de medidas de posici�n promediadas en distancia */
#define NUMHOLES 21  /* N�mero de agujeros en discos de encoder del motor */

#define PITO_PIN 26
#define VBAT_PIN 19
#define WMSCAN_PIN 12
#define AUDR_PIN 18
#define AUDR_ALT PI_ALT5   /* ALT function for audio pin, ALT5 for pin 18 */
#define LSENSOR_PIN 5
#define RSENSOR_PIN 6

#define DISTMIN 50  /* distancia a la que entendemos que hay un obst�culo */


void* play_wav(void *filename);  // Funci�n en fichero sound.c
volatile bool playing_audio, cancel_audio;  // Variables compartidas con fichero sound.c

void speedSensor(int gpio, int level, uint32_t tick);
void wmScan(int gpio, int level, uint32_t tick);

 
/****************** Variables y tipos globales **************************/

typedef enum {ADELANTE, ATRAS} Sentido_t;
typedef struct {
    const unsigned int en_pin, in1_pin, in2_pin, sensor_pin;  /* Pines  BCM */
    Sentido_t sentido;   /* ADELANTE, ATRAS */
    int velocidad;       /* 0 a 100, velocidad (no real) impuesta al motor */
    int PWMduty;         /* Valor de PWM para alcanzar la velocidad objetivo, 0-100 */
    volatile uint32_t counter, tick, rpm;
    pthread_mutex_t mutex;
} Motor_t;


typedef struct {
    cwiid_wiimote_t *wiimote;
    uint16_t buttons;
} MandoWii_t;


typedef struct {
    bool pitando;
    const unsigned int pin;
    pthread_mutex_t mutex;
} Bocina_t;


volatile uint32_t distancia = UINT32_MAX;
volatile int velocidadCoche = 50;  // velocidad objetivo del coche. Entre 0 y 100; el sentido de la marcha viene dado por el bot�n pulsado (A/B)
volatile int powerState = PI_ON;
volatile bool esquivando;

sem_t semaphore;
bool remoteOnly, useEncoder, checkBattery, softTurn;
char *alarmFile = "sounds/police.wav";
MandoWii_t mando;


Bocina_t bocina = {
    .pitando = false,
    .pin = PITO_PIN,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

Motor_t m_izdo = {
    .en_pin = MI_ENA,
    .in1_pin = MI_IN1,
    .in2_pin = MI_IN2,
    .sensor_pin = LSENSOR_PIN,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

Motor_t m_dcho = {
    .en_pin = MD_ENA,
    .in1_pin = MD_IN1,
    .in2_pin = MD_IN2,
    .sensor_pin = RSENSOR_PIN,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};




/****************** Funciones de control de los motores **************************/
void ajustaSentido(Motor_t *motor, Sentido_t dir)
{
    switch(dir) {
      case ADELANTE:
        gpioWrite(motor->in1_pin, PI_OFF);
        gpioWrite(motor->in2_pin, PI_ON);
        break;
      case ATRAS:
        gpioWrite(motor->in1_pin, PI_ON);
        gpioWrite(motor->in2_pin, PI_OFF);
        break;
    }
    motor->sentido = dir;
}


void stopMotor(Motor_t *motor)
{
    pthread_mutex_lock(&motor->mutex);
    gpioWrite(motor->in1_pin, PI_OFF);
    gpioWrite(motor->in2_pin, PI_OFF);
    motor->PWMduty = motor->velocidad = 0;
    gpioPWM(motor->en_pin, motor->PWMduty);
    pthread_mutex_unlock(&motor->mutex);  
}


/* v va de 0 a 100 */
void ajustaMotor(Motor_t *motor, int v, Sentido_t sentido)
{    
    if (v > 100) v = 100;
    if (v < 0) {
        fprintf(stderr, "Error en ajustaMotor: v<0!\n");
        v = 0;
    }
    if (motor->velocidad == v && motor->sentido == sentido) return;
    
    pthread_mutex_lock(&motor->mutex);
    ajustaSentido(motor, sentido);
    motor->PWMduty = motor->velocidad = v;
    gpioPWM(motor->en_pin, motor->PWMduty);
    pthread_mutex_unlock(&motor->mutex);   
}



/* Rota el coche a la derecha (dextr�giro, sentido>0) o a la izquierda (lev�giro, sentido<0)*/
void rota(Motor_t *izdo, Motor_t *dcho, int sentido)  
{
    if (sentido>0) {  // sentido horario
       ajustaMotor(izdo, 100, ADELANTE);
       ajustaMotor(dcho, 100, ATRAS);
    }
    else{
       ajustaMotor(izdo, 100, ATRAS);
       ajustaMotor(dcho, 100, ADELANTE);
    }    
}


int setupMotor(Motor_t *motor)
{
    int r = 0;
    
    r |= gpioSetMode(motor->in1_pin, PI_OUTPUT);
    r |= gpioSetMode(motor->in2_pin, PI_OUTPUT);
    
    if (gpioSetPWMfrequency(motor->en_pin, 100)<0) r = -1;   /* 100 Hz, low but not audible */
    if (gpioSetPWMrange(motor->en_pin, 100)<0) r = -1;       /* Range: 0-100, real range = 2000 */
    
    if (useEncoder) {
        r |= gpioSetMode(motor->sensor_pin, PI_INPUT);
        gpioSetPullUpDown(motor->sensor_pin, PI_PUD_DOWN);
        gpioSetAlertFunc(motor->sensor_pin, speedSensor);
        gpioGlitchFilter(motor->sensor_pin, 1000);      
    }
    
    if (r) fprintf(stderr, "Cannot initialise motor!\n");
    return r;
}



/****************** Funciones de control del sensor de distancia de ultrasonidos HC-SR04 **************************/

/* trigger a sonar reading */
void sonarTrigger(void)
{
   gpioWrite(SONAR_TRIGGER, PI_ON);
   gpioDelay(10); /* 10us trigger pulse */
   gpioWrite(SONAR_TRIGGER, PI_OFF);
}


/* callback llamado cuando el pin SONAR_ECHO cambia de estado. Ajusta la variable global distancia */
void sonarEcho(int gpio, int level, uint32_t tick)
{
   static uint32_t startTick, endTick;
   static uint32_t distance_array[NUMPOS], pos_array=0;
   uint32_t diffTick, d;
   uint32_t i, suma;
   static int firstTime=0;

 switch (level) {
 case PI_ON: 
           startTick = tick;
           break;

 case PI_OFF: 
           endTick = tick;
           diffTick = endTick - startTick;
           //if (diffTick > 30000 || diffTick < 50) break;  /* out of range */
           d = (diffTick*17)/1000;

           distance_array[pos_array++] = d;
           if (pos_array == NUMPOS) pos_array = 0;
 
           if (firstTime>=0) {
              if (firstTime < NUMPOS) { /* The first NUMPOS times until array is filled */
                  distancia = d;
                  firstTime++;
                  break;
              }
              else firstTime = -1;  /* Initialisation is over */
           }
           
           /* Calculate moving average */
           for (i=0, suma=0; i<NUMPOS; i++) suma += distance_array[i]; 
           distancia = suma/NUMPOS;   /* La variable de salida, global */
           //printf("Distancia: %u\n", distancia);
           if (!remoteOnly && distancia < DISTMIN && !esquivando) {
                esquivando = true;
                i = sem_post(&semaphore);  // indica a main que hay un obst�culo delante
                if (i) perror("Error al activar sem�foro");
           }
           break;
 } 
}


int setupSonar(void)
{
   gpioSetMode(SONAR_TRIGGER, PI_OUTPUT);
   gpioWrite(SONAR_TRIGGER, PI_OFF);
   gpioSetMode(SONAR_ECHO, PI_INPUT);

   /* update sonar several times a second, timer #0 */
   if (gpioSetTimerFunc(0, 60, sonarTrigger) ||     /* every 60ms */
        gpioSetAlertFunc(SONAR_ECHO, sonarEcho)) {  /* monitor sonar echos */
        fprintf(stderr, "Error al inicializar el sonar!\n");
        return -1;
       }
   return 0;
}



/****************** Funciones de la bocina **************************/
static void activaPito(void)
{
    pthread_mutex_lock(&bocina.mutex);
    if (bocina.pitando == 0) gpioWrite(bocina.pin, PI_ON);
    bocina.pitando++;
    pthread_mutex_unlock(&bocina.mutex);
}


static void desactivaPito(void)
{
    pthread_mutex_lock(&bocina.mutex);
    if (bocina.pitando > 0) bocina.pitando--;
    if (bocina.pitando == 0) gpioWrite(bocina.pin, PI_OFF);
    pthread_mutex_unlock(&bocina.mutex);
}


/*  Funci�n interna auxiliar */
static void* duerme_pitando(void *arg)
{
    uint32_t decimas, s, m;
    
    decimas = *(uint32_t*)arg;
    s = decimas/10;
    m = 100000*(decimas%10);
    activaPito();
    gpioSleep(PI_TIME_RELATIVE, s, m);   
    desactivaPito();
    free(arg);
    return NULL;
}


/* Toca el pito durante un tiempo (en decimas de segundo) 
modo=0; pita en otro hilo; vuelve inmediatamente 
modo=1; pita en este hilo, vuelve despu�s de haber pitado */
void pito(uint32_t decimas, int modo)
{
    pthread_t pth;
    uint32_t *pd;
        
    if (decimas == 0) return;
    pd = malloc(sizeof(uint32_t));
    if (!pd) return;
    *pd = decimas;
    if (modo == 0) {
        if (pthread_create(&pth, NULL, duerme_pitando, pd)) {
            free(pd);
            return;
        }
        pthread_detach(pth);
    }
    else duerme_pitando(pd);
}



/******************Funciones de audio ************************/


/* Reproduce "file" en otro hilo.
file debe ser un string invariable, en memoria 
modo=0; vuelve inmediatamente, sin esperar el final
modo=1; vuelve despu�s de haber tocado */
void audioplay(char *file, int modo)
{
    pthread_t pth;
    
    /* Si ya estamos reproduciendo algo, manda se�al de cancelaci�n al thread de audio */
    if (playing_audio) {
        cancel_audio = true;
        return;
    }
    if (pthread_create(&pth, NULL, play_wav, file)) return;
    if (modo == 0) pthread_detach(pth);
    else pthread_join(pth, NULL);    
}




/****************** Funciones del Wiimote **************************/
void wiiErr(cwiid_wiimote_t *wiimote, const char *s, va_list ap)
{
  if (wiimote) printf("Wiimote %d:", cwiid_get_id(wiimote)); else printf("-1:");
  vprintf(s, ap);
  printf("\n");
}


/*** wiimote event loop ***/
static void wiiCallback(cwiid_wiimote_t *wiimote, int mesg_count, union cwiid_mesg mesg[], struct timespec *t)
{
    int i, v_izdo, v_dcho;
    Sentido_t s_izdo, s_dcho;
    static uint16_t previous_buttons;
    unsigned int bateria;
    static int LEDs[4] = { CWIID_LED1_ON,  CWIID_LED1_ON | CWIID_LED2_ON,
                CWIID_LED1_ON | CWIID_LED2_ON | CWIID_LED3_ON,
                CWIID_LED1_ON | CWIID_LED2_ON | CWIID_LED3_ON | CWIID_LED4_ON };        

     for (i = 0; i < mesg_count; i++) {
        switch (mesg[i].type) {
        case CWIID_MESG_BTN:  // Change in buttons
            mando.buttons = mesg[i].btn_mesg.buttons;
            
            /* ajusta la velocidad del coche y la marca en leds del mando */
            if (previous_buttons&CWIID_BTN_PLUS && ~mando.buttons&CWIID_BTN_PLUS) {
                velocidadCoche += 10;
                if (velocidadCoche > 100) velocidadCoche = 100;
                cwiid_set_led(wiimote, LEDs[velocidadCoche/26]);
            }
            if (previous_buttons&CWIID_BTN_MINUS && ~mando.buttons&CWIID_BTN_MINUS) {
                velocidadCoche -= 10;
                if (velocidadCoche < 0) velocidadCoche = 0;
                cwiid_set_led(wiimote, LEDs[velocidadCoche/26]);
            }
            
            /*** Botones A y B, leen la variable global "velocidadCoche" ***/
            if (mando.buttons&(CWIID_BTN_A | CWIID_BTN_B)) { // if A or B or both pressed
                v_izdo = v_dcho = velocidadCoche;
                if (mando.buttons&CWIID_BTN_A) s_izdo = s_dcho = ADELANTE;
                else s_izdo = s_dcho = ATRAS;  // si vamos marcha atr�s (bot�n B), invierte sentido

                /*** Botones LEFT y RIGHT, giran el coche ***/
                if (mando.buttons&CWIID_BTN_RIGHT) {
                    if (softTurn) v_dcho = 0; s_dcho = 1 - s_dcho;
                    //v_dcho = softTurn?0:50; s_dcho = 1 - s_dcho;
                    v_izdo += softTurn?0:30;
                } 
            
                if (mando.buttons&CWIID_BTN_LEFT) {
                    if (softTurn) v_izdo = 0; s_izdo = 1 - s_izdo;
                    //v_izdo = softTurn?0:50; s_izdo = 1 - s_izdo;
                    v_dcho += softTurn?0:30;                
                }
            }
            else {  // Ni A ni B pulsados
                v_izdo = v_dcho = 0;
                s_izdo = s_dcho = ADELANTE;
            }
                    
            /*** Ahora activa la velocidadCoche calculada en cada motor ***/
            ajustaMotor(&m_izdo, v_izdo, s_izdo);
            ajustaMotor(&m_dcho, v_dcho, s_dcho);
        
    
            /*** pito ***/
            if (~previous_buttons&CWIID_BTN_DOWN && mando.buttons&CWIID_BTN_DOWN) activaPito();    
            if (previous_buttons&CWIID_BTN_DOWN && ~mando.buttons&CWIID_BTN_DOWN) desactivaPito();    
            
            
            /*** sonido ***/
            if (~previous_buttons&CWIID_BTN_UP && mando.buttons&CWIID_BTN_UP) {
                audioplay(alarmFile, 0);
            }
        
            /*** End of buttons loop ***/
            previous_buttons = mando.buttons;
            break;
                    
        case CWIID_MESG_STATUS:
            bateria = (100*mesg[i].status_mesg.battery)/CWIID_BATTERY_MAX;
            printf("Bateria del wiimote: %u\%\n", bateria);
            cwiid_set_led(wiimote, LEDs[velocidadCoche/26]);
            break;
            
        default:
            printf("Mensaje desconocido del wiimote!!\n");
            break;
        }
    }
}


void setupWiimote(void)
{
    cwiid_wiimote_t *wiimote;
    bdaddr_t ba;
    
    if (mando.wiimote) cwiid_close(mando.wiimote);
    cwiid_set_err(wiiErr);
    mando.buttons = 0;
    pito(5, 1);   // Pita 5 d�cimas para avisar que comienza b�squeda de mando
    printf("Pulsa las teclas 1 y 2 en el mando de la Wii...\n");
    gpioSleep(PI_TIME_RELATIVE, 2, 0);  // para desconectar el mando si estaba conectado
    ba = *BDADDR_ANY;
    wiimote = cwiid_open_timeout(&ba, 0, 5);
    if (!wiimote ||
        cwiid_set_rpt_mode(wiimote, CWIID_RPT_BTN | CWIID_RPT_STATUS) || 
        cwiid_set_mesg_callback(wiimote, wiiCallback) ||
        cwiid_enable(wiimote, CWIID_FLAG_MESG_IFC)) {
            fprintf(stderr, "No puedo conectarme al mando de la Wii!\n");
            mando.wiimote = NULL;
            return;  // No es error si no hay wiimote, el coche funciona sin mando
    } 
    mando.wiimote = wiimote;
    printf("Conectado al mando de la Wii\n");
    cwiid_set_rumble(wiimote, 1);  // se�ala mediante zumbido el mando sincronizado
    gpioSleep(PI_TIME_RELATIVE, 0, 500000);   // Espera 0,5 segundos
    cwiid_set_rumble(wiimote, 0);
    return;
}


/* callback llamado cuando el pin WMSCAN_PIN cambia de estado. Tiene un pull-up a VCC, OFF==pulsado */
void wmScan(int gpio, int level, uint32_t tick)
{
static int button;
static uint32_t time;
    
    switch (level) {
        case PI_ON:   // Sync button released
            if (button != 1) return;   // elimina clicks espureos
            button = 0;
            /* First, check for shutdown command: long press */
            if (time-tick > 2*1000000) {
                pito(10, 1);
                execlp("halt", "halt", NULL);
                return; // should never return
            }
            
            /* Short press: scan wiimotes */
            ajustaMotor(&m_izdo, 0, ADELANTE);  // Para el coche mientras escanea wiimotes
            ajustaMotor(&m_dcho, 0, ADELANTE);
            velocidadCoche = 50;   // Nueva velocidad inicial, con o sin mando  
            setupWiimote();   
            if (!mando.wiimote && !remoteOnly) {  // No hay mando, coche es aut�nomo
                ajustaMotor(&m_izdo, velocidadCoche, ADELANTE);
                ajustaMotor(&m_dcho, velocidadCoche, ADELANTE);                
            }
            break;
        case PI_OFF:  // Sync button pressed
            button = 1;
            time = tick;
            break;
    }
}



/***************Funciones de control de la velocidad ********************/
/* callback llamado cuando el pin LSENSOR_PIN o RSENSOR_PIN cambia de estado
Se usa para medir la velocidad de rotaci�n de las ruedas */
void speedSensor(int gpio, int level, uint32_t tick)
{
Motor_t *motor;

    switch (gpio) {
        case LSENSOR_PIN: 
            motor = &m_izdo; 
            break;
        case RSENSOR_PIN: 
            motor = &m_dcho; 
            break;  
    }

    switch (level) {
        case PI_ON:
            motor->counter++;
            motor->tick = tick;
            break;
        case PI_OFF:
            motor->counter++;
            motor->tick = tick;          
            break;
    }    
}


/* Callback llamado regularmente. Realiza el lazo de control de la velocidad, comparando
la diferencia de velocidades entre los motores para igualarlas */
void speedControl(void)
{
static uint32_t past_lcounter, past_ltick;    
uint32_t current_lcounter =  m_izdo.counter, current_ltick = m_izdo.tick;  
uint32_t lpulses=0, lperiod, lfreq;

static uint32_t past_rcounter, past_rtick;    
uint32_t current_rcounter =  m_dcho.counter, current_rtick = m_dcho.tick;  
uint32_t rpulses=0, rperiod, rfreq;
  
int pv, kp=5;  
  
    // Left motor
    if (past_ltick == 0) goto l_end;  // Exceptionally, it seems a good use of goto
    lperiod = current_ltick - past_ltick;
    if (lperiod) {
        lpulses = current_lcounter - past_lcounter;
        lfreq = (1000000*lpulses)/lperiod;
        m_izdo.rpm = lfreq*60/NUMHOLES/2;
        printf("Left motor: pulses=%u, rpm=%u\n", lpulses, m_izdo.rpm);
    }
    
    l_end:
    if (past_ltick == 0 || lperiod == 0) {
        //printf("Left motor stopped\n");
        m_izdo.rpm = 0;
    }
    past_lcounter = current_lcounter;
    past_ltick = current_ltick;
    
    // Right motor
    if (past_rtick == 0) goto r_end;  // Exceptionally, it seems a good use of goto
    rperiod = current_rtick - past_rtick;
    if (rperiod) {
        rpulses = current_rcounter - past_rcounter;
        rfreq = (1000000*rpulses)/rperiod;
        m_dcho.rpm = rfreq*60/NUMHOLES/2;
        printf("Right motor: pulses=%u, rpm=%u\n", rpulses, m_dcho.rpm);
    }
    
    r_end:
    if (past_rtick == 0 || rperiod == 0) {
        //printf("Right motor stopped\n");
        m_dcho.rpm = 0;    
    }
    past_rcounter = current_rcounter;
    past_rtick = current_rtick;
    
    /******* P control loop. SP=0, PV=lpulses-rpulses *********/
    if (m_izdo.velocidad>0 && m_izdo.rpm==0) printf("Left motor stalled\n");
    if (m_dcho.velocidad>0 && m_dcho.rpm==0) printf("Right motor stalled\n");   
    if (m_izdo.velocidad != m_dcho.velocidad) return;  // Enter control section if straight line desired: both speeds equal
    if (m_izdo.velocidad == 0) return;  // If speed is 0 (in both), do not enter control section
    pv = lpulses - rpulses;
    if (pv<=1 && pv >=-1) return;  // Tolerable error, do not enter control section
    
    pthread_mutex_lock(&m_izdo.mutex);
    pthread_mutex_lock(&m_dcho.mutex);   
    m_izdo.PWMduty -= (kp*pv)/10;
    m_dcho.PWMduty += (kp*pv)/10;
    if (m_izdo.PWMduty>100) m_izdo.PWMduty = 100;
    if (m_izdo.PWMduty<0) m_izdo.PWMduty = 0;
    if (m_dcho.PWMduty>100) m_dcho.PWMduty = 100;
    if (m_dcho.PWMduty<0) m_dcho.PWMduty = 0;
    gpioPWM(m_izdo.en_pin, m_izdo.PWMduty);
    gpioPWM(m_dcho.en_pin, m_dcho.PWMduty);   
    pthread_mutex_unlock(&m_izdo.mutex); 
    pthread_mutex_unlock(&m_dcho.mutex);
}



/****************** Funciones auxiliares varias **************************/


/* Comprueba si los motores tienen alimentaci�n.
Devuelve valor de las bater�as de alimentaci�n del motor en la variable global powerState: 
PI_OFF si no hay alimentaci�n o es baja, PI_ON si todo OK
Toma 3 muestras espaciadas 400 ms, en bucle, hasta que coincidan.
Esto es necesario porque al arrancar los motores hay un tiempo de oscilaci�n de las bater�as, 
aunque est� amortiguado por el condensador del circuito */    
void getPowerState(void)
{
    int power1, power2, power3;
    int n = 3;
    
    do {
        power1 = gpioRead(VBAT_PIN);
        if (power1 < 0) return;  // Error al leer, valor inv�lido
        gpioSleep(PI_TIME_RELATIVE, 0, 400000);  
        power2 = gpioRead(VBAT_PIN);
        if (power2 < 0) return;
        gpioSleep(PI_TIME_RELATIVE, 0, 400000);  
        power3 = gpioRead(VBAT_PIN);
        if (power3 < 0) return;
    } while (power1!=power2 || power2!=power3);
    powerState = power1;  // set global variable
    
    // se�al ac�stica en caso de bater�a baja
    if (powerState == PI_OFF) {
        while(n--) {
            pito(2, 1);  // pita 2 d�cimas en este hilo (vuelve despu�s de pitar)
            gpioSleep(PI_TIME_RELATIVE, 0, 200000);  // espera 2 d�cimas de segundo
        }
    }
}



/* Al recibir una se�al, para el coche, cierra todo y termina el programa */
void terminate(int signum)
{
   stopMotor(&m_izdo);
   stopMotor(&m_dcho);
   if (mando.wiimote) cwiid_close(mando.wiimote);
   gpioSetPullUpDown(WMSCAN_PIN, PI_PUD_OFF);
   gpioSetPullUpDown(VBAT_PIN, PI_PUD_OFF); 
   gpioSetPullUpDown(m_izdo.sensor_pin, PI_PUD_OFF);   
   gpioSetPullUpDown(m_dcho.sensor_pin, PI_PUD_OFF); 
   gpioWrite(bocina.pin, PI_OFF);
   gpioSetMode(AUDR_PIN, PI_INPUT);
   gpioTerminate();
   exit(1);
}



int setup(void)
{
   int r = 0;
    
   if (gpioCfgClock(5, PI_CLOCK_PCM, 0)<0) return 1;   /* Standard settings: Sample rate: 5 us, PCM clock */
   gpioCfgInterfaces(PI_DISABLE_FIFO_IF | PI_DISABLE_SOCK_IF);
   if (gpioInitialise()<0) return 1;
   if (gpioSetSignalFunc(SIGINT, terminate)<0) return 1;
    
   gpioSetMode(bocina.pin, PI_OUTPUT);
   gpioWrite(bocina.pin, PI_OFF);
   gpioSetMode(AUDR_PIN, AUDR_ALT);  // Saca PWM0 (audio right) por el GPIO al amplificador
   
   if (checkBattery) { 
    gpioSetMode(VBAT_PIN, PI_INPUT);
    gpioSetPullUpDown(VBAT_PIN, PI_PUD_DOWN);   // pull-down resistor; avoids floating pin if circuit not connected  
    gpioSetTimerFunc(1, 15000, getPowerState);  // Comprueba tensi�n motores cada 15 seg, timer#1
    getPowerState();    
   }
   gpioSetMode(WMSCAN_PIN, PI_INPUT);
   gpioSetPullUpDown(WMSCAN_PIN, PI_PUD_UP);  // pull-up resistor; button pressed == OFF
   gpioGlitchFilter(WMSCAN_PIN, 100000);      // 0,1 sec filter
   
   r |= setupMotor(&m_izdo);
   r |= setupMotor(&m_dcho);
    
   if (powerState == PI_OFF) {
       fprintf(stderr, "La bateria de los motores esta descargada. Coche no arranca!\n");
       terminate(SIGINT);
   }    

   setupWiimote();
   gpioSetAlertFunc(WMSCAN_PIN, wmScan);  // Call wmScan when button changes. Debe llamarse despu�s de setupWiimote
   if (useEncoder) gpioSetTimerFunc(2, 200, speedControl);  // Control velocidad motores, timer#2
   if (!remoteOnly) r |= setupSonar();
   r |= sem_init(&semaphore, 0, 0);
   return r;
}


/****************** Main **************************/
void main(int argc, char *argv[])
{
   int r;
   
   opterr = 0;  // Prevent getopt from outputting error messages
   while ((r = getopt(argc, argv, "rbesf:")) != -1)
       switch (r) {
           case 'r':  /* Remote only mode: does not measure distance */
               remoteOnly = true;
               break;
           case 'b':  /* Check battery */
               checkBattery = true;
               break;          
           case 'e':  /* Use wheel encoders */
               useEncoder = true;
               break;    
           case 's':  /* Soft turning (for 2WD) */
               softTurn = true;
               break;                 
           case 'f': /* Set sound file to be played with UP arrow */
               alarmFile = optarg;
               break;
           default:
               fprintf(stderr, "Uso: %s [-r] [-b] [-e] [-s] [-f <fichero de alarma>]\n", argv[0]);
               exit(1);
     }
   
   
   r = setup();
   if (r) {
       fprintf(stderr, "Error al inicializar. Coche no arranca!\n");
       if (r < 0) terminate(SIGINT);   // Si el error estaba al inicializar pigpio (r>0), no llames a terminate
       else exit(1);
   }
    
   audioplay("sounds/ready.wav", 1);
   if (mando.wiimote==NULL) {  // No hay mando, el coche es aut�nomo
        ajustaMotor(&m_izdo, velocidadCoche, ADELANTE);
        ajustaMotor(&m_dcho, velocidadCoche, ADELANTE);       
   }
   
   for (;;) { 
       esquivando = false;  // se�ala a sonarEcho que ya se puede volver a enviar la se�al de obst�culo encontrado
       r = sem_wait(&semaphore);   // bloquea hasta que encontremos un obst�culo
       if (r) {
            perror("Error al esperar al semaforo");
       }
       if (remoteOnly) continue;      
       if (mando.wiimote && ~mando.buttons&CWIID_BTN_A) continue;
       
       while (distancia < DISTMIN) {
            //printf("Distancia: %d cm\n", distancia); 
            stopMotor(&m_izdo);
            stopMotor(&m_dcho);
            //pito(2, 0);  // pita 2 d�cimas en otro hilo (vuelve inmediatamente)
            gpioSleep(PI_TIME_RELATIVE, 1, 0);
       
            // No esquiva si ya no pulsamos A
            if (mando.wiimote && ~mando.buttons&CWIID_BTN_A) break;

            // Gira un poco el coche para esquivar el obst�culo
            if (mando.wiimote && mando.buttons&CWIID_BTN_LEFT) rota(&m_izdo, &m_dcho, -1);  // con LEFT pulsado, esquiva a la izquierda
            else rota(&m_izdo, &m_dcho, 1);  // en caso contrario a la derecha
            gpioSleep(PI_TIME_RELATIVE, 0, 500000);
            stopMotor(&m_izdo);
            stopMotor(&m_dcho);
       }
       // Hemos esquivado el obst�culo, ahora velocidad normal si A est� pulsada
       if (!mando.wiimote || mando.buttons&CWIID_BTN_A) {
            ajustaMotor(&m_izdo, velocidadCoche, ADELANTE);
            ajustaMotor(&m_dcho, velocidadCoche, ADELANTE);
       }
   }
   
   
}




