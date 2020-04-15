//Shell by SR Yoon
//2019.03.21

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

#define INITIAL_STAT 1
#define END_STAT -1
#define BUILTIN_STAT -2

#define NO_BUILTIN_COMM -1

#define MAX_STR_SIZE 256
#define DELIMITER " \t\r\n\a"
#define BACH_DELIMITER ";\n"
#define LINE_DELIMITER "\n"


void shl_loop(char* bach_file);
char* read_file(char *arg);
char** strtoked_line(char *arg, char *delimiter);
char* builtin_comm[] = { "cd", "quit" };

int command_launch(char **args);
int process_run(char **args);
int check_built_comm(char *arg);
int built_comm_launch(int comm, char **args);

void shl_cd(char **args);
void shl_quit(void);




int main(int argc, char **argv){

  if(argc == 1) //interactive mode
    shl_loop(NULL);
  else
    shl_loop(argv[1]); //batch mode

  return EXIT_SUCCESS;
}


void shl_loop(char* bach_file){
  size_t buf_size = MAX_STR_SIZE;

  char *line = NULL;
  char **args = malloc(MAX_STR_SIZE * sizeof(char*));
  char **args2 = malloc(MAX_STR_SIZE * sizeof(char*));
  char **args3 = malloc(MAX_STR_SIZE * sizeof(char*));
  int len_args;
  int stat;


  if(bach_file == NULL){ // if it is interactive mode
    while(1){
      printf("prompt> ");
      if(getline(&line, &buf_size, stdin)==-1){ //if it takes EOF then exit
        printf("\n");
        return ;
      }
      args = strtoked_line(line, BACH_DELIMITER); // input is stroked

      if(args!=NULL){

          len_args = sizeof(args) / sizeof(args[0]);
          pid_t pid[len_args];
          int i=0;
          for(i=0;args[i]!=NULL;i++){
            args2 = strtoked_line(args[i], DELIMITER);
            pid[i] = command_launch(args2); //then each arguments of input string will be launched as command
          }
          len_args = i;
          for(int i=0;i<len_args;i++){
            if(pid[i] != BUILTIN_STAT && pid[i] != END_STAT)
              waitpid(pid[i], NULL, 0);
            else if (pid[i] == END_STAT)
              stat = END_STAT;
          }
          if (stat == END_STAT)
            exit(0);
      }
    }
  }else{ // if it is batch mode
    line = malloc(MAX_STR_SIZE * sizeof(char*));
    line = read_file(bach_file);
    if(line == NULL)
      return;
    args = strtoked_line(line, LINE_DELIMITER); // line will be stroked
    for(int i=0;args[i]!=NULL;i++){
      args2 = strtoked_line(args[i], BACH_DELIMITER); // ; will be stroked

      int j=0;
      pid_t pid[len_args];
      for(j=0;args2[j]!=NULL;j++){
        args3 = strtoked_line(args2[j], DELIMITER); // " ", tab,, will be stroked
        pid[j] = command_launch(args3);
      }
      len_args = j;
      for(int j=0;j<len_args;j++){
        if(pid[j] != BUILTIN_STAT && pid[j] != END_STAT)
          waitpid(pid[j], NULL, 0);
        else if (pid[j] == END_STAT)
          stat = END_STAT;
      }
      if (stat == END_STAT){
        exit(0);
      }
    }
  }
}

char* read_file(char *arg){ //read files and return string
  int fd;
  char* s = malloc(MAX_STR_SIZE * sizeof(char));


  if ( 0 < ( fd = open(arg, O_RDONLY)))
  {
     read( fd, s, MAX_STR_SIZE);
     close( fd);
  }else{
     printf( "File Not Found Error.\n");
     return NULL;
   }
  return s;
}

char** strtoked_line(char *arg, char *delimiter){ // string will be stroked by delimiter
    char **args = malloc(MAX_STR_SIZE * sizeof(char*));
    char *temp_tok = malloc(MAX_STR_SIZE * sizeof(char));
    int pos = 0;

    temp_tok = strtok(arg, delimiter);
    if(temp_tok==NULL)
      return NULL;
    while(temp_tok){
      args[pos++] = temp_tok;
      temp_tok = strtok(NULL, delimiter);
    }
    free(temp_tok);
    return args;
}

int command_launch(char **args){ // check if it is shell builtin or not and launch process or command
  if(args==NULL)
    return BUILTIN_STAT;

  int comm = check_built_comm(args[0]);

  if(comm == NO_BUILTIN_COMM){
    int pid = process_run(args);
    return pid;
  }else
    return built_comm_launch(comm, args);

}

int process_run(char **args){
  pid_t pid;

  pid = fork();
  if(pid == 0){
    if(execvp(args[0], args) == -1){
      perror("");
      kill(getpid(), SIGINT);
      return BUILTIN_STAT;
    }
  }else if(pid < 0){
    perror("");
    return BUILTIN_STAT;
  }

  return pid;
}

int check_built_comm(char *arg){
  int len_builtin = sizeof(builtin_comm) / sizeof(char*);

  if(arg!=NULL){
    for(int i=0;i<len_builtin;i++){
      if(strcmp(arg, builtin_comm[i]) == 0)
        return i;
    }
  }
  return -1;
}

int built_comm_launch(int comm, char **args){
  switch(comm){

    case 0:
      shl_cd(args);
      return BUILTIN_STAT;
      break;

    case 1:
      return END_STAT;
    }
    return END_STAT;
}

void shl_cd(char **args){
  if (args[1] == NULL)
    printf("no argument");
  else {
    if (chdir(args[1]) != 0)
      perror("");
  }
}

void shl_quit(void){
  exit(0);
}
