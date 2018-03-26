/*Author: Sandeep Raj Kumbrgeri
Date Published: 10/23/17
Description: This is a HTTP server which reads and sets can handle MAX_THREADS requests at the same
							time. It suppors pipelining (keep-alive) and also POST requests.

Written for the course ECEN 5273 - Network Systems in University of Colorado Boulder
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

#define MAX_THREADS 1000
#define MAX_PENDING_CONNECTIONS 10
#define THREAD_OCCUPIED 0xFF
#define THREAD_FREE 0x00
#define RX_BUFFER_SIZE 1024
#define TX_BUFFER_SIZE 102400
#define KEEP_ALIVE_YES 0xFF
#define KEEP_ALIVE_NO 0x00
#define RESPONSE_NOT_READY 0x00
#define RESPONSE_READY 0xFF

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
	unsigned char rx_buffer[RX_BUFFER_SIZE];
} pthread_args_t;

typedef struct http_header					//http request header storing structure
{
	unsigned char method[8];
	unsigned char path[256];
	unsigned char version[16];
	unsigned char content_type[256];
	unsigned char content_length[32];
	unsigned char connection_timeout[64];
	unsigned char content[102400];
} http_header_t;

typedef struct http_response				//http response elements structure
{
	unsigned char version[16];
	unsigned char code[8];
	unsigned char description[256];
	unsigned char content_type[32];
	unsigned char content_length[32];
	unsigned char connection_timeout[64];
	unsigned char content[1024000];
} http_response_t;


pthread_t thread_id[MAX_THREADS];
pthread_attr_t thread_attr;
sem_t sem_slots, sem_thread[MAX_THREADS];
config_t config;
unsigned char thread_slot_tracker[MAX_THREADS];
struct timeval tv;
unsigned char keep_alive[MAX_THREADS];

void *accept_thread(void *);
void *respond_thread(void *);
int get_config(FILE*, unsigned char*, unsigned char*);
void config_error_handler(unsigned char*, unsigned int);
int find_available_slot(void);
void set_thread_slot_state(int, unsigned char);
int get_matches(unsigned char*, unsigned char*, unsigned char*);
void send_http_response(int, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*, unsigned char*);
size_t file_to_buffer(FILE*, unsigned char*);
void get_file_format(unsigned char*, unsigned char*);


int main (int argc, unsigned char * argv[])
{
	int sock, client_sock, sock_addr_size, thread_slot;
	struct sockaddr_in sin, client;     //"Internet socket address structure"
	int status = 0;
	unsigned char parsed_result[512];
	pthread_args_t args;

	for(int i = 0; i < MAX_THREADS; i++)				//clearing the thread tracker
		thread_slot_tracker[i] = THREAD_FREE;

	bzero(keep_alive, sizeof(keep_alive));

  FILE *config_file;

	tv.tv_sec = 0;							//setting the pipelining timeout to 0
	tv.tv_usec = 0;

	if (argc > 1)
	{
		printf ("\nUSAGE: ./server\n");
    printf ("\nEdit the server configuration file for changing its configuration\n");
		exit(-1);
	}

  config_file = fopen("ws.conf", "r");

  if(config_file == NULL)
  {
    printf("\n## ERROR ## Unable to open server configuration file. EXITING THE SERVER\n");
    exit(-1);
  }

	printf("## SERVER ##  Reading configuration file.\n");

///////////////////////////////////////////////////////////////////////////// Reading Listen Config
  bzero(parsed_result, sizeof(parsed_result));
  if((status = get_config(config_file, "Listen ", parsed_result)) != 1)
		config_error_handler("Listen", status);

	else
	{
		config.port = atoi(parsed_result);
		printf("\n## SERVER ##  Port Number: %d\n", config.port);
	}
///////////////////////////////////////////////////////////////////////////// Reading DocumentRoot Config
	rewind(config_file);
	bzero(parsed_result, sizeof(parsed_result));
  if((status = get_config(config_file, "DocumentRoot ", parsed_result)) != 1)
		config_error_handler("DocumentRoot", status);

	else
	{
		memcpy(config.document_root, parsed_result, sizeof(parsed_result));
		printf("## SERVER ##  Document Root: \"%s\"\n", config.document_root);
	}
///////////////////////////////////////////////////////////////////////////// Reading DirectoryIndex Config
	rewind(config_file);
	bzero(parsed_result, sizeof(parsed_result));
  if((status = get_config(config_file, "DirectoryIndex ", parsed_result)) != 1)
		config_error_handler("DirectoryIndex", status);

	else
	{
		memcpy(config.directory_index, parsed_result, sizeof(parsed_result));
		printf("## SERVER ##  Directory Index: \"%s\"\n", config.directory_index);
	}
///////////////////////////////////////////////////////////////////////////// Reading Keep Alive Config
rewind(config_file);
bzero(parsed_result, sizeof(parsed_result));
if((status = get_config(config_file, "KeepAlive ", parsed_result)) != 1)
	config_error_handler("Keep Alive", status);

else
{
	tv.tv_sec = atoi(parsed_result);
	printf("\n## SERVER ##  Connection Keep-Alive Time: %ld sec\n", tv.tv_sec);
}
///////////////////////////////////////////////////////////////////////////// Reading Content Types Config
	rewind(config_file);
	bzero(parsed_result, sizeof(parsed_result));
	if((status = get_config(config_file, ".", parsed_result)) == 0)
		config_error_handler("Content Types (.)", status);

	else
	{
		memcpy(config.content_types, parsed_result, sizeof(parsed_result));
		printf("## SERVER ##  Supported Content Types: \n\"%s\"\n\n", config.content_types);
	}
/////////////////////////////////////////////////////////////////////////////


fclose(config_file);

//This code populates the sockaddr_in struct with the information about our socket
	bzero(&sin,sizeof(sin));                    //zero the struct
	sin.sin_family = AF_INET;                   //address family
	sin.sin_port = htons(config.port);        //htons() sets the port # to network byte order
	sin.sin_addr.s_addr = INADDR_ANY;           //supplies the IP address of the local machine

//Causes the system to create a generic socket of type UDP (datagram)
	if((sock = socket(sin.sin_family, SOCK_STREAM, 0)) < 0)
	{
		printf("## ERROR ##  Unable to create socket.\n");
		exit(-1);
	}

	else
		printf("## SERVER ##  Socket successfully created\n");

//Once we've created a socket, we must bind that socket to the local address and port we've supplied in the sockaddr_in struct
	if(bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		printf("## ERROR ##  Unable to bind socket.\n");
		exit(-1);
	}

	sock_addr_size = sizeof(struct sockaddr_in);

	//listen for active TCP connections
	if(listen(sock, MAX_PENDING_CONNECTIONS) != 0)
	{
		printf("## ERROR ## Listening Failed.\n");
		exit(-1);
	}

	printf("## SERVER ## Listening!\n");

	//initialize the thread attributes and semaphores for sync
	pthread_attr_init(&thread_attr);
	sem_init(&sem_slots, 0, 0);
	sem_post(&sem_slots);

	for(int i = 0; i < MAX_THREADS; i++)
		sem_init(&sem_thread[i], 0, 0);

	while(1)
	{
		//wait for connections
		if((client_sock = accept(sock, (struct sockaddr *) &client, (socklen_t*) &sock_addr_size)) < 0)
    {
			printf("## ERROR ## Unable to accept connection.\n");
			continue;
		}

		else
			{
				while((thread_slot = find_available_slot()) < 0)
					printf("## ERROR ## All threads are currently in use.\n");

				//setting the threat we're about to use to occupied state
				set_thread_slot_state(thread_slot, THREAD_OCCUPIED);

				printf("## SERVER ## Thread #%d connection accepted.\n", thread_slot);
				args.client_sock_arg = client_sock;
				args.thread_id_arg = thread_slot;
				pthread_create(&thread_id[thread_slot], &thread_attr, accept_thread, (void *) &args);
				sem_wait(&sem_thread[thread_slot]);
			}
	}
}

void *accept_thread(void *args)				//here is a thread for every active connection
{
	int rx_length = 0, thread_slot = 0;
	pthread_args_t *accept_thread_args_ptr = args;
	pthread_args_t accept_thread_args = *accept_thread_args_ptr;
	pthread_args_t respond_thread_args;
	sem_post(&sem_thread[accept_thread_args.thread_id_arg]);

	respond_thread_args.client_sock_arg = accept_thread_args.client_sock_arg;

	//printf("The thread ID is %u.\n", thread_args->thread_id_arg);
	bzero(respond_thread_args.rx_buffer, RX_BUFFER_SIZE);
	while((rx_length = recv(accept_thread_args.client_sock_arg, respond_thread_args.rx_buffer, RX_BUFFER_SIZE, 0)) > 0)
  {
		//printf("%srx_length = %d\nstr_len = %lu\n", respond_thread_args.rx_buffer, rx_length, strlen(respond_thread_args.rx_buffer));

		while((thread_slot = find_available_slot()) < 0)
			printf("## ERROR ## All threads are currently in use.\n");

		//setting thread slot to occupied
		set_thread_slot_state(thread_slot, THREAD_OCCUPIED);

		printf("## SERVER ## Thread #%d connection accepted.\n", thread_slot);
		respond_thread_args.thread_id_arg = thread_slot;

		pthread_create(&thread_id[thread_slot], &thread_attr, respond_thread, (void *) &respond_thread_args);

		//wait for the child thead to give the semaphore
		sem_wait(&sem_thread[thread_slot]);

		bzero(respond_thread_args.rx_buffer, RX_BUFFER_SIZE);

		if(keep_alive[thread_slot] == KEEP_ALIVE_YES)
			setsockopt(accept_thread_args.client_sock_arg, SOL_SOCKET, SO_RCVTIMEO, (const unsigned char*) &tv,sizeof(struct timeval));

		else if(keep_alive[thread_slot] == KEEP_ALIVE_NO)
			break;
  }

	//if broken suddenly due to no keep-alive, wait for the child thread to finish
	pthread_join(thread_id[thread_slot], NULL);
	close(accept_thread_args.client_sock_arg);	//close the client socket
	set_thread_slot_state(accept_thread_args.thread_id_arg, THREAD_FREE); //set the client sock to free
	keep_alive[thread_slot] = KEEP_ALIVE_NO;		//reset the keep-alive flag for that thread
	printf("## SERVER ## Thread #%d connection closed.\n", accept_thread_args.thread_id_arg);
}

void *respond_thread(void *args)
{
	pthread_args_t *respond_thread_args_ptr = args;
	pthread_args_t respond_thread_args = *respond_thread_args_ptr;
	http_header_t request_header;
	http_response_t response_header;
	size_t response_content_length = 0;
	unsigned char path[512];
	unsigned char parsed_result[512];
	char* str_token;
	unsigned char response_ready = RESPONSE_NOT_READY;

	bzero(parsed_result, sizeof(parsed_result));
	bzero(path, sizeof(path));

	if((get_matches(respond_thread_args.rx_buffer, "Connection: ", parsed_result) == 1))
		memcpy(request_header.connection_timeout, parsed_result, strlen(parsed_result));

	if((strcmp(request_header.connection_timeout, "keep-alive") == 0) ||
	 	(strcmp(request_header.connection_timeout, "Keep-Alive") == 0) ||
		(strcmp(request_header.connection_timeout, "Keep Alive") == 0) ||
		(strcmp(request_header.connection_timeout, "keep alive") == 0) ||
		(strcmp(request_header.connection_timeout, "KEEP ALIVE") == 0) ||
		(strcmp(request_header.connection_timeout, "KEEP-ALIVE") == 0))
	{
			keep_alive[respond_thread_args.thread_id_arg] = KEEP_ALIVE_YES;
			printf("## SERVER ## Found Keep Alive\n";
	}
	else
	{
		printf("## SERVER ## Keep Alive Missing\n";
		keep_alive[respond_thread_args.thread_id_arg] = KEEP_ALIVE_NO;
	}

	//give the semaphore to the parent thread after posting the keep-alive information
	sem_post(&sem_thread[respond_thread_args.thread_id_arg]);

	//printf("@@@@\n%s@@@@\n", respond_thread_args.rx_buffer);

	unsigned char *rx_buff_dup = malloc(RX_BUFFER_SIZE);
	bzero(rx_buff_dup, RX_BUFFER_SIZE);
	memcpy(rx_buff_dup, respond_thread_args.rx_buffer, RX_BUFFER_SIZE);

	//get the request header attributes
	str_token = strtok(rx_buff_dup," ");
	strcpy(request_header.method, str_token);

	str_token = strtok(NULL," ");
	strcpy(request_header.path, str_token);

	str_token = strtok(NULL," ,\n");
	strcpy(request_header.version, str_token);
	request_header.version[strlen(request_header.version) - 1] = '\0';

	bzero(rx_buff_dup, RX_BUFFER_SIZE);
	free(rx_buff_dup);

	strcpy(path, config.document_root);
	strcat(path, request_header.path);
	//printf("-->%s<--\n", path);

	//printf("%s\n%s\n%s\n", request_header.method, path, request_header.version);

	//check for keep-alive flag and add it to the response header
	if(keep_alive[respond_thread_args.thread_id_arg] == KEEP_ALIVE_YES)
		strcpy(response_header.connection_timeout, request_header.connection_timeout);

	//function to check if the HTTP version is valid
	if((strcmp(request_header.version, "HTTP/1.1") != 0) && (strcmp(request_header.version, "HTTP/1.0") != 0))
	{
		printf("--inside invalid version\n");
		strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		strcpy(response_header.content, "<html><body>400 Bad Request Reason: Invalid HTTP-Version: <<");
		strcat(response_header.content, request_header.version);
		strcat(response_header.content, ">></body></html>");

		sprintf(response_header.content_length, "%lu", strlen(response_header.content));
		response_ready = RESPONSE_READY;
	}

	//if the GET header matches
	else if((response_ready != RESPONSE_READY) && (strcmp(request_header.method, "GET") == 0))
	{
		printf("--inside get\n");
		char* invalid_chars[7];
		FILE *file;
		DIR *dir;

		strcpy(response_header.version, "HTTP/1.1");

		invalid_chars[0] = strstr(request_header.path, "\\");
		invalid_chars[1] = strstr(request_header.path, "<");
		invalid_chars[2] = strstr(request_header.path, ">");
		invalid_chars[3] = strstr(request_header.path, ":");
		invalid_chars[4] = strstr(request_header.path, "\"");
		invalid_chars[5] = strstr(request_header.path, "?");
		invalid_chars[6] = strstr(request_header.path, "*");

		//check for incorrect formatting options in the file path
		for(int i = 0; i < 7; i++)
		{
			if(invalid_chars[i] != NULL)
				{
					printf("--Found some weird character\n");
					strcpy(response_header.code, "400");
					strcpy(response_header.description, "Bad Request");

					strcpy(response_header.content_type, "text/html");
					strcpy(response_header.content, "<html><body>400 Bad Request Reason: Invalid URL: <<");
					strcat(response_header.content, request_header.path);
					strcat(response_header.content, ">></body></html>");

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
					break;
				}
		}

		//check for the path and proceed
		if(response_ready != RESPONSE_READY)
		{
			dir = opendir(path);

			//if the path leads to a directory
			if(dir != NULL)
			{
				printf("--DIR exists\n");
				closedir(dir);

				//append /index.html & proceed
				bzero(path, sizeof(path));
				strcpy(path, config.document_root);
				strcpy(path, config.document_root);
				strcat(path, request_header.path);

				if((unsigned char*) strrchr(path, '/') == (path + (strlen(path) - 1)))
					strcat(path, "index.html");
				else
					strcat(path, "/index.html");

				file = fopen(path, "r");

				//if index.html is found in the path, proceed
				if(file != NULL)
				{
					printf("--Index File exists\n");

					strcpy(response_header.code, "200");
					strcpy(response_header.description, "Document Follows");
					bzero(parsed_result, sizeof(parsed_result));
					get_file_format(path, parsed_result);
					//printf("-----%s\n", parsed_result);
					strcat(parsed_result, " ");
					get_matches(config.content_types, parsed_result, response_header.content_type);
					strcpy(response_header.content_type, response_header.content_type);
					//printf("-----%s\n", response_header.content_type);
					response_content_length = file_to_buffer(file, response_header.content);
					sprintf(response_header.content_length, "%lu", response_content_length);
					response_ready = RESPONSE_READY;

					fclose(file);
				}

				//if not, proceed
				else
				{
					printf("--index NULL\n");
					strcpy(response_header.code, "404");
					strcpy(response_header.description, "Not Found");

					strcpy(response_header.content_type, "text/html");
					strcpy(response_header.content, "<html><body>404 Not Found Reason: URL does not exist: <<");
					strcat(response_header.content, request_header.path);
					strcat(response_header.content, ">></body></html>");

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
				}
			}

			//if the path does not lead to a directory, proceed
			else
			{
				int matches = 0;

				//check for the path/file format
				bzero(parsed_result, sizeof(parsed_result));
				get_file_format(request_header.path, parsed_result);
				strcat(parsed_result, " ");
				matches = get_matches(config.content_types, parsed_result, response_header.content_type);

				//check for the formats in the config file contents buffer
				if(matches == 0)
				{
					strcpy(response_header.code, "501");
					strcpy(response_header.description, "Not Implemented");

					strcpy(response_header.content_type, "text/html");
					strcpy(response_header.content, "<html><body>501 File format requested unsupported by the server:<<");
					strcat(response_header.content, parsed_result);
					strcat(response_header.content, ">></body></html>");

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
				}

				//content format match occurs, proceed
				else
				{
					file = fopen(path, "r");

					//if file found, proceed
					if(file != NULL)
					{
						printf("--Found file\n");

						//check for the size of the file and determine buffer overflow
						response_content_length = file_to_buffer(file, response_header.content);

						//if buffer overflow is anticipated, proceed
						if(response_content_length > sizeof(response_header.content))
						{
							strcpy(response_header.code, "500");
							strcpy(response_header.description, "Internal Server Error");

							strcpy(response_header.content_type, "text/html");
							bzero(response_header.content, sizeof(response_header.content));
							strcpy(response_header.content, "<html><body>500 Internal Server Error. File requested is too large<<");
							strcat(response_header.content, ">></body></html>");

							sprintf(response_header.content_length, "%lu", strlen(response_header.content));
							response_ready = RESPONSE_READY;
						}

						//if not, proceed and send the file
						else
						{
							strcpy(response_header.code, "200");
							strcpy(response_header.description, "Document Follows");
							bzero(parsed_result, sizeof(parsed_result));
							get_file_format(request_header.path, parsed_result);
							strcat(parsed_result, " ");
							get_matches(config.content_types, parsed_result, response_header.content_type);
							strcpy(response_header.content_type, response_header.content_type);

							sprintf(response_header.content_length, "%lu", response_content_length);
							response_ready = RESPONSE_READY;
						}
					}

					//if the file is not found, proceed
					else
					{
						strcpy(response_header.code, "404");
						strcpy(response_header.description, "Not Found");

						strcpy(response_header.content_type, "text/html");
						strcpy(response_header.content, "<html><body>404 Not Found Reason: URL does not exist: <<");
						strcat(response_header.content, request_header.path);
						strcat(response_header.content, ">></body></html>");

						sprintf(response_header.content_length, "%lu", strlen(response_header.content));
						response_ready = RESPONSE_READY;
					}
				}
			}
		}
	}

	//same thing as absove, but with the post information in the header
	else if((response_ready != RESPONSE_READY) && (strcmp(request_header.method, "POST") == 0))
	{
		printf("GOT INTO POST\n");

		bzero(request_header.content, sizeof(request_header.content));
		bzero(request_header.content_length, sizeof(request_header.content_length));

		if(get_matches(respond_thread_args.rx_buffer, "Content-Length: ", request_header.content_length) == 1)
			memcpy(request_header.content, strrchr(respond_thread_args.rx_buffer,'\n') + 1, atoi(request_header.content_length));

		//printf("@@@POST DATA__%s\n%s\n", request_header.content, request_header.content_length);

		printf("--inside post\n");
		char* invalid_chars[7];
		FILE *file;
		DIR *dir;

		strcpy(response_header.version, "HTTP/1.1");

		invalid_chars[0] = strstr(request_header.path, "\\");
		invalid_chars[1] = strstr(request_header.path, "<");
		invalid_chars[2] = strstr(request_header.path, ">");
		invalid_chars[3] = strstr(request_header.path, ":");
		invalid_chars[4] = strstr(request_header.path, "\"");
		invalid_chars[5] = strstr(request_header.path, "?");
		invalid_chars[6] = strstr(request_header.path, "*");

		for(int i = 0; i < 7; i++)
		{
			if(invalid_chars[i] != NULL)
				{
					printf("--Found some weird character\n");
					strcpy(response_header.code, "400");
					strcpy(response_header.description, "Bad Request");

					strcpy(response_header.content_type, "text/html");
					sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>400 Bad Request Reason: Invalid URL: <<%s>></body></html>", request_header.content, request_header.path);

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
					break;
				}
		}


		if(response_ready != RESPONSE_READY)
		{
			dir = opendir(path);

			if(dir != NULL)
			{
				printf("--DIR exists\n");
				closedir(dir);

				bzero(path, sizeof(path));
				strcpy(path, config.document_root);
				strcpy(path, config.document_root);
				strcat(path, request_header.path);

				if((unsigned char*) strrchr(path, '/') == (path + (strlen(path) - 1)))
					strcat(path, "index.html");
				else
					strcat(path, "/index.html");

				file = fopen(path, "r");

				if(file != NULL)
				{
					printf("--Index File exists\n");

					strcpy(response_header.code, "200");
					strcpy(response_header.description, "Document Follows");
					bzero(parsed_result, sizeof(parsed_result));
					get_file_format(path, parsed_result);
					strcat(parsed_result, " ");
					get_matches(config.content_types, parsed_result, response_header.content_type);
					strcpy(response_header.content_type, response_header.content_type);

					sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>", request_header.content);

					unsigned char buffer[sizeof(response_header.content)];
					unsigned long int size;
					size = strlen(response_header.content);
					response_content_length = file_to_buffer(file, buffer);
					memcpy(response_header.content + size, buffer, response_content_length);
					sprintf(response_header.content_length, "%lu", response_content_length + size);
					response_ready = RESPONSE_READY;

					fclose(file);
				}

				else
				{
					printf("--index NULL\n");
					strcpy(response_header.code, "404");
					strcpy(response_header.description, "Not Found");

					strcpy(response_header.content_type, "text/html");
					sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>404 Not Found Reason: URL does not exist: <<%s>></body></html>", request_header.content, request_header.path);

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
				}
			}

			else
			{
				int file_format_matches = 0;

				bzero(parsed_result, sizeof(parsed_result));
				get_file_format(request_header.path, parsed_result);
				strcat(parsed_result, " ");
				file_format_matches = get_matches(config.content_types, parsed_result, response_header.content_type);

				if(file_format_matches == 0)
				{
					strcpy(response_header.code, "501");
					strcpy(response_header.description, "Not Implemented");

					strcpy(response_header.content_type, "text/html");
					sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>501 File format requested unsupported by the server:<<%s>></body></html>", request_header.content, parsed_result);

					sprintf(response_header.content_length, "%lu", strlen(response_header.content));
					response_ready = RESPONSE_READY;
				}

				else
				{
					file = fopen(path, "r");

					if(file != NULL)
					{
						printf("--Found file\n");

						response_content_length = file_to_buffer(file, response_header.content);

						if(response_content_length > sizeof(response_header.content))
						{
							strcpy(response_header.code, "500");
							strcpy(response_header.description, "Internal Server Error");

							strcpy(response_header.content_type, "text/html");
							bzero(response_header.content, sizeof(response_header.content));
							sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>500 Internal Server Error. File requested is too large<<>></body></html>", request_header.content);

							sprintf(response_header.content_length, "%lu", strlen(response_header.content));
							response_ready = RESPONSE_READY;
						}

						else
						{
							printf("Hello there\n");
							strcpy(response_header.code, "200");
							strcpy(response_header.description, "Document Follows");
							bzero(parsed_result, sizeof(parsed_result));
							get_file_format(request_header.path, parsed_result);
							strcat(parsed_result, " ");
							get_matches(config.content_types, parsed_result, response_header.content_type);
							strcpy(response_header.content_type, response_header.content_type);

							bzero(response_header.content, sizeof(response_header.content));
							sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>", request_header.content);

							int size = 0;
							unsigned char buffer[sizeof(response_header.content)];
							size = strlen(response_header.content);

							rewind(file);
							response_content_length = file_to_buffer(file, buffer);
							printf("####%lu\n", response_content_length);
							memcpy(response_header.content + size, buffer, response_content_length);

							printf("~~~~~~~~~~~~%s\n", response_header.content);

							sprintf(response_header.content_length, "%lu", response_content_length);
							response_ready = RESPONSE_READY;
						}
					}

					else
					{
						strcpy(response_header.code, "404");
						strcpy(response_header.description, "Not Found");

						strcpy(response_header.content_type, "text/html");
						sprintf(response_header.content, "<html><body><pre><h1>%s</h1></pre>404 Not Found Reason: URL does not exist: <<%s>></body></html>", request_header.content, request_header.path);

						sprintf(response_header.content_length, "%lu", strlen(response_header.content));
						response_ready = RESPONSE_READY;
					}
				}
			}
		}
	}

	//if all the above cases are missed, proceed and throw an error
	else
	{
		printf("--Final else\n");
		strcpy(response_header.version, "HTTP/1.1");
		strcpy(response_header.code, "400");
		strcpy(response_header.description, "Bad Request");

		strcpy(response_header.content_type, "text/html");
		strcpy(response_header.content, "<html><body>400 Bad Request Reason: Invalid Method :<<request method>></body></html>");
		sprintf(response_header.content_length, "%lu", strlen(response_header.content));
		response_ready = RESPONSE_READY;
	}

	//prepare to send the http request
	if(response_ready == RESPONSE_READY)
		send_http_response(respond_thread_args.client_sock_arg,
											response_header.version,
											response_header.code,
											response_header.description,
											response_header.connection_timeout,
											response_header.content_type,
											response_header.content_length,
											response_header.content);

	//Send the message back to client
	//write(client_sock , client_message , strlen(client_message));

	//free the thread slot
	set_thread_slot_state(respond_thread_args.thread_id_arg, THREAD_FREE);
	printf("## SERVER ## Thread #%d connection closed.\n", respond_thread_args.thread_id_arg);
}

//funtion to keep a track and maintain thread slots
int find_available_slot(void)
{
	int slot = 0;

	sem_wait(&sem_slots);
	for(slot = 0; slot < MAX_THREADS; slot++)
	{
		if(thread_slot_tracker[slot] == THREAD_FREE)
			break;
	}
	sem_post(&sem_slots);

	if(slot < MAX_THREADS)
		return slot;
	else
		return -1;
}

//function to get config params frm the config file
int get_config(FILE* config_file, unsigned char* entity, unsigned char* entity_config)
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

//function to handle erros with config file
void config_error_handler(unsigned char* entity, unsigned int occurances)
{
	if(occurances == 0)
		printf("## ERROR ##  Entity \"%s\" not found in config file.\n", entity);

	else
		printf("## ERROR ##  %d entries for entity \"%s\" found in the config file.\n", occurances, entity);

		printf("## SERVER ## Exiting.\n");
		exit(-1);
}

//function to set and clear the thread slot states
void set_thread_slot_state(int thread_slot, unsigned char state)
{
	sem_wait(&sem_slots);
	thread_slot_tracker[thread_slot] = state;
	sem_post(&sem_slots);
}

//function to find matches in a buffer and report tht results
int get_matches(unsigned char* input, unsigned char* entity, unsigned char* entity_config)
{
    char* token;
		unsigned char found_matches = 0;
		char* match;
		unsigned char first_time = 1;

		unsigned char* input_duplicate = malloc(RX_BUFFER_SIZE);
		bzero(input_duplicate, RX_BUFFER_SIZE);
		memcpy(input_duplicate, input, RX_BUFFER_SIZE);

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

	FILE *fp;
	fp = fopen("new_file", "w");

	fwrite(response, sizeof(unsigned char), header_length + atoi(content_length), fp);
	fclose(fp);

	write(client_sock , response , header_length + atoi(content_length));
}

//function to convert a file into a buffer
size_t file_to_buffer(FILE *file, unsigned char* storage)
{
	size_t total_bytes_read = 0;
	size_t bytes_read = 0;
	unsigned char* storage_init = storage;
	unsigned char buffer[1024];

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
