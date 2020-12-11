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

#define DEBUG 1
#define DEFAULT_MOLE_DIR_ENV "MOLE_DIR" 
#define DEFAULT_INDEX_PATH_ENV "MOLE_INDEX_PATH"
#define DEFAULT_INDEX_PATH "HOME"
#define DEFAULT_INDEX_FILENAME "/.mole-index"
#define MAX_PATH 255

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

typedef unsigned int UINT;

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
                if ((*pathd = malloc(strlen(optarg) + 1)) == NULL) ERR("malloc");
                strcpy(*pathd, optarg);
                break;
            case 'f':
                if(++fcount > 1) usage();
                if ((*pathf = malloc(strlen(optarg) + 1)) == NULL) ERR("malloc");
                strcpy(*pathf, optarg);
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

        if ( (*pathd = malloc(strlen(env)+1) ) == NULL) ERR("malloc");
        
        strcpy(*pathd, env);
    }

    if (fcount == 0) // assign $MOLE_INDEX_PATH or $HOME/.mole-index
    {
        char* env = getenv(DEFAULT_INDEX_PATH_ENV);
        
        if (env == NULL) // $MOLE_INDEX_PATH not set
        {
            env = getenv(DEFAULT_INDEX_PATH);            
            
            if ( (*pathf = malloc(strlen(env)+13) ) == NULL) ERR("malloc");  // should be 13?
            
            strcpy(*pathf, env);
            strcat(*pathf, DEFAULT_INDEX_FILENAME);            
        }
        else // $MOLE_INDEX_PATH set
        {
            if ((*pathf = malloc(strlen(env) + 1)) == NULL) ERR("malloc");
            strcpy(*pathf, env);            
        }
    }    

    if (tcount == 0) // periodic re-indexing disabled
    {
        *t = -1;
    }    

    if (argc>optind) usage();
}

void read_fsignature(char* fname)
{
    unsigned char sign[4];    
    int state;
    int fd;
    printf("Filename: %s, Signature: \n", fname);
    if((fd=open(fname, O_RDONLY)) < 0) ERR("open");
    if ((state = read(fd, &sign, 4)) < 0) ERR("read");
    printf("%x %x %x %x - \n", sign[0], sign[1],sign[2],sign[3]);//[0], sign[1]);
    if(close(fd)) ERR("fclose");


}


int index_dir(char* pathd)
{
    char cwdir[MAX_PATH];
    if (getcwd(cwdir, MAX_PATH) == NULL) ERR("getcwd");
    if (chdir(pathd) == -1) ERR("chdir");    
    if(DEBUG) printf("Directory changed to %s:\n", pathd);

    DIR* dirp;
    struct dirent* dp;
    struct stat filestat;
    int dirs=0,files=0,links=0, other=0;
    if (NULL == (dirp = opendir("."))) ERR("opendir");

    do 
    {
        errno = 0;
        if ((dp = readdir(dirp)) != NULL)
        {
            if (lstat(dp->d_name,&filestat) != 0) ERR("lstat");
            
            
            
            if (S_ISDIR(filestat.st_mode)) dirs++;
            else if (S_ISREG(filestat.st_mode))
            { 
                read_fsignature(dp->d_name);
                files++;
            }
            else if (S_ISLNK(filestat.st_mode)) links++;
            else other++;
        }        
    } while (dp != NULL);

    if (errno != 0) ERR("readdir");
    if (closedir(dirp) != 0) ERR("closedir");
    printf("Files:%d, Dirs%d, Links: %d, Other: %d\n" \
                    , files,dirs, links, other);    


    if(chdir(cwdir)) ERR("chdir");
    if(DEBUG) printf("Directory changed back to %s:\n", cwdir);

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
