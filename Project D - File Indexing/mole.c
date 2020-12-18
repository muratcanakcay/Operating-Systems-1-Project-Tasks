#define _XOPEN_SOURCE 500
#include <sys/stat.h>
#include <ftw.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#define DEBUGMAIN 1
#define DEBUGTHREAD 0
#define DEBUGINDEXING 0
#define DEBUGWRITEFILE 0
#define DEBUGREADFILE 0
#define DEBUGQUICKEXIT 1
#define DEBUGSIMULATION 1

#define DEFAULT_MOLE_DIR_ENV "MOLE_DIR" 
#define DEFAULT_INDEX_PATH_ENV "MOLE_INDEX_PATH"
#define MAX_PATH 128
#define MAX_FILE 32
#define MAXFD 100

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

typedef unsigned int UINT;
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
    char* pathd;
    char* pathf;
    unsigned short t;
    unsigned short newIndex; //0:old index file exists, 1:does not exist new needed, 2: new indexing initiated by user
    bool quitFlag;
    struct stat* pIndexStat;
    sigset_t* pMask;
    pthread_mutex_t* pmxIndexer;
} thread_t;

void displayHelp()
{
    printf("\ntest         : Tests reading from indexfile.\n\n");
    printf("exit         : Terminate program – wait for any indexing to finish\n\n");
    printf("exit!        : Terminate program – cancel any indexing in process.\n\n");
    printf("index        : Start indexing procedure.\n\n");
    printf("count        : Print the counts of each file type in index.\n\n");
    printf("largerthan x : Print the full path, size and type of all files in index that have size larger than x.\n\n");
    printf("namepart y   : Print the full path, size and type of all files in index that y in the name.\n\n");
    printf("owner uid    : Print the full path, size and type of all files in index that owner is uid.\n\n");
    printf("help         : prints this help message.\n\n");
}
void usage()
{
    fprintf(stderr,"\n\e[4mUSAGE\e[0m: mole ([-d \e[4mpathd\e[0m] [-f \e[4mpathf\e[0m] [-t \e[4mn\e[0m])\n\n");
    fprintf(stderr,"\e[4mpathd\e[0m : the path to a directory that will be traversed, if the option is not present a path set in an environment variable $MOLE_DIR is used. If the environment variable is not set the program end with an error.\n\n");
    fprintf(stderr,"\e[4mpathf\e[0m : a path to a file where index is stored. If the option is not present, the value from environment variable $MOLE_INDEX_PATH is used. If the variable is not set, the default value of file `.mole-index` in user's home directory is used\n\n");
    fprintf(stderr,"\e[4mn\e[0m : is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index. This parameter is optional. If it is not present, the periodic re-indexing is disabled\n\n");
    exit(EXIT_FAILURE); 
}
void readArgs(int argc, char** argv, char** pathd, char** pathf, unsigned short* t)
{
	int c, dcount = 0, fcount = 0, tcount = 0;

    while ((c = getopt(argc, argv, "d:f:t:")) != -1)
        switch (c)
        {
            case 't':
                *t = (unsigned short)atoi(optarg);
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
        char* env = getenv(DEFAULT_MOLE_DIR_ENV);        
        
        if (env == NULL) 
        {
            printf("Program executed without -d argument but environment variable $MOLE_DIR not found.\n");
            usage();
        }

        *pathd = env;
    }

    if (fcount == 0) // assign $MOLE_INDEX_PATH or $HOME/.mole-index
    {        
        char* env = getenv(DEFAULT_INDEX_PATH_ENV);
        
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
        *t = -0;
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
    
    for (int i = 0; i + sublength <= strlength; i++) 
    {
        if ( strncmp(sub, str + i, sublength) == 0) return true;
    }
    
    return false;
}
void addToTempFile(const char* fpath, const char* fname, off_t fsize, uid_t fuid, enum ftype ftype)
{
    int state;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if (strlen(fpath) >= MAX_PATH) fprintf(stderr, "WARNING! Size of absolute path %s longer than buffer MAX_PATH (%d). Shortening...\n", fpath, MAX_PATH-1);
    strncpy(fileinfo.path, fpath, MAX_PATH-1);
    if (strlen(fname) >= MAX_FILE) fprintf(stderr, "WARNING! Size of file name %s longer than MAX_FILE (%d). Shortening...\n", fname, MAX_FILE-1);
    strncpy(fileinfo.name, fname, MAX_FILE-1);
    fileinfo.size = fsize;
    fileinfo.uid = fuid;
    fileinfo.type = ftype;

    if (DEBUGWRITEFILE)
    {
        printf("[addToTempFile] Writing to file:\n");
        printf("[addToTempFile] Abs. path: %s\n", fileinfo.path);
        printf("[addToTempFile] File name: %s\n", fileinfo.name);
        printf("[addToTempFile] File size: %lu\n", fileinfo.size);
        printf("[addToTempFile] File uid: %d\n", fileinfo.uid);
        printf("[addToTempFile] File type: %s\n", typeToText(fileinfo.type));
    }

    if ((state = write(tempfile, &fileinfo, sizeof(finfo_t))) <= 0) ERR("write");

    if (DEBUGWRITEFILE) printf("[addToTempFile] Finished writing %d bytes (should be sizeof(finfo_t) = %lu bytes)\n\n", state, sizeof(finfo_t));
}
int walkTree(const char* name, const struct stat* s, int type, struct FTW* f)
{
    
    char* path;
    enum ftype ftype;
    errno = 0;
    
    if ((path = realpath(name, NULL)) == NULL) 
    {        
        if (DEBUGINDEXING) 
        {
            printf("name: %s gives error in realpath.\n", name);
        }

        return 0; // ignore unresolved path
    }

    if (DEBUGINDEXING) printf("\n[walkTree] Abs. path length: %lu\n", strlen(path));
    
    switch (type)
    {
        case FTW_DNR:
        case FTW_D:
	        if (DEBUGINDEXING) printf("[walkTree] Abs. path: %s \n[walkTree] Directory name: %s\n", path, name + f->base);
            if (f->level == 0) // ignore root
            {
                if (DEBUGINDEXING) printf("[walkTree] Ignoring \033[0;35mroot\033[0m %s\n", name + f->base);
                return 0;
            }
            ftype = 0;
            break;
        case FTW_F:	        
            if (DEBUGINDEXING) printf("[walkTree] Abs. path: %s \n[walkTree] File name: %s\n", path, name + f->base);
            ftype = getType(name);
            break;
    }

    if (DEBUGINDEXING) printf("[walkTree] Size: %lo \n[walkTree] UID: %d\n", s->st_size, s->st_uid);

    if (ftype < 5) addToTempFile(path, name+f->base, s->st_size, s->st_uid, ftype);
    
    free(path); // [CORRECTION] freeing the buffer returned from realpath
    return 0;
}
void testRead(const char* pathf) // will be completely removed eventually
{
    if ((tempfile = open(pathf, O_RDONLY)) < 0) ERR("open");

    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    int state;
    while ((state = read(tempfile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (DEBUGREADFILE)
        {
            printf("[testRead] Reading from file %s:\n", realpath(pathf, NULL));
            printf("[testRead] Abs. path: %s\n", fileinfo.path);
            printf("[testRead] File name: %s\n", fileinfo.name);
            printf("[testRead] File size: %lu\n", fileinfo.size);
            printf("[testRead] File uid: %d\n", fileinfo.uid);
            printf("[testRead] File type: %s\n", typeToText(fileinfo.type));
            printf("[testRead] Read %d bytes (should be sizeof(finfo_t) = %lu bytes)\n\n", state, sizeof(finfo_t));
        }
    }
    if (state < 0) ERR("read");

    if (close(tempfile)) ERR("close");
}
void quickexit(void* tempfile)  // cleanup function for thread during quick exit
{
    if(DEBUGQUICKEXIT) printf("[quickExit] Starting cleanup.\n[quickExit] Closing tempfile.\n");
    if (close(*(int*)tempfile)) ERR("close"); // close file descriptor if still open
    if(DEBUGQUICKEXIT) printf("[quickExit] Deleting tempfile.\n");
    remove("./.temp"); // delete the temp file
    if(DEBUGQUICKEXIT) printf("[quickExit] Cleanup complete.\n");
}
void indexDir(const char* pathd, const char* pathf)
{
    // open temp file for writing and assign to global file descriptor
    // nftw() will write to the file at each step
    if ((tempfile = open("./.temp", O_WRONLY|O_CREAT|O_TRUNC|O_APPEND, 0777)) < 0) ERR("open");
    
    // prepare cleanup for quick exit
    pthread_cleanup_push(quickexit, &tempfile);
    
    //start tree walk process
    if ( 0 != nftw(pathd, walkTree, MAXFD, FTW_PHYS))
        printf("%s: cannot access\n", pathd);

    // close temp file
    if (close(tempfile)) ERR("close");
    
    // atomically rename temp file to actual file
    // loop while EBUSY so that if indexfile is open in another stream it waits?
    int state = 0;
    
    // protect renaming operation against cancellation
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    do
    {
        state = rename(".temp", pathf);
    } while (state == EBUSY);
    if (state != 0) ERR("rename");
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    pthread_cleanup_pop(0);
}
void* threadWork(void* voidArgs)
{
    thread_t* threadArgs = voidArgs;
    unsigned short timeElapsed = time(NULL) - threadArgs->pIndexStat->st_mtime;
    int sigNo;

    if (DEBUGTHREAD) // debug messages
    {
        printf("[threadWork] Thread with TID: %lu started.\n", (unsigned long)threadArgs->tid);
        if (threadArgs->newIndex == 1) printf("[threadWork] Index file \"%s\" does not exist.\n", threadArgs->pathf);
        else if (threadArgs->newIndex == 2) printf("[threadWork] Indexing initiated by user.\n");
        else 
        {
            printf("[threadWork] Index file \"%s\" exists. Last modification: %lo\n", threadArgs->pathf, threadArgs->pIndexStat->st_mtime);
            printf("[threadWork] Current time: %lo\n", time(NULL));
            printf("[threadWork] Time elapsed since last indexing is: %d seconds.\n", timeElapsed);
            printf("[threadWork] Will wait for: %d - %d = %d seconds before re-indexing.\n", threadArgs->t, timeElapsed, threadArgs->t - timeElapsed);
        }
    }

    if (threadArgs->newIndex == 0) // old index file exists 
    { 
        // check the age of the index file and if necessary wait until it's old enough
        if(timeElapsed < threadArgs->t) 
        {
            if(DEBUGTHREAD) printf("[threadWork] Waiting: %d seconds...\n", threadArgs->t - timeElapsed);
            sleep(threadArgs->t - timeElapsed);
        }
        else printf("--Index file too old.\n");
    }

    // At this point:
    // EITHER 
    // -- old index file does not exist
    // OR 
    // -- old index file exists and it's old enough to be re-indexed
    // Start indexing.
    printf("--Starting indexing.\n");
    if (threadArgs->newIndex == 2) printf("> Enter command (\"help\" for list of commands): \n");
    
    pthread_mutex_lock(threadArgs->pmxIndexer);
    if (DEBUGTHREAD) printf("[threadWork] MUTEX LOCKED.\n");
    indexDir(threadArgs->pathd, threadArgs->pathf);
    if (DEBUGSIMULATION) 
    {
        printf("[threadWork] Simulating 5 second long indexing.\n");
        sleep(5); // to simulate a 5 second long indexing procedure
    }
    pthread_mutex_unlock(threadArgs->pmxIndexer);
    if (DEBUGTHREAD) printf("[threadWork] MUTEX UNLOCKED - now user can run \"index\".\n");
    
    printf("--Indexing complete.\n");
    if (threadArgs->newIndex != 1 && threadArgs->quitFlag == 0) printf("> Enter command (\"help\" for list of commands): \n");
    
    // inform main thread that start-up indexing is finished
    if (threadArgs->newIndex == 1) pthread_kill(threadArgs->mtid, SIGUSR1);

    // enter periodic indexing loop if it is set
    while (threadArgs->t > 0)
    {
        if (DEBUGTHREAD) printf("[threadWork] Setting alarm for %d seconds and waiting for periodic indexing.\n", threadArgs->t);
        
        // set alarm for periodic indexing 
        alarm(threadArgs->t); 
        
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

        if (sigNo == SIGUSR2) break; // end the thread

        printf("--Starting indexing.\n");
        printf("> Enter command (\"help\" for list of commands): \n");

        pthread_mutex_lock(threadArgs->pmxIndexer);
        if (DEBUGTHREAD) printf("[threadWork] MUTEX LOCKED.\n");
        if (DEBUGSIMULATION) 
        {
            printf("[threadWork] Simulating 5 second long indexing.\n");
            sleep(5); // to simulate a 5 second long indexing procedure
        }
        indexDir(threadArgs->pathd, threadArgs->pathf);
        pthread_mutex_unlock(threadArgs->pmxIndexer);
        if (DEBUGTHREAD) printf("[threadWork] MUTEX UNLOCKED - now user can run \"index\".\n");
        
        printf("--Indexing complete.\n");
        if (threadArgs->quitFlag == 0) printf("> Enter command (\"help\" for list of commands): \n");
    }
    
    if (DEBUGTHREAD) printf("[threadWork] Ending thread.\n");
    return NULL;
}
void count(const char* pathf)
{
    int indexFile, state, dir = 0, jpg = 0, png = 0, gzip = 0, zip = 0;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if ((indexFile = open(pathf, O_RDONLY)) < 0) ERR("open");

    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
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
void largerThan(const char* pathf, long size)
{
    int indexFile, state, count = 0;
    char* pager;
    FILE* stream;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if ((indexFile = open(pathf, O_RDONLY)) < 0) ERR("open");

    // count how many records there are
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (fileinfo.size > size) count++; // comparing off_t with long - is this a problem?
    }

    stream = stdout;
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
    fprintf(stream, "--List of files in the index with size larger than %ld\n\n", size);
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (fileinfo.size > size) // comparing off_t with long - is this a problem?
        {
            fprintf(stream, "File path: %s\n", fileinfo.path);
            fprintf(stream, "File size: %lu bytes\n", fileinfo.size);
            fprintf(stream, "File type: %s\n\n", typeToText(fileinfo.type));
        }
    }
    if (state < 0) ERR("read");

    if (close(indexFile)) ERR("close");
    if (stream != stdout && pclose(stream) < 0) ERR("pclose");    
}
void namePart(char* pathf, char* y)
{
    int indexFile, state, count = 0;
    char* pager;
    FILE* stream;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if ((indexFile = open(pathf, O_RDONLY)) < 0) ERR("open");

    // count how many records there are
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (isSubstring(y, fileinfo.name)) count++;
    }

    stream = stdout;
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
    fprintf(stream, "List of files in the index with name containing \"%s\"\n\n", y);
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (isSubstring(y, fileinfo.name))
        {
            fprintf(stream, "File path: %s\n", fileinfo.path);
            fprintf(stream, "File size: %lu bytes\n", fileinfo.size);
            fprintf(stream, "File type: %s\n\n", typeToText(fileinfo.type));
        }
    }
    if (state < 0) ERR("read");

    if (close(indexFile)) ERR("close");
    if (stream != stdout && pclose(stream) < 0) ERR("pclose");        
}
void owner(const char* pathf, long uid)
{
    int indexFile, state, count = 0;
    char* pager;
    FILE* stream;
    finfo_t fileinfo;
    memset(&fileinfo, 0, sizeof(finfo_t));

    if ((indexFile = open(pathf, O_RDONLY)) < 0) ERR("open");

    // count how many records there are
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (fileinfo.uid == uid) count++;
    }

    stream = stdout;
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
    fprintf(stream, "--List of files in the index with owner uid = %ld\n\n", uid);
    while((state = read(indexFile, &fileinfo, sizeof(finfo_t))) > 0)
    {
        if (fileinfo.uid == uid)
        {
            fprintf(stream, "File path: %s\n", fileinfo.path);
            fprintf(stream, "File size: %lu bytes\n", fileinfo.size);
            fprintf(stream, "File type: %s\n\n", typeToText(fileinfo.type));
        }
    }
    if (state < 0) ERR("read");

    if (close(indexFile)) ERR("close");
    if (stream != stdout && pclose(stream) < 0) ERR("pclose");    
}
void startupIndexing(int indexStatus, thread_t* threadArgs)
{
    if (indexStatus == 0 && threadArgs->t == 0) // no indexing necessary
    {
        printf("--Index file \"%s\" exists.\n--Periodic indexing disabled.\n", threadArgs->pathf);
    }
    else if (indexStatus == 0 || (indexStatus == -1 && errno == ENOENT)) // start indexer thread
    {
        // display informative messages
        if (indexStatus == 0) printf("--Index file %s exists.\n", threadArgs->pathf);
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
        fgets(buf, 255, stdin);          
        
        
        if (memcmp(buf, "test\n", 5) == 0) //requires DEBUGREADFILE 1 to display
        {
            testRead(threadArgs->pathf); //test reading from index file
        }
        
        
        // exit
        else if (memcmp(buf, "exit\n", 5) == 0)
        {
            threadArgs->quitFlag = 1;
            if (threadArgs->tid > 0) // if there's an active thread send signal for termination
            {
                // printf("--Waiting for indexing to finish.\n"); // looks wrong
                pthread_kill(threadArgs->tid, SIGUSR2);
            }
            break; 
        }

        // exit!
        else if (memcmp(buf, "exit!\n", 6) == 0)
        {
            if (threadArgs->tid > 0) // if there's an active thread cancel it (and trigger cleanup)
            {
                pthread_cancel(threadArgs->tid);
            }            
            break;
        }        
        
        // index
        else if (memcmp(buf, "index\n", 6) == 0)
        {
            int mxStatus = 0;
            // check if there's an indexing operation currently in progress  
            if ( (mxStatus = pthread_mutex_trylock(threadArgs->pmxIndexer) ) == EBUSY)
            {
                printf ("--There's already an indexing process running...\n");
                continue;
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

        // count
        else if (memcmp(buf, "count\n", 6) == 0)
        {
            count(threadArgs->pathf);             
        }

        // largerthan x
        else if (memcmp(buf, "largerthan ", 11) == 0)
        {
            long x;
            if ( (x = strtol(buf+11, NULL, 10)) < 0 ) printf("You must enter a positive integer value for x.\n");
            else if (x > 0)
            {
                largerThan(threadArgs->pathf, x);
            }
            else printf("--Invalid command or arguments missing.\n");
        }

        // namepart y
        else if (memcmp(buf, "namepart ", 9) == 0)
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
                namePart(threadArgs->pathf, y);
            }
            else printf("--Invalid command or arguments missing.\n");
        }

        // owner uid
        else if (memcmp(buf, "owner ", 6) == 0)
        {
            long uid;
            
            if ( (uid = strtol(buf+5, NULL, 10)) < 0 ) printf("You must enter a positive integer value for x.\n");
            else if (uid > 0)
            {
                owner(threadArgs->pathf, uid);
            }
            else printf("--Invalid command or arguments missing.\n");
        }

        // help
        else if (memcmp(buf, "help\n", 5) == 0)
        {
            displayHelp();
        }

        // invalid
        else printf("--Invalid command or arguments missing.\n");
    }
}
void exitSequence(thread_t* threadArgs)
{
    if (threadArgs->tid && DEBUGMAIN) printf("[main] Waiting to join with indexing thread.\n");
    
    // if there's an active thread wait for its termination
    if (threadArgs->tid && pthread_join(threadArgs->tid, NULL)) ERR("Can't join with indexer thread");
    else if (threadArgs->tid && DEBUGMAIN) printf("[main] Joined with indexer thread.\n");
    else if (threadArgs->tid == 0 && DEBUGMAIN) printf("[main] There's no indexer thread to join.\n");
}

int main(int argc, char** argv)
{	
    // INITIALIZATION
    
    // initialize variables
    unsigned short t;
    int indexStatus, sigNo;
    char *pathd = NULL, *pathf, *home;
    struct stat indexStat;
    pthread_mutex_t mxIndexer = PTHREAD_MUTEX_INITIALIZER;
    
    // initialize pathf=$HOME/.mole_index (to be modified in readArgs() if necessary)
    home = getenv("HOME");
    char* tempbuffer = (char*) calloc( (strlen(home) + strlen("/.mole-index") + 1), sizeof(char));
    strcat(strcat(tempbuffer, home), "/.mole-index");
    pathf = tempbuffer;
    
    // initialize command line arguments & check index file status
    readArgs(argc, argv, &pathd, &pathf, &t);
    indexStatus = lstat(pathf, &indexStat); // fstatat gives implicit declaration error
    
    if (DEBUGMAIN) printf("[main] pathd = \"%s\"\n[main] pathf = \"%s\"\n[main] t = %d\n", pathd, pathf, t);

    // initialize signal mask
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);  // "user initiated index" signal for indexer thread
                                // "start-up indexing completed" signal for main thread
    sigaddset(&mask, SIGUSR2);  // "exit" signal for indexer thread
    sigaddset(&mask, SIGALRM);  // "periodic index" signal for indexer thread
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
    
    // initialize thread_t struct to pass to the thread
    struct thread_t threadArgs;
    threadArgs.tid = 0;
    threadArgs.mtid = pthread_self();
    threadArgs.pathd = pathd;
    threadArgs.pathf = pathf;
    threadArgs.t = t;
    threadArgs.newIndex = indexStatus ? 1 : 0;
    threadArgs.pIndexStat = &indexStat;
    threadArgs.pMask = &mask;
    threadArgs.pmxIndexer = &mxIndexer;
    threadArgs.quitFlag = 0;

    printf("\nStarting **mole**.\n");

    // STARTUP INDEXING
    startupIndexing(indexStatus, &threadArgs);
    
    // if a prevous index file did not exist, wait for it to be created
    // wait for confirmation from indexer thread via SIGUSR1
    if (indexStatus != 0)
    {
        while (sigNo != SIGUSR1) sigwait(threadArgs.pMask, &sigNo);
        if (DEBUGMAIN) printf("[main] SIGUSR1 received from thread!\n");
    }
    
    // at this point index file exists and if periodic
    // indexing is set indexer thread is waiting for signals
    // and carrying out perodic indexing as required
    
    // USER INPUT
    sleep(1); // allow time for all messages to be displayed
    getUserInput(&threadArgs); // start accepting user commands
    
    // EXIT SEQUENCE
    exitSequence(&threadArgs);

    printf("Ending **mole**.\n");

    free(tempbuffer);
    return EXIT_SUCCESS;
}
