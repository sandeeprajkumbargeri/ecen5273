/*Author: Sandeep Raj Kumbrgeri
Date Published: 12/13/17
Description: This is a Web Proxy server which blocks blacklisted websites, caches responses with a timeout. All the functionality has been implemented with less robustness.

Written for the course ECEN 5273 - Network Systems at University of Colorado Boulder
*/


#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <stdlib.h>
#include <memory.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <semaphore.h>
#include <dirent.h>
#include <openssl/md5.h>

#define MAX_THREADS									1000
#define MAX_PENDING_CONNECTIONS			10
#define THREAD_OCCUPIED							0xFF
#define THREAD_FREE									0x00
#define KEEP_ALIVE_YES							0xFF
#define KEEP_ALIVE_NO								0x00
#define RESPONSE_NOT_READY					0x00
#define RESPONSE_READY							0xFF

#define BUFFER_SIZE									512
#define ENTITY_NOT_FOUND						0x00
#define ENTITY_FOUND								0xFF

typedef struct configuration					//for storing config file configuration
{
	unsigned int port;
	unsigned char document_root[512];
	unsigned char directory_index[64];
	unsigned char content_types[512];
} config_t;

typedef struct pthread_args				//agruments for passing from thread to thread
{
	int client_sock_arg;
	unsigned int thread_id_arg;
	unsigned char rx_buffer[BUFFER_SIZE];
} pthread_args_t;

typedef struct http_request					//http request header storing structure
{
	char method[8];
	char path[256];
	char version[16];
	char content_type[256];
	char content_length[32];
	char connection_timeout[64];
} http_request_t;

typedef struct http_response				//http response elements structure
{
	char version[16];
	char code[8];
	char description[256];
	char content_type[32];
	char content_length[32];
	char connection_timeout[64];
	char content[102400];
} http_response_t;

typedef struct cache
{
	FILE* file;
	char mdString[(2 * MD5_DIGEST_LENGTH) + 1];
	char filename[(2 * MD5_DIGEST_LENGTH) + 7];
	char path[128];
	unsigned char* stream;
	size_t size;
} cache_t;

typedef struct host_ip
{
	unsigned int entries;
	unsigned char host[1000][256];
	struct in_addr ip[1000];
} host_ip_t;


pthread_t thread_id[MAX_THREADS];
pthread_attr_t thread_attr;
sem_t sem_thread_slots, sem_thread[MAX_THREADS], sem_cache, sem_db;
config_t config;
unsigned char thread_slot_tracker[MAX_THREADS];
struct timeval tv;
unsigned char keep_alive[MAX_THREADS];
unsigned int timeout_seconds = 0;

void *accept_connection(void *);
void *parse(void *);
int parse_file(FILE*, unsigned char*, unsigned char*);
unsigned char search_config(unsigned char *, unsigned char *, unsigned char *);
void config_error_handler(unsigned char*, unsigned int);
int find_available_slot(void);
void set_thread_slot_state(int, unsigned char);
int get_matches(unsigned char*, unsigned char*, unsigned char*);
void send_http_response(int, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*);
size_t file_to_buffer(FILE*, unsigned char*);
void get_file_format(unsigned char*, unsigned char*);
void md5_digest(unsigned char*, size_t, unsigned char*);
void send_stream(int, unsigned char*, size_t);
size_t receive_cache_forward_stream(int, int, unsigned char*, size_t);
void* timeout(void* args);


int main (int argc, unsigned char* argv[])
{
	int proxy_sock, client_sock, sock_addr_size, thread_slot;
	struct sockaddr_in sin, client;     //"Internet socket address structure"
	int proxy_port = 0;
	unsigned char parsed_result[512];
	pthread_args_t args;

	for(int i = 0; i < MAX_THREADS; i++)				//clearing the thread tracker
		thread_slot_tracker[i] = THREAD_FREE;

	bzero(keep_alive, sizeof(keep_alive));

	tv.tv_sec = 1;							//setting the pipelining timeout to 0
	tv.tv_usec = 0;

	if (argc != 3)
	{
		printf ("\nUSAGE: %s <port number> <timeout in seconds>\n", argv[0]);
		exit(1);
	}

	proxy_port = atoi(argv[1]);
	timeout_seconds = atoi(argv[2]);

	if((timeout_seconds <= 0) || (timeout_seconds > 1000))
	{
		printf("## ERROR ## Timeout needs to be an integer between 1 and 1000.\n");
		exit(1);
	}

  //This code populates the sockaddr_in struct with the information about our socket
	bzero(&sin,sizeof(sin));                    //zero the struct
	sin.sin_family = AF_INET;                   //address family
	sin.sin_port = htons(proxy_port);        //htons() sets the port # to network byte order
	sin.sin_addr.s_addr = inet_addr("127.0.0.1");           //supplies the IP address of the local machine

//Causes the system to create a generic socket of type UDP (datagram)
	if((proxy_sock = socket(sin.sin_family, SOCK_STREAM, 0)) < 0)
	{
		printf("## ERROR ##  Unable to create socket.\n");
		exit(1);
	}

	else
		printf("## SERVER ##  Socket successfully created\n");

//Once we've created a socket, we must bind that socket to the local address and port we've supplied in the sockaddr_in struct
	if(bind(proxy_sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		printf("## ERROR ##  Unable to bind socket.\n");
		exit(1);
	}

	sock_addr_size = sizeof(struct sockaddr_in);

	//listen for active TCP connections
	if(listen(proxy_sock, MAX_PENDING_CONNECTIONS) != 0)
	{
		printf("## ERROR ## Listening Failed.\n");
		exit(1);
	}

	printf("## SERVER ## Listening!\n");

	//initialize the thread attributes and semaphores for sync
	pthread_attr_init(&thread_attr);
	sem_init(&sem_thread_slots, 0, 0);
	sem_init(&sem_cache, 0, 0);
	sem_init(&sem_db, 0, 0);
	sem_post(&sem_thread_slots);
	sem_post(&sem_cache);
	sem_post(&sem_db);

	for(int i = 0; i < MAX_THREADS; i++)
		sem_init(&sem_thread[i], 0, 0);

	system("exec rm -r cache/*");
	remove("./host_ip.db");

	while(1)
	{
		//wait for connections
		if((client_sock = accept(proxy_sock, (struct sockaddr *) &client, (socklen_t*) &sock_addr_size)) < 0)
    {
			printf("## ERROR ## Unable to accept connection.\n");
			continue;
		}

		else
		{
			while((thread_slot = find_available_slot()) < 0)
				printf("## ERROR ## All threads are currently in use.\n");

			//setting the thread we're about to use to occupied state
			set_thread_slot_state(thread_slot, THREAD_OCCUPIED);

			printf("## SERVER ## Thread %d -> Accept thread.\n", thread_slot);
			args.client_sock_arg = client_sock;
			args.thread_id_arg = thread_slot;
			pthread_create(&thread_id[thread_slot], &thread_attr, accept_connection, (void *) &args);
			sem_wait(&sem_thread[thread_slot]);
		}
	}
}

void *accept_connection(void *args)				//here is a thread for every active connection
{
	int rx_length = 0, thread_slot = 0;
	pthread_args_t *accept_args_ptr = args;
	pthread_args_t accept_args = *accept_args_ptr;
	pthread_args_t parse_args;
	sem_post(&sem_thread[accept_args.thread_id_arg]);

	parse_args.client_sock_arg = accept_args.client_sock_arg;

	//printf("The thread ID is %u.\n", thread_args->thread_id_arg);
	bzero(parse_args.rx_buffer, BUFFER_SIZE);
	while((rx_length = recv(accept_args.client_sock_arg, parse_args.rx_buffer, BUFFER_SIZE, 0)) > 0)
  {
		//printf("%srx_length = %d\nstr_len = %lu\n", parse_args.rx_buffer, rx_length, strlen(parse_args.rx_buffer));

		while((thread_slot = find_available_slot()) < 0)
			printf("## ERROR ## All threads are currently in use.\n");

		//setting thread slot to occupied
		set_thread_slot_state(thread_slot, THREAD_OCCUPIED);

		printf("## SERVER ## Thread %d -> Parsing.\n", thread_slot);
		parse_args.thread_id_arg = thread_slot;

		pthread_create(&thread_id[thread_slot], &thread_attr, parse, (void *) &parse_args);

		//wait for the child thead to give the semaphore
		sem_wait(&sem_thread[thread_slot]);

		bzero(parse_args.rx_buffer, BUFFER_SIZE);

		//if(keep_alive[thread_slot] == KEEP_ALIVE_YES)
		//setsockopt(accept_args.client_sock_arg, SOL_SOCKET, SO_RCVTIMEO, (const unsigned char*) &tv, sizeof(struct timeval));

		/*else if(keep_alive[thread_slot] == KEEP_ALIVE_NO)
			break;*/
  }

	//if broken suddenly due to no keep-alive, wait for the child thread to finish
	pthread_join(thread_id[thread_slot], NULL);
	close(accept_args.client_sock_arg);	//close the client socket
	set_thread_slot_state(accept_args.thread_id_arg, THREAD_FREE); //set the client sock to free
	keep_alive[thread_slot] = KEEP_ALIVE_NO;		//reset the keep-alive flag for that thread
	printf("## SERVER ## Thread #%d -> Accept thread closing.\n", accept_args.thread_id_arg);
}

void *parse(void *args)
{
	pthread_args_t *parse_args_ptr = args;
	pthread_args_t parse_args;
	http_request_t request_header;
	http_response_t response_header;

	unsigned char resource_id[512], parsed_result[512];
	unsigned char *response;
	size_t response_length;

	struct hostent *remote_host_ent;
	struct in_addr **remote_addr_list;
	//bzero(remote_addr_list, sizeof(remote_addr_list));
	int sock_outgoing;

	FILE* db_file;
	unsigned char host_ip_found = 0;
	host_ip_t ip_database;
	bzero(&ip_database, sizeof(ip_database));

	cache_t cache;
	size_t bytes_read;
	struct sockaddr_in server_sock_addr;
	int sem_slot_match;

	bzero(&parse_args, sizeof(parse_args));
	parse_args = *parse_args_ptr;

	bzero(&request_header, sizeof(request_header));
	bzero(&response_header, sizeof(response_header));
	bzero(parsed_result, sizeof(parsed_result));
	bzero(resource_id, sizeof(resource_id));

	/*if((get_matches(parse_args.rx_buffer, "Connection: ", parsed_result) == 1))
		memcpy(request_header.connection_timeout, parsed_result, strlen(parsed_result));

	if((strcmp(request_header.connection_timeout, "keep-alive") == 0) ||
	 	(strcmp(request_header.connection_timeout, "Keep-Alive") == 0) ||
		(strcmp(request_header.connection_timeout, "Keep Alive") == 0) ||
		(strcmp(request_header.connection_timeout, "keep alive") == 0) ||
		(strcmp(request_header.connection_timeout, "KEEP ALIVE") == 0) ||
		(strcmp(request_header.connection_timeout, "KEEP-ALIVE") == 0))
	{
			keep_alive[parse_args.thread_id_arg] = KEEP_ALIVE_YES;
			printf("## SERVER ## Found Keep Alive\n");
	}
	else
	{
		printf("## SERVER ## Keep Alive Missing\n");
		keep_alive[parse_args.thread_id_arg] = KEEP_ALIVE_NO;
	}*/

	//give the semaphore to the parent thread after posting the keep-alive information
	sem_post(&sem_thread[parse_args.thread_id_arg]);

	//printf("@@@@@@@@@@@@@@@@_%s__@@\n", parse_args.rx_buffer);
	sscanf(parse_args.rx_buffer, "%s %s %s", request_header.method, request_header.path, request_header.version);
	printf("@@@@Method: _%s_\n@@URL: _%s_\n@@Version: _%s_@@\n", request_header.method, request_header.path, request_header.version);

	//check for keep-alive flag and add it to the response header
	if(keep_alive[parse_args.thread_id_arg] == KEEP_ALIVE_YES)
		strcpy(response_header.connection_timeout, request_header.connection_timeout);

	if(strcmp(request_header.method, "GET") != 0)
	{
		printf("Received method %s.. Closing.\n\n\n", request_header.method);
		strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		sprintf(response_header.content, "<html><body>Error 400: Bad Request\nReason: Invalid Method \"%s\"</body></html>", request_header.method);
		sprintf(response_header.content_length, "%lu", strlen(response_header.content));

		send_http_response(parse_args.client_sock_arg,
											response_header.version,
											response_header.code,
											response_header.description,
											response_header.connection_timeout,
											response_header.content_type,
											response_header.content_length,
											response_header.content);

		close(parse_args.client_sock_arg);
		set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
    printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
		pthread_exit(0);
	}

	if(strstr(request_header.path, "http://") == request_header.path)
			sscanf(request_header.path + 7, "%[^/]", resource_id);

	else if(strstr(request_header.path, "https://") == request_header.path)
	{
		printf("Received HTTPS.. Exiting.\n");
		/*strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		sprintf(response_header.content, "<html><body>Error 400: Bad Request - Reason: HTTPS request.</body></html>");
		sprintf(response_header.content_length, "%lu", strlen(response_header.content));

		send_http_response(parse_args.client_sock_arg,
											response_header.version,
											response_header.code,
											response_header.description,
											response_header.connection_timeout,
											response_header.content_type,
											response_header.content_length,
											response_header.content);*/

		close(parse_args.client_sock_arg);
		set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
    printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
		pthread_exit(0);
	}
	else
		sscanf(request_header.path, "%[^/]", resource_id);

	if(strstr(resource_id, ":443") != NULL)
	{
		printf("Received port 443 request, exiting.\n\n");
		/*strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		sprintf(response_header.content, "<html><body>Error 400: Bad Request - Reason: HTTPS request.</body></html>");
		sprintf(response_header.content_length, "%lu", strlen(response_header.content));

		send_http_response(parse_args.client_sock_arg,
											response_header.version,
											response_header.code,
											response_header.description,
											response_header.connection_timeout,
											response_header.content_type,
											response_header.content_length,
											response_header.content);*/

		close(parse_args.client_sock_arg);
		set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
		printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
		pthread_exit(0);
	}

	printf("Resource Address: \"%s\"\n", resource_id);

	bzero(parsed_result, sizeof(parsed_result));
	if(search_config("blacklist.conf", resource_id, parsed_result) == ENTITY_FOUND)
	{
		printf("Received request found in blacklist. Closing.\n");
		strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		sprintf(response_header.content, "<html><body>Error 400: Bad Request - Reason: Website \"%s\" has been blacklisted.</body></html>", resource_id);
		sprintf(response_header.content_length, "%lu", strlen(response_header.content));

		send_http_response(parse_args.client_sock_arg,
											response_header.version,
											response_header.code,
											response_header.description,
											response_header.connection_timeout,
											response_header.content_type,
											response_header.content_length,
											response_header.content);

		set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
    printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
		pthread_exit(0);
	}

	else
	{
		bzero(&cache, sizeof(cache_t));
		md5_digest(parse_args.rx_buffer, strlen(parse_args.rx_buffer), cache.mdString);
		sprintf(cache.filename, "%s.cache", cache.mdString);
		sprintf(cache.path, "cache/%s", cache.filename);
		cache.file = fopen(cache.path, "r");

		if(cache.file != NULL)
		{
			printf("== FILE EXITS ==\n");

			sem_wait(&sem_cache);
			printf("## SERVER ## Got the semaphore.\n");

			//fseek(cache.file, SEEK_SET, SEEK_END);
			//cache.size = ftell(cache.file);

			rewind(cache.file);
			cache.stream = (unsigned char *) malloc(sizeof(unsigned char) * BUFFER_SIZE);

			while(feof(cache.file) == 0)
			{
				bzero(cache.stream, BUFFER_SIZE);
				bytes_read = fread(cache.stream, sizeof(unsigned char), BUFFER_SIZE, cache.file);
				printf("## SERVER ## CACHE BYTES READ: %lu.\n", bytes_read);
				cache.size = cache.size + bytes_read;
				send_stream(parse_args.client_sock_arg, cache.stream, bytes_read);
			}

/*			if(cache.size == 0)
			{
				fclose(cache.file);
				remove(cache.path);
				printf("## ERROR ## Cache size is 0. Deleted File.\n");
				set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
				sem_post(&sem_cache);
				printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
				pthread_exit(0);
			}

			printf("## SERVER ## Cache Size = %lu.\n", cache.size);
			cache.stream = (unsigned char *) malloc(sizeof(unsigned char) * cache.size);

			rewind(cache.file);

			if(fread(cache.stream, sizeof(unsigned char), cache.size, cache.file) != cache.size)
			{
				fclose(cache.file);
				free(cache.stream);
				printf("## ERROR ## During fread cache file.\n");
				set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
				sem_post(&sem_cache);
				printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
				pthread_exit(0);
			}

			send_stream(parse_args.client_sock_arg, cache.stream, cache.size);*/
			fclose(cache.file);
			free(cache.stream);
			sem_post(&sem_cache);
			set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
			printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
			pthread_exit(0);
		}

		else
		{
			printf("FILE == NULL.\n");
			sem_wait(&sem_db);
			db_file = fopen("host_ip.db", "rb");
			bzero(&server_sock_addr, sizeof(server_sock_addr));

			if(db_file != NULL)
			{
				bzero(&ip_database, sizeof(host_ip_t));
				fread(&ip_database, sizeof(host_ip_t), 1, db_file);
				fclose(db_file);

				for(int i = 0; i < ip_database.entries; i++)
				{
					if(strcmp(resource_id, ip_database.host[i]) == 0)
					{
						server_sock_addr.sin_family = AF_INET;
						server_sock_addr.sin_port = htons(80);
						server_sock_addr.sin_addr = ip_database.ip[i];
						host_ip_found = 1;
						//server_sock_addr.sin_addr = *remote_addr_list[0];
						break;
					}
				}

				if(host_ip_found != 1)
				{
					if((remote_host_ent = gethostbyname(resource_id)) == NULL)
					{  // get the host info
						printf("Gethostbyname error.\n");
						sem_post(&sem_db);
						set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
						printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
						pthread_exit(0);
					}

					// print information about this host:
					printf("Official name is: %s\n", remote_host_ent->h_name);
					printf("IP addresses: ");
					remote_addr_list = (struct in_addr **) remote_host_ent->h_addr_list;

					for(int i = 0; remote_addr_list[i] != NULL; i++)
							printf("\r\t\t\t#%d -> %s\n", i, inet_ntoa(*remote_addr_list[i]));

					server_sock_addr.sin_family = AF_INET;
					server_sock_addr.sin_port = htons(80);
					server_sock_addr.sin_addr = *remote_addr_list[0];

					remove("host_ip.db");
					db_file = fopen("host_ip.db", "wb");
					strcpy(ip_database.host[ip_database.entries], resource_id);
					ip_database.ip[ip_database.entries] = *remote_addr_list[0];
					ip_database.entries++;
					fwrite((const void *) &ip_database, sizeof(ip_database), 1, db_file);
					fclose(db_file);
				}
			}

			else
			{
				if((remote_host_ent = gethostbyname(resource_id)) == NULL)
				{  // get the host info
					printf("Gethostbyname error.\n");
					sem_post(&sem_db);
					set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
					printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
					pthread_exit(0);
				}

				// print information about this host:
				printf("Official name is: %s\n", remote_host_ent->h_name);
				printf("IP addresses: ");
				remote_addr_list = (struct in_addr **) remote_host_ent->h_addr_list;

				for(int i = 0; remote_addr_list[i] != NULL; i++)
						printf("\r\t\t\t#%d -> %s\n", i, inet_ntoa(*remote_addr_list[i]));

				server_sock_addr.sin_family = AF_INET;
				server_sock_addr.sin_port = htons(80);
				server_sock_addr.sin_addr = *remote_addr_list[0];

				db_file = fopen("host_ip.db", "wb");
				strcpy(ip_database.host[0], resource_id);
				ip_database.ip[0] = *remote_addr_list[0];
				ip_database.entries = 1;
				fwrite((const void *) &ip_database, sizeof(ip_database), 1, db_file);
				fclose(db_file);
			}

			sem_post(&sem_db);

			if((sock_outgoing = socket(AF_INET, SOCK_STREAM, 0)) < 0)
			{
				printf("## ERROR ##  Unable to create socket.\n");
				set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
				printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
				exit(1);
			}


			if(connect(sock_outgoing, (struct sockaddr *) &server_sock_addr, sizeof(struct sockaddr_in)) != 0)
			{
				printf("## ERROR ## Cannot connect to %s.\n", inet_ntoa(*remote_addr_list[0]));

				strcpy(response_header.version, "HTTP/1.1");
				strcpy(response_header.code, "400");
				strcpy(response_header.description, "Bad Request");

				strcpy(response_header.content_type, "text/html");
				sprintf(response_header.content, "<html><body>Error 400: Bad Request - Reason: Cannot connect to \"%s\".</body></html>", inet_ntoa(*remote_addr_list[0]));
				sprintf(response_header.content_length, "%lu", strlen(response_header.content));

				send_http_response(parse_args.client_sock_arg,
													response_header.version,
													response_header.code,
													response_header.description,
													response_header.connection_timeout,
													response_header.content_type,
													response_header.content_length,
													response_header.content);

				set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
		    printf("## SERVER ## Thread #%d exiting.\n", parse_args.thread_id_arg);
				pthread_exit(0);
			}

			printf("## SERVER ## Connected to %s.\n", inet_ntoa(server_sock_addr.sin_addr));
			send_stream(sock_outgoing, parse_args.rx_buffer, strlen(parse_args.rx_buffer));

			receive_cache_forward_stream(sock_outgoing, parse_args.client_sock_arg, parse_args.rx_buffer, strlen(parse_args.rx_buffer));
		}
	}

	set_thread_slot_state(parse_args.thread_id_arg, THREAD_FREE);
	printf("## SERVER ## Thread #%d closed.\n", parse_args.thread_id_arg);
}


void* timeout(void* args)
{
	char path[128];
	pthread_args_t *timeout_args_ptr = args;
	pthread_args_t timeout_args = *timeout_args_ptr;
	sem_post(&sem_thread[timeout_args.thread_id_arg]);

	sleep(timeout_seconds);
	sprintf(path, "cache/%s.cache", timeout_args.rx_buffer);
	sem_wait(&sem_cache);

	remove(path);
	printf("## SERVER ## Removed %s.\n", timeout_args.rx_buffer);

	sem_post(&sem_cache);

	set_thread_slot_state(timeout_args.thread_id_arg, THREAD_FREE);
	printf("## SERVER ## Thread #%d closed.\n", timeout_args.thread_id_arg);
}

//funtion to keep a track and maintain thread slots
int find_available_slot(void)
{
	int slot = 0;

	sem_wait(&sem_thread_slots);
	for(slot = 0; slot < MAX_THREADS; slot++)
	{
		if(thread_slot_tracker[slot] == THREAD_FREE)
			break;
	}
	sem_post(&sem_thread_slots);

	if(slot < MAX_THREADS)
		return slot;
	else
		return -1;
}


unsigned char search_config(unsigned char *config_filename, unsigned char *entity, unsigned char *entity_match)
{
  FILE *config_file;

  config_file = fopen(config_filename, "r");

	if(config_file == NULL)
	{
		printf("## ERROR ## File blacklist.conf not found.\n");
		exit(1);
	}

  if(parse_file(config_file, entity, entity_match) == 0)
  {
    fclose(config_file);
		return ENTITY_NOT_FOUND;
  }

  fclose(config_file);
  return ENTITY_FOUND;
}


//function to get config params frm the config file
int parse_file(FILE* config_file, unsigned char* entity, unsigned char* entity_config)
{
    unsigned char config_buffer[512];
		unsigned char found_matches = 0;
		unsigned char* match;

    while(feof(config_file) == 0)
    {
      bzero(config_buffer, sizeof(config_buffer));
      fgets(config_buffer, sizeof(config_buffer), config_file);
			match = strstr(config_buffer, entity);

      if(match == NULL)
				continue;

      else if(match == config_buffer)
      {
        match = config_buffer + strlen(entity);
        strncat(entity_config, match, strlen(match));

        found_matches++;
      }
    }

		entity_config[strlen(entity_config) - 1] = '\0';

		return found_matches;
}


void md5_digest(unsigned char* buffer, size_t size, unsigned char* mdString)
{
	//The mdString should be created in the parent function and should be initialized as follows:
	//char mdString[(2 * MD5_DIGEST_LENGTH) + 1];
  MD5_CTX ctx;
  unsigned char md5sum[MD5_DIGEST_LENGTH];

  MD5_Init(&ctx);
  MD5_Update(&ctx, buffer, size);
  MD5_Final(md5sum, &ctx);

  for(int i = 0; i < 16; i++)
     sprintf(&mdString[i*2], "%02x", (unsigned int) md5sum[i]);

  printf("## MD5 DIGEST ## Size = %lu\t\tMD5SUM = %s\n", size, mdString);
	return;
}

//function to set and clear the thread slot states
void set_thread_slot_state(int thread_slot, unsigned char state)
{
	sem_wait(&sem_thread_slots);
	thread_slot_tracker[thread_slot] = state;
	sem_post(&sem_thread_slots);
}

//function to find matches in a buffer and report tht results
int get_matches(unsigned char* input, unsigned char* entity, unsigned char* entity_config)
{
    char* token;
		unsigned char found_matches = 0;
		char* match;
		unsigned char first_time = 1;

		unsigned char* input_duplicate = malloc(BUFFER_SIZE);
		bzero(input_duplicate, BUFFER_SIZE);
		memcpy(input_duplicate, input, BUFFER_SIZE);

    do
    {
			if(first_time == 0)
				token = strtok (NULL, "\n");

      else
			{
				token = strtok(input_duplicate, "\n");
				first_time = 0;
			}

			if(token == NULL)
				continue;

			match = strstr(token, entity);

      if(match == NULL)
				continue;

      else if(match == token)
      {
        match = token + strlen(entity);
        strcpy(entity_config, match);
        found_matches++;
      }
    } while(token != NULL);

		if((entity_config[strlen(entity_config) - 1] < 0x20) || (entity_config[strlen(entity_config) - 1] > 0x7F))
			entity_config[strlen(entity_config) - 1] = '\0';

		free(input_duplicate);
		return found_matches;
}


void send_stream(int sock, unsigned char* data, size_t size)
{
  unsigned char *read_location;
  size_t bytes_left = size, bytes_send = 0;

  read_location = data;

	while(bytes_send != size)
		bytes_send += write(sock, read_location + bytes_send, size - bytes_send);
}

size_t receive_cache_forward_stream(int outgoing_sock, int client_sock, unsigned char* request, size_t request_length)
{
	ssize_t bytes_read = 0;
	unsigned int recv_fail_count = 0;
	unsigned char buffer[BUFFER_SIZE] = {0};

	cache_t cache;
	pthread_args_t args;
	int thread_slot = 0;

	struct timeval get_tv;
	struct timeval set_tv;

	bzero(&cache, sizeof(cache));
	md5_digest(request, request_length, cache.mdString);
	sprintf(cache.filename, "%s.cache", cache.mdString);
	sprintf(cache.path, "cache/%s", cache.filename);
	cache.file = fopen(cache.path, "w");

	size_t opt_len = sizeof(get_tv);

	set_tv.tv_sec = 1;
	set_tv.tv_usec = 0;

	//setsockopt(outgoing_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *) &set_tv, sizeof(struct timeval));

	//if(getsockopt(outgoing_sock, SOL_SOCKET, SO_RCVTIMEO, &get_tv, (socklen_t *) &opt_len) == 0)
		//printf("\nGetSockOpt = %ld %ld\n", get_tv.tv_sec, get_tv.tv_usec);

  do
	{
		bzero(buffer, sizeof(buffer));
    bytes_read = recv(outgoing_sock, buffer, BUFFER_SIZE, 0);
		printf("## WEBPROXY ## Bytes Read = %ld\n", bytes_read);

		if(bytes_read > 0)
		{
			send_stream(client_sock, buffer, bytes_read);
			fwrite(buffer, sizeof(unsigned char), bytes_read, cache.file);
		}

		else
		{
			printf("## ERROR ## %s\n", strerror(errno));
			recv_fail_count++;
		}
	}while((bytes_read != 0) && (recv_fail_count != 20));
	close(outgoing_sock);

	fclose(cache.file);

	if(recv_fail_count != 20)
	{
		while((thread_slot = find_available_slot()) < 0)
			printf("## ERROR ## All threads are currently in use.\n");

		//setting thread slot to occupied
		set_thread_slot_state(thread_slot, THREAD_OCCUPIED);

		bzero(&args, sizeof(args));
		args.thread_id_arg = thread_slot;
		strcpy(args.rx_buffer, cache.mdString);
		printf("## SERVER ## Daemon thread (#%d) created -> Timeout for %s.\n", thread_slot, cache.mdString);
		pthread_create(&thread_id[thread_slot], &thread_attr, timeout, (void *) &args);
		sem_wait(&sem_thread[thread_slot]);
	}

	else
		remove(cache.path);
}


//function to send the http response
void send_http_response(int client_sock, unsigned char* version, unsigned char* code, unsigned char* description,
unsigned char* connection_timeout, unsigned char* content_type, unsigned char* content_length, unsigned char* content)
{
	unsigned char response[1024900];
	size_t header_length = 0;

	strcpy(response, version);
	strcat(response, " ");
	strcat(response, code);
	strcat(response, " ");
	strcat(response, description);
	strcat(response, "\n");

	if(strcmp(connection_timeout, "\0") != 0)
	{
		strcat(response, "Connection: ");
		strcat(response, connection_timeout);
		strcat(response, "\n");
	}

	strcat(response, "Content-Type: ");
	strcat(response, content_type);
	strcat(response, "\n");

	strcat(response, "Content-Length: ");
	strcat(response, content_length);
	strcat(response, "\n");
	strcat(response, "\n");

	header_length = strlen(response);

	memcpy(strrchr(response, '\n') + 1, content, atoi(content_length));

	/*FILE *fp;
	fp = fopen("new_file", "w");

	fwrite(response, sizeof(unsigned char), header_length + atoi(content_length), fp);
	fclose(fp);*/

	write(client_sock , response , header_length + atoi(content_length));
}

//function to convert a file into a buffer
size_t file_to_buffer(FILE *file, unsigned char* storage)
{
	size_t total_bytes_read = 0;
	size_t bytes_read = 0;
	unsigned char* storage_init = storage;
	unsigned char buffer[1024];

	rewind(file);

	while(feof(file) == 0)
	{
		bzero(buffer, sizeof(buffer));
		bytes_read = fread(buffer, sizeof(unsigned char), 1024, file);
		memcpy(storage, buffer, bytes_read);

		total_bytes_read = total_bytes_read + bytes_read;

		if(total_bytes_read < sizeof(storage));
			storage = storage + bytes_read;
	}
	storage = storage_init;
	return total_bytes_read;
}

//function to get the file format from the received path
void get_file_format(unsigned char* input, unsigned char* entity_config)
{
    char* token;

		unsigned char* input_duplicate = malloc(256);
		bzero(input_duplicate, 256);
		memcpy(input_duplicate, input, 256);

		token = strtok(input_duplicate, ".");
    while(token != NULL)
		{
			bzero(entity_config, sizeof(entity_config));
			strcpy(entity_config, token);
			token = strtok (NULL, ".");
		}

		free(input_duplicate);
}
