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

// used to turn the debug messages on/off
#define HERE printf("********************HERE******************\n");
#define DEBUGMAIN 1
#define DEBUGINIT 1
#define DEBUGMAP 1
#define DEBUGARGS 1
#define DEBUGPLACEFOOD 1
#define DEBUGSPAWNSNAKE 1
#define MAPTEST 0

#define MAX_X 100
#define MIN_X 10
#define MAX_Y 100
#define MIN_Y 10
#define MIN_SPEED 50
#define MAX_SPEED 1000
#define DEFAULT_X 20
#define DEFAULT_Y 20

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

enum direction {right = 3, down = 6, left = 9, up = 12};
typedef unsigned int UINT;

typedef struct xy_t
{
    int x;
    int y;
} xy_t;

typedef struct segment          // double or single linked list?
{
    xy_t pos;
    struct segment* next;
    struct segment* previous;   // useless?
    enum direction direction;   // useless?    
} segment;

typedef struct snake_t
{
    pthread_t tid;
    UINT seed;
    char c;
    int s;
    xy_t target;                // targeted food tile
    segment* head;
    segment* tail;
} snake_t;

typedef struct gamedata_t
{
    xy_t mapDim;                // map dimensions
    char** map;                 // 2x2 map array            // MUST BE FREED IN EXIT SEQ!!!
    int snakeCount;             // number of snakes
    snake_t* snakes;            // data for each snake  -   // MUST BE FREED IN EXIT SEQ!!!
    xy_t* foods;                // pos. of each food    -   // MUST BE FREED IN EXIT SEQ!!!
    char* pathf;                // save file path
    unsigned short saveFile;    // 0: save file not given, 1: save file given
} gamedata_t;

//function declarations
void processSnakeArgs(char* snake, gamedata_t* gamedata);
void createMap(gamedata_t* gamedata);
void spawnSnake(gamedata_t* gamedata, int snakeNo);
void printMap(gamedata_t* gamedata, int fd);
void placeFood(gamedata_t* gamedata, int foodNo, UINT seed);
void extendArrays(gamedata_t* gamedata);


//-----------

void usage(){
    fprintf(stderr,"\nUSAGE : tsnake [-x xdim=%d] [-y ydim=%d] [-f file=$SNAKEFILE] c1:s1 [c2:s2 ...]\n\n", DEFAULT_X, DEFAULT_Y);
    fprintf(stderr,"x : x dimension of the map. - Default value is %d\n\n", DEFAULT_X);
    fprintf(stderr,"y : y dimension of the map. - Default value is %d\n\n", DEFAULT_X);
    fprintf(stderr,"file : a path to a save file. If the option is not present, the value from environment variable $SNAKEFILE is used. If the variable is not set, it will not be possible to save the game state.\n\n");
    fprintf(stderr,"c1:s1 : c1 is the first snake's character (must be uppercase and unique for each snake), s1 is the first snake's speed in milliseconds.");
    fprintf(stderr," (must be between %d and %d) - At least one snake must be declared.\n\n", MIN_SPEED, MAX_SPEED);
    exit(EXIT_FAILURE);}

void readArgs(int argc, char** argv, gamedata_t* gamedata)
{
	if (DEBUGARGS) printf("[READARGS]\n");
    
    int c, xcount = 0, ycount = 0, fcount = 0;
    gamedata->saveFile = 1;
    gamedata->mapDim.x = DEFAULT_X;
    gamedata->mapDim.y = DEFAULT_Y;
    gamedata->snakeCount = 0;
    xy_t* foods = NULL;
    gamedata->foods = foods;
    snake_t* snakes = NULL;
    gamedata->snakes = snakes;
    
    while ((c = getopt(argc, argv, "x:y:f:")) != -1)
        switch (c)
        {
            case 'x':
                gamedata->mapDim.x = atoi(optarg);
                if (gamedata->mapDim.x < MIN_X || gamedata->mapDim.x > MAX_X || ++xcount > 1) usage();
                break;
            case 'y':
                gamedata->mapDim.y = atoi(optarg);
                if (gamedata->mapDim.y < MIN_Y || gamedata->mapDim.y > MAX_Y || ++ycount > 1) usage();
                break;
            case 'f':
                if (++fcount > 1) usage();                
                gamedata->pathf = *(argv + optind - 1); 
                break;
            default:
                usage();
        }
    
    if (fcount == 0) // no save file given, assign $SNAKEFILE if it's set
    {        
        char* env = getenv("SNAKEFILE");
        
        if (env != NULL) // $SNAKEFILE set
        {
            gamedata->pathf = env;          
        }
        else // saving not possible
        {
            printf("Warning: Save file not set. The game will not be saved!\n");
            gamedata->pathf = NULL;
            gamedata->saveFile = 0;
        }
    }    

    if (DEBUGARGS)
    {
        printf("[READARGS] x:%d y:%d saveFile:%s, ", gamedata->mapDim.x, gamedata->mapDim.y, gamedata->pathf);
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
        processSnakeArgs(argv[i], gamedata);
    }

    if (DEBUGARGS) printf("[END READARGS]\n");
    return;
}


void processSnakeArgs(char* datastring, gamedata_t* gamedata)
{
    if (DEBUGARGS) printf("[PROCESSSNAKEARGS]\n");
    
    int count = 0;
    char* p = NULL;
    
    extendArrays(gamedata); // extend foods and snakes arrays in gamedata by one
    
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
            
            if (DEBUGARGS) printf("[PROCESSSNAKEARGS] first argument: %s  in gamedata: %c\n", p, newSnake->c);
        }
        
        if (count == 2) 
        {
            if ( (newSnake->s = strtol(p, NULL, 10)) == 0) usage(); // check integer
            if (newSnake->s < MIN_SPEED || newSnake->s > MAX_SPEED) usage(); // check speed value
            
            if (DEBUGARGS) printf("[PROCESSSNAKEARGS] second argument: %s in gamedata: %d\n", p, newSnake->s);
        }
        
        p = strtok(NULL, ":");        
    }

    gamedata->snakes[gamedata->snakeCount++] = *newSnake;

    if (DEBUGARGS) printf("[END PROCESSSNAKEARGS]\n");
    return;
}


void extendArrays(gamedata_t* gamedata)
{
    // extend snakes array by 1 to accomodate new snake
    snake_t* extendedSnakes;    
    
    if ( (extendedSnakes = (snake_t*) calloc(gamedata->snakeCount + 1, sizeof(snake_t))) == NULL) ERR("calloc");
    memcpy(extendedSnakes, gamedata->snakes, gamedata->snakeCount * sizeof(snake_t));
    free(gamedata->snakes); // free previous snakes array
    gamedata->snakes = extendedSnakes;

    // extend foods array by 1 to accomodate new foods
    xy_t* extendedFoods;
    
    if ( (extendedFoods = (xy_t*) calloc(gamedata->snakeCount + 1, sizeof(xy_t))) == NULL) ERR("calloc");
    memcpy(extendedFoods, gamedata->foods, gamedata->snakeCount * sizeof(xy_t));
    free(gamedata->foods); // free previous foods array
    gamedata->foods = extendedFoods;

    return;
}



void initSavedGame(gamedata_t* gamedata) 
{
    // TODO
}

void initNewGame(gamedata_t* gamedata) 
{
    createMap(gamedata);

    // place initial food on the map
    for(int i = 0; i < gamedata->snakeCount; i++)
        placeFood(gamedata, i, rand());      


    for(int i = 0; i < gamedata->snakeCount; i++)
        spawnSnake(gamedata, i);   /// new thread should execute this
}

void spawnSnake(gamedata_t* gamedata, int snakeNo)
{
    if (DEBUGSPAWNSNAKE) printf("[SPAWNSNAKE]\n");
    
    segment* head = (segment*) calloc(1, sizeof(segment));
    head->next = head->previous = NULL;
    
    int placed = 0;   
    
    while (!placed)
    {
        int r = (rand_r(&gamedata->snakes[snakeNo].seed) % gamedata->mapDim.y);
        int c = (rand_r(&gamedata->snakes[snakeNo].seed) % gamedata->mapDim.x);

        if (gamedata->map[r][c] == ' ')
        {
            (gamedata->map[r][c] = gamedata->snakes[snakeNo].c) && (placed = 1);
            head->pos.x = c;
            head->pos.y = r;
        }        
    }

    printMap(gamedata, 1);
    
    if (DEBUGSPAWNSNAKE) printf("[END SPAWNSNAKE]\n");
    return;    
}

void printMap(gamedata_t* gamedata, int fd) // writes the map to the file descriptor fd ( fd = 1 for stdout)
{
    // create top and bottom border
    printf("\n");
    char border[gamedata->mapDim.x + 3];
    border[0] = border[gamedata->mapDim.x + 1] = 'X';
    for (int i = 0; i < gamedata->mapDim.x; i++)
        border[i+1] = '-';
    border[gamedata->mapDim.x + 2] = '\n';
    
    // print map
    write(fd, border, sizeof(border));
    for(int r = 0; r < gamedata->mapDim.y; r++)
    {
        write(1, "|", 1);
        for(int c = 0; c < gamedata->mapDim.x; c++)
            write(fd, &gamedata->map[r][c], sizeof(char));
        write(1, "|\n", 2);
    }
    write(fd, border, sizeof(border));
}

void placeFood(gamedata_t* gamedata, int foodNo, UINT seed)
{
    if (DEBUGPLACEFOOD) printf("[PLACEFOOD]\n");
    
    int placed = 0;   
    xy_t* newFood = (xy_t*) calloc(1, sizeof(xy_t));
    
    while (!placed)
    {
        int r = (rand_r(&seed) % gamedata->mapDim.y);
        int c = (rand_r(&seed) % gamedata->mapDim.x);

        if (gamedata->map[r][c] == ' ')
        {
            (gamedata->map[r][c] = 'o') && (placed = 1);
            newFood->x = c;
            newFood->y = r;
            gamedata->foods[foodNo] = *newFood;
        }
    }

    if (DEBUGPLACEFOOD) printf("[END PLACEFOOD]\n");
    return;
}

void createMap(gamedata_t* gamedata)
{
    if (DEBUGMAP) printf("[CREATEMAP]\n");

    char** map; 

    // allocate memory for map
    if ( (map = (char**) calloc(gamedata->mapDim.y, sizeof(char*))) == NULL ) ERR("calloc()");
    for (int r = 0 ; r < gamedata->mapDim.y ; r++)
    {
        map[r] = (char*) calloc(gamedata->mapDim.x, sizeof(char));
        if ( map[r] == NULL ) ERR("calloc()"); 
    }   

    // initialize map with space characters
    for(int r = 0; r < gamedata->mapDim.y; r++)
        for(int c = 0; c < gamedata->mapDim.x; c++)
            map[r][c] = ' ';
    
    gamedata->map = map;      

    return;
}


void initialization(int argc, char** argv, gamedata_t* gamedata)
{
    if (DEBUGINIT) printf("[INITIALIZATION]\n");

    int saveExists = 0;

    // initialize command line arguments
    readArgs(argc, argv, gamedata);

    // initialize random seeds for snake threads
    for(int i = 0; i < gamedata->snakeCount; i++)
        gamedata->snakes[i].seed = rand();
    
    // check if an old save file exists // DO I NEED TO DO IT HERE OR IN CREATE MAP? 
    if (gamedata->saveFile)
    {        
        struct stat* saveStat;
        if ( (saveStat = (struct stat*) malloc(sizeof(struct stat))) == NULL ) ERR ("malloc");
        saveExists = 1 + lstat(gamedata->pathf, saveStat);
        free(saveStat);
    }

    if (DEBUGINIT) printf("[INITIALIZATION] Save file exists: %d\n", saveExists);
    
    if (saveExists) initSavedGame(gamedata); 
    else initNewGame(gamedata);

    /*
    
    // initialize signal mask
    sigset_t* mask;
    if ( (mask = (sigset_t*) malloc(sizeof(sigset_t))) == NULL ) ERR ("malloc"); // free'd in exit_sequence()
    sigemptyset(mask);
    sigaddset(mask, SIGUSR1);  // "user initiated index" signal for indexer thread
                               // "start-up indexing completed" signal for main thread
    sigaddset(mask, SIGUSR2);  // "exit" signal for indexer thread
    sigaddset(mask, SIGALRM);  // "periodic index" signal for indexer thread
    sigaddset(mask, SIGPIPE);  // to ignore the EPIPE error in pclose()
    pthread_sigmask(SIG_BLOCK, mask, NULL);

    // initialize mutex
    pthread_mutex_t* mxIndexer;
    mxIndexer = (pthread_mutex_t*) malloc(sizeof(pthread_mutex_t)); // free'd in exit_sequence()
    if (pthread_mutex_init(mxIndexer, NULL)) ERR("Couldn't initialize mutex!");

    // initialize thread_t struct to pass to the thread and other functions    
    
    threadArgs->tid = 0;    
    threadArgs->mtid = pthread_self();
    threadArgs->pathd = pathd;
    threadArgs->pathf = pathf;
    threadArgs->t = t;
    threadArgs->newIndex = indexStatus ? 1 : 0;
    threadArgs->pIndexStat = indexStat;
    threadArgs->pMask = mask;
    threadArgs->pmxIndexer = mxIndexer;
    threadArgs->exitFlag = 0;
    
    */

    printf("\nStarting **tsnake**.\n");
}

int main(int argc, char** argv)
{	
    srand(time(NULL));
    struct gamedata_t gamedata;

    // INITIALIZATION
    initialization(argc, argv, &gamedata);

    if (DEBUGMAIN) 
        for(int i = 0; i < gamedata.snakeCount; i++)
            printf("[MAIN] Snake no: %d char: %c speed: %d\n", i+1, gamedata.snakes[i].c, gamedata.snakes[i].s);
    
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

    return EXIT_SUCCESS;
}

