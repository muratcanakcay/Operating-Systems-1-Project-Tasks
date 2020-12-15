#include <asm-generic/errno-base.h>
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
#define DEBUGTHREAD 1
#define DEBUGINDEXING 1
#define DEBUGWRITEFILE 1
#define DEBUGREADFILE 1
#define DEBUGCOUNT 1

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

typedef struct {
    char name[MAX_FILE]; // filename
    char path[MAX_PATH]; //absolute path
    off_t size; // file size in bytes
    uid_t uid;  // owner's uid
    enum ftype type; // file type
} finfo;
typedef struct thread_t {
    pthread_t tid;
    char* pathd;
    char* pathf;
    unsigned short t;
    bool indexExists;
    struct stat* indexBuffer;
} thread_t;


// TO BE DONE: revise displayHelp to be more "helpful"
void displayHelp(){
    printf("\ntest         : Tests reading from indexfile.\n");
    
    printf("\nexit         : starts a termination procedure – the program \
    stops reading commands from stdin. If an indexing is currently in progress, \
    the program waits for it to finish (including writing the result to the file) \
    and then the program ends\n\n");

    printf("exit!        : quick termination – the program stops reading \
    commands from stdin. If any indexing is in progress it is canceled. If the result \
    of the indexing if currently being written to a file, the program waits for it to \
    be finished (it is required that after the program termination the index file is \
    not broken in any way, for instance by unfinished writes).\n\n");

    printf("index        : if there is no currently running indexing \
    operation a new indexing is stated in background and the program immediately \
    starts waiting for the next command. If there is currently running indexing \
    operation a warining message is printed and no additional tasks are performed.\n\n");

    printf("count        : calculates the counts of each file type in index \
    and prints them to stdout.\n\n");

    printf("largerthan x : x is the requested file size. Prints full path, \
    size and type of all files in index that have size larger than x.\n\n");

    printf("namepart y   : y is a part of a filename, it may contain spaces. \
    Prints the same information as previous command about all files that contain y in the \
    name.\n\n");

    printf("owner uid    : uid is owner's identifier. Same as the previous one \
    but prints information about all files that owner is uid.\n\n");

    printf("help         : prints this help message.\n\n");
}
void usage(){
    fprintf(stderr,"\n\e[4mUSAGE\e[0m: mole ([-d \e[4mpathd\e[0m] [-f \e[4mpathf\e[0m] [-t \e[4mn\e[0m])\n\n");
    fprintf(stderr,"\e[4mpathd\e[0m : the path to a directory that will be traversed, if the option is not present a path set in an environment variable $MOLE_DIR is used. If the environment variable is not set the program end with an error.\n\n");
    fprintf(stderr,"\e[4mpathf\e[0m : a path to a file where index is stored. If the option is not present, the value from environment variable $MOLE_INDEX_PATH is used. If the variable is not set, the default value of file `.mole-index` in user's home directory is used\n\n");
    fprintf(stderr,"\e[4mn\e[0m : is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index. This parameter is optional. If it is not present, the periodic re-indexing is disabled\n\n");
    exit(EXIT_FAILURE); 
}
void readArgs(int argc, char** argv, char** pathd, char** pathf, unsigned short* t) {
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
char* typeToText(int type) {   // returns type based on enum
    if (type == 0) return "dir";
    else if (type == 1) return "jpg";
    else if (type == 2) return "png";
    else if (type == 3) return "gzip";
    else if (type == 4) return "zip";
    else if (type == 5) return "other";
    return "error";
}
enum ftype getType(const char* fname) { // returns file type based on signature
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
    
    if ((state = read(fd, sig, 8)) < 0) ERR("read");
    if (close(fd)) ERR("fclose");

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
int addToTempFile(const char* fpath, const char* fname, off_t fsize, uid_t fuid, enum ftype ftype) {
    int state;
    finfo fileinfo;
    memset(&fileinfo, 0, sizeof(finfo)); // probably not necessary?

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

    if ((state = write(tempfile, &fileinfo, sizeof(finfo))) <= 0) ERR("write");

    if (DEBUGWRITEFILE) printf("[addToTempFile] Finished writing %d bytes (should be sizeof(finfo) = %lu bytes)\n\n", state, sizeof(finfo));

    return 0;
}
int walkTree(const char* name, const struct stat* s, int type, struct FTW* f) {
    
    char* path;
    enum ftype ftype;

    errno = 0;
    
    if ((path = realpath(name, NULL)) == NULL) 
    {        
        ERR("realpath");
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
    
    return 0;
}
void testRead(const char* pathf) { // will be completely removed eventually
    if ((tempfile = open(pathf, O_RDONLY)) < 0) ERR("open");

    finfo fileinfo;
    memset(&fileinfo, 0, sizeof(finfo)); // probably not necessary?

    int state;
    while ((state = read(tempfile, &fileinfo, sizeof(finfo))) > 0)
    {
        if (DEBUGREADFILE)
        {
            printf("[testRead] Reading from file %s:\n", realpath(pathf, NULL));
            printf("[testRead] Abs. path: %s\n", fileinfo.path);
            printf("[testRead] File name: %s\n", fileinfo.name);
            printf("[testRead] File size: %lu\n", fileinfo.size);
            printf("[testRead] File uid: %d\n", fileinfo.uid);
            printf("[testRead] File type: %s\n", typeToText(fileinfo.type));
            printf("[testRead] Read %d bytes (should be sizeof(finfo) = %lu bytes)\n\n", state, sizeof(finfo));
        }
    }
    if (state < 0) ERR("read");

    if (close(tempfile)) ERR("close");
}
int indexDir(const char* pathd, const char* pathf) {
    // open temp file for writing and assign to global file descriptor
    // nftw() will write to the file at each step
    if ((tempfile=open("./temp",O_WRONLY|O_CREAT|O_TRUNC|O_APPEND,0777)) < 0) ERR("open");

    //start tree walk process
    if ( 0 != nftw(pathd, walkTree, MAXFD, FTW_PHYS))
        printf("%s: cannot access\n", pathd);

    // close temp file
    if (close(tempfile)) ERR("close");
    
    // atomically rename temp file to actual file
    int state = 0;
    
    // while NOT EBUSY so that if index is open in another stream it waits?
    do
    {
        state = rename("temp", pathf);
    } while (state == EBUSY);
    if (state != 0) ERR("rename");

    return 0;
}
int count(const char* pathf) {
    if ((tempfile=open(pathf,O_RDONLY)) < 0) ERR("open");

    finfo fileinfo;
    memset(&fileinfo, 0, sizeof(finfo)); // probably not necessary?

    int state, dir = 0, jpg = 0, png = 0, gzip = 0, zip = 0;
    while((state = read(tempfile, &fileinfo, sizeof(finfo))) > 0)
    {
        if (DEBUGCOUNT)
        {
            printf("[count] Reading from file %s:\n", realpath(pathf, NULL));
            printf("[count] Abs. path: %s\n", fileinfo.path);
            printf("[count] File name: %s\n", fileinfo.name);
            printf("[count] File size: %lu\n", fileinfo.size);
            printf("[count] File uid: %d\n", fileinfo.uid);
            printf("[count] File type: %s\n", typeToText(fileinfo.type));
            printf("[count] Read %d bytes (should be sizeof(finfo) = %lu bytes)\n\n", state, sizeof(finfo));
        }

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

    if (close(tempfile)) ERR("close");

    printf("Files count: dir:%d, jpg:%d, png:%d, gzip:%d, zip: %d\n", dir, jpg, png, gzip, zip);

    return 0;
}

void* threadWork(void* voidArgs)
{
    thread_t* threadArgs = voidArgs;

    if (DEBUGTHREAD) printf("[threadWork] Thread with TID: %lu started.\n", (unsigned long)threadArgs->tid);
    if (DEBUGTHREAD && threadArgs->indexExists) printf("[threadWork] %s status: %d last access: %lo last modify: %lo\n", threadArgs->pathf, threadArgs->indexExists, threadArgs->indexBuffer->st_atime, threadArgs->indexBuffer->st_mtime);
    if (DEBUGTHREAD && !threadArgs->indexExists) printf("[threadWork] %s status: %d Index file does not exist\n", threadArgs->pathf, threadArgs->indexExists);
    if (DEBUGTHREAD) printf("[threadWork] Current time: %lo\n", time(NULL));    
    
    while(true) sleep(1);

    return NULL;
}

int main(int argc, char** argv)
{	
    unsigned short t;
    int status;
    char *pathd = NULL, *pathf, *home;
    struct stat indexBuffer;
    
    // preassign pathf=$HOME/.mole_index (to be modified in readArgs() if necessary)
    home = getenv("HOME");
    char* tempbuffer = (char*) malloc( (strlen(home) + strlen("/.mole-index") + 1) * sizeof(char) );
    strcat(strcat(tempbuffer, home), "/.mole-index");
    pathf = tempbuffer;
    
    readArgs(argc, argv, &pathd, &pathf, &t);
    
    if (DEBUGMAIN) printf("[main] pathd = \"%s\"\n[main] pathf = \"%s\"\n[main] t = %d\n", pathd, pathf, t);

    status = lstat(pathf, &indexBuffer); // fstatat gives implicit declaration error
  
    if (status == 0 && t == 0) // no indexing necessary at this stage
    {
        printf("\nIndex file %s exists.\nPeriodic indexing disabled.\n", pathf);
    }
    else if (status == 0 || (status == -1 && errno == ENOENT))// create thread for indexing
    {
        struct thread_t threadArgs; 
        threadArgs.pathd = pathd;
        threadArgs.pathf = pathf;
        threadArgs.t = t;
        threadArgs.indexExists = status + 1;
        threadArgs.indexBuffer = &indexBuffer;
    
        if (pthread_create(&threadArgs.tid, NULL, threadWork, &threadArgs)) ERR("pthread_create");

        if (DEBUGMAIN) printf("[main] Thread with TID: %lu started.\n", (unsigned long)threadArgs.tid);    
    }    
    
    /*
    
    if ( (indexfile = open(pathf, O_RDONLY)) < 0 ) // index file does not exist
    {
        printf("\nIndex file %s does not exist.\nPlease wait for indexing to finish...\n", pathf);
        
        // TO BE IMPLEMENTED: create thread for indexing
        indexDir(pathd, pathf); // thread will run this (in a loop if -t is set)

        // wait for thread to finish.

        printf("Indexing finished and index file %s created succesfully.\n", pathf);
    }

    */

    // at this point index file exists

    // TO BE IMPLEMENTED: if periodic indexing is set start thread for that

    sleep(1);

    // wait for user input until exited
    while(true)
    {
        char buf[256] = {'\0'};

        printf("\nEnter command (\"help\" for list of commands): ");
        fflush(stdin);
        fgets(buf, 255, stdin);          
        
        if (memcmp(buf, "test\n", 5) == 0)
        {            
            //requires DEBUGREADTEMPFILE 1
            testRead(pathf); //test reading from pathf file
        }
        else if (memcmp(buf, "exit\n", 5) == 0)
        {
            break; // TO BE IMPLEMENTED: TERMINATION PROCEDURE
        }
        else if (memcmp(buf, "exit!\n", 6) == 0)
        {
            break; // TO BE IMPLEMENTED: QUICK TERMINATION
        }        
        else if (memcmp(buf, "index\n", 6) == 0)
        {
            indexDir(pathd, pathf); // TO BE IMPLEMENTED: THREAD! and warning if already indexing
        }
        else if (memcmp(buf, "count\n", 6) == 0)
        {
            count(pathf);             
        }
        else if (memcmp(buf, "help\n", 5) == 0)
        {
            displayHelp();            
        }
    }



    







    
    

    

    free(tempbuffer);    
    return EXIT_SUCCESS;
}
