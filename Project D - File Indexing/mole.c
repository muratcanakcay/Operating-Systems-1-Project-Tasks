#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>

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
                if(*t < 30 || t > 7200 || ++tcount > 1) usage();
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
        char* env = getenv("MOLE_DIR");        
        
        if (env == NULL) ERR("Program executed without -d argument but environment variable $MOLE_DIR not found.");
        if ( (*pathd = malloc(strlen(env)+1) ) == NULL) ERR("malloc");
        
        strcpy(*pathd, env);
    }

    if (fcount == 0) // assign $MOLE_INDEX_PATH or $HOME/.mole-index
    {
        char* env = getenv("MOLE_INDEX_PATH");
        
        if (env == NULL) // $MOLE_INDEX_PATH not set
        {
            env = getenv("HOME");            
            
            if ( (*pathf = malloc(strlen(env)+13) ) == NULL) ERR("malloc");  // should be 13?
            
            strcpy(*pathf, env);
            strcat(*pathf, "/.mole-index");            
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


int main(int argc, char** argv) 
{	
    int t;
    char *pathd = NULL, *pathf = NULL;
    
    readArguments(argc, argv, &pathd, &pathf, &t);

    printf("pathd = \"%s\"\npathf = \"%s\"\nt = %d\n", pathd, pathf, t);   
    
    return EXIT_SUCCESS;
}
