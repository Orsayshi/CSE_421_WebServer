//
// Created by  Gmyth on 17/6/5.
//
#define	BUF_LEN	8192

#include <dirent.h>
#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<ctype.h>
#include	<unistd.h>
#include	<sys/types.h>
#include	<sys/socket.h>
#include	<netdb.h>
#include  <getopt.h>
#include  <time.h>
#include  <errno.h>
#include  <sys/stat.h>
#include  <pthread.h>
#include  <semaphore.h>
#include  <sys/sendfile.h>
#include	<netinet/in.h>
#include	<inttypes.h>


char *progname;
char buf[BUF_LEN];

struct request
{
    char ip[21];
    char *request_type;
    char time_arrival[250];
    int  content_size;
    char content[601];
    char file_dir[601];
    char *content_type;
    char last_modified[250];
    char scheduled[250];
    struct request *tail;
};

struct invalid_request
{
    char ip_address[21];
    char time_arrival[250];
    char *msg;
    char index[5000];
    char content[300];
    char last_modified[250];
    struct invalid_request *tail;
};

void usage();
void setup_server();
void *scheduling(void *);
void *servicing(void *);
void *listening(void *);
void enqueue(struct request *rq);
void get_shortest_job();
int queue_size();
void file_log(char *info);
void send_err_feedback();
void queue_err_feedback(struct invalid_request *rq);
int req_parser(char ip[]);
int request_handler(struct request *rq);
void init_daemon();

int s;
int sock;
int ch;
int server;
int done;
int bytes;
int aflg;
int soctype = SOCK_STREAM;
int queuing_time = 60;
int threads = 4;
char *mode;
char *log_file = NULL;
char dir_buf[1000];
char hostname[128];
char *host = NULL;
char *port = NULL;
char *root = getcwd(dir_buf,1000);
sem_t *sem = (sem_t *)malloc(sizeof(sem_t *));
ssize_t trash = 0;
struct request *head = NULL;
struct invalid_request *errhead = NULL;
pthread_t scheduler_id,service_id,listen_id;
struct request *ready_rq = NULL;

pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t scheduler_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t output_lock=PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

// all flags
int debugging = 0;
int NOT_FCFS = 0;



extern char *optarg;
extern int optind;

int
main(int argc,char *argv[]) {
    if ((progname = rindex(argv[0], '/')) == NULL)
        progname = argv[0];
    else
        progname++;
    while ((ch = getopt(argc, argv, "dt:p:n:s:hr:l:")) != -1)
        switch (ch) {
            case 'd':
                // entering the debug mode
                debugging = 1;
                break;
            case 't':
                // Set the queuing time to time seconds. The default should be 60 seconds

                // make sure the arg is an integer
                if(atoi(optarg) != 0){
                  queuing_time = atoi(optarg);
                }
                else{
                  perror("Invalid Argument To Queuing Time");
                  exit(1);
                }
                break;
            case 'p':
                // Listen on the given port. If not provided, myhttpd will listen on port 8080.
                if(atoi(optarg) >= 0){
                  port = optarg;
                }
                else{
                  perror("Invalid Argument To Port Number");
                  exit(1);
                }
                break;
            case 'n':
                /*
                 * Set number of threads waiting ready in the execution thread pool to threadnum.
                 * The default should be 4 execution threads.
                 */
                if(atoi(optarg) >= 2){
                  threads = atoi(optarg);
                }
                else{
                  perror("Invalid Argument To Thread Number");
                  exit(1);
                }
                break;
            case 's':
                // Set the scheduling policy. It can be either FCFS or SJF. The default will be FCFS.
                if((strcmp(optarg, "SJF") == 0) || (strcmp(optarg, "FCFS") == 0)){
                  mode = optarg;
                  if (strcmp(mode, "SJF") == 0) {
                    NOT_FCFS = 1;
                  }
                }
                else{
                  perror("Invalid Argument To Scheduling Policy");
                  exit(1);
                }
                break;
            case 'h':
                // print usage with all the option and exit
                usage();
                break;
            case 'l':
                // Log all requests to the given file.
                //function needed
                log_file = optarg;
                if(log_file[0]!='/'){
                    log_file = log_file -1;
                }
                log_file[0]='/';
                char temp[1000];
                strcpy(temp,root);
                strcat(temp,log_file);
                strcpy(log_file,temp);
                break;
            case 'r':
                // Set the root directory for the http server to dir.
                root = optarg;
                if((chroot(root)<0) || (chdir(root)<0)){
                    fprintf(stderr,"Invalid Directory %s\n",root);
                    root = getcwd(dir_buf,1000);
                }
                fprintf(stderr,"Current working directory is %s\n",getcwd(dir_buf,1000));
                break;
            default:
                usage();
        }
    argc -= optind; // reduces the argument number by optind
    if (argc != 0) {
        usage();
    }

    if (debugging == 0) {
      init_daemon();
    }

    /*
     * Create socket on local host.
     */
    if ((s = socket(AF_INET, soctype, 0)) < 0) {
        perror("socket");
        exit(1);
    }

    setup_server();
    exit(0);
}
void *listening(void *nulptr){
/*
 * Set up select(2) on both socket and terminal, anything that comes
 * in on socket goes to terminal, anything that gets typed on terminal
 * goes out socket...
 */
    (void) nulptr;
    fd_set ready;
    struct sockaddr_in msgfrom;
    socklen_t msgsize;
    union {
        uint32_t addr;
        char bytes[4];
    } fromaddr;
    while (!done) {
        FD_ZERO(&ready);
        FD_SET(sock, &ready);
        FD_SET(fileno(stdin), &ready);
        if (select((sock + 1), &ready, 0, 0, 0) < 0) {
            perror("select");
            exit(1);
        }
        if (FD_ISSET(fileno(stdin), &ready)) {
            if ((bytes = read(fileno(stdin), buf, BUF_LEN)) <= 0){
                done++;
            }
        }

        msgsize = sizeof(msgfrom);
        if (FD_ISSET(sock, &ready)) {
            if ((bytes = recvfrom(sock, buf, BUF_LEN, 0, (struct sockaddr *)&msgfrom, &msgsize)) <= 0) {
                done++;
            } else if (aflg) {
                fromaddr.addr = ntohl(msgfrom.sin_addr.s_addr);
                fprintf(stderr, "%d.%d.%d.%d: ", 0xff & (unsigned int)fromaddr.bytes[0],
                        0xff & (unsigned int)fromaddr.bytes[1],
                        0xff & (unsigned int)fromaddr.bytes[2],
                        0xff & (unsigned int)fromaddr.bytes[3]);
            }
            char ip[21];
            fromaddr.addr = ntohl(msgfrom.sin_addr.s_addr);
            sprintf(ip, "%d.%d.%d.%d: ", 0xff & (unsigned int)fromaddr.bytes[0],
                    0xff & (unsigned int)fromaddr.bytes[1],
                    0xff & (unsigned int)fromaddr.bytes[2],
                    0xff & (unsigned int)fromaddr.bytes[3]);
            req_parser(ip);
        }
    }
}



void setup_server() {
    struct sockaddr_in serv, remote;
    struct servent *se;
    socklen_t newsock, len;
    len = sizeof(remote);
    memset((void *)&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    if (port == NULL)
        serv.sin_port = htons(8080);
    else if (isdigit(*port))
        serv.sin_port = htons(atoi(port));
    else {
        if ((se = getservbyname(port, (char *)NULL)) < (struct servent *) 0) {
            perror(port);
            exit(1);
        }
        serv.sin_port = se->s_port;
    }
    if (bind(s, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("bind");
        exit(1);
    }
    if (getsockname(s, (struct sockaddr *) &remote, &len) < 0) {
        perror("getsockname");
        exit(1);
    }
    hostname[127] = '\0';
    if (gethostname(hostname, 127) < 0) {
        perror("gethostname");
        exit(1);
    }
    fprintf(stderr, "Port number is %d\n", ntohs(remote.sin_port));
    listen(s, 1);
    newsock = s;
    if (soctype == SOCK_STREAM) {
        fprintf(stderr, "Entering accept() waiting for connection.\n");
        sem_init(sem,0,threads);
        do {
          sock = accept(s, (struct sockaddr *) &remote, &len);
          /*
          * Set up service thread and scheduler thread
          */
          //sem = sem_open("service",0,threads);

          pthread_t services[threads];
          for (int i = 1; i < threads; i++) {
              pthread_create(&services[i], NULL, &servicing, NULL);
              // create threads pool
          }
          pthread_create(&scheduler_id, NULL, scheduling, NULL);
          pthread_create(&listen_id, NULL, listening, NULL);
          pthread_join(listen_id, NULL);
          pthread_join(scheduler_id, NULL);
        } while(debugging == 0);
    }
}
/*
 * servicing method, working on request
 *
 */
void *servicing(void * pointer){
    (void) pointer;
     while(1){
         pthread_mutex_lock(&scheduler_lock);
         pthread_cond_wait(&cv,&scheduler_lock);
         struct request *input = ready_rq;
         pthread_mutex_unlock(&scheduler_lock);
         pthread_mutex_lock(&output_lock);
         request_handler(input);
         //printf("\nservice finished\n");
         pthread_mutex_unlock(&output_lock);
         sem_post(sem);
     }
}
/*
 * scheduling method, assign quest to worker
 *
 */
void *scheduling(void *nptr){
    (void *)nptr;
    if(head == NULL){
        if(debugging) {
            fprintf(stdout, "\nNo available request detected...\n");
        }
        send_err_feedback();
    }
    while (1) {
            sleep(queuing_time);
            if (head == NULL) {
                if(debugging) {
                    fprintf(stdout, "\nNo available request detected...\n");
                }
            } else {
                if(debugging) {
                    fprintf(stdout, "\nWaiting finished, start scheduled...\n");
                }
            }
            if (!NOT_FCFS) {
                //fprintf(stderr,"here 1\n");
                while (head != NULL) {
                    sem_wait(sem);
                    ready_rq = NULL;
                    pthread_mutex_lock(&scheduler_lock);
                    pthread_mutex_lock(&queue_lock);
                    time_t now;
                    time(&now);
                    struct tm *Current = gmtime(&now);
                    char current_ts[30];
                    //printf("\ncontent size:\"%d\"", size);
                    strftime(current_ts, 30, "%a, %d %b %Y %T GMT", Current);
                    ready_rq = head;
                    strcpy(ready_rq->scheduled, current_ts);
                    if(debugging){
                        fprintf(stdout,"Request start to scheduled, scheduled time : %s \n",ready_rq->scheduled);
                    }
                    // ready holds the scheduled request ready to assign to service
                    head = head->tail;
                    pthread_mutex_unlock(&queue_lock);
                    pthread_cond_signal(&cv);
                    pthread_mutex_unlock(&scheduler_lock);
                    sleep(1);
                }
            } else {
                //fprintf(stderr,"here 2\n");
                while (head != NULL) {
                    sem_wait(sem);
                    ready_rq = NULL;
                    pthread_mutex_lock(&scheduler_lock);
                    pthread_mutex_lock(&queue_lock);
                    get_shortest_job();
                    // ready holds the scheduled request ready to assign to service
                    //head = head->tail;
                    pthread_mutex_unlock(&queue_lock);
                    pthread_cond_signal(&cv);
                    pthread_mutex_unlock(&scheduler_lock);
                    sleep(1);
                }
            }
            send_err_feedback();
        if(debugging) {
            fprintf(stderr, "\nQueue gets emptied...\n");
        }
        }
}
/*
 * Request Enqueue;
 *
 */
void enqueue(struct request *rq){
    if(head == NULL ){
        head = rq;
    }else{
        struct request *temp = head;
        while(NULL!=temp->tail){
            temp = temp->tail;
        }
        temp->tail = rq;
    }
    if(debugging) {
        fprintf(stdout, "New request added, now size is: %d \n", queue_size());
    }
}
/*
 * parse the incoming request, maintian information
 *
 */
int
req_parser(char ip[]){
    if(debugging) {
        fprintf(stdout, "New Request detected, start parsing process....\n");
    }
    FILE *in;
    char content[300];
    strcpy(content,buf);
    char *Request_type = strtok(buf," ");
    char *dir = strtok(NULL," ");
    if(debugging) {
        fprintf(stdout, "Request_Type: %s\n", Request_type);
        fprintf(stdout, "dir: %s\n", dir);
    }
    char temp[1300];
    char dir_1[1300];
    strcpy(temp,root);
    if(dir != NULL){
        strcpy(dir_1,dir);
        if(dir_1[0]=='~'){
            dir = dir +1;// move one bit
            strcpy(dir_1,dir);
            strncat(temp,dir_1,strlen(dir_1)+strlen(temp)+1);
        }else{
            strcpy(temp,dir);
        }
    }
    char *type;
    time_t now;
    time(&now);
    struct tm * Current=gmtime(&now);
    char current_ts[30];
    //printf("\ncontent size:\"%d\"", size);
    strftime(current_ts, 30, "%a, %d %b %Y %T GMT", Current);
    //fprintf(stderr,"\ntime stamp:\"%s\"", current_ts);
    //printf("\nck 1");
    if(Request_type == NULL){
        Request_type = strtok(NULL," ");
    }
    if(Request_type == NULL){
        struct invalid_request *current;
        current = (struct invalid_request *)malloc(sizeof(struct invalid_request));
        strcpy(current->ip_address,ip);
        strcpy(current->content,content);
        strcpy(current->time_arrival,current_ts);
        strcpy(current->last_modified,current_ts);
        current->msg = (char*)"\nUnsupported request type\n";
        current->tail = NULL;
        queue_err_feedback(current);
        //write(sock,"\nunsuportted request type\n",26);
        return 5;
    }
    else{
        while(Request_type[0]=='\n'){
            Request_type = Request_type +1;
        }
        // printf("\nRequest_type: \"%s\"",Request_type);
        if(strcmp(Request_type,"GET")==0 || strcmp(Request_type,"HEAD")==0){
            // printf("\nck 2");
//            char *temp = (char *)malloc(strlen(dir)+strlen(def)+1);
//            strcpy(temp,def);
//            strcat(temp,dir);

            //fprintf(stderr,"\ntemp: \"%s\"",temp);
            in = fopen(temp,"rb");//in read mode
            if(in == NULL){
                struct invalid_request *current;
                current = (struct invalid_request *)malloc(sizeof(struct invalid_request));
                DIR  *d;
                struct dirent *dire;
                d = opendir(root);
                if (d)
                {
                    while ((dire = readdir(d)) != NULL)
                    {
                        char check[100];
                        strcpy(check,dire->d_name);
                        if(check[0]!='.') {
                            strcat(dire->d_name, "\n");
                            strcat(current->index, dire->d_name);
                        }
                    }
                    closedir(d);
                }
                strcpy(current->ip_address,ip);
                strcpy(current->time_arrival,current_ts);
                strcpy(current->content,content);
                strcpy(current->last_modified,current_ts);
                current->msg = (char*)"\nUnable to open file\n";
                current->tail = NULL;
                queue_err_feedback(current);
                //fprintf(stderr,"hit~\n");
                return 2;//no file
            }
            strtok(dir,".");
            type =strtok(NULL,".");
            //printf("\ncame to here");
            //printf("\ntype: \"%s\"",type);
            if(type==NULL){
                //write(sock,"\nunsuportted file type\n",23);
                struct invalid_request *current;
                current = (struct invalid_request *)malloc(sizeof(struct invalid_request));
                strcpy(current->ip_address,ip);
                strcpy(current->time_arrival,current_ts);
                strcpy(current->content,content);
                strcpy(current->last_modified,current_ts);
                current->msg = (char*)"\nCan't read file type from request\n";
                current->tail = NULL;
                queue_err_feedback(current);
                return 3; // unsuportted file
            }
            if(strcmp(type,"html")==0||strcmp(type,"txt")==0){
                type = (char*)"text/html";
            }else if(strcmp(type,"gif")==0||strcmp(type,"jpg")==0){
                type = (char*)"image/gif";
            }else{
                //write(sock,"\nunsuportted file type\n",23);
                struct invalid_request *current;
                current = (struct invalid_request *)malloc(sizeof(struct invalid_request));
                strcpy(current->ip_address,ip);
                strcpy(current->time_arrival,current_ts);
                strcpy(current->content,content);
                strcpy(current->last_modified,current_ts);
                current->msg = (char*)"\nFile type is not supported...\n";
                current->tail = NULL;
                queue_err_feedback(current);
                return 3;//no file
            }
            fseek(in, 0, SEEK_END); // seek to end of file
            int size = ftell(in);
            fclose(in);
            struct stat meta;
            stat(temp,&meta);
            char modified[30];
            strftime(modified,sizeof(modified),"%a, %d %b %Y %T GMT",gmtime(&(meta.st_mtime)));
            struct request *new_request;
            new_request = (struct request *)malloc(sizeof(struct request));
            new_request->content_size = size;
            new_request->content_type = type;
            strcpy(new_request->ip,ip);
            strcpy(new_request->content,content);
            strcpy(new_request->file_dir,temp);
            strcpy(new_request->last_modified ,modified);
            strcpy(new_request->time_arrival , current_ts);
            new_request->request_type = Request_type;
            new_request->tail = NULL;
//            free(in);
//            free(type);
//            free(Request_type);
//            free(dir);
//            free(def);
//            free(temp);
            if(debugging) {
                fprintf(stdout, "\nRequest_type: \"%s\"", new_request->request_type);
                fprintf(stdout, "\nRequest content_size: \"%d\"", new_request->content_size);
                fprintf(stdout, "\nRequest content_type: \"%s\"", new_request->content_type);
                fprintf(stdout, "\nRequest file_dir: \"%s\"", new_request->file_dir);
                fprintf(stdout, "\nRequest last_modified: \"%s\"", new_request->last_modified);
                fprintf(stdout, "\nRequest serverName: \"%s\"", (char *)hostname);
                fprintf(stdout, "\nRequest time_arrival: \"%s\"", new_request->time_arrival);
                fprintf(stdout, "\n");
            }
            enqueue(new_request);
        }else{
            // wrong request type
            struct invalid_request *current;
            current = (struct invalid_request *)malloc(sizeof(struct invalid_request));
            strcpy(current->ip_address,ip);
            strcpy(current->time_arrival,current_ts);
            strcpy(current->content,content);
            strcpy(current->time_arrival,current_ts);
            strcpy(current->last_modified,current_ts);
            current->msg = (char*)"\nWrong request! Only GET and HEAD request can be accepted.(if you get content:ET or EA, it means you send empty request)\n";
            current->tail = NULL;
            //fprintf(stderr,"\n-------------%s--------------\n",Request_type);
            queue_err_feedback(current);
            return 1;// 1 is the err code for req_parser can't find correct tyoe
        }
    }
    return 0;
}

int request_handler(struct request *rq){
    //int status = 400;
    char buf [200];
    time_t now;
    time(&now);
    struct tm * Current=gmtime(&now);
    char current_ts[30];
    //printf("\ncontent size:\"%d\"", size);
    strftime(current_ts, 30, "%a, %d %b %Y %T GMT", Current);
    if(rq == NULL){
        return -1;
    }
        // GET response
        //get file by directory, since the directory is already checked in parser function, no need re-check here
    char length_buffer[20];
        //strcpy(rq->last_modified ,current_ts);
    sprintf(length_buffer,"%d",rq->content_size); // convert int to char
    trash=write(sock,"HTTP/1.0 200 OK\n",16);
    trash=write(sock,"Date: ",6);
    trash=write(sock,current_ts,30);
    trash=write(sock,"\nServer: ",9);
    trash=write(sock,hostname,128);
    trash=write(sock,"\nLast-Modified: ",16);
    trash=write(sock,rq->last_modified,strlen(rq->last_modified));
    trash=write(sock,"\nContent-Type: text/html\n",25);
    trash=write(sock,"Content-Length: ",16);
    trash= write(sock,length_buffer,strlen(length_buffer));
    trash=write(sock,"\n\n",2);
    if(strcmp(rq->request_type,"GET")==0){
      FILE* in = fopen(rq->file_dir,"rb");
      int check =sendfile(sock,fileno(in),NULL,sizeof(buf)); //one for linux
      fclose(in);
    }
    trash=write(sock,"\n",1);
    if(close(sock) < 0){
      perror("Close Socket");
      exit(1);
    }
    char log_buf[3000];
    sprintf(log_buf,"%s - [%s] [%s] \"%s\" %d %d",rq->ip,rq->time_arrival,rq->scheduled,rq->content,200,rq->content_size);
    file_log(log_buf);
    if(debugging){
        fprintf(stdout,"%s",log_buf);
        fprintf(stdout,"\n");
    }

    if(rq != NULL) {
        free(rq);
    }
    return 0;
}
/*
 * helper function
 */
// print the size of current queue
int queue_size(){
    int count= 1;
    struct request *temp = head;
    //struct request *further;
    while(temp->tail!= NULL){
        temp = temp->tail;
        count = count +1;
    }
    return count;
}
void get_shortest_job(){
    int head_is_shortest = 1;
    struct request *parent = head; // previous node for shortest we find
    struct request *shortest = head; // shortest node
    struct request *tracker = head; // iterator
//    if(head->tail != NULL){
//        if((tracker->tail->content_size) < (shortest->content_size)){
//            head_is_shortest = false;
//            parent = tracker;
//            shortest = tracker->tail;
//        }
//    }
    while(tracker->tail!=NULL){
        if((tracker->tail->content_size) < (shortest->content_size)){
            head_is_shortest = 0;
            parent = tracker;
            shortest = tracker->tail;
        }
        tracker = tracker->tail;
    }
//    time_t now;
//    time(&now);
//    struct tm *Current = localtime(&now);
//    char current_ts[250];
//    printf("\ncontent size:\"%d\"", size);
//    strftime(current_ts, 250, "[%d/%b/%Y %H:%M:%S]", Current);

    if(!head_is_shortest){
        parent->tail = shortest->tail;
    }else{
        head = head ->tail;
    }
    time_t now;
    time(&now);
    struct tm *Current = gmtime(&now);
    char current_ts[30];
    strftime(current_ts, 30, "%a, %d %b %Y %T GMT", Current);
    ready_rq = head;
    strcpy(shortest->scheduled, current_ts);
    if(debugging){
        fprintf(stdout,"Request start to be scheduled, scheduled time : %s \n",shortest->scheduled);
    }
    ready_rq = shortest;

    //strcpy(ready_rq->last_modified, current_ts);
}
void send_err_feedback(){

    if(errhead == NULL){}//nothing
    else {
      while(NULL != errhead){
          if(errhead != NULL) {
            trash=write(sock, "HTTP/1.0 404 Not Found",22);
            trash=write(sock, "\nServer: ",9);
            trash=write(sock, hostname, 128);
            trash=write(sock, "\nLast Modified: ", 16);
            trash=write(sock, errhead->last_modified, strlen(errhead->last_modified));
            trash=write(sock, "\nERROR: ", 8);
            trash=write(sock, errhead->msg, strlen(errhead->msg));
            trash=write(sock, "\n", 1);
            trash=write(sock, "Content: ", 9);
            trash=write(sock, errhead->content, strlen(errhead->content));
            trash=write(sock, "\n", 1);
            if(strlen(errhead->index) > 0){
                trash=write(sock, "Directory: ", 11);
                trash=write(sock, errhead->index, strlen(errhead->index));
                trash=write(sock, "\n", 1);
            }
            if(close(sock) < 0){
              perror("Close Socket");
              exit(1);
            }
            char log_buf[3000];
            sprintf(log_buf,"%s - [%s] [%s] \"%s\" %d %d",errhead->ip_address,errhead->time_arrival,errhead->last_modified,errhead->content,404,0);
            file_log(log_buf);
            if(debugging){
                fprintf(stderr,"%s",log_buf);
                fprintf(stderr,"\n");
            }
          }
          errhead = errhead->tail;
      }

    }
}
void queue_err_feedback(struct invalid_request *rq){
    if(errhead == NULL ){
        errhead = rq;
        //fprintf(stderr,"now errhead: %s\n",errhead->content);
    }else{
        struct invalid_request *temp = errhead;
        while(NULL!=temp->tail){
            temp = temp->tail;
        }
        temp->tail = rq;
    }

}
void file_log(char *info){
    if(log_file!=NULL){
        FILE *out = fopen(log_file,"a");
        fprintf(out,"%s",info);
        fprintf(out,"\n");
        fclose(out);
    }
}
/*
 * usage - print usage string and exit
 */

 void init_daemon(){
    pid_t pid, sid;
    if((pid = fork()) < 0) {
        perror("daemon\n");
        exit(1);
    }
    if(pid > 0) {
        exit(0);
    }
    umask(0);
    sid = setsid();
    if((chdir("/"))<0)  {
        perror("daemon\n");
        exit(1);
    }
 }

void usage() {
    // change this to new usage
    fprintf(stderr, "usage: %s −d : Enter debugging mode. That is, do not daemonize, only accept one connection at a\n"
                    "                    time and enable logging to stdout. Without this option, the web server should run\n"
                    "                    as a daemon process in the background.\n", progname);
    fprintf(stderr, "usage: %s −h : Print a usage summary with all options and exit.\n", progname);
    fprintf(stderr, "usage: %s −l [filename] : Log all requests to the given file. See LOGGING for details.\n", progname);
    fprintf(stderr, "usage: %s −p [port] : Listen on the given port. If not provided, myhttpd will listen on port 8080.\n", progname);
    fprintf(stderr, "usage: %s −r [dir] : Set the root directory for the http server to dir.\n", progname);
    fprintf(stderr, "usage: %s −t [time] : Set the queuing time to time seconds. The default should be 60 seconds.\n", progname);
    fprintf(stderr, "usage: %s −n [threadnum]: Set number of threads waiting ready in the execution thread pool to threadnum.\n"
                    "                               The default should be 4 execution threads.\n", progname);
    fprintf(stderr, "usage: %s −s [sched] : Set the scheduling policy. It can be either FCFS or SJF. The default will be FCFS.\n", progname);
    exit(1);
}
