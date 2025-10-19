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
    q->tail = q->head;
}
static void q_close(Queue *q) {
    q->closed = 1;
}
static void q_push(Queue *q, Job *j) {
    pthread_mutex_lock(&(q->mtx));
    q->tail->next = j;
    q->tail = q->tail->next;
    pthread_mutex_unlock(&(q->mtx));
}
static Job *q_pop(Queue *q) {
    pthread_mutex_lock(&(q->mtx));
    Job * ret = q->head->next;
    q->head->next = q->head->next->next;
    pthread_mutex_unlock(&(q->mtx));
    return ret;
}

void * worker(void * arg)
{
    Queue * job_queue = ((thread_arg *)arg)->job_queue;
    Queue * bcast_queue = ((thread_arg *)arg)->bcast_queue;
    for (;;)
    {
        if (job_queue->head->next == NULL)
        {
            continue;
        }
        else
        {
            Job * curr = q_pop(job_queue);
            if (strncmp(curr->msg, "/who", 4) == 0)
            {
                curr->msg[0] = '\0'; //TODO: null message - when using broadcast, check for this and substitute list of client usernames
                curr->flag = 1;
            }
            else if (strncmp(curr->msg, "/me", 3) == 0)
            {
                char msg[MAX_MSG+1]; //TODO: may be some size fuckery here in corner case testing.
                snprintf(msg, sizeof(msg), "*%s%s*", curr->username, (curr->msg)+3);
                memcpy(curr->msg, msg, strlen(msg)+1);
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
                char msg[MAX_MSG+1]; //TODO: may be some size fuckery here in corner case testing.
                snprintf(msg, sizeof(msg), "%s: %s", curr->username, (curr->msg));
                memcpy(curr->msg, msg, strlen(msg)+1);
            }
            q_push(bcast_queue, curr);
        }
    }
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

    // pthread_t tids[num_workers];
    pthread_t *tids = malloc(num_workers * sizeof(pthread_t));
    thread_arg targ;
    targ.job_queue = &job_queue;
    targ.bcast_queue = &bcast_queue;

    // Set up signal handlers for cleanup
	signal(SIGINT, cleanup_and_exit);   // Ctrl+C
	signal(SIGTERM, cleanup_and_exit);  // Termination signal

    // Segfaulting in this loop don't know why
    printf("Hit here 1\n");
    for (i = 0; i < num_workers; i++)
    {
        int err = pthread_create(tids+i, NULL, worker, &targ);
        pthread_detach(tids[i]);
    }
    printf("Hit here 2\n");
    for ( ; ; ) {
		rset = allset;		/* structure assignment */
		size_t nready = select(maxfd+1, &rset, NULL, NULL, NULL);

		// Check for EOF on stdin (Ctrl+D)
		if (FD_ISSET(STDIN_FILENO, &rset)) {
			char stdin_buf[1];
			if (read(STDIN_FILENO, stdin_buf, 1) <= 0) {
				printf("Shutting down server due to EOF.\n");
				cleanup_and_exit(0);
			}
			if (--nready <= 0)
				continue;
		}

		if (FD_ISSET(listenfd, &rset)) {	/* new client connection */
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
			}

			FD_SET(connfd, &allset);	/* add new descriptor to set */
			if (connfd > maxfd)
				maxfd = connfd;			/* for select */
			if (i > maxi)
				maxi = i;				/* max index in client[] array */
            bzero(buf, sizeof(buf));
            snprintf(buf, sizeof(buf), "Welcome to Chatroom! Please enter your name: \n");
            write(connfd, buf, sizeof(buf));
            bzero(buf, sizeof(buf));
			if (--nready <= 0)
				continue;				/* no more readable descriptors */
		}

		for (i = 0; i <= maxi; i++) {	/* check all clients for data */
            int sockfd, n;
			if ( (sockfd = client[i]) < 0)
				continue;
			if (FD_ISSET(sockfd, &rset)) {
                int tmp_flag1 = 0;
				if ( (n = read(sockfd, buf, MAX_MSG)) == 0) {
						/*4connection closed by client */
					printf("Client disconnected.\n");
					close(sockfd);
					FD_CLR(sockfd, &allset);
					client[i] = -1;
				}
                else if (client_names[i] == '\0') //first connection - user has sent in username
                {
                    buf[n] = '\0';
                    memcpy(client_names[i], buf, n+1);
                    bzero(buf, sizeof(buf));
                    snprintf(buf, sizeof(buf), "Let's start chatting, %s!\n", client_names[i]);
                    write(sockfd, buf, sizeof(buf));
                    bzero(buf, sizeof(buf));
                    snprintf(buf, sizeof(buf), "%s joined the chat.\n", client_names[i]);
                    memcpy(client_buff[i], buf, n+20);
                    n = n+20;
                    tmp_flag1 = 2;
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
                    q_push(&job_queue, nj);
                }
				if (--nready <= 0)
					break;				/* no more readable descriptors */
			}
		}
	}
    free(tids);
}
