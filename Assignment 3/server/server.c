#include <sys/stat.h>
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

#define TOTAL_SERVERS                           4
#define TOTAL_COMMANDS                          6
#define MAX_PENDING_CONNECTIONS                 10
#define USERNAME_PASSWORD_LENGTH                32
#define TOTAL_LIST_ENTRIES											50
#define FILE_READ_BUFFER_SIZE                   1048576

#define AUTHENTICATION_SUCCESSFUL               0xFF
#define AUTHENTICATION_USER_NOT_FOUND           0x01
#define AUTHENTICATION_USER_CONFIG_NOT_FOUND    0x02
#define AUTHENTICATION_INVALID_CONFIG_FILE      0x03
#define AUTHENTICATION_INVALID_PASSWORD         0x04

#define LOGOUT_COMMAND                          0x05
#define EXIT_COMMAND                            0x06

#define USER_CONFIG_ENTITY_NOT_FOUND            0x05
#define USER_CONFIG_ENTITY_FOUND                0x06
#define SERVER_ATTR_NOT_FOUND                   0x07
#define SERVER_ATTR_FOUND                       0x08
#define INVALID_COMMAND                         0x09

#define CONNECTION_SUCCESSFUL                   0xFF
#define CONNECTION_UNSUCCESSFUL                 0x0D
#define INVALID_ENTITY                          0x0E

#define COMMAND_LENGTH                          1024
#define FILENAME_LENGTH                         1024
#define DIRNAME_LENGTH													512

#define SERVICE_ID_GET_REQUEST                  0x10
#define SERVICE_ID_GET_RESPONSE                 0x11
#define SERVICE_ID_GET_FILE_REQUEST             0x12

#define SERVICE_ID_PUT_REQUEST                  0x22
#define SERVICE_ID_PUT_REQUEST_ACK              0x23
#define SERVICE_ID_PUT_ACK                      0x24

#define SERVICE_ID_LIST_REQUEST                 0x30
#define SERVICE_ID_LIST_RESPONSE_EMPTY          0x31
#define SERVICE_ID_LIST_RESPONSE_SUCCESS        0x32

#define SERVICE_ID_MKDIR_REQUEST                0x26
#define SERVICE_ID_MKDIR_RESPONSE_SUCCESS       0x27
#define SERVICE_ID_MKDIR_RESPONSE_EXISTS        0x28

#define SERVICE_ID_FILE_NOT_FOUND               0x50
#define SERVICE_ID_FILE_FOUND                   0x51

#define WRITE_BLOCK_SIZE                        102400

#define ZERO_RESPONSE                           0x17

#define SUCCESS                                 0xFF
#define ERROR                                   0x00

#define MAP                                     0xFF
#define DISCARD                                 0x00



#define MAX_THREADS 1000
#define MAX_PENDING_CONNECTIONS 10
#define THREAD_OCCUPIED 0xFF
#define THREAD_FREE 0x00
#define KEEP_ALIVE_YES 0xFF
#define KEEP_ALIVE_NO 0x00

typedef struct database
{
	unsigned int entries;
	unsigned char dirname[TOTAL_LIST_ENTRIES][DIRNAME_LENGTH];
	unsigned char filename[TOTAL_LIST_ENTRIES][FILENAME_LENGTH];
	unsigned char parts[TOTAL_LIST_ENTRIES][TOTAL_SERVERS];
} database_t;

typedef struct pthread_args				//agruments for passing from thread to thread
{
	int client_sock_arg;
	unsigned int thread_id_arg;
	unsigned char base_path[128];
} pthread_args_t;

typedef struct request
{
  unsigned char username[USERNAME_PASSWORD_LENGTH];
  unsigned char password[USERNAME_PASSWORD_LENGTH];
  unsigned char service_id;
	unsigned char dirname[DIRNAME_LENGTH];
  unsigned char filename[FILENAME_LENGTH];
  size_t size;
  unsigned char parts[TOTAL_SERVERS];
} request_t;

typedef struct response
{
	unsigned char header;
  size_t size;
  unsigned char parts[TOTAL_SERVERS];
} response_t;

sem_t sem_slots, sem_thread[MAX_THREADS];
unsigned char thread_slot_tracker[MAX_THREADS];
unsigned char port[16];

void *accept_thread(void *);
void *respond_thread(void *);
int find_available_slot(void);
void set_thread_slot_state(int, unsigned char);
void *connection_accepted(void *);
unsigned int parse_file(FILE *, unsigned char*, unsigned char*);
unsigned char authenticator(FILE *, unsigned char*, unsigned char*);
void send_response(int, unsigned char, size_t, unsigned int);
void execute_service(request_t, pthread_args_t);
unsigned int identify_part(unsigned char*);
void xor_crypto(unsigned char*, unsigned char*, size_t);
void registry(request_t, pthread_args_t);
void get(request_t, pthread_args_t);
void put(request_t, pthread_args_t);
void makedir(request_t, pthread_args_t);
void list(request_t, pthread_args_t);
void receive_part_file(int, unsigned char*, size_t);
void query_database(request_t, pthread_args_t, response_t*);
void send_part_file(int, unsigned char*, size_t);
size_t get_filesize(FILE*);
size_t md5_digest(FILE*, unsigned char*);
void md5_digest_buffer(unsigned char*, size_t);

int main(int argc, char *argv[])
{
  int sock, client_sock, sock_addr_size, thread_slot;
	struct sockaddr_in sin, client;
	FILE *file;
	DIR* dir;
  pthread_args_t args;
	pthread_t thread_id[MAX_THREADS];
	pthread_attr_t thread_attr;

  for(int i = 0; i < MAX_THREADS; i++)				//clearing the thread tracker
    thread_slot_tracker[i] = THREAD_FREE;

  if (argc != 3)
	{
		printf ("USAGE: ./server <port> <directory_name/path>\n");
		exit(1);
	}

	dir = opendir(argv[2]);

	if(dir == NULL)
	{
		printf("## ERROR ## Error finding the specified directory");
		exit(1);
	}

	closedir(dir);

	strcpy(args.base_path, argv[2]);
	printf("Base path = %s\n", args.base_path);

	file = fopen("dfs.conf", "r");
	if(file == NULL)
	{
		printf("## ERROR ## Unable to open the server's configuration file.\n");
		exit(1);
	}

	fclose(file);

//This code populates the sockaddr_in struct with the information about our socket
	bzero(&sin,sizeof(sin));                    //zero the struct
	sin.sin_family = AF_INET;                   //address family
	sin.sin_port = htons(atoi(argv[1]));        //htons() sets the port # to network byte order
	sin.sin_addr.s_addr = htonl(INADDR_ANY);           //supplies the IP address of the local machine

	bzero(port, sizeof(port));
	strcpy(port, argv[1]);

//Causes the system to create a generic socket of type UDP (datagram)
	if((sock = socket(sin.sin_family, SOCK_STREAM, 0)) < 0)
	{
		printf("## ERROR ##  Unable to create socket.\n");
		exit(1);
	}

	else
		printf("## SERVER %s ##  Socket successfully created.\n", port);

//Once we've created a socket, we must bind that socket to the local address and port we've supplied in the sockaddr_in struct
	if(bind(sock, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		printf("## ERROR ##  Unable to bind socket.\n");
		close(sock);
		exit(1);
	}

	sock_addr_size = sizeof(struct sockaddr_in);

	//listen for active TCP connections
	if(listen(sock, MAX_PENDING_CONNECTIONS) != 0)
	{
		printf("## ERROR ## Listening Failed.\n");
		exit(1);
	}

	printf("## SERVER %s ## Listening on port %s\n", port, port);

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

				printf("## SERVER %s ## New connection accepted -> Handled by thread #%d.\n", port, thread_slot);
				args.client_sock_arg = client_sock;
				args.thread_id_arg = thread_slot;
				pthread_create(&thread_id[thread_slot], &thread_attr, connection_accepted, (void *) &args);
				sem_wait(&sem_thread[thread_slot]);
			}
	}
}


void *connection_accepted(void *args)				//here is a thread for every active connection
{
	FILE *config_file;
	unsigned char status;
	size_t request_length = 0;
	pthread_args_t *received_args_ptr = args;
	pthread_args_t received_args = *received_args_ptr;
  request_t request;
	sem_post(&sem_thread[received_args.thread_id_arg]);

	while((request_length = recv(received_args.client_sock_arg, &request, sizeof(request_t), 0)) > 0)
  {
		if(request_length == sizeof(request_t))
		{
			config_file = fopen("dfs.conf", "r");

			if(config_file == NULL)
			{
				printf("## ERROR ## Unable to open the server's configuration file.\n");
				exit(1);
			}

			if((status = authenticator(config_file, request.username, request.password)) != AUTHENTICATION_SUCCESSFUL)
			{
				if(status == AUTHENTICATION_USER_NOT_FOUND)
				{
					printf("## ERROR ## User not found.\n");
					send_response(received_args.client_sock_arg, AUTHENTICATION_USER_NOT_FOUND, request.size, identify_part(request.parts));
				}

				else if(status == AUTHENTICATION_INVALID_PASSWORD)
				{
					printf("## ERROR ## Incorrect password.\n");
					send_response(received_args.client_sock_arg, AUTHENTICATION_INVALID_PASSWORD, request.size, identify_part(request.parts));
				}

				break;
			}

			send_response(received_args.client_sock_arg, AUTHENTICATION_SUCCESSFUL, request.size, identify_part(request.parts));
			printf("## SERVER %s ## Authentication Successful.\n", port);

			printf("## SERVER %s ## Received Service ID: %X\n", port, request.service_id);
			execute_service(request, received_args);
		}

		else
		{
			printf("## SERVER %s ## Non-Header packet of size %lu received.\n", port, request_length);
		}
  }

	close(received_args.client_sock_arg);	//close the client socket
	set_thread_slot_state(received_args.thread_id_arg, THREAD_FREE); //set the client sock to free
	printf("## SERVER %s ## Connection with thread #%d closed.\n", port, received_args.thread_id_arg);
}

void execute_service(request_t request, pthread_args_t args)
{
	switch(request.service_id)
	{
		case SERVICE_ID_GET_REQUEST:
		case SERVICE_ID_GET_FILE_REQUEST:
		get(request, args);
		break;

		case SERVICE_ID_PUT_REQUEST:
		put(request, args);
		break;

		case SERVICE_ID_LIST_REQUEST:
		list(request, args);
		break;

		case SERVICE_ID_MKDIR_REQUEST:
		makedir(request, args);
		break;
	}
}

void put(request_t request, pthread_args_t args)
{
	printf("## SERVER %s ## Request received: PUT.\n", port);
	FILE* file;
	DIR* dir;
	unsigned char path[DIRNAME_LENGTH];
	unsigned char md5sum[MD5_DIGEST_LENGTH];
	unsigned int part_id = 0;
	//printf("%s__req_size = %lu.\n", port, request.size);
	size_t bytes_written;

	unsigned char* part_file = (unsigned char *) malloc(request.size * sizeof(unsigned char));
	bzero(part_file, request.size);
	receive_part_file(args.client_sock_arg, part_file, request.size);
	md5_digest_buffer(part_file, request.size);

	bzero(path, sizeof(path));
	sprintf(path, "%s/%s", args.base_path, request.username);

	if((dir = opendir(path)) == NULL)
		mkdir(path, 0777);

	else
		closedir(dir);

	if(strlen(request.dirname) != 0)
	{
		bzero(path, sizeof(path));
		sprintf(path, "%s/%s/%s", args.base_path, request.username, request.dirname);

		if((dir = opendir(path)) == NULL)
			mkdir(path, 0777);

		else
			closedir(dir);
	}

	part_id = identify_part(request.parts);
	sprintf(path, "%s/%s.%d", path, request.filename, part_id);
	file = fopen(path,"wb");
	xor_crypto(request.password, part_file, request.size);
	//printf("## SERVER %s ## After decryping: \n", port);
	md5_digest_buffer(part_file, request.size);
	bytes_written = fwrite(part_file, sizeof(unsigned char), request.size, file);

	free(part_file);
	fclose(file);
	//file = fopen(path,"rb");
	//md5_digest(file, md5sum);
	//fclose(file);

	if(bytes_written != request.size)
	{
		printf("## SERVER %s ## While writing to file. Expected to write %lu bytes. Written %lu bytes.\n", port, request.size, bytes_written);
		return;
	}

	printf("## SERVER %s ## File %s saved successfully.\n", port, path);
	registry(request, args);
}

void get(request_t request, pthread_args_t args)
{
	FILE* file;
	unsigned char path[DIRNAME_LENGTH];
	unsigned int part_id = 0;
	unsigned char* part_file;
	size_t part_file_size = 0;
	size_t bytes_read = 0;
	response_t response;

	bzero(&response, sizeof(response_t));

	if(request.service_id == SERVICE_ID_GET_REQUEST)
	{
		printf("## SERVER %s ## Request received: GET_REQUEST.\n", port);
		query_database(request, args, &response);
		printf("## SERVER %s ## GET_REQUEST_RESPONSE => Parts: ", port);
		for(int i = 0; i < TOTAL_SERVERS; i++)
			printf("%02X  ", response.parts[i]);
		printf("\n");
		write(args.client_sock_arg, &response, sizeof(response_t));
	}

	else if(request.service_id == SERVICE_ID_GET_FILE_REQUEST)
	{
		printf("## SERVER %s ## Request received: GET_FILE_REQUEST.\n", port);
		bzero(path, sizeof(path));
		sprintf(path, "%s/%s", args.base_path, request.username);

		if(strlen(request.dirname) != 0)
		{
			bzero(path, sizeof(path));
			sprintf(path, "%s/%s/%s", args.base_path, request.username, request.dirname);
		}

		part_id = identify_part(request.parts);
		sprintf(path, "%s/%s.%d", path, request.filename, part_id);

		if((file = fopen(path,"rb")) != NULL)
		{
			part_file_size = get_filesize(file);
			response.size = part_file_size;
			write(args.client_sock_arg, &response, sizeof(response_t));

			part_file = (unsigned char*) malloc((part_file_size * sizeof(unsigned char)));
			bzero(part_file, part_file_size);
			rewind(file);
			bytes_read = fread(part_file, sizeof(unsigned char), part_file_size, file);
			//printf("Here before encryption\n");
			md5_digest_buffer(part_file, part_file_size);
			xor_crypto(request.password, part_file, response.size);
			//printf("Here after encryption\n");
			md5_digest_buffer(part_file, part_file_size);

			send_part_file(args.client_sock_arg, part_file, part_file_size);
			free(part_file);
		}

		else
			printf("## SERVER %s ##  File \"%s\" was not found.\n", port, path);
	}
}

void makedir(request_t request, pthread_args_t args)
{
	DIR* dir;
	unsigned char path[DIRNAME_LENGTH];
	response_t response;
	bzero(&response, sizeof(response_t));

	bzero(path, sizeof(path));
	sprintf(path, "%s/%s", args.base_path, request.username);

	printf("## SERVER %s ## Request received: MKDIR.\n", port);
	if((dir = opendir(path)) == NULL)
		mkdir(path, 0777);

	else
		closedir(dir);

	if(strlen(request.dirname) != 0)
	{
		bzero(path, sizeof(path));
		sprintf(path, "%s/%s/%s", args.base_path, request.username, request.dirname);

		if((dir = opendir(path)) == NULL)
		{
			mkdir(path, 0777);
			response.header = SERVICE_ID_MKDIR_RESPONSE_SUCCESS;
			write(args.client_sock_arg, &response, sizeof(response_t));
			printf("## SERVER %s ## Directory \"%s\" has been created successfully.\n", port, path);
		}

		else
		{
			response.header = SERVICE_ID_MKDIR_RESPONSE_EXISTS;
			write(args.client_sock_arg, &response, sizeof(response_t));
			printf("## SERVER %s ## Directory \"%s\" already exists.\n", port, path);
			closedir(dir);
		}
	}

	return;
}

void list(request_t request,pthread_args_t args)
{
	FILE *db;
	unsigned char path[DIRNAME_LENGTH];
	database_t read;
	response_t response;

	printf("## SERVER %s ## Request received: LIST.\n", port);

	bzero(path, sizeof(path));
	bzero(&response, sizeof(response_t));
	sprintf(path, "%s/%s/%s.db", args.base_path, request.username, request.username);

	if((db = fopen(path, "rb")) == NULL)
	{
		response.header = SERVICE_ID_LIST_RESPONSE_EMPTY;
		write(args.client_sock_arg, &response, sizeof(response_t));
		printf("## SERVER %s ## Response sent: Server empty!\n", port);
		return;
	}

	else
	{
		response.header = SERVICE_ID_LIST_RESPONSE_SUCCESS;
		write(args.client_sock_arg, &response, sizeof(response_t));
		fread(&read, sizeof(database_t), 1, db);
		send_part_file(args.client_sock_arg, (unsigned char *) &read, sizeof(database_t));
		fclose(db);
		printf("## SERVER %s ## List file sent.\n", port);
		return;
	}
}

size_t get_filesize(FILE* file)
{
	unsigned char* buffer = (unsigned char *) malloc(FILE_READ_BUFFER_SIZE * sizeof(unsigned char));
	size_t bytes_read = 0;
	size_t total_bytes_read = 0;

	rewind(file);
	bzero(buffer, FILE_READ_BUFFER_SIZE);

	while(feof(file) == 0)
  {
    bytes_read = fread(buffer, sizeof(unsigned char), FILE_READ_BUFFER_SIZE, file);
    total_bytes_read = total_bytes_read + bytes_read;
  }

	free(buffer);
	rewind(file);
	return total_bytes_read;
}

void send_part_file(int client_sock, unsigned char* part_file, size_t size)
{
  unsigned char *read_location;
  size_t bytes_left = size, bytes_send = 0;

  read_location = part_file;

	while(bytes_send != size)
		bytes_send += write(client_sock, read_location + bytes_send, size - bytes_send);
}

void receive_part_file(int sock, unsigned char* part_file, size_t size)
{
	size_t bytes_read = 0;
	unsigned char* write;

	write = part_file;

  while(bytes_read != size)
    bytes_read += recv(sock, write + bytes_read, size - bytes_read, 0);
}

void query_database(request_t request, pthread_args_t args, response_t* response)
{
	FILE *db;
	unsigned char path[DIRNAME_LENGTH];
	database_t store;

	bzero(path, sizeof(path));
	sprintf(path, "%s/%s/%s.db", args.base_path, request.username, request.username);

	if((db = fopen(path, "rb")) == NULL)
	{
		response->header = SERVICE_ID_FILE_NOT_FOUND;
		return;
	}

	else
	{
		fread(&store, sizeof(database_t), 1, db);
		fclose(db);

		for(int i = 0; i < store.entries; i++)
		{
			if((strcmp(store.dirname[i], request.dirname) == 0) && (strcmp(store.filename[i], request.filename) == 0))
			{
				response->header = SERVICE_ID_FILE_FOUND;
				memcpy(response->parts, store.parts[i], TOTAL_SERVERS);
				return;
			}
		}
		response->header = SERVICE_ID_FILE_NOT_FOUND;
		return;
	}
}

void registry(request_t request, pthread_args_t args)
{
	FILE *db;
	unsigned char path[DIRNAME_LENGTH];
	unsigned int stored_flag = 500;
	database_t store;

	bzero(path, sizeof(path));
	bzero(&store, sizeof(store));

	printf("## SERVER %s ## OPENING REGISTRY.\n", port);
	sprintf(path, "%s/%s/%s.db", args.base_path, request.username, request.username);

	if((db = fopen(path, "rb")) == NULL)
	{
		printf("\n## REGISTER %s ## Creating a new database file.\n", port);
		db = fopen(path, "wb");

		memcpy(store.dirname[0], request.dirname, DIRNAME_LENGTH);
		memcpy(store.filename[0], request.filename, FILENAME_LENGTH);
		memcpy(store.parts[0], request.parts, TOTAL_SERVERS);
		store.entries = 1;
		printf("## REGISTER %s ## Filename: _%s_\n", port, store.filename[0]);
		printf("## REGISTER %s ## Folder name: _%s_\n", port, store.dirname[0]);
		printf("## REGISTER %s ## Parts: ", port);

		for(int i = 0; i < TOTAL_SERVERS; i++)
			printf("%02X  ",store.parts[0][i]);

		printf("\n\n");
		fwrite((void *) &store, sizeof(database_t), 1, db);
		fclose(db);
	}

	else
	{
		fread(&store, sizeof(database_t), 1, db);

		for(unsigned int i = 0; i < store.entries; i++)
		{
			if((strcmp(store.dirname[i], request.dirname) == 0) && (strcmp(store.filename[i], request.filename) == 0))
			{
				stored_flag = i;
				for(int j = 0; j < TOTAL_SERVERS; j++)
					store.parts[i][j] = (store.parts[i][j] | request.parts[j]);
			}
		}


		fclose(db);
		remove(path);
		db = fopen(path, "wb");

		if(stored_flag > store.entries)
		{
			memcpy(store.dirname[store.entries], request.dirname, DIRNAME_LENGTH);
			memcpy(store.filename[store.entries], request.filename, FILENAME_LENGTH);
			memcpy(store.parts[store.entries], request.parts, TOTAL_SERVERS);

			printf("\n\n## REGISTER %s ## Filename: _%s_\n", port, store.filename[store.entries]);
			printf("## REGISTER %s ## Folder name: _%s_\n", port, store.dirname[store.entries]);
			printf("## REGISTER %s ## Parts: ", port);

			for(int i = 0; i < TOTAL_SERVERS; i++)
				printf("%02X  ",store.parts[store.entries][i]);

			printf("\n\n");
			store.entries = store.entries + 1;
			fwrite((void *) &store, sizeof(database_t), 1, db);
		}

		else
		{
			printf("\n\n## REGISTER %s ## Element: _%u_\n", port, stored_flag);
			printf("## REGISTER %s ## Filename: _%s_\n", port, store.filename[stored_flag]);
			printf("## REGISTER %s ## Folder name: _%s_\n", port, store.dirname[stored_flag]);
			printf("## REGISTER %s ## Parts: ", port);

			for(int i = 0; i < TOTAL_SERVERS; i++)
				printf("%02X  ",store.parts[stored_flag][i]);

			printf("\n\n");
			fwrite((void *) &store, sizeof(database_t), 1, db);
		}

		fclose(db);
	}
}

unsigned int identify_part(unsigned char* parts_table)
{
	unsigned int part_id = 0;

	for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
	{
		if(parts_table[part_id] == MAP)
			return part_id;
	}
}

void xor_crypto(unsigned char* password, unsigned char* input, size_t size)
{
	int keylen;
	keylen = strlen(password);

  // for(int i = 0; i < pwd_length; i++)
  //   key = key + (password[i]/pwd_length);
	//
  // //printf("\n$$ CIPHER %X $$\n", key);

	for(int i = 0; i < size; i++)
		input[i] = input[i] ^ password[i % keylen];
}

void send_response(int sock, unsigned char header, size_t size, unsigned int part_id)
{
	response_t response;
	bzero(&response, sizeof(response_t));

	response.header = header;
	response.size = size;
	response.parts[part_id] = MAP;

	//printf("sending response %x\n", response.header);
	write(sock, &response, sizeof(response_t));
}

unsigned char authenticator(FILE *config_file, unsigned char* username, unsigned char* password)
{
  unsigned char read_password[USERNAME_PASSWORD_LENGTH];

  bzero(read_password, sizeof(read_password));

  if(parse_file(config_file, username, read_password) == 0)
    return AUTHENTICATION_USER_NOT_FOUND;

  if(strcmp(password, read_password + 1) != 0)
    return AUTHENTICATION_INVALID_PASSWORD;

  return AUTHENTICATION_SUCCESSFUL;
}

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


void set_thread_slot_state(int thread_slot, unsigned char state)
{
	sem_wait(&sem_slots);
	thread_slot_tracker[thread_slot] = state;
	sem_post(&sem_slots);
}

unsigned int parse_file(FILE* config_file, unsigned char* entity, unsigned char* entity_match)
{
    unsigned char buffer[512];
		unsigned char found_matches = 0;
		unsigned char* match;

    rewind(config_file);

    while(feof(config_file) == 0)
    {
      bzero(buffer, sizeof(buffer));
      fgets(buffer, sizeof(buffer), config_file);
			match = strstr(buffer, entity);

      if(match == NULL)
				continue;

      else if(match == buffer)
      {
        match = buffer + strlen(entity);
        strncat(entity_match, match, strlen(match));

        found_matches++;
      }
    }

		entity_match[strlen(entity_match) - 1] = '\0';

		return found_matches;
}

size_t md5_digest(FILE* file, unsigned char* md5sum)
{
  MD5_CTX ctx;
  size_t bytes_read = 0;
  size_t total_bytes_read = 0;
  char mdString[(2 * MD5_DIGEST_LENGTH) + 1];
  unsigned char* buffer = (unsigned char *) malloc(FILE_READ_BUFFER_SIZE * sizeof(unsigned char));

  MD5_Init(&ctx);
  rewind(file);
	bzero(buffer, FILE_READ_BUFFER_SIZE);

  while(feof(file) == 0)
  {
    bytes_read = fread(buffer, sizeof(unsigned char), FILE_READ_BUFFER_SIZE, file);
    MD5_Update(&ctx, buffer, bytes_read);
    total_bytes_read = total_bytes_read + bytes_read;
  }

  MD5_Final(md5sum, &ctx);

  for(int i = 0; i < 16; i++)
     sprintf(&mdString[i*2], "%02x", (unsigned int) md5sum[i]);

  printf("## MD5 DIGEST ## File Size = %lu\t\tMD5SUM = %s\n", total_bytes_read, mdString);

  free(buffer);
  rewind(file);
  return total_bytes_read;
}

void md5_digest_buffer(unsigned char* buffer, size_t size)
{
  MD5_CTX ctx;
  unsigned char md5sum[MD5_DIGEST_LENGTH];
  char mdString[(2 * MD5_DIGEST_LENGTH) + 1];

  MD5_Init(&ctx);
  MD5_Update(&ctx, buffer, size);
  MD5_Final(md5sum, &ctx);

  for(int i = 0; i < 16; i++)
     sprintf(&mdString[i*2], "%02x", (unsigned int) md5sum[i]);

  printf("## MD5 DIGEST ## Size = %lu\t\tMD5SUM = %s\n", size, mdString);
}
