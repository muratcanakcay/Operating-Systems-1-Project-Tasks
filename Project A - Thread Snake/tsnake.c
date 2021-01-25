#include <ctype.h>
#define _XOPEN_SOURCE 500
#include <ftw.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

// used to turn the debug messages on/off
#define HERE printf("********************HERE******************\n");
#define DEBUGMAIN 0
#define DEBUGINIT 0
#define DEBUGMAP 0
#define DEBUGARGS 0
#define DEBUGPLACEFOOD 0
#define DEBUGSPAWNSNAKE 0
#define DEBUGSNAKEWORK 0
#define DEBUGMOVESNAKE 0

#define MAX_X 100
#define MIN_X 10
#define MAX_Y 100
#define MIN_Y 10
#define MIN_SPEED 50
#define MAX_SPEED 1000
#define DEFAULT_X 20
#define DEFAULT_Y 20
#define DELAY 200

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

enum direction {up = 1, right = 2, down = 4, left = 8};
typedef unsigned int UINT;
typedef struct timespec timespec_t;
volatile sig_atomic_t last_signal = 0;

typedef struct xy_t
{
    int x;
    int y;
} xy_t;

typedef struct segment          // snake body parts
{
    xy_t pos;                   // position of the segment on the map
    struct segment* next;       // pointer to next segment (tail at the end)
    struct segment* previous;   // pointer to previous segment (head at the beginning)
} segment;

typedef struct snake_t          // snake data
{
    pthread_t tid;              // thread id of the snake
    UINT seed;                  // random seed for the snake
    char c;                     // symbol of the snake
    int s;                      // speed of the snake
    int l;                      // length of the snake
    bool grow_flag;             // if true snake will grow on the next move    
    xy_t target;                // food tile targeted by the snake
    enum direction direction;   // snake's direction of movement
    segment* head;              // snake's head segment
    segment* tail;              // snakes tail segment
} snake_t;

typedef struct gamedata_t       // game data
{
    xy_t mapDim;                // map dimensions
    char** map;                 // 2x2 map array            // MUST BE FREED IN EXIT SEQ!!!
    int snakeCount;             // number of snakes
    snake_t* snakes;            // data for snake threads   // MUST BE FREED IN EXIT SEQ!!!
    xy_t* foods;                // pos. of each food    -   // MUST BE FREED IN EXIT SEQ!!!
    char* pathf;                // save file path
    unsigned short saveFile;    // 0: save file not given, 1: save file given
    pthread_mutex_t* pmxMap;    // mutex for updateMap()
    sigset_t* pMask;            // signal mask
} gamedata_t;

//function declarations
void processSnakeArgs(char* snake, gamedata_t* gameData);
void createMap(gamedata_t* gameData);
void spawnSnake(gamedata_t* gameData, int snakeNo);
void printMap(gamedata_t* gameData, int fd);
void placeFood(gamedata_t* gameData, int foodNo);
void extendArrays(gamedata_t* gameData);
int getSnakeNo(gamedata_t* gameData);

//-----------

void usage(){
    fprintf(stderr,"\nUSAGE : tsnake [-x xdim=%d] [-y ydim=%d] [-f file=$SNAKEFILE] c1:s1 [c2:s2 ...]\n\n", DEFAULT_X, DEFAULT_Y);
    fprintf(stderr,"x : x dimension of the map. - Default value is %d\n\n", DEFAULT_X);
    fprintf(stderr,"y : y dimension of the map. - Default value is %d\n\n", DEFAULT_X);
    fprintf(stderr,"file : a path to a save file. If the option is not present, the value from environment variable $SNAKEFILE is used. If the variable is not set, it will not be possible to save the game state.\n\n");
    fprintf(stderr,"c1:s1 : c1 is the first snake's character (must be uppercase and unique for each snake), s1 is the first snake's speed in milliseconds.");
    fprintf(stderr," (must be between %d and %d) - At least one snake must be declared.\n\n", MIN_SPEED, MAX_SPEED);
    exit(EXIT_FAILURE);}

void msleep(UINT milisec) 
{
    time_t sec= (int)(milisec/1000);
    milisec = milisec - (sec*1000);
    timespec_t req= {0};
    req.tv_sec = sec;
    req.tv_nsec = milisec * 1000000L;
    if(nanosleep(&req,&req)) ERR("nanosleep");
}
void sethandler( void (*f)(int), int sigNo) {
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1==sigaction(sigNo, &act, NULL)) ERR("sigaction");
}
void sig_handler(int sig) {
	printf("[%d] received signal %d\n", getpid(), sig);
	last_signal = sig;
}
void readArgs(int argc, char** argv, gamedata_t* gameData)
{
	if (DEBUGARGS) printf("[READARGS]\n");
    
    int c, xcount = 0, ycount = 0, fcount = 0;
        
    while ((c = getopt(argc, argv, "x:y:f:")) != -1)
        switch (c)
        {
            case 'x':
                gameData->mapDim.x = atoi(optarg);
                if (gameData->mapDim.x < MIN_X || gameData->mapDim.x > MAX_X || ++xcount > 1) usage();
                break;
            case 'y':
                gameData->mapDim.y = atoi(optarg);
                if (gameData->mapDim.y < MIN_Y || gameData->mapDim.y > MAX_Y || ++ycount > 1) usage();
                break;
            case 'f':
                if (++fcount > 1) usage();                
                gameData->pathf = *(argv + optind - 1); 
                break;
            default:
                usage();
        }
    
    if (fcount == 0) // no save file given, assign $SNAKEFILE if it's set
    {        
        char* env = getenv("SNAKEFILE");
        
        if (env != NULL) // $SNAKEFILE set
        {
            gameData->pathf = env;          
        }
        else // saving not possible
        {
            printf("Warning: Save file not set. The game will not be saved!\n");
            gameData->pathf = NULL;
            gameData->saveFile = 0;
        }
    }    

    if (DEBUGARGS)
    {
        printf("[READARGS] x:%d y:%d saveFile:%s, ", gameData->mapDim.x, gameData->mapDim.y, gameData->pathf);
        printf("arguments: argc:%d optind:%d\n", argc, optind);
    }

    if (argc<=optind) // no [c:s] argument given
    {
        printf("Error: At least one snake must be declared in the form [c1:s1]\n");
        usage();
    }

    for(int i = optind; i < argc; i++) 
    {
        if (DEBUGARGS) printf("\n[READARGS] Processing snake argument %d : %s\n", i-optind+1, argv[i]);        
        processSnakeArgs(argv[i], gameData);
    }

    if (DEBUGARGS) printf("[END READARGS]\n");
    return;
}

// also will be used for user command "spawn"
void processSnakeArgs(char* datastring, gamedata_t* gameData)
{
    if (DEBUGARGS) printf("[PROCESSSNAKEARGS]\n");
    
    int count = 0;
    char* p = NULL;
    
    extendArrays(gameData); // extend foods and snakes arrays in gameData by one
    
    // allocate memory for new snake
    snake_t* newSnake = (snake_t*) calloc(1, sizeof(snake_t));

    if (datastring[strlen(datastring)-1] == ':') usage(); // incorrect format - argument ends with ':'
    
    p = strtok(datastring, ":");    

    while ( p != NULL)
    {
        if (++count > 2) usage();
        
        if (count == 1) 
        {
            if (strlen(p) > 1 || !isupper(p[0])) usage();  // check uppercase char            
            newSnake->c = *p;              
            
            if (DEBUGARGS) printf("[PROCESSSNAKEARGS] first argument: %s  in gameData: %c\n", p, newSnake->c);
        }
        
        if (count == 2) 
        {
            if ( (newSnake->s = strtol(p, NULL, 10)) == 0) usage(); // check integer
            if (newSnake->s < MIN_SPEED || newSnake->s > MAX_SPEED) usage(); // check speed value
            
            if (DEBUGARGS) printf("[PROCESSSNAKEARGS] second argument: %s in gameData: %d\n", p, newSnake->s);
        }
        
        p = strtok(NULL, ":");        
    }

    gameData->snakes[gameData->snakeCount++] = *newSnake;

    if (DEBUGARGS) printf("[END PROCESSSNAKEARGS]\n");
    return;
}
// will be used whenever a new snake is added
void extendArrays(gamedata_t* gameData)
{
    // extend snakes array by 1 to accomodate new snake
    snake_t* extendedSnakes;    
    
    if ( (extendedSnakes = (snake_t*) calloc(gameData->snakeCount + 1, sizeof(snake_t))) == NULL) ERR("calloc");
    memcpy(extendedSnakes, gameData->snakes, gameData->snakeCount * sizeof(snake_t));
    free(gameData->snakes); // free previous snakes array
    gameData->snakes = extendedSnakes;

    // extend foods array by 1 to accomodate new foods
    xy_t* extendedFoods;
    
    if ( (extendedFoods = (xy_t*) calloc(gameData->snakeCount + 1, sizeof(xy_t))) == NULL) ERR("calloc");
    memcpy(extendedFoods, gameData->foods, gameData->snakeCount * sizeof(xy_t));
    free(gameData->foods); // free previous foods array
    gameData->foods = extendedFoods;

    return;
}

void initSavedGame(gamedata_t* gameData) 
{
    // TODO
}

void initNewGame(gamedata_t* gameData) 
{
    createMap(gameData);

    // place initial food on the map
    for(int i = 0; i < gameData->snakeCount; i++)
        placeFood(gameData, i);      

    // spawn snakes on the map
    for(int i = 0; i < gameData->snakeCount; i++)
        spawnSnake(gameData, i);   /// new thread should execute this
}
// helper function for snakeThread()
int getSnakeNo(gamedata_t* gameData)
{    
    pthread_t tid = pthread_self();
    for (int i = 0; i < gameData->snakeCount ; i++)
        if (pthread_equal(tid, gameData->snakes[i].tid)) return i;
    return -1;
}

xy_t selectTarget(gamedata_t* gameData, int snakeNo)
{
    return gameData->foods[rand_r(&gameData->snakes[snakeNo].seed) % gameData->snakeCount];    
}

int getEmptyTiles(gamedata_t* gameData, int snakeNo)
{
    int emptyTiles = 0;
    xy_t snakePos = gameData->snakes[snakeNo].head->pos;
    
    if (snakePos.y > 0 && gameData->map[snakePos.y - 1][snakePos.x] == ' ') emptyTiles += 1;
    if (snakePos.x < gameData->mapDim.x - 1  && gameData->map[snakePos.y][snakePos.x + 1] == ' ') emptyTiles += 2;
    if (snakePos.y < gameData->mapDim.y - 1 && gameData->map[snakePos.y + 1][snakePos.x] == ' ') emptyTiles += 4;
    if (snakePos.x > 0 && gameData->map[snakePos.y][snakePos.x - 1] == ' ') emptyTiles += 8;
    
    if (DEBUGMOVESNAKE) printf("[GETEMPTYTILES] EmptyTiles: %d\n", emptyTiles);

    return emptyTiles;
}

int getFoodDirection(gamedata_t* gameData, int snakeNo)
{
    int foodDirection = 0;
    xy_t snakePos = gameData->snakes[snakeNo].head->pos;
    xy_t foodPos = gameData->snakes[snakeNo].target;
    
    if (foodPos.y - snakePos.y < 0) foodDirection += 1;
    if (foodPos.x - snakePos.x > 0) foodDirection += 2;
    if (foodPos.y - snakePos.y > 0) foodDirection += 4;
    if (foodPos.x - snakePos.x < 0) foodDirection += 8;

    return foodDirection;
}

void selectDirection(gamedata_t* gameData, int snakeNo)
{
    int direction = 0;
    int emptyTiles = getEmptyTiles(gameData, snakeNo);
    int foodDirection = getFoodDirection(gameData, snakeNo);
    xy_t snakePos = gameData->snakes[snakeNo].head->pos;
    xy_t foodPos = gameData->snakes[snakeNo].target;
    
    
    
    if ( (abs(snakePos.x - foodPos.x) == 1 && (snakePos.y == foodPos.y)) || 
         (abs(snakePos.y - foodPos.y) == 1 && (snakePos.x == foodPos.x)) )  // snake is next to the food
    {
        direction = foodDirection;
    }
    else if (emptyTiles == 0) // TRAPPED - GAME OVER
    {
        direction = 0; // TODO: END GAME
    }
    else if ( (emptyTiles & foodDirection) == 0 ) // foodDirection is blocked go another direction
    {
        while( (emptyTiles & direction) == 0 )
        {
            direction = (int)pow(2, rand_r(&gameData->snakes[snakeNo].seed) % 4);
        }
    }
    else                                    // foodDirection is available go towards food
    {
        while( (emptyTiles & foodDirection & direction) == 0 ) 
        {
            direction = (int)pow(2, rand_r(&gameData->snakes[snakeNo].seed) % 4);
        }
    }

    
    
    if (DEBUGMOVESNAKE) printf("[SELECTDIRECTION] Direction: %d\n", direction);
    gameData->snakes[snakeNo].direction = direction;
}

int getFoodNo(gamedata_t* gameData, int snakeNo) // returns foodNo of the food targeted by snakeNo
{
    xy_t target = gameData->snakes[snakeNo].target;
    for (int i = 0; i < gameData->snakeCount; i++)
        if (target.x == gameData->foods[i].x && target.y == gameData->foods[i].y) return i;

    return -1;
}

void moveSnake(gamedata_t* gameData, int snakeNo)
{
    xy_t target = gameData->snakes[snakeNo].target;
    if (DEBUGMOVESNAKE) printf("[MOVESNAKE] Target: (%d, %d)\n", target.x, target.y);
    int l = gameData->snakes[snakeNo].l;
    bool grow_flag = gameData->snakes[snakeNo].grow_flag;
    segment* oldTail = gameData->snakes[snakeNo].tail;
    
    //Add new head
    segment* newHead = (segment*) calloc(1, sizeof(segment));
    segment* oldHead = gameData->snakes[snakeNo].head;
    newHead->pos.x = oldHead->pos.x;
    newHead->pos.y = oldHead->pos.y;
    newHead->previous = NULL;
    newHead->next = oldHead;
    oldHead->previous = newHead;
    gameData->snakes[snakeNo].head = newHead;
    
    // new head position (MUTEX)
    int d = gameData->snakes[snakeNo].direction;    
    switch(d)
    {
            case 1: // up
                gameData->snakes[snakeNo].head->pos.y--;
                break;
            case 2: // right
                gameData->snakes[snakeNo].head->pos.x++;
                break;
            case 4: // down
                gameData->snakes[snakeNo].head->pos.y++;
                break;
            case 8: // left
                gameData->snakes[snakeNo].head->pos.x--;
                break;
    }
    
    // print new head position
    segment* head = gameData->snakes[snakeNo].head;
    gameData->map[head->pos.y][head->pos.x] = gameData->snakes[snakeNo].c;        
    
    if (grow_flag)  // grow the snake (and keep the oldTail)
    {
        gameData->map[head->next->pos.y][head->next->pos.x] = tolower(gameData->snakes[snakeNo].c);
        gameData->snakes[snakeNo].l++;
    }
    else            // just move the snake (remove the oldTail)
    {
        if (l == 1)  // if snake length is 1
        {
            gameData->snakes[snakeNo].tail = gameData->snakes[snakeNo].head;            
        }
        else        // if snake length is longer than 1
        {
            gameData->snakes[snakeNo].tail = oldTail->previous;
            gameData->snakes[snakeNo].tail->next = NULL;
            gameData->map[head->next->pos.y][head->next->pos.x] = tolower(gameData->snakes[snakeNo].c);
        }

        gameData->map[oldTail->pos.y][oldTail->pos.x] = ' ';
        free(oldTail);
    }
    
    // check if food is eaten
    gameData->snakes[snakeNo].grow_flag = 0;
    if ( gameData->snakes[snakeNo].head->pos.x == gameData->snakes[snakeNo].target.x && gameData->snakes[snakeNo].head->pos.y == gameData->snakes[snakeNo].target.y )
    {        
        gameData->snakes[snakeNo].grow_flag = true;
        int foodNo = getFoodNo(gameData, snakeNo);
        placeFood(gameData, foodNo);
        
        // TODO: signal to all snakes that food is eaten so they choose new target if necessary!
        gameData->snakes[snakeNo].target = selectTarget(gameData, snakeNo);
    }

    //msleep(500);

}

void checkFood(gamedata_t* gameData, int snakeNo) // if targeted food is gone select new target
{
    xy_t target = gameData->snakes[snakeNo].target;
    if (gameData->map[target.y][target.x] != 'o') gameData->snakes[snakeNo].target = selectTarget(gameData, snakeNo);
}

void* snakeThread(void* voidData)
{
    msleep(50);
    
    gamedata_t* gameData = voidData;
    int snakeNo;
    if ( (snakeNo = getSnakeNo(gameData)) == -1) ERR("getSnakeNo()");

    if (DEBUGSNAKEWORK) printf("Created snake thread for snake #%d with TID = %ld\n", snakeNo, gameData->snakes[snakeNo].tid); 
    
    // select food to target
    gameData->snakes[snakeNo].target = selectTarget(gameData, snakeNo);
    if (DEBUGSNAKEWORK) printf("Selected food at (%d, %d) as target\n", gameData->snakes[snakeNo].target.x, gameData->snakes[snakeNo].target.y); 
    
    // select direction & move snake
    while(1)
    {
        msleep(DELAY);
        pthread_mutex_lock(gameData->pmxMap);
        checkFood(gameData, snakeNo);
        selectDirection(gameData, snakeNo);
        moveSnake(gameData, snakeNo);
        pthread_mutex_unlock(gameData->pmxMap);
    }
    
    return NULL;
}

void spawnSnake(gamedata_t* gameData, int snakeNo)
{
    if (DEBUGSPAWNSNAKE) printf("[SPAWNSNAKE]\n");
    
    segment* head = (segment*) calloc(1, sizeof(segment));
    head->next = head->previous = NULL;
    gameData->snakes[snakeNo].head = head;
    
    int r, c, placed = 0;
    
    while (!placed)
    {
        r = (rand_r(&gameData->snakes[snakeNo].seed) % gameData->mapDim.y);
        c = (rand_r(&gameData->snakes[snakeNo].seed) % gameData->mapDim.x);

        // lock map when checking & updating
        pthread_mutex_lock(gameData->pmxMap);
        if (gameData->map[r][c] == ' ')
        {
            gameData->map[r][c] = gameData->snakes[snakeNo].c;            
            placed = 1;
        }
        pthread_mutex_unlock(gameData->pmxMap);
    }

    gameData->snakes[snakeNo].head->pos.x = c;
    gameData->snakes[snakeNo].head->pos.y = r;
    gameData->snakes[snakeNo].tail = gameData->snakes[snakeNo].head;
    gameData->snakes[snakeNo].l = 1;
    
    if (pthread_create(&gameData->snakes[snakeNo].tid, NULL, snakeThread, gameData)) ERR("pthread_create");
    
    if (DEBUGSPAWNSNAKE) printf("Snake #%d TID = %ld spawned.\n", snakeNo, gameData->snakes[snakeNo].tid);

    if (DEBUGSPAWNSNAKE) printf("[END SPAWNSNAKE]\n");
    return;    
}

void printMap(gamedata_t* gameData, int fd) // writes the map to the file descriptor fd ( fd = 1 for stdout)
{
    // create top and bottom border
    printf("\n");
    char border[gameData->mapDim.x + 3];
    border[0] = border[gameData->mapDim.x + 1] = 'X';
    for (int i = 0; i < gameData->mapDim.x; i++)
        border[i+1] = '-';
    border[gameData->mapDim.x + 2] = '\n';
    
    // print map
    write(fd, border, sizeof(border));
    for(int r = 0; r < gameData->mapDim.y; r++)
    {
        write(1, "|", 1);
        for(int c = 0; c < gameData->mapDim.x; c++)
            write(fd, &gameData->map[r][c], sizeof(char));
        write(1, "|\n", 2);
    }
    write(fd, border, sizeof(border));
}

void placeFood(gamedata_t* gameData, int foodNo)
{
    if (DEBUGPLACEFOOD) printf("[PLACEFOOD]\n");
    
    int r, c, placed = 0;   
    
    // xy_t* newFood = (xy_t*) calloc(1, sizeof(xy_t));
    // gameData->foods[foodNo] = *newFood;
    
    while (!placed)
    {
        r = (rand_r(&gameData->snakes[foodNo].seed) % gameData->mapDim.y);
        c = (rand_r(&gameData->snakes[foodNo].seed) % gameData->mapDim.x);

        if (gameData->map[r][c] == ' ')
        {
            gameData->map[r][c] = 'o';
            placed = 1;
        }
    }

    gameData->foods[foodNo].x = c;
    gameData->foods[foodNo].y = r;    

    if (DEBUGPLACEFOOD) printf("[END PLACEFOOD]\n");
    return;
}

void createMap(gamedata_t* gameData)
{
    if (DEBUGMAP) printf("[CREATEMAP]\n");

    char** map; 

    // allocate memory for map
    if ( (map = (char**) calloc(gameData->mapDim.y, sizeof(char*))) == NULL ) ERR("calloc()");
    for (int r = 0 ; r < gameData->mapDim.y ; r++)
    {
        map[r] = (char*) calloc(gameData->mapDim.x, sizeof(char));
        if ( map[r] == NULL ) ERR("calloc()"); 
    }   

    // initialize map with space characters
    for(int r = 0; r < gameData->mapDim.y; r++)
        for(int c = 0; c < gameData->mapDim.x; c++)
            map[r][c] = ' ';
    
    gameData->map = map;      

    return;
}

void initialization(int argc, char** argv, gamedata_t* gameData)
{
    if (DEBUGINIT) printf("[INITIALIZATION]\n");

    /*
    
    // initialize signal mask
    sigset_t* mask;
    if ( (mask = (sigset_t*) malloc(sizeof(sigset_t))) == NULL ) ERR ("malloc"); // free'd in exit_sequence()
    sigemptyset(mask);
    sigaddset(mask, SIGQUIT);  // stop real time map display
    sigaddset(mask, SIGUSR1);  // "user initiated index" signal for indexer thread
                               // "start-up indexing completed" signal for main thread
    sigaddset(mask, SIGUSR2);  // "exit" signal for indexer thread
    sigaddset(mask, SIGALRM);  // "periodic index" signal for indexer thread
    sigaddset(mask, SIGPIPE);  // to ignore the EPIPE error in pclose()
    pthread_sigmask(SIG_BLOCK, mask, NULL);
    */
    
    
    // initialize gameData 
    gameData->saveFile = 1;
    gameData->mapDim.x = DEFAULT_X;
    gameData->mapDim.y = DEFAULT_Y;
    gameData->snakeCount = 0;    
    gameData->foods = NULL;    
    gameData->snakes = NULL;
    
    //gameData->pMask = mask;
    int saveExists = 0;

    // initialize command line arguments
    readArgs(argc, argv, gameData);

    // initialize random seeds for snake threads
    for(int i = 0; i < gameData->snakeCount; i++)
        gameData->snakes[i].seed = rand();

    // check if an old save file exists // DO I NEED TO DO IT HERE OR IN CREATE MAP? 
    if (gameData->saveFile)
    {        
        struct stat* saveStat;
        if ( (saveStat = (struct stat*) malloc(sizeof(struct stat))) == NULL ) ERR ("malloc");
        saveExists = 1 + lstat(gameData->pathf, saveStat);
        free(saveStat);
    }

    if (DEBUGINIT) printf("[INITIALIZATION] Save file exists: %d\n", saveExists);
    
    if (saveExists) initSavedGame(gameData); 
    else initNewGame(gameData);

    printf("\nStarting **tsnake**.\n");
}

int main(int argc, char** argv)
{	
    srand(time(NULL));
    struct gamedata_t gameData;

    // initialize mutex
    pthread_mutex_t mxUpdateMap;
    if (pthread_mutex_init(&mxUpdateMap, NULL)) ERR("Couldn't initialize mutex!");
    gameData.pmxMap = &mxUpdateMap;
    
    
    
    // INITIALIZATION
    initialization(argc, argv, &gameData);

    if (DEBUGMAIN) 
        for(int i = 0; i < gameData.snakeCount; i++)
            printf("[MAIN] Snake no: %d char: %c speed: %d\n", i+1, gameData.snakes[i].c, gameData.snakes[i].s);
    

    while(1) 
    {
        system("clear");
        printMap(&gameData, 1);
        msleep(DELAY);
    }

    
    /*
    // STARTUP INDEXING
    startupIndexing(&threadArgs);
    
    // if a prevous index file did not exist, wait for it to be created
    // wait for confirmation from indexer thread via SIGUSR1
    if (threadArgs.newIndex != 0) 
        while (sigNo != SIGUSR1) 
            sigwait(threadArgs.pMask, &sigNo);
    
    // at this point index file exists and if periodic
    // indexing is set indexer thread is waiting for signals
    // and carrying out periodic indexing as required
    
    // USER INPUT
    getUserInput(&threadArgs); // start accepting user commands
    
    // EXIT SEQUENCE
    exitSequence(&threadArgs);
    */

    
    msleep(200);
    return EXIT_SUCCESS;
}

