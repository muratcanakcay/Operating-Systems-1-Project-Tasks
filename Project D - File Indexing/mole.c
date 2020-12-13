#define _XOPEN_SOURCE 500
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
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <ftw.h>

#define DEBUG 1
#define DEFAULT_MOLE_DIR_ENV "MOLE_DIR" 
#define DEFAULT_INDEX_PATH_ENV "MOLE_INDEX_PATH"
#define DEFAULT_INDEX_FILENAME "/.mole-index"
#define MAX_PATH 255
#define MAXFD 100

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

typedef unsigned int UINT;

enum ftype {dir, jpeg, png, gzip, zip, other};

typedef struct 
{
    char* name; // filename
    char* path; //absolute path
    off_t size; // file size in bytes
    uid_t uid;  // owner's uid
    enum ftype type; // file type
} finfo;

void usage(){
    fprintf(stderr,"\n\e[4mUSAGE\e[0m: mole ([-d \e[4mpathd\e[0m] [-f \e[4mpathf\e[0m] [-t \e[4mn\e[0m])\n\n");
    fprintf(stderr,"\e[4mpathd\e[0m : the path to a directory that will be traversed, if the option is not present a path set in an environment variable $MOLE_DIR is used. If the environment variable is not set the program end with an error.\n\n");
    fprintf(stderr,"\e[4mpathf\e[0m : a path to a file where index is stored. If the option is not present, the value from environment variable $MOLE_INDEX_PATH is used. If the variable is not set, the default value of file `.mole-index` in user's home directory is used\n\n");
    fprintf(stderr,"\e[4mn\e[0m : is an integer from the range [30,7200]. n denotes a time between subsequent rebuilds of index. This parameter is optional. If it is not present, the periodic re-indexing is disabled\n\n");
    exit(EXIT_FAILURE); 
}
void readArguments(int argc, char** argv, char** pathd, char** pathf, int* t) {
	int c, dcount = 0, fcount = 0, tcount = 0;

    while ((c = getopt(argc, argv, "d:f:t:")) != -1)
        switch (c)
        {
            case 't':
                *t=atoi(optarg);
                if(*t < 30 || *t > 7200 || ++tcount > 1) usage();
                break;
            case 'd':
                if(++dcount > 1) usage();
                *pathd = *(argv + optind - 1);                                 
                break;
            case 'f':
                if(++fcount > 1) usage();                
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
        
        if (env == NULL) // $MOLE_INDEX_PATH not set
        {
            env = getenv("HOME");            
            *pathd = env;
        }
        else // $MOLE_INDEX_PATH set
        {
            *pathf = env;          
        }
    }    

    if (tcount == 0) // periodic re-indexing disabled
    {
        *t = -1;
    }    

    if (argc>optind) usage();
}
char* typeToText(int type) {   // returns type based on enum
    if(type == 0) return "dir";
    else if(type == 1) return "jpg";
    else if(type == 2) return "png";
    else if(type == 3) return "gzip";
    else if(type == 4) return "zip";
    return "other";
}

enum ftype getType(const char* fname) // returns type of file based on signature
{
    unsigned char sig[9];
    enum ftype type = 5;
    
    int state;
    int fd;
    if (DEBUG) printf("Filename: %s, Signature: \n", fname);
    if((fd=open(fname, O_RDONLY)) < 0) ERR("open");
    if ((state = read(fd, sig, 8)) < 0) ERR("read");            
    if(close(fd)) ERR("fclose");

    if (DEBUG) printf("%02x %02x %02x %02x %02x %02x %02x %02x - \n", sig[0], sig[1],sig[2],sig[3], sig[4], sig[5],sig[6],sig[7]);

    // how to elegantly store and use these?
    unsigned char jpg[] = { 0xff, 0xd8, 0xff };
    unsigned char png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };
    unsigned char gzip[] = { 0x1f, 0x8b };
    unsigned char zip[] = { 0x50, 0x4b, 0x03, 0x04 };
    unsigned char zip2[] = { 0x50, 0x4b, 0x05, 0x06 };
    unsigned char zip3[] = { 0x50, 0x4b, 0x07, 0x08 };    
    
    switch (sig[0]) 
    {
        case 0xff : // checking for jpg
            type = 1;
            for (int i = 1; i < 3; i++) 
            {
                if (DEBUG) printf("comparing %x to %x\n", sig[i], jpg[i]);
                if (sig[i] != jpg[i]) type = 5;                
            }
            break;
        case 0x89 : // checking for png
            type = 2;
            for (int i = 1; i < 8; i++) 
            {
                if (DEBUG) printf("comparing %x to %x\n", sig[i], png[i]);
                if (sig[i] != png[i]) type = 5;                
            }
            break;
        case 0x1f : // checking for gzip
            type = 3;
            if (DEBUG) printf("comparing %x to %x\n", sig[1], gzip[1]);
            if (sig[1] != png[1]) type = 5;                
            break;
        case 0x50 : // checking for zip
            type = 4;
            for (int i = 1; i < 4; i++)
            {
                if (DEBUG) printf("comparing %x to %x\n", sig[i], zip[i]);
                if (sig[i] != zip[i] && sig[i] != zip2[i] && sig[i] != zip3[i]) type = 5;
            }
            break;
    }    

    if (DEBUG) printf("File %s is \033[0;35m%s\033[0m\n", fname, typeToText(type));

    return type;
}

int walk(const char* name,const struct stat* s, int type, struct FTW* f)
{
    // printf("File: %s\n", name); // prints names of files traversed
    char path[MAX_PATH];

    switch (type)
    {
        case FTW_DNR:
        case FTW_D:                         
	        if(getcwd(path, MAX_PATH)==NULL) ERR("getcwd");
            printf("Path: %s Directory name: %s\n", path, name + f->base);            
            break;
        case FTW_F:             
	        if(getcwd(path, MAX_PATH)==NULL) ERR("getcwd");
            printf("Path: %s File name: %s\n", path, name  + f->base);
            getType(name + f->base);
            break;        
    }
    return 0;
}

int index_dir(char* pathd)
{
    // (to be implemented) 
    // OPEN FILE FOR WRITING HERE AND ASSIGN USE GLOBAL FILE DESCRIPTOR
    // nftw() will write to the file at each step
    
    
    
    if( 0 != nftw(pathd, walk, MAXFD, FTW_PHYS | FTW_CHDIR)) 
        printf("%s: cannot access\n", pathd);

    return 0;
}

int main(int argc, char** argv) 
{	
    int t;
    char *pathd = NULL, *pathf = NULL;
    
    readArguments(argc, argv, &pathd, &pathf, &t);
    
    if(DEBUG) printf("pathd = \"%s\"\npathf = \"%s\"\nt = %d\n", pathd, pathf, t);

    index_dir(pathd);
    
    return EXIT_SUCCESS;
}
