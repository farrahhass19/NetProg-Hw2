/*
 * CSCI 4220 - Assignment 2 Reference Solution
 * Concurrent Chatroom Server (select() + pthread worker pool)
 * Classic IRC-style "/me" action messages: *username text*
 *
 * This program demonstrates:
 *   - I/O multiplexing with select()
 *   - Multi-threaded worker pool using pthreads
 *   - Thread-safe producer/consumer queues
 *   - Message broadcasting to multiple clients
 *   - Basic command handling (/who, /me, /quit)
 *
 * Build:
 *   clang -Wall -Wextra -O2 -pthread chatroom_server.c -o chatroom_server.out
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>

#define MAX_NAME     32
#define MAX_MSG      1024
#define MAX_CLIENTS  64
#define INBUF        2048

/* ---------------- Data Structures ---------------- */

typedef struct Job {
    int sender_fd;                  // The file descriptor (socket) of the client who sent the message
    char username[MAX_NAME];        // Username of the sender
    char msg[MAX_MSG];              // Raw message text sent by the client
    int flag;                       // 0 = send to everyone, 1 = send back to sender only, 2 = send to everyone but sender
    struct Job *next;               // Pointer to the next Job in the queue (linked-list structure)
} Job;

/*
 * Thread-safe FIFO queue structure.
 * Used for both job_queue (raw messages from clients)
 * and bcast_queue (formatted messages ready to broadcast).
 */
typedef struct Queue {
    Job *head;              // Pointer to the first Job in the queue
    Job *tail;              // Pointer to the last Job in the queue
    pthread_mutex_t mtx;    // Mutex to protect access to the queue
    pthread_cond_t cv;      // Condition variable for thread signaling
    int closed;             // Flag: 1 when queue is closed (no new Jobs)
} Queue;

static Queue job_queue, bcast_queue;

typedef struct thread_arg {
    Queue * job_queue;
    Queue * bcast_queue;
} thread_arg;

/* ---------------- Queue Utilities ---------------- */
static void q_init(Queue *q) {
    q->head = calloc(1, sizeof(Job)); //dummy head node
    q->head->next = NULL;
    q->tail = q->head;

    pthread_mutex_init(&(q->mtx), NULL);
    pthread_cond_init(&(q->cv), NULL);
}
static void q_close(Queue *q) {
    q->closed = 1;
}
static void q_push(Queue *q, Job *j) {
    pthread_mutex_lock(&(q->mtx));
    q->tail->next = j;
    q->tail = q->tail->next;
    pthread_cond_signal(&(q->cv));
    pthread_mutex_unlock(&(q->mtx));
}
static Job *q_pop(Queue *q) {
    pthread_mutex_lock(&(q->mtx));
    // Wait in a 'while' loop as long as the queue is empty
    while (q->head->next == NULL) {
        // (This check lets threads exit if you call q_close)
        if (q->closed) {
            pthread_mutex_unlock(&(q->mtx));
            return NULL; 
        }
        // ulock the mutex (so q_push can work)and re-locks mutex upon wakeup.
        pthread_cond_wait(&(q->cv), &(q->mtx));
    }
    // dequeue the job from the front
    Job * ret = q->head->next;
    q->head->next = ret->next;

    // if only jon set tail
    if (q->head->next == NULL) {
        q->tail = q->head;
    }

    pthread_mutex_unlock(&(q->mtx));
    return ret;
    /* // This is old code
    pthread_mutex_lock(&(q->mtx));
    Job * ret = q->head->next;
    q->head->next = q->head->next->next;
    pthread_mutex_unlock(&(q->mtx));
    return ret;
    */
}

void * worker(void * arg)
{
    Queue * job_queue = ((thread_arg *)arg)->job_queue;
    Queue * bcast_queue = ((thread_arg *)arg)->bcast_queue;
    //printf("ONLINE\n");
    for (;;)
    {
        Job * curr = q_pop(job_queue);
        if (curr == NULL) 
        {
            break; 
        }
        if (strncmp(curr->msg, "/who", 4) == 0)
        {
            curr->msg[0] = '\0'; //TODO: null message - when using broadcast, check for this and substitute list of client usernames
            curr->flag = 1;
        }
        else if (strncmp(curr->msg, "/me", 3) == 0)
        {
            printf("curr_msg_orig: %s\n", curr->msg);
            char msg[MAX_MSG+1]; //TODO: may be some size stuff here in corner case testing.
            for (int j = 0; j < strlen(curr->msg); j++)
            {
                if (curr->msg[j] == '\n')
                {
                    curr->msg[j] = '\0';
                }
            }
            snprintf(msg, MAX_MSG, "*%s%s*\n", curr->username, (curr->msg)+3);
            printf("msg: %s\n", msg);
            memcpy(curr->msg, msg, strlen(msg)+1);
            printf("curr_msg: %s\n", curr->msg);
        }
        else if (strncmp(curr->msg, "/quit", 5) == 0)
        {
            char msg[MAX_MSG+1];
            snprintf(msg, sizeof(msg), "%s left the chat.\n",curr->username);
            memcpy(curr->msg, msg, strlen(msg)+1);
            curr->flag = 3;
        }
        else if (strncmp(curr->msg, "/", 1) == 0) //bad command -> send private error message
        {
            char msg[MAX_MSG+1];
            snprintf(msg, sizeof(msg), "Invalid command. Type /who, /me, or /quit.\n");
            memcpy(curr->msg, msg, strlen(msg)+1);
            curr->flag = 1;
        }
        else
        {
            char msg[MAX_MSG+1]; //TODO: may be some size stuff here in corner case testing.
            snprintf(msg, sizeof(msg), "%s: %s", curr->username, (curr->msg));
            memcpy(curr->msg, msg, strlen(msg)+1);
        }
        q_push(bcast_queue, curr);
        printf("pushed!\n");
    }
    return NULL;
}

struct Message {
    char* data;
    struct Message* next;
};

void cleanup_and_exit(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    
    int client_size = 5;
    int client[5];
    int listenfd;
    int maxi = -1;
    struct Message* head = NULL;
    
    // Close all client connections
    for (int i = 0; i <= maxi; i++) {
        if (client[i] >= 0) {
            close(client[i]);
        }
    }
    
    // Close listening socket
    if (listenfd >= 0) {
        close(listenfd);
    }
    
    // Free message list
    struct Message* current = head;
    while (current != NULL) {
        struct Message* next = current->next;
        if (current->data != NULL) {
            free(current->data);
        }
        free(current);
        current = next;
    }
    
    printf("Cleanup complete. Exiting.\n");
    exit(0);
}

void err_quit(const char *, ...);

char* convertStringToLower(char *str) {
    char* ret = malloc(strlen(str) + 1);
    for (int i = 0; str[i] != '\0'; i++) {
        ret[i] = tolower(str[i]);
    }
    return ret;
}

/* ---------------- Main ---------------- */
int main(int argc, char **argv) {
    if (argc != 4)
    {
        perror("Wrong number of clients. Usage is ./chatroom_server.out <port> <num_workers> <max_clients>");
        exit(1);
    }
    int port = atoi(argv[1]);
    int num_workers = atoi(argv[2]);
    int max_clients = atoi(argv[3]);
    
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in servaddr, cliaddr;
    bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family      = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port        = htons(port);
    bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(listenfd, MAX_CLIENTS);

    int maxfd, maxi;
    int client[max_clients];
    char client_buff[max_clients][INBUF]; // TODO: change this and below to dynamically allocate memory when needed for space saving
    char client_names[max_clients][MAX_NAME];
    int i;
    char buf[MAX_MSG+1];
    fd_set	rset, allset;
    maxfd = listenfd;			/* initialize */
	maxi = -1;					/* index into client[] array */
	for (i = 0; i < max_clients; i++)
		client[i] = -1;			/* -1 indicates available entry */
	FD_ZERO(&allset);
	FD_SET(listenfd, &allset);

    q_init(&job_queue);
    q_init(&bcast_queue);

    // pthread_t tids[num_workers];
    pthread_t *tids = malloc(num_workers * sizeof(pthread_t));
    thread_arg targ;
    targ.job_queue = &job_queue;
    targ.bcast_queue = &bcast_queue;

    // Set up signal handlers for cleanup
	signal(SIGINT, cleanup_and_exit);   // Ctrl+C
	signal(SIGTERM, cleanup_and_exit);  // Termination signal
    
    for (i = 0; i <= num_workers; i++)
    {
        int err = pthread_create(tids+i, NULL, worker, &targ);
        pthread_detach(tids[i]);
    }
    
    for ( ; ; ) {
        //printf("Enter for loop (DEBUG)\n");
        pthread_mutex_lock(&(bcast_queue.mtx));
        while (bcast_queue.head->next != NULL)
        {
            printf("\nBEFORE POP\n");
            Job * j = bcast_queue.head->next;
            bcast_queue.head->next = j->next;
            // if only jon set tail
            if (bcast_queue.head->next == NULL) {
                bcast_queue.tail = bcast_queue.head;
            }
            printf("\nAFTER POP\n");
            for (i = 0; i <= maxi; i++)
            {
                if (j->flag == 1 && (client[i] == j->sender_fd))
                {
                    if (j->msg[0] == '\0')
                    {
                        char response[MAX_MSG] = "";//TODO: size issues may happen here, idk
                        snprintf(response, MAX_MSG, "Active users:\n");
                        write(j->sender_fd, response, strlen(response));
                        bzero(response, MAX_MSG);
                        for (int n = 0; n <= maxi; n++)
                        {
                            if (client[n] <= 0)
                            {
                                continue;
                            }
                            snprintf(response, MAX_MSG, " - %s\n", client_names[n]);
                            write(j->sender_fd, response, strlen(response));
                            bzero(response, MAX_MSG);
                        }
                    }
                    else
                    {
                        write(j->sender_fd, j->msg, strlen(j->msg));
                    }
                }
                if (j->flag == 2 && (client[i] != j->sender_fd))
                {
                    printf("SEND TO ALMOST EVERYONE\n");
                    write(client[i], j->msg, strlen(j->msg));
                }
                if (j->flag == 0)
                {
                    printf("SEND TO EVERYONE\n");
                    write(client[i], j->msg, strlen(j->msg));
                }
                if (j->flag == 3)
                {
                    printf("SEND EVERYONE A QUIT MESSAGE");
                    for (int n = 0; n <= maxi; n++)
                    {
                        if (strcmp(client_names[n], j->username) == 0)
                        {
                            close(client[n]);
                            FD_CLR(client[n], &allset);
                            client[n] = -1;
                            client_names[n][0] = '\0';
                            client_buff[n][0] = '\0';
                        }
                    }
                    write(client[i], j->msg, strlen(j->msg));
                }
            }
            free(j);
        }
        pthread_mutex_unlock(&(bcast_queue.mtx));
		rset = allset;		/* structure assignment */
        //printf("BEFORE SELECT\n");
        
        // Use a timeout so select() returns periodically to check broadcast queue
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 1000;  // 100ms timeout
        
		int nready = select(maxfd+1, &rset, NULL, NULL, &tv);
        //printf("AFTER SELECT\n");
        
        // If select timed out (nready == 0), continue to check broadcast queue
        if (nready == 0) {
            continue;
        }
		// Check for EOF on stdin (Ctrl+D)
		if (FD_ISSET(STDIN_FILENO, &rset)) {
            printf("Forced shut down (DEBUG)\n");
			char stdin_buf[1];
			if (read(STDIN_FILENO, stdin_buf, 1) <= 0) {
				printf("Shutting down server due to EOF.\n");
				cleanup_and_exit(0);
			}
			if (--nready <= 0)
				continue;
		}

		if (FD_ISSET(listenfd, &rset)) {	/* new client connection */
            //printf("Receive new client connection (DEBUG)\n");
			size_t clilen = sizeof(cliaddr);
			int connfd = accept(listenfd, (struct sockaddr *)&cliaddr, &clilen);
			for (i = 0; i < max_clients; i++)
				if (client[i] < 0) {
					client[i] = connfd;	/* save descriptor */
					break;
				}
			if (i == max_clients){
				close(connfd);
				FD_CLR(connfd, &allset);
                snprintf(buf, sizeof(buf), "Server is full, rejecting connection request.\n");
				write(connfd, buf, strlen(buf));
                close(connfd);
                printf("Connection rejected: server is full.\n");
                continue;
			}
			FD_SET(connfd, &allset);	/* add new descriptor to set */
			if (connfd > maxfd)
				maxfd = connfd;			/* for select */
			if (i > maxi)
				maxi = i;				/* max index in client[] array */
            bzero(buf, sizeof(buf));
            snprintf(buf, sizeof(buf), "Welcome to Chatroom! Please enter your username: \n");
            write(connfd, buf, sizeof(buf));
            bzero(buf, sizeof(buf));
			if (--nready <= 0)
            {
                continue;				/* no more readable descriptors */
            }
		}

		for (i = 0; i <= maxi; i++) {	/* check all clients for data */
            printf("Enter for loop for clients(DEBUG)\n");
            int sockfd, n;
            //printf("client %d: %d\n", i, client[i]);
			if ( (sockfd = client[i]) < 0)
				continue;
            //printf("1\n");
			if (FD_ISSET(sockfd, &rset)) {
                int tmp_flag1 = 2;
				if ( (n = read(sockfd, buf, MAX_MSG)) == 0) {
						/*4connection closed by client */
					printf("Client disconnected.\n");
					close(sockfd);
					FD_CLR(sockfd, &allset);
					client[i] = -1;
				}
                else if (client_names[i][0] == '\0') //first connection - user has sent in username
                {
                    buf[n-1] = '\0'; //get rid of newline
                    //check if username is already takenc
                    bzero(client_names[i], sizeof(client_names[i]));
                    bool name_taken = false;
                    for (int j = 0; j <= maxi; j++) {
                        // check if another active client already has this exact name
                        char* name_j = convertStringToLower(client_names[j]);
                        char* name_i = convertStringToLower(buf);
                        printf("client[j] name = %s, inputted name = %s\n",name_j,name_i);
                        if (i != j && client[j] >= 0 && strcmp(name_j, name_i) == 0) {
                            name_taken = true;
                            break;
                        }
                    }

                    if(name_taken)
                    {
                        // If the username is already taken prompt for another username
                        char tmp_msg[MAX_MSG];
                        snprintf(tmp_msg, sizeof(tmp_msg), "Username \"%s\" is already in use. Try another:\n", buf);
                        write(sockfd, tmp_msg, strlen(tmp_msg));
                    }
                    else
                    {
                        strncpy(client_names[i], buf, MAX_NAME);
                        client_names[i][MAX_NAME - 1] = '\0';
                        // Welcome message to just sender
                        Job * nj_welcome = calloc(1, sizeof(Job));
                        nj_welcome->sender_fd = sockfd;
                        strncpy(nj_welcome->username, client_names[i], MAX_NAME);
                        snprintf(nj_welcome->msg, MAX_MSG, "Let's start chatting, %s!\n", client_names[i]);
                        nj_welcome->flag = 1;
                        nj_welcome->next = NULL;
                        q_push(&bcast_queue, nj_welcome);
                        
                        // Joined message to everyone but sender
                        Job * nj_joined = calloc(1, sizeof(Job));
                        nj_joined->sender_fd = sockfd;
                        strncpy(nj_joined->username, client_names[i], MAX_NAME);
                        snprintf(nj_joined->msg, MAX_MSG, "%s joined the chat.\n", client_names[i]);
                        nj_joined->flag = 2; 
                        nj_joined->next = NULL;
                        q_push(&bcast_queue, nj_joined);
                    }
                }
                else
                {
                    buf[n] = '\0';
                    size_t len = strlen(client_buff[i]);
                    memcpy(client_buff[i]+len, buf, n+1);
                }
                if (buf[n-1] == '\n')
                {
                    Job * nj = calloc(1, sizeof(Job));
                    nj->sender_fd = sockfd;
                    memcpy(nj->username, client_names[i], strlen(client_names[i])+1);
                    memcpy(nj->msg, client_buff[i], strlen(client_buff[i])+1);
                    nj->next = NULL;
                    nj->flag = tmp_flag1;
                    bzero(client_buff[i], sizeof(client_buff[i]));
                    q_push(&job_queue, nj);
                    printf("PUSHED TO JOB QUEUE\n");
                }
				if (--nready <= 0)
                    printf("4\n");
					break;				/* no more readable descriptors */
			}
		}
	}
    q_close(&job_queue);
    q_close(&bcast_queue);
    free(tids);
}
