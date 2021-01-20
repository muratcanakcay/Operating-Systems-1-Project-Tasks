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
#define DEBUGMAIN 0
#define DEBUGARGS 1
#define DEBUGTHREAD 0
#define DEBUGINDEXING 0
#define DEBUGWRITEFILE 0
#define DEBUGQUICKEXIT 0
#define DEBUGSIMULATION 0

#define MAX_X 100
#define MIN_X 10
#define MAX_Y 100
#define MIN_Y 10
#define DEFAULT_X 20
#define DEFAULT_Y 20


#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

int tempfile; // global file descriptor for temp file

typedef struct gamedata_t
{
    int x;
    int y; 
    char* pathf;      
    unsigned short saveFile; //0: save file not given, 1: save file given
    
    bool exitFlag;
    
} gamedata_t;

void usage()  // TODO correct help textx
{
    fprintf(stderr,"\nUSAGE : tsnake [-x xdim=20] [-y ydim=10] [-f file=$SNAKEFILE] c1:s1 [c2:s2 ...]\n\n");
    fprintf(stderr,"pathd : the path to a directory that will be traversed, if the option is not present a path set in an environment variable $MOLE_DIR is used. If the environment variable is not set the program end with an error.\n\n");
    fprintf(stderr,"pathf : a path to a file where index is stored. If the option is not present, the value from environment variable $MOLE_INDEX_PATH is used. If the variable is not set, the default value of file `.mole-index` in user's home directory is used\n\n");
    fprintf(stderr,"n : is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index. This parameter is optional. If it is not present, the periodic re-indexing is disabled\n\n");
    exit(EXIT_FAILURE); 
}

void readArgs(int argc, char** argv, gamedata_t* data)
{
	int c, xcount = 0, ycount = 0, fcount = 0;
    data->saveFile = 1;

    while ((c = getopt(argc, argv, "x:y:f:")) != -1)
        switch (c)
        {
            case 'x':
                data->x = atoi(optarg);
                if (data->x < MIN_X || data->x > MAX_X || ++xcount > 1) usage();
                break;
            case 'y':
                data->y = atoi(optarg);
                if (data->y < MIN_Y || data->y > MAX_Y || ++ycount > 1) usage();
                break;
            case 'f':
                if (++fcount > 1) usage();                
                data->pathf = *(argv + optind - 1); 
                break;
            default:
                usage();
        }
    
    if (fcount == 0) // assign $SNAKEFILE if it's set
    {        
        char* env = getenv("SNAKEFILE");
        
        if (env != NULL) // $SNAKEFILE set
        {
            data->pathf = env;          
        }
        else // no save file given
        {
            data->pathf = NULL;
            data->saveFile = 0;
        }
    }    

    if (xcount == 0) // use default x dimension 
    {
        data->x = DEFAULT_X;
    }

    if (ycount == 0) // use default y dimension
    {
        data->y = DEFAULT_Y;
    }


    // process c1:s1 ... 

    if(DEBUGARGS)
    {
        printf("x:%d y:%d saveFile:%s, ", data->x, data->y, data->pathf);

    }


    if (argc>optind) usage();
}

void initialization(int argc, char** argv, gamedata_t* gamedata)
{
    // initialize command line arguments
    readArgs(argc, argv, gamedata);
    
    /*
    
    // check if an old save file exists
    int saveStatus = -1;
    struct stat* saveStat;
    if ( (saveStat = (struct stat*) malloc(sizeof(struct stat))) == NULL ) ERR ("malloc"); // free'd in exit_sequence()
    saveStatus = lstat(data->pathf, saveStat);

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


// void startupIndexing(thread_t* threadArgs)
// {
//     if (threadArgs->newIndex == 0 && threadArgs->t == 0) // startup indexing IS NOT necessary
//     {
//         printf("--Index file \"%s\" exists.\n--Periodic indexing disabled.\n", threadArgs->pathf);
//     }
//     else if (threadArgs->newIndex == 0 || errno == ENOENT) // startup indexing IS necessary: create indexer thread
//     {
//         // display informative messages for user
//         if (threadArgs->newIndex == 0) printf("--Index file \"%s\" exists.\n", threadArgs->pathf);
//         else printf("--Index file \"%s\" does not exist. \n", threadArgs->pathf);
//         if (threadArgs->t == 0) printf("--Periodic indexing disabled.\n");
//         else printf("--Periodic indexing enabled. t = %d seconds\n", threadArgs->t);
    
//         // create thread
//         if (pthread_create(&threadArgs->tid, NULL, threadWork, threadArgs)) ERR("pthread_create");
        
//         if (DEBUGMAIN) printf("[main] Thread with TID: %lu started.\n", (unsigned long)threadArgs->tid);
//     }
// }
// void getUserInput(thread_t* threadArgs)
// {    
//     // wait for user input until exited
//     while(true) 
//     {
//         char buf[256] = {'\0'};

//         printf("> Enter command (\"help\" for list of commands): \n");
//         fflush(stdin);
        
//         if ( fgets(buf, 255, stdin) == NULL ) ERR("fgets");
    
//         if (memcmp(buf, "exit\n", 5) == 0)
//         {
//             threadArgs->exitFlag = 1;
//             // if there's an active thread send signal for termination
//             if (threadArgs->tid > 0) pthread_kill(threadArgs->tid, SIGUSR2);
//             break;
//         }
//         else if (memcmp(buf, "exit!\n", 6) == 0)
//         {
//             // if there's an active thread cancel it (and trigger cleanup)
//             if (threadArgs->tid > 0) pthread_cancel(threadArgs->tid);
//             break;
//         }
//         else if (memcmp(buf, "index\n", 6) == 0)
//         {
//             u_index(threadArgs);
//         }
//         else if (memcmp(buf, "count\n", 6) == 0)
//         {
//             u_count(threadArgs->pathf);
//         }
//         else if (memcmp(buf, "listall\n", 8) == 0)
//         {
//             listRecords(threadArgs->pathf, NULL, -1);            
//         }
//         else if (memcmp(buf, "largerthan ", 11) == 0)
//         {
//             u_largerthan(threadArgs->pathf, buf);
//         }
//         else if (memcmp(buf, "namepart ", 9) == 0)
//         {
//             u_namepart(threadArgs->pathf, buf);
//         }
//         else if (memcmp(buf, "owner ", 6) == 0)
//         {
//             u_owner(threadArgs->pathf, buf);
//         }
//         else if (memcmp(buf, "help\n", 5) == 0)
//         {
//             displayHelp();
//         }
//         else printf("--Invalid command or arguments missing.\n");
//     }
// }

// void exitSequence(thread_t* threadArgs)
// {
//     free(threadArgs->pMask);
//     free(threadArgs->pmxIndexer);
//     free(threadArgs->pIndexStat);
//     free(threadArgs->tempBuffer);

//     if (DEBUGMAIN && threadArgs->tid) printf("[main] Waiting to join with indexing thread.\n");
    
//     // if there was an active thread check for its termination
//     if (threadArgs->tid && pthread_join(threadArgs->tid, NULL)) ERR("Can't join with indexer thread");
//     else if (DEBUGMAIN && threadArgs->tid != 0) printf("[main] Joined with indexer thread.\n");
//     else if (DEBUGMAIN && threadArgs->tid == 0) printf("[main] There's no indexer thread to join.\n");

//     printf("Ending **mole**.\n");
// }

// typedef struct thread_t
// {
//     pthread_t tid;
//     pthread_t mtid;         // main thread's tid
//     char* tempBuffer;
//     char* pathd;
//     char* pathf;
//     int t;
//     unsigned short newIndex; //0:old index file exists, 1:does not exist new needed, 2:indexing initiated by user
//     bool exitFlag;
//     struct stat* pIndexStat;
//     sigset_t* pMask;
//     pthread_mutex_t* pmxIndexer;
// } thread_t;




int main(int argc, char** argv)
{	
    
    struct gamedata_t gamedata;    

    // INITIALIZATION
    initialization(argc, argv, &gamedata);

    
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

