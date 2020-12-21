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

#define DEBUGMAIN 0
#define DEBUGTHREAD 0
#define DEBUGINDEXING 0
#define DEBUGWRITEFILE 0
#define DEBUGQUICKEXIT 0
#define DEBUGSIMULATION 0

#define MAX_PATH 1024
#define MAX_FILE 256
#define MAXFD 100

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

int tempfile; // global file descriptor for temp file

enum ftype {dir, jpeg, png, gzip, zip, other, error};

typedef struct finfo_t
{
    char name[MAX_FILE]; // filename
    char path[MAX_PATH]; //absolute path
    off_t size; // file size in bytes
    uid_t uid;  // owner's uid
    enum ftype type; // file type
} finfo_t;
typedef struct thread_t
{
    pthread_t tid;
    pthread_t mtid; // main thread's tid
    char* tempBuffer;
    char* pathd;
    char* pathf;
    int t;
    unsigned short newIndex; //0:old index file exists, 1:does not exist new needed, 2:indexing initiated by user
    bool exitFlag;
    struct stat* pIndexStat;
    sigset_t* pMask;
    pthread_mutex_t* pmxIndexer;
} thread_t;

// function declarations
void displayHelp();
void usage();
void readArgs(int argc, char** argv, char** pathd, char** pathf, int* t);
char* typeToText(int type); // returns type based on enum
enum ftype getType(const char* fname); // returns file type based on signature
bool isSubstring(const char* sub, const char* str); // checks if sub is a substring of str
void quickexit(void* tempfile);  // cleanup function for thread during quick exit
void addToTempFile(const char* fpath, const char* fname, off_t fsize, uid_t fuid, enum ftype ftype);
int walkTree(const char* name, const struct stat* s, int type, struct FTW* f);
void indexDir(const char* pathd, const char* pathf);
void* threadWork(void* voidArgs);
void u_index(thread_t* threadArgs);
void u_count(const char* pathf);
void u_namepart(const char* pathf, const char* buf);
void u_largerthan(const char* pathf, const char* buf);
void u_owner(const char* pathf, const char* buf);
bool queryTest(finfo_t* fileinfo, void* value, int option);
void listRecords(const char* pathf, void* value, int option);
void initialization(thread_t* threadArgs, int argc, char** argv);
void startupIndexing(thread_t* threadArgs);
void getUserInput(thread_t* threadArgs);
void exitSequence(thread_t* threadArgs);

int main(int argc, char** argv)
{	
    int sigNo = 0;
    struct thread_t threadArgs;    

    // INITIALIZATION
    initialization(&threadArgs, argc, argv);    

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

    return EXIT_SUCCESS;
}

void displayHelp()
{
    printf("\nindex        : Start indexing procedure.\n\n");
    printf("count        : Print the counts of each file type in index.\n\n");
    printf("listall      : List all records in the index.\n\n");
    printf("largerthan x : Print the full path, size and type of all files in index that have size larger than x.\n\n");
    printf("namepart y   : Print the full path, size and type of all files in index that have y in the name.\n\n");
    printf("owner uid    : Print the full path, size and type of all files in index that owner is uid.\n\n");
    printf("exit         : Terminate program – wait for any indexing to finish\n\n");
    printf("exit!        : Terminate program – cancel any indexing in process.\n\n");
    printf("help         : prints this help message.\n\n");
}
void usage()
{
    fprintf(stderr,"\nUSAGE : mole [-d pathd] [-f pathf] [-t n]\n\n");
    fprintf(stderr,"pathd : the path to a directory that will be traversed, if the option is not present a path set in an environment variable $MOLE_DIR is used. If the environment variable is not set the program end with an error.\n\n");
    fprintf(stderr,"pathf : a path to a file where index is stored. If the option is not present, the value from environment variable $MOLE_INDEX_PATH is used. If the variable is not set, the default value of file `.mole-index` in user's home directory is used\n\n");
    fprintf(stderr,"n : is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index. This parameter is optional. If it is not present, the periodic re-indexing is disabled\n\n");
    exit(EXIT_FAILURE); 
}
void readArgs(int argc, char** argv, char** pathd, char** pathf, int* t)
{
	int c, dcount = 0, fcount = 0, tcount = 0;

    while ((c = getopt(argc, argv, "d:f:t:")) != -1)
        switch (c)
        {
            case 't':
                *t = atoi(optarg);
                if (*t < 30 || *t > 7200 || ++tcount > 1) usage();
                break;
            case 'd':
                if (++dcount > 1) usage();
                *pathd = *(argv + optind - 1);                                 
                break;
            case 'f':
                if (++fcount > 1) usage();                
                *pathf = *(argv + optind - 1); 
                break;
            default:
                usage();
        }
    
    if (dcount == 0) // assign $MOLE_DIR or error
    {
        char* env = getenv("MOLE_DIR");        
        
        if (env == NULL) 
        {
            printf("Program executed without -d argument but environment variable $MOLE_DIR not found.\n");
            usage();
        }

        *pathd = env;
    }

    if (fcount == 0) // assign $MOLE_INDEX_PATH or $HOME/.mole-index
    {        
        char* env = getenv("MOLE_INDEX_PATH");
        
        if (env != NULL) // $MOLE_INDEX_PATH set
        {
            *pathf = env;          
        }
        else // $MOLE_INDEX_PATH not set
        {
            // leave pathf as it is, i.e. "$HOME/.mole_index" assigned in main() 
        }
    }    

    if (tcount == 0) // periodic re-indexing disabled
    {
        *t = 0;
    }    

    if (argc>optind) usage();
}
char* typeToText(int type) // returns type based on enum
{
    if (type == 0) return "dir";
    else if (type == 1) return "jpg";
    else if (type == 2) return "png";
    else if (type == 3) return "gzip";
    else if (type == 4) return "zip";
    else if (type == 5) return "other";
    return "error";
}
enum ftype getType(const char* fname) // returns file type based on signature
{
    unsigned char sig[9];
    enum ftype type = 5;
    int state;
    int fd;
    
    if (DEBUGINDEXING) printf("[getType] Reading file: %s\n", fname);
    
    if ((fd=open(fname, O_RDONLY)) < 0)
    {
        // couldn't open file for reading, return "6 = error" as file type and continue
        if (DEBUGINDEXING) printf("[getType] File %s NOT opened \033[0;35m%s\033[0m\n", fname, typeToText(type));
        return 6;
    }
    
    // [CORRECTION] added checking for reading the 8 bytes
    // and removed ERR() which stopped program execution. Now it just skips the file. 
    if ((state = read(fd, sig, 8)) < 0 || state < 8)
    {
        // couldn't read from the file, return "6 = error" as file type and continue
        if (DEBUGINDEXING) printf("[getType] File %s NOT opened \033[0;35m%s\033[0m\n", fname, typeToText(type));
        return 6;
    }    
    
    if (close(fd)) ERR("fclose"); // these ERR() executions stop the program. Should I do something else?

    if (DEBUGINDEXING) printf("[getType] Signature: %02x %02x %02x %02x %02x %02x %02x %02x - \n", sig[0], sig[1],sig[2],sig[3], sig[4], sig[5],sig[6],sig[7]);

    // how to elegantly store and use these?
    unsigned char jpg[] = { 0xff, 0xd8, 0xff };
    unsigned char png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    unsigned char gzip[] = { 0x1f, 0x8b };
    unsigned char zip[] = { 0x50, 0x4b, 0x03, 0x04 };
    unsigned char zip2[] = { 0x50, 0x4b, 0x05, 0x06 };
    unsigned char zip3[] = { 0x50, 0x4b, 0x07, 0x08 };    
    
    // recognize file type
    if (memcmp(sig, gzip, 2) == 0) type = 3;
    else if (memcmp(sig, jpg, 3) == 0) type = 1;
    else if (memcmp(sig, zip, 4) == 0 || memcmp(sig, zip2, 4) == 0 || memcmp(sig, zip3, 4) == 0) type = 4;
    else if (memcmp(sig, png, 8) == 0) type = 2;
    else type = 5;

    if (DEBUGINDEXING) printf("[getType] File %s is \033[0;35m%s\033[0m\n", fname, typeToText(type));

    return type;
}
bool isSubstring(const char* sub, const char* str) // checks if sub is a substring of str
{
    int sublength = strlen(sub);
    int strlength = strlen(str);
    
    for (int i = 0; i+sublength <= strlength; i++) 
    {
        if ( strncmp(sub, str+i, sublength) == 0 ) return true;
    }
    
    return false;
}
void quickexit(void* tempfile)  // cleanup function for thread during quick exit
{
    if(DEBUGQUICKEXIT) printf("[quickExit] Starting cleanup.\n[quickExit] Closing tempfile.\n");
    if (close(*(int*)tempfile)) ERR("close"); // close file descriptor if still open
    
    if(DEBUGQUICKEXIT) printf("[quickExit] Deleting tempfile.\n");
    remove("./.temp"); // delete the temp file
    
    if(DEBUGQUICKEXIT) printf("[quickExit] Cleanup complete.\n");
}
void addToTempFile(const char* fpath, const char* fname, off_t fsize, uid_t fuid, enum ftype ftype)
{
    int state;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    // prepare fileinfo struct
    if (strlen(fpath) >= MAX_PATH) fprintf(stderr, "WARNING! Size of absolute path %s longer than buffer MAX_PATH (%d). Shortening...\n", fpath, MAX_PATH-1);
    strncpy(fileinfo.path, fpath, MAX_PATH-1);
    if (strlen(fname) >= MAX_FILE) fprintf(stderr, "WARNING! Size of file name %s longer than MAX_FILE (%d). Shortening...\n", fname, MAX_FILE-1);
    strncpy(fileinfo.name, fname, MAX_FILE-1);
    fileinfo.size = fsize;
    fileinfo.uid = fuid;
    fileinfo.type = ftype;

    if (DEBUGWRITEFILE) // debug messages
    {
        printf("[addToTempFile] Writing to file:\n");
        printf("[addToTempFile] Abs. path: %s\n", fileinfo.path);
        printf("[addToTempFile] File name: %s\n", fileinfo.name);
        printf("[addToTempFile] File size: %lu\n", fileinfo.size);
        printf("[addToTempFile] File uid: %d\n", fileinfo.uid);
        printf("[addToTempFile] File type: %s\n", typeToText(fileinfo.type));
    }

    //write struct to file
    if ((state = write(tempfile, &fileinfo, sizeof(finfo_t))) <= 0) ERR("write");

    if (DEBUGWRITEFILE) printf("[addToTempFile] Finished writing %d bytes (should be sizeof(finfo_t) = %lu bytes)\n", state, sizeof(finfo_t));
}
int walkTree(const char* name, const struct stat* s, int type, struct FTW* f)
{    
    char* path;
    enum ftype ftype;
    errno = 0;
    
    if ( (path = realpath(name, NULL)) == NULL ) return 0; // ignore unresolved paths
    
    if (DEBUGINDEXING) printf("\n[walkTree] Abs. Path: %s \n[walkTree] File/Dir. Name: %s\n", path, name + f->base);
    
    switch (type)
    {
        case FTW_DNR:
        case FTW_D:
	        if (f->level != 0) ftype = 0;
            else ftype = 5; // ignore root
            break;
        case FTW_F:	        
            ftype = getType(name);
            break;
    }

    if (DEBUGINDEXING) printf("[walkTree] Size: %lo \n[walkTree] UID: %d\n", s->st_size, s->st_uid);

    if (ftype < 5) addToTempFile(path, name+f->base, s->st_size, s->st_uid, ftype);
    
    free(path); // free the buffer returned from realpath
    return 0;
}
void indexDir(const char* pathd, const char* pathf)
{
    // open temp file for writing and assign to global file descriptor
    // nftw() will write to the file at each step
    if ((tempfile = open("./.temp", O_WRONLY|O_CREAT|O_TRUNC, 0777)) < 0) ERR("open");
    
    // prepare cleanup for quick exit
    pthread_cleanup_push(quickexit, &tempfile);
    
    if (DEBUGSIMULATION) 
    {
        printf("[indexDir] Simulating 5 second long indexing.\n");
        sleep(5); // to simulate a 5 second long indexing procedure
    }

    //start tree walk process
    if ( 0 != nftw(pathd, walkTree, MAXFD, FTW_PHYS))
        printf("%s: cannot access\n", pathd);

    // close temp file
    if (close(tempfile)) ERR("close");
    
    int state = 0;
    // atomically rename temp file to actual file
    // protect renaming operation against cancellation
    // loop while EBUSY so that if indexfile is open in another stream it waits?
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    do
    {
        if ( (state = rename(".temp", pathf)) == EBUSY) sleep(1);
    } while (state == EBUSY);
    if (state != 0) ERR("rename");
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    pthread_cleanup_pop(0);
}
void* threadWork(void* voidArgs)
{
    thread_t* threadArgs = voidArgs;
    long timeElapsed = threadArgs->newIndex>0 ? 0 : time(NULL) - threadArgs->pIndexStat->st_mtime;
    long timeLeft = threadArgs->newIndex>0 ? threadArgs->t : threadArgs->t - timeElapsed;
    int sigNo = 0;

    if (DEBUGTHREAD) // debug messages
    {
        printf("[threadWork] Thread with TID: %lu started.\n", (unsigned long)threadArgs->tid);
        if (threadArgs->newIndex == 1) printf("[threadWork] Index file \"%s\" does not exist.\n", threadArgs->pathf);
        else if (threadArgs->newIndex == 2) printf("[threadWork] Indexing initiated by user.\n");
        else if (threadArgs->newIndex == 0)
        {
            printf("[threadWork] Index file \"%s\" exists. Last modification: %ld\n", threadArgs->pathf, threadArgs->pIndexStat->st_mtime);
            printf("[threadWork] Current time: %ld\n", time(NULL));
            printf("[threadWork] Time elapsed since last indexing is: %ld seconds.\n", timeElapsed);
            printf("[threadWork] Will wait for: %d - %ld = %ld seconds before re-indexing.\n", threadArgs->t, timeElapsed, timeLeft);
        }
    }

    if (threadArgs->newIndex != 0) // start-up without old index file / user initiated indexing 
    {
        printf("--Starting indexing.\n");
        if (threadArgs->newIndex == 2) printf("> Enter command (\"help\" for list of commands): \n");
        
        pthread_mutex_lock(threadArgs->pmxIndexer);
        indexDir(threadArgs->pathd, threadArgs->pathf);
        pthread_mutex_unlock(threadArgs->pmxIndexer);
        
        printf("--Indexing complete.\n");
        if (threadArgs->newIndex != 1 && threadArgs->exitFlag == 0) printf("> Enter command (\"help\" for list of commands): \n");
        
        // if necessary, inform main thread that start-up indexing is finished
        if (threadArgs->newIndex == 1) pthread_kill(threadArgs->mtid, SIGUSR1);

        // set alarm for periodic indexing
        if (threadArgs->t > 0) alarm(threadArgs->t);        
    }
    else // start-up with old index file and perodic indexing set
    { 
        if(timeLeft > 0) // index file not old enough - set alarm to wait until it's old enough
        {
            if (DEBUGTHREAD) printf("[threadWork] Setting alarm for %ld seconds and waiting for periodic indexing.\n", timeLeft);
            alarm(timeLeft);
        }
        else // index file too old - trigger indexing in periodic indexing loop
        {
            pthread_kill(threadArgs->tid, SIGALRM); 
            printf("--Index file too old.\n");
        }
    }
    
    timeLeft = threadArgs->t;

    // enter periodic indexing loop if it is set
    while (threadArgs->t > 0)
    {
        // wait for a signal
        do
        {
            sigwait(threadArgs->pMask, &sigNo);
        } while (sigNo != SIGUSR1 && sigNo != SIGUSR2 && sigNo != SIGALRM);
        
        if (DEBUGTHREAD) // debug messages
        {
            if (sigNo == SIGUSR1) printf("[threadWork] SIGUSR1 (index) received from user.\n");
            if (sigNo == SIGUSR2) printf("[threadWork] SIGUSR2 (exit) received from user.\n");
            if (sigNo == SIGALRM) printf("[threadWork] SIGALRM (periodic indexing) triggered.\n");
        }

        if (sigNo == SIGUSR2) break; // exit signal received - end the thread

        printf("--Starting indexing.\n");
        printf("> Enter command (\"help\" for list of commands): \n");

        pthread_mutex_lock(threadArgs->pmxIndexer);
        indexDir(threadArgs->pathd, threadArgs->pathf);
        pthread_mutex_unlock(threadArgs->pmxIndexer);
        
        printf("--Indexing complete.\n");
        if (threadArgs->exitFlag == 0) printf("> Enter command (\"help\" for list of commands): \n");

        // set new alarm for periodic indexing
        if (DEBUGTHREAD) printf("[threadWork] Setting alarm for %ld seconds and waiting for periodic indexing.\n", timeLeft);
        alarm(timeLeft);
    }
    
    if (DEBUGTHREAD) printf("[threadWork] Ending thread.\n");
    return NULL;
}
void u_index(thread_t* threadArgs)
{
    int mxStatus = 0;
            // check if there's an indexing operation currently in progress
            if ( (mxStatus = pthread_mutex_trylock(threadArgs->pmxIndexer) ) == EBUSY)
            {
                printf ("--There's already an indexing process running...\n");
                return;
            }
            else if ( mxStatus == 0 )
            {
                if(threadArgs->t == 0)  // perodic indexing disabled - there's no active thread
                {
                    // create a thread for indexing
                    threadArgs->newIndex = 2;
                    if (DEBUGMAIN) printf("[main] Creating new thread to start indexing...\n");
                    if (pthread_create(&threadArgs->tid, NULL, threadWork, threadArgs)) ERR("pthread_create");
                }
                else // periodic indexing enabled - there's an active thread
                {
                    // send SIGUSR1 to active thread to start indexing
                    if (DEBUGMAIN) printf("[main] Sending SIGUSR1 to thread to start indexing...\n");
                    pthread_kill(threadArgs->tid, SIGUSR1);
                }
                
                pthread_mutex_unlock(threadArgs->pmxIndexer);
            }
            else ERR("pthread_mutex_lock");
}
void u_count(const char* pathf)
{
    int indexFile, state, dir = 0, jpg = 0, png = 0, gzip = 0, zip = 0;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if ( (indexFile = open(pathf, O_RDONLY)) < 0 ) ERR("open");

    while( (state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0 )
    {
        switch (fileinfo.type)
        {
            case 0:
                dir++;
                break;
            case 1:
                jpg++;
                break;
            case 2:
                png++;
                break;
            case 3:
                gzip++;
                break;
            case 4:
                zip++;
                break;
            default:
                break;
        }
    }
    if (state < 0) ERR("read");

    if (close(indexFile)) ERR("close");

    printf("--Files count: dir:%d, jpg:%d, png:%d, gzip:%d, zip: %d\n", dir, jpg, png, gzip, zip);
}
void u_largerthan(const char* pathf, const char* buf)
{
    long x;
    if ( (x = strtol(buf+11, NULL, 10)) < 0 ) printf("You must enter a positive integer value for x.\n");
    else if (x > 0)
    {
        listRecords(pathf, (void*)&x, 0);
    }
    else printf("--Invalid command or arguments missing.\n");
}
void u_namepart(const char* pathf, const char* buf)
{
    int length;
    char y[MAX_FILE];
    strcpy(y, buf+9);
    length = strlen(y);
    y[length-1] = '\0'; // get rid of the \n character

    if (strlen(y) > MAX_FILE-1) 
    {
        printf("Name part string exceeds maximum file length of %d characters.\n", MAX_FILE-1);
    }
    else if (strlen(y) > 0)
    {
        listRecords(pathf, (void*)&y, 1);
    }
    else printf("--Invalid command or arguments missing.\n");
}
void u_owner(const char* pathf, const char* buf)
{
    long uid;
            
    if ( (uid = strtol(buf+5, NULL, 10)) < 0 ) printf("You must enter a positive integer value for x.\n");
    else if (uid > 0)
    {
        listRecords(pathf, (void*)&uid, 2);
    }
    else printf("--Invalid command or arguments missing.\n");
}
bool queryTest(finfo_t* fileinfo, void* value, int option)
{
    switch (option)
    {
        case -1: return true;                                       // listall
        case 0 : return fileinfo->size > *(long*)value;             // largerthan
        case 1 : return isSubstring((char*)value, fileinfo->name);  // namepart
        case 2 : return fileinfo->uid == *(int*)value;              // owner
    }
    
    ERR("Wrong option number passed to tests from getUserInput ");
}
void listRecords(const char* pathf, void* value, int option)
{
    int indexFile, state, count = 0;
    char* pager;
    FILE* stream = stdout;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));
    
    if ((indexFile = open(pathf, O_RDONLY)) < 0) ERR("open");

    // count how many records meet the query requirements
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (queryTest(&fileinfo, value, option)) count++;
        if (count >= 3) break;
    }

    // if more than 2 records and $PAGER env. variable is set, change stream to $PAGER
    if (count >= 3 && (pager = getenv("PAGER")) != NULL )
    {
        if ( (stream = popen(pager, "w")) == NULL )
        {
            printf("WARNING! The $PAGER variable is invalid. Pagination disabled.\n");
            stream = stdout;
        }        
    }
    
    // move file descriptor offset to the beginning
    lseek(indexFile, 0, SEEK_SET);    

    //print to stream    
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (queryTest(&fileinfo, value, option))
        {
            fprintf(stream, "File path: %s\n", fileinfo.path);
            fprintf(stream, "File size: %lu bytes\n", fileinfo.size);
            fprintf(stream, "File type: %s\n\n", typeToText(fileinfo.type));
        }
    }
    if (state < 0) ERR("read");

    if (close(indexFile)) ERR("close");
    if (stream != stdout && pclose(stream) != 0) 
    {
        if (errno != EPIPE) ERR("pclose"); // ignore broken pipe error
    }

    if (count == 0) printf("No records match the query criteria.\n");
}
void initialization(thread_t* threadArgs, int argc, char** argv)
{
    int t = 0;
    char *pathd, *pathf, *home; 
    
    // initialize pathf=$HOME/.mole_index (to be modified in readArgs() if necessary)
    if ( (home = getenv("HOME")) == NULL ) ERR("$HOME environment variable is NOT defined!!!");
    if ( (threadArgs->tempBuffer = (char*) calloc( (strlen(home) + strlen("/.mole-index") + 1), sizeof(char))) == NULL) ERR("calloc");
    strcat(strcat(threadArgs->tempBuffer, home), "/.mole-index");
    pathf = threadArgs->tempBuffer;  // free'd in exit_sequence()
    
    // initialize command line arguments & check index file status
    readArgs(argc, argv, &pathd, &pathf, &t);
    
    // check if an old index file exists
    int indexStatus = -1;
    struct stat* indexStat;
    if ( (indexStat = (struct stat*) malloc(sizeof(struct stat))) == NULL ) ERR ("malloc"); // free'd in exit_sequence()
    indexStatus = lstat(pathf, indexStat);

    // initialize signal mask
    sigset_t* mask;
    if ( (mask = (sigset_t*) malloc(sizeof(sigset_t))) == NULL ) ERR ("malloc"); // free'd in exit_sequence()
    sigemptyset(mask);
    sigaddset(mask, SIGUSR1);  // "user initiated index" signal for indexer thread
                               // "start-up indexing completed" signal for main thread
    sigaddset(mask, SIGUSR2);  // "exit" signal for indexer thread
    sigaddset(mask, SIGPIPE);  // "exit" signal for indexer thread
    sigaddset(mask, SIGALRM);  // "periodic index" signal for indexer thread
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

    printf("\nStarting **mole**.\n");
}
void startupIndexing(thread_t* threadArgs)
{
    if (threadArgs->newIndex == 0 && threadArgs->t == 0) // no startup indexing necessary
    {
        printf("--Index file \"%s\" exists.\n--Periodic indexing disabled.\n", threadArgs->pathf);
    }
    else if (threadArgs->newIndex == 0 || errno == ENOENT) // create indexer thread
    {
        // display informative messages for user
        if (threadArgs->newIndex == 0) printf("--Index file \"%s\" exists.\n", threadArgs->pathf);
        else printf("--Index file \"%s\" does not exist. \n", threadArgs->pathf);
        if (threadArgs->t == 0) printf("--Periodic indexing disabled.\n");
        else printf("--Periodic indexing enabled. t = %d seconds\n", threadArgs->t);
    
        // create thread
        if (pthread_create(&threadArgs->tid, NULL, threadWork, threadArgs)) ERR("pthread_create");
        
        if (DEBUGMAIN) printf("[main] Thread with TID: %lu started.\n", (unsigned long)threadArgs->tid);
    }
}
void getUserInput(thread_t* threadArgs)
{    
    // wait for user input until exited
    while(true) 
    {
        char buf[256] = {'\0'};

        printf("> Enter command (\"help\" for list of commands): \n");
        fflush(stdin);
        
        if ( fgets(buf, 255, stdin) == NULL ) ERR("fgets)");
    
        if (memcmp(buf, "exit\n", 5) == 0)
        {
            threadArgs->exitFlag = 1;
            // if there's an active thread send signal for termination
            if (threadArgs->tid > 0) pthread_kill(threadArgs->tid, SIGUSR2);
            break;
        }
        else if (memcmp(buf, "exit!\n", 6) == 0)
        {
            // if there's an active thread cancel it (and trigger cleanup)
            if (threadArgs->tid > 0) pthread_cancel(threadArgs->tid);
            break;
        }
        else if (memcmp(buf, "index\n", 6) == 0)
        {
            u_index(threadArgs);
        }
        else if (memcmp(buf, "count\n", 6) == 0)
        {
            u_count(threadArgs->pathf);
        }
        else if (memcmp(buf, "listall\n", 8) == 0)
        {
            listRecords(threadArgs->pathf, NULL, -1);            
        }
        else if (memcmp(buf, "largerthan ", 11) == 0)
        {
            u_largerthan(threadArgs->pathf, buf);
        }
        else if (memcmp(buf, "namepart ", 9) == 0)
        {
            u_namepart(threadArgs->pathf, buf);
        }
        else if (memcmp(buf, "owner ", 6) == 0)
        {
            u_owner(threadArgs->pathf, buf);
        }
        else if (memcmp(buf, "help\n", 5) == 0)
        {
            displayHelp();
        }
        else printf("--Invalid command or arguments missing.\n");
    }
}
void exitSequence(thread_t* threadArgs)
{
    free(threadArgs->pMask);
    free(threadArgs->pmxIndexer);
    free(threadArgs->pIndexStat);
    free(threadArgs->tempBuffer);

    if (threadArgs->tid && DEBUGMAIN) printf("[main] Waiting to join with indexing thread.\n");
    
    // if there was an active thread check for its termination
    if (threadArgs->tid && pthread_join(threadArgs->tid, NULL)) ERR("Can't join with indexer thread");
    else if (threadArgs->tid && DEBUGMAIN) printf("[main] Joined with indexer thread.\n");
    else if (threadArgs->tid == 0 && DEBUGMAIN) printf("[main] There's no indexer thread to join.\n");

    printf("Ending **mole**.\n");
}