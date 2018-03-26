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

#define TOTAL_LIST_ENTRIES											50

typedef struct database
{
	unsigned int entries;
	unsigned char dirname[TOTAL_LIST_ENTRIES][DIRNAME_LENGTH];
	unsigned char filename[TOTAL_LIST_ENTRIES][FILENAME_LENGTH];
	unsigned char parts[TOTAL_LIST_ENTRIES][TOTAL_SERVERS];
} database_t;

typedef struct server_attr
{
  unsigned char ip[16];
  unsigned char port[16];
  int socket;
} server_attr_t;

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

typedef struct table
{
  unsigned char servers[TOTAL_SERVERS];
  unsigned char parts[TOTAL_SERVERS][TOTAL_SERVERS];
} table_t;


int server_sock_addr_size;
server_attr_t server_attributes[TOTAL_SERVERS];
struct sockaddr_in server_sock_addr[TOTAL_SERVERS];
struct timespec current_time;

void home(unsigned char *, unsigned char *);
unsigned char parse_command(unsigned char*);
unsigned char authenticator(FILE *, unsigned char *, unsigned char *, unsigned char*);
unsigned int parse_file(FILE *, unsigned char*, unsigned char*);
unsigned char search_user_config(unsigned char *, unsigned char *, unsigned char *);
unsigned char fetch_server_attr(unsigned char *, unsigned char *, server_attr_t *);
void execute_service(unsigned char *, unsigned char *, unsigned char, unsigned char *);
void get(unsigned char *, unsigned char *, unsigned char *);
void put(unsigned char *, unsigned char *, unsigned char *);
void list(unsigned char *, unsigned char *);
void mkdir(unsigned char *, unsigned char *, unsigned char *);
void establish_connection(int*, unsigned char*);
size_t md5_digest(FILE*, unsigned char*);
unsigned char pre_verify(table_t* , unsigned int);
unsigned char get_partfile_sizes(size_t*, size_t);
unsigned char get_part(FILE*, unsigned int, size_t* , unsigned char*);
void close_sockets(table_t, int*);
void send_request(int, unsigned char*, unsigned char*, unsigned char, unsigned char*, unsigned char*, size_t, unsigned int);
void xor_crypto(unsigned char*, unsigned char*, size_t);
unsigned char authenticate_and_receive(int, response_t*);
void send_part_file(int, unsigned char*, size_t);
void receive_part_file(int, unsigned char*, size_t);
size_t write_part_to_file(unsigned char*, unsigned char*, size_t);
void md5_digest_buffer(unsigned char*, size_t);

int main(int argc, char** argv[])
{
  unsigned char username[USERNAME_PASSWORD_LENGTH];
  unsigned char password[USERNAME_PASSWORD_LENGTH];
  unsigned char user_config_filename[64];
  unsigned int server_id = 0;
  unsigned char service_id = 0;
  unsigned char command[COMMAND_LENGTH];
  unsigned char temp[64];
  unsigned char status = 0x00;

  FILE *config_file;

  if (argc > 1)
	{
		printf ("\nUSAGE: ./client\n");
    printf ("\nEdit the client configuration file dfc.conf to edit its properties.\n");
		exit(1);
	}

  while(1)
  {
    if(config_file == NULL)
    {
      printf("\n## ERROR ## Unable to open client configuration file or dfc.conf is missing. EXITING THE CLIENT.\n");
      exit(1);
    }

    bzero(username, sizeof(username));
    bzero(password, sizeof(password));

    home(username, password);
    bzero(user_config_filename, sizeof(user_config_filename));
    config_file = fopen("dfc.conf", "r");

    if((status = authenticator(config_file, username, password, user_config_filename)) != AUTHENTICATION_SUCCESSFUL)
    {
      if(status == AUTHENTICATION_USER_NOT_FOUND)
        printf("## ERROR ## User not found.\n");

      else if(status == AUTHENTICATION_USER_CONFIG_NOT_FOUND)
        printf("## ERROR ## User config file not found.\n");

      else if(status == AUTHENTICATION_INVALID_CONFIG_FILE)
        printf("## ERROR ## Invalid user config file.\n");

      else if(status == AUTHENTICATION_INVALID_PASSWORD)
        printf("## ERROR ## Incorrect password.\n");

      else
        printf("## ERROR ## Unknown authentication error.\n");

      fclose(config_file);
      continue;
    }

    fclose(config_file);
    printf("## CLIENT ## Authentication successful.\n");

    printf("## CLIENT ## Reading %s's configuration..\n", username);
    printf("\t\t----------\t\t-----------\t\t--------\n");
    printf("\t\t  SERVER  \t\t  ADDRESS  \t\t  PORT  \n");
    printf("\t\t----------\t\t-----------\t\t--------\n");

    for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
    {
      bzero(temp, sizeof(temp));
      bzero(&server_attributes[server_id], sizeof(server_attributes));
      sprintf(temp, "server%d ", server_id);

      if(fetch_server_attr(user_config_filename, temp, &server_attributes[server_id]) == SERVER_ATTR_NOT_FOUND)
      {
        printf("## ERROR ## Server %u attributes not found / multiple occurances of server attributes found / invalid syntax\n", server_id);
        status = SERVER_ATTR_NOT_FOUND;
        continue;
      }

      printf("\t\t  DFS #%u   \t\t %s\t\t  %s  \n", server_id, server_attributes[server_id].ip, server_attributes[server_id].port);
    }

    printf("\t\t----------\t\t-----------\t\t--------\n");

    if(status == SERVER_ATTR_NOT_FOUND)
      continue;

    for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
    {
      bzero(&server_sock_addr[server_id], sizeof(server_sock_addr[server_id]));                    //zero the struct

      server_sock_addr[server_id].sin_family = AF_INET;                   //address family
    	server_sock_addr[server_id].sin_port = htons(atoi(server_attributes[server_id].port));    //htons() sets the port # to network byte order
      server_sock_addr[server_id].sin_addr.s_addr = inet_addr(server_attributes[server_id].ip);
    }

    server_sock_addr_size = sizeof(struct sockaddr_in);

    printf("\n\t\t\t## AVAILABLE COMMANDS ##\nget [<remote_directory> :: <remote_filename>]     put [<local_filename/path> :: <remote_directory>]     list     mkdir     logout     exit\n");

    while(1)
    {
      bzero(command, sizeof(command));
  		printf("\n$ ");
  		fgets(command, sizeof(command), stdin);

  		command[strlen(command) - 1] = '\0';
      service_id = parse_command(command);

  		if(service_id == INVALID_COMMAND)
  		{
  			printf("\n## ERROR ## Invalid command.\n");
  			continue;
  		}

      else if(service_id == EXIT_COMMAND)
      {
        printf("\n## CLIENT ## Exiting gracefully\n");
  			break;
      }

      else if(service_id == LOGOUT_COMMAND)
      {
        printf("## CLIENT ## Logging out %s.\n", username);
        break;
      }

  	  else
  			execute_service(username, password, service_id, command);
    }

    if(service_id == EXIT_COMMAND)
      break;
  }

}

void home(unsigned char* username, unsigned char* password)
{
  printf("\nUsername: ");
  fgets(username, USERNAME_PASSWORD_LENGTH, stdin);
  username[strlen(username) - 1] = ' ';

  printf("Password: ");
  fgets(password, USERNAME_PASSWORD_LENGTH, stdin);
  password[strlen(password) - 1] = ' ';
}

unsigned char parse_command(unsigned char* input)
{
		int i = 0;
		unsigned char* commands[TOTAL_COMMANDS];
		unsigned char* match;

		commands[0] = "get ";
		commands[1] = "put ";
		commands[2] = "list";
		commands[3] = "mkdir ";
		commands[4] = "logout";
    commands[5] = "exit";

		for(i = 0; i < TOTAL_COMMANDS; i++)
		{
			match = strstr(input, commands[i]);

			if(match == input)
				return (unsigned char)(i+1);
		}

		return (INVALID_COMMAND);
}

void execute_service(unsigned char* username, unsigned char* password, unsigned char service, unsigned char* command)
{
	unsigned char entity[COMMAND_LENGTH];

	switch(service)
	{
		case 0x01:
		strcpy(entity, command + 4);
		get(username, password, entity);
		break;

		case 0x02:
		strcpy(entity, command + 4);
		put(username, password, entity);
		break;

		case 0x03:
		list(username, password);
		break;

		case 0x04:
    strcpy(entity, command + 6);
		mkdir(username, password, entity);
		break;
	}
}

void get(unsigned char *username, unsigned char *password, unsigned char *entity)
{
  int client_sock[TOTAL_SERVERS];
  table_t get_table;
  unsigned char filename[FILENAME_LENGTH];
  unsigned char dirname[DIRNAME_LENGTH];
  unsigned char verify = 0;
  unsigned int server_id = 0;
  unsigned int part_id = 0;
  unsigned char* part_file;
  size_t file_size = 0;
  size_t partfile_sizes[TOTAL_SERVERS];
  unsigned char pre_verify_parts[TOTAL_SERVERS];
  unsigned char post_verify_parts[TOTAL_SERVERS];
  response_t response;

  bzero(client_sock, sizeof(client_sock));
  bzero(filename, sizeof(filename));
  bzero(dirname, sizeof(dirname));

  if(sscanf(entity, "%[^::]%*[^ ] %[^\n]", filename, dirname) < 1)
  {
    printf("## ERROR ## Usage: get <remote_filename> :: <remote_directory>\n");
    return;
  }

  if(filename[strlen(filename) - 1] == ' ')
    filename[strlen(filename) - 1] = '\0';

  printf("## CLIENT ## Remote File: \"%s\" \t\t Remote Directory: \"%s\"\n", filename, dirname);

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if((client_sock[server_id] = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
      printf("## ERROR ##  Unable to create socket.\n");
      exit(1);
    }
  }

  printf("## CLIENT ## Sockets successfully created.\n");

  printf("## CLIENT ## Establishing connection with the servers: \n");
  bzero(get_table.servers, sizeof(get_table.servers));
  establish_connection(client_sock, get_table.servers);

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id ++)
  {
    printf("\n## CLIENT ## If remainder: %d\n", server_id);

    bzero(get_table.parts, sizeof(get_table.parts));
    pre_verify_parts[server_id] = pre_verify(&get_table, server_id);
  }

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id ++)
  {
    if(pre_verify_parts[server_id] == DISCARD)
    {
      printf("\n## ERROR ## Download wouldn't be successsful. Minimum required servers are not online.\n");
      close_sockets(get_table, client_sock);
      return;
    }
  }

  bzero(get_table.parts, sizeof(get_table.parts));

  bzero(&current_time, sizeof(time_t));
  clock_gettime(CLOCK_MONOTONIC, &current_time);

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id ++)
  {

    if(get_table.servers[server_id] == CONNECTION_SUCCESSFUL)
    {
      send_request(client_sock[server_id], username, password, SERVICE_ID_GET_REQUEST, dirname, filename, 0, 0);
      printf("GET REQUEST sent to Server %d\n", server_id);

      if((verify = authenticate_and_receive(client_sock[server_id], &response)) != AUTHENTICATION_SUCCESSFUL)
      {
        if(verify == ZERO_RESPONSE)
          printf("## CLIENT ## No authentication response from the server %d.\n", server_id);

        else if(verify == AUTHENTICATION_INVALID_PASSWORD)
          printf("## SERVER %d ## Invalid password.\n", server_id);

        else if(verify == AUTHENTICATION_USER_NOT_FOUND)
          printf("## SERVER %d ## User not found.\n", server_id);

        printf("## CLIENT ## Exiting GET.\n");
        return;
      }

      printf("## SERVER %d ## authentication successful\n", server_id);
      authenticate_and_receive(client_sock[server_id], &response);

      if(response.header == SERVICE_ID_FILE_NOT_FOUND)
      {
        printf("## SERVER %d ## FILE NOT FOUND.\n", server_id);
        continue;
      }

      else
      {
        memcpy(get_table.parts[server_id], response.parts, TOTAL_SERVERS);
        printf("File parts received.\n");
      }
    }
  }

  bzero(partfile_sizes, sizeof(partfile_sizes));
  bzero(post_verify_parts, sizeof(post_verify_parts));

  for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
  {
    verify = ERROR;

    for(server_id = 0; server_id < TOTAL_SERVERS; server_id ++)
    {
      if(verify == SUCCESS)
        continue;

      if(get_table.parts[server_id][part_id] == MAP)
      {
        send_request(client_sock[server_id], username, password, SERVICE_ID_GET_FILE_REQUEST, dirname, filename, 0, part_id);
        printf("## CLIENT ## Sending GET_FILE request to server %d.\n", server_id);

        if((verify = authenticate_and_receive(client_sock[server_id], &response)) != AUTHENTICATION_SUCCESSFUL)
        {
          if(verify == ZERO_RESPONSE)
            printf("## CLIENT ## No Authentication response from server %d.\n", server_id);

          else if(verify == AUTHENTICATION_INVALID_PASSWORD)
            printf("## SERVER %d ## Invalid password.\n", server_id);

          else if(verify == AUTHENTICATION_USER_NOT_FOUND)
            printf("## SERVER %d ## User not found.\n", server_id);

          printf("## CLIENT ## Exiting GET command.\n");
          continue;
        }

        printf("## SERVER %d ## Authentication successful\n", server_id);
        authenticate_and_receive(client_sock[server_id], &response);
        partfile_sizes[part_id] = response.size;

        printf("## SERVER %d ## PartFile Size = %lu\n", server_id, response.size);

        part_file = (unsigned char *) malloc(partfile_sizes[part_id] * (sizeof(unsigned char)));
        bzero(part_file, partfile_sizes[part_id]);

        receive_part_file(client_sock[server_id], part_file, partfile_sizes[part_id]);
        printf("## SERVER %d ## Received Part %d\n", server_id, part_id);
        md5_digest_buffer(part_file, partfile_sizes[part_id]);
        xor_crypto(password, part_file, partfile_sizes[part_id]);
        md5_digest_buffer(part_file, partfile_sizes[part_id]);

        if(write_part_to_file(filename, part_file, partfile_sizes[part_id]) != partfile_sizes[part_id])
        {
          printf("## CLIENT ## Error riting part %d to the file.\n", part_id);
          free(part_file);
          continue;
        }

        free(part_file);
        verify = SUCCESS;
        post_verify_parts[part_id] = MAP;
      }
    }
  }

  for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
  {
    if(post_verify_parts[part_id] != MAP)
    {
      printf("## CLIENT ## Download unsuccessful.\n");
      verify = ERROR;
      remove(filename);
      break;
    }
  }

  if(verify != ERROR)
    printf("## CLIENT ## Download successful.\n");

  close_sockets(get_table, client_sock);
}

void put(unsigned char *username, unsigned char *password, unsigned char *entity)
{
  int client_sock[TOTAL_SERVERS];
  table_t put_table;
  unsigned char* filename_pointer;
  unsigned char path[FILENAME_LENGTH];
  unsigned char filename[FILENAME_LENGTH];
  unsigned char dirname[DIRNAME_LENGTH];
  unsigned char md5sum[MD5_DIGEST_LENGTH];
  unsigned int md5_remainder = 0;
  unsigned char verify = 0x00;
  unsigned int server_id;
  unsigned int part_id;
  unsigned char* part_file;
  size_t file_size = 0;
  size_t partfile_sizes[TOTAL_SERVERS];
  unsigned char post_verify_parts[TOTAL_SERVERS];
  response_t response;

  FILE *file;

  bzero(client_sock, sizeof(client_sock));
  bzero(path, sizeof(path));
  bzero(filename, sizeof(filename));
  bzero(dirname, sizeof(dirname));

  if(sscanf(entity, "%[^::]%*[^ ] %[^\n]", path, dirname) < 1)
  {
    printf("## ERROR ## Usage: put <local_filename/path> :: <remote_directory>\n");
    return;
  }

  if(path[strlen(path) - 1] == ' ')
    path[strlen(path) - 1] = '\0';

  printf("## CLIENT ## Local File: \"%s\" \t\t Remote Directory: \"%s\"\n", path, dirname);

  if((file = fopen(path, "r")) == NULL)
  {
    printf("## ERROR ## Unable to open the local file \"%s\"\n", filename);
    return;
  }

  filename_pointer = strrchr(path, '/');
  if(filename_pointer == NULL)
    strcpy(filename, path);
  else
    strcpy(filename, filename_pointer + 1);

  //printf("FILENAME --> _%s_", filename);

  printf("## CLIENT ## Calculating MD5. Please wait !\n");
  file_size = md5_digest(file, md5sum);

  md5_remainder = ((unsigned int) md5sum[sizeof(md5sum) - sizeof(unsigned int)]) % TOTAL_SERVERS;

  //Causes the system to create a generic socket of type UDP (datagram)
  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if((client_sock[server_id] = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
      printf("## ERROR ##  Unable to create socket.\n");
      exit(1);
    }
  }

  printf("## CLIENT ## Sockets successfully created.\n");

  printf("## CLIENT ## Establishing connection with the servers: \n");
  bzero(put_table.servers, sizeof(put_table.servers));
  establish_connection(client_sock, put_table.servers);

  bzero(put_table.parts, sizeof(put_table.parts));
  verify = pre_verify(&put_table, md5_remainder);

  if(verify == DISCARD)
  {
    printf("\n## ERROR ## Upload wouldn't be successsful. Minimum required servers are not online.\n");
    close_sockets(put_table, client_sock);
    return;
  }

  if(get_partfile_sizes(partfile_sizes, file_size) == DISCARD)
  {
    printf("\n## ERROR ## Part file sizes doesn't add up to the total size.\n");
    close_sockets(put_table, client_sock);
    return;
  }

  bzero(post_verify_parts, sizeof(post_verify_parts));

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    verify = AUTHENTICATION_SUCCESSFUL;

    for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
    {
      if(verify != AUTHENTICATION_SUCCESSFUL)
        continue;

      if(put_table.parts[server_id][part_id] == MAP)
      {
        part_file = (unsigned char*) malloc(partfile_sizes[part_id] * sizeof(unsigned char));
        bzero(part_file, partfile_sizes[part_id]);

        send_request(client_sock[server_id], username, password, SERVICE_ID_PUT_REQUEST, dirname, filename, partfile_sizes[part_id], part_id);

        if((verify = authenticate_and_receive(client_sock[server_id], &response)) != AUTHENTICATION_SUCCESSFUL)
        {
          if(verify == ZERO_RESPONSE)
            printf("## CLIENT ## No Authentication response from the server %d.\n", server_id);

          else if(verify == AUTHENTICATION_INVALID_PASSWORD)
            printf("## SERVER %d ## Invalid password.\n", server_id);

          else if(verify == AUTHENTICATION_USER_NOT_FOUND)
            printf("## SERVER %d ## User not found.\n", server_id);

          free(part_file);
          continue;
        }

        if(get_part(file, part_id, partfile_sizes, part_file) == ERROR)
        {
          printf("## ERROR ## While obtaining part files: Part file size doesn't match!\n");
          close_sockets(put_table, client_sock);
          free(part_file);
          return;
        }

        md5_digest_buffer(part_file, partfile_sizes[part_id]);
        xor_crypto(password, part_file, partfile_sizes[part_id]);
        //printf("After encryption: \n");
        md5_digest_buffer(part_file, partfile_sizes[part_id]);
        send_part_file(client_sock[server_id], part_file, partfile_sizes[part_id]);
        post_verify_parts[part_id] = MAP;
        free(part_file);
      }
    }
  }

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id ++)
  {
    printf("%X\t", post_verify_parts[server_id]);
    if(post_verify_parts[server_id] != MAP)
      {
        printf("## CLIENT ## Upload unsuccessful.\n");
        verify = ERROR;
        break;
      }
  }

  if(verify != ERROR)
    printf("## CLIENT ## Upload successful.\n");

  close_sockets(put_table, client_sock);
  fclose(file);
}

void list(unsigned char *username, unsigned char *password)
{
  int client_sock[TOTAL_SERVERS];
  table_t list_table;
  unsigned int server_id = 0;
  unsigned char verify = 0;
  response_t response;
  database_t part_list, final_list;
  unsigned int part_entry = 0, final_entry = 0, stored_entry_location = 0;

  bzero(client_sock, sizeof(client_sock));

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if((client_sock[server_id] = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
      printf("## ERROR ##  Unable to create socket.\n");
      exit(1);
    }
  }
  printf("## CLIENT ## Sockets successfully created.\n");

  printf("## CLIENT ## Establishing connection with the servers: \n");
  bzero(list_table.servers, sizeof(list_table.servers));
  establish_connection(client_sock, list_table.servers);

  /*if(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if(list_table.servers[server_id] == CONNECTION_UNSUCCESSFUL)
    {
      printf("\n## ERROR ## Listing unsuccesssful. All servers are not online.\n");
      close_sockets(get_table, client_sock);
      return;
    }
  }*/

  bzero(&response, sizeof(response_t));
  bzero(&final_list, sizeof(database_t));

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if(list_table.servers[server_id] == CONNECTION_SUCCESSFUL)
    {
      send_request(client_sock[server_id], username, password, SERVICE_ID_LIST_REQUEST, NULL, NULL, 0, 0);

      if((verify = authenticate_and_receive(client_sock[server_id], &response)) != AUTHENTICATION_SUCCESSFUL)
      {
        if(verify == ZERO_RESPONSE)
          printf("## CLIENT ## No Authentication response from the server %d.\n", server_id);

        else if(verify == AUTHENTICATION_INVALID_PASSWORD)
          printf("## SERVER %d ## Invalid password.\n", server_id);

        else if(verify == AUTHENTICATION_USER_NOT_FOUND)
          printf("## SERVER %d ## User not found.\n", server_id);

        continue;
      }

      bzero(&response, sizeof(response_t));
      authenticate_and_receive(client_sock[server_id], &response);
      if(response.header == SERVICE_ID_LIST_RESPONSE_EMPTY)
      {
        printf("## SERVER %d ## NO FILES.\n", server_id);
        continue;
      }

      else if(response.header == SERVICE_ID_LIST_RESPONSE_SUCCESS)
        printf("## SERVER %d ## FILE LIST SENT.\n", server_id);

      bzero(&part_list, sizeof(database_t));
      receive_part_file(client_sock[server_id], (unsigned char *) &part_list, sizeof(database_t));

      for(part_entry = 0; part_entry < part_list.entries; part_entry++)
      {
        stored_entry_location = final_list.entries + 1;
        for(final_entry = 0; final_entry < final_list.entries; final_entry++)
        {
          if((strcmp(part_list.dirname[part_entry], final_list.dirname[final_entry]) == 0) && (strcmp(part_list.filename[part_entry], final_list.filename[final_entry]) == 0))
    			{
            stored_entry_location = final_entry;
    				for(int j = 0; j < TOTAL_SERVERS; j++)
    					final_list.parts[final_entry][j] = (final_list.parts[final_entry][j] | part_list.parts[part_entry][j]);

            break;
    			}
        }

        if(stored_entry_location > final_list.entries)
        {
          memcpy(final_list.dirname[final_list.entries], part_list.dirname[part_entry], DIRNAME_LENGTH);
    			memcpy(final_list.filename[final_list.entries], part_list.filename[part_entry], FILENAME_LENGTH);
    			memcpy(final_list.parts[final_list.entries], part_list.parts[part_entry], TOTAL_SERVERS);
          final_list.entries = final_list.entries + 1;
        }
      }
    }

    else
      printf("## SERVER %d ## OFFLINE.\n", server_id);
  }

  close_sockets(list_table, client_sock);

  printf("\t\t\t## FILE LIST FROM AVAILABLE SERVERS ##\n\n");
  printf("\t------------------------------- \t --------------------------------\t");

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
    printf("------\t");

  printf("\n");
  printf("\t            DIRNAME                 \t             FILENAME            \t");

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
    printf("  P%d  \t", server_id);

  printf("\n");
  printf("\t------------------------------- \t --------------------------------\t");

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
    printf("------\t");

  for(final_entry = 0; final_entry < final_list.entries; final_entry++)
  {
    printf("\n\t\t%s", final_list.dirname[final_entry]);

    if(strlen(final_list.dirname[final_entry]) < 12)
      printf("\t\t\t\t\t");
    else if(strlen(final_list.dirname[final_entry]) > 15)
      printf("\t\t\t");
    else
      printf("\t\t\t\t");

    printf("%s", final_list.filename[final_entry]);

    if(strlen(final_list.filename[final_entry]) < 12)
      printf("\t\t\t\t");
    else if(strlen(final_list.filename[final_entry]) > 15)
      printf("\t\t");
    else
      printf("\t\t\t");

    for(int i = 0; i < TOTAL_SERVERS; i++)
      printf("  %02X  \t", final_list.parts[final_entry][i]);
  }

  printf("\n\n");
  return;
}

void mkdir(unsigned char *username, unsigned char *password, unsigned char *entity)
{
  int client_sock[TOTAL_SERVERS];
  table_t mkdir_table;
  unsigned char dirname[DIRNAME_LENGTH];
  unsigned int server_id = 0;
  unsigned char verify = 0;
  response_t response;

  bzero(client_sock, sizeof(client_sock));
  bzero(dirname, sizeof(dirname));

  if(sscanf(entity, "%[^\n]", dirname) < 1)
  {
    printf("## ERROR ## Usage: mkdir <remote_directory>\n");
    return;
  }

  if(strlen(dirname) == 0)
	{
    printf("## ERROR ## Usage: mkdir <remote_directory>\n");
    return;
  }

  printf("## CLIENT ## Remote Directory: \"%s\"\n", dirname);

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if((client_sock[server_id] = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
      printf("## ERROR ##  Unable to create socket.\n");
      exit(1);
    }
  }

  printf("## CLIENT ## Sockets successfully created.\n");

  printf("## CLIENT ## Establishing connection with the servers: \n");
  bzero(mkdir_table.servers, sizeof(mkdir_table.servers));
  establish_connection(client_sock, mkdir_table.servers);

  bzero(&response, sizeof(response_t));
  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if(mkdir_table.servers[server_id] == CONNECTION_SUCCESSFUL)
    {
      send_request(client_sock[server_id], username, password, SERVICE_ID_MKDIR_REQUEST, dirname, NULL, 0, 0);

      if((verify = authenticate_and_receive(client_sock[server_id], &response)) != AUTHENTICATION_SUCCESSFUL)
      {
        if(verify == ZERO_RESPONSE)
          printf("## CLIENT ## No Authentication response from the server %d.\n", server_id);

        else if(verify == AUTHENTICATION_INVALID_PASSWORD)
          printf("## SERVER %d ## Invalid password.\n", server_id);

        else if(verify == AUTHENTICATION_USER_NOT_FOUND)
          printf("## SERVER %d ## User not found.\n", server_id);

        continue;
      }

      bzero(&response, sizeof(response_t));
      authenticate_and_receive(client_sock[server_id], &response);
      if(response.header == SERVICE_ID_MKDIR_RESPONSE_SUCCESS)
        printf("## CLIENT ## Directory \"%s\" has been created on server %d.\n", dirname, server_id);

      else if(response.header == SERVICE_ID_MKDIR_RESPONSE_EXISTS)
        printf("## CLIENT ## Directory \"%s\" already exists on server %d.\n", dirname, server_id);
      }
  }

  close_sockets(mkdir_table, client_sock);
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


size_t write_part_to_file(unsigned char* filename, unsigned char* part_file, size_t size)
{
  unsigned char* format_position;
  unsigned char format[8];
  unsigned char new_filename[FILENAME_LENGTH];
  size_t total_bytes_written;

  bzero(new_filename, sizeof(new_filename));
  strcpy(new_filename, filename);

  if((format_position = strrchr(new_filename, '.')) != NULL)
  {
    strcpy(format, format_position);
    *format_position = '\0';
    sprintf(new_filename, "%s_%ld%s",new_filename, current_time.tv_sec, format);
    printf("## CLIENT ## New file downloaded as \"%s\".\n", new_filename);
  }

  else
    sprintf(new_filename, "%s_%ld",new_filename, current_time.tv_sec);

  FILE* file = fopen(new_filename, "a");

  total_bytes_written = fwrite (part_file, sizeof(unsigned char), size, file);

  fclose(file);

  return total_bytes_written;
}


unsigned char authenticate_and_receive(int client_sock, response_t* response)
{
  bzero(response, sizeof(response_t));

  if((recv(client_sock, response, sizeof(response_t), 0)) > 0)
      return response->header;

  else
    return ZERO_RESPONSE;
}

void send_request(int client_sock, unsigned char* username, unsigned char* password, unsigned char request_id, unsigned char* dirname, unsigned char* filename, size_t size, unsigned int part_id)
{
  request_t request_packet;
  bzero(&request_packet, sizeof(request_t));

  if(username != NULL)
    strcpy(request_packet.username, username);

  if(password != NULL)
  strcpy(request_packet.password, password);

  request_packet.service_id = request_id;

  if(dirname != NULL)
  strcpy(request_packet.dirname, dirname);

  if(filename != NULL)
  strcpy(request_packet.filename, filename);

  request_packet.size = size;
  request_packet.parts[part_id] = MAP;

  write(client_sock, &request_packet, sizeof(request_t));
	printf("## CLIENT ## Sent request 0x%02X to server socket %d.\n", request_packet.service_id, client_sock);
}

void close_sockets(table_t table, int* client_sock)
{
  unsigned int server_id = 0;

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if(table.servers[server_id] == CONNECTION_SUCCESSFUL)
      close(client_sock[server_id]);
  }
}

unsigned char get_part(FILE* file, unsigned int part_id, size_t* partfile_sizes, unsigned char* part_file)
{
  unsigned int current_part = 0;
  long int offset = 0;
  size_t bytes_read = 0;

  for(current_part = 0; current_part < part_id; current_part++)
    offset = offset + partfile_sizes[current_part];

  fseek(file, offset, SEEK_SET);
  long int a = ftell(file);

  bytes_read = fread(part_file, sizeof(unsigned char), partfile_sizes[part_id], file);

  if(bytes_read == partfile_sizes[part_id])
    return SUCCESS;

  else
    return ERROR;
}

unsigned char get_partfile_sizes(size_t* partfile_size, size_t total_size)
{
  size_t unit_size = 0;
  size_t remaining = total_size;
  size_t total_size_check = 0;
  unsigned int part_id = 0;

  unit_size = total_size / TOTAL_SERVERS;

  for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
  {
    if(part_id != TOTAL_SERVERS - 1)
      partfile_size[part_id] = unit_size;

    else
      partfile_size[part_id] = remaining;

    remaining = remaining - unit_size;
  }

  for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
    total_size_check = total_size_check + partfile_size[part_id];

  if(total_size_check == total_size)
    return MAP;

  else
    return DISCARD;
}

unsigned char pre_verify(table_t* table, unsigned int offset)
{
  unsigned int server_id = 0;
  unsigned int part_id = 0;
  unsigned char parts_table[TOTAL_SERVERS];

  printf("\n## CLIENT # Remainder: %d\n", offset);

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    if(table->servers[server_id] == CONNECTION_SUCCESSFUL)
      {
        if((int) (server_id - offset) < 0)
          table->parts[server_id][TOTAL_SERVERS + server_id - offset] = MAP;

        else
          table->parts[server_id][server_id - offset] = MAP;

        if((int) (server_id - offset + 1) < 0)
          table->parts[server_id][TOTAL_SERVERS + server_id - offset + 1] = MAP;

        else if((int) (server_id - offset + 1) > (TOTAL_SERVERS - 1))
          table->parts[server_id][server_id - offset + 1 - TOTAL_SERVERS] = MAP;

        else
          table->parts[server_id][server_id - offset + 1] = MAP;
      }
  }

  printf("\n\t--------------------------------------------");
  printf("\n\t  SERVER\tP0\tP1\tP2\tP3");
  printf("\n\t--------------------------------------------");

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    printf("\n\t  DFS %d\t\t", server_id);
    for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
    {
      printf("%X\t", table->parts[server_id][part_id]);

      if(table->parts[server_id][part_id] == MAP)
        parts_table[part_id] = MAP;
    }
  }

  printf("\n\t--------------------------------------------\n");

  for(part_id = 0; part_id < TOTAL_SERVERS; part_id++)
  {
    if(parts_table[part_id] != MAP)
      return DISCARD;
  }

  return MAP;
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

void establish_connection(int* client_sock, unsigned char* table)
{
  unsigned int server_id = 0;
  unsigned char ip4[INET_ADDRSTRLEN];
  struct timeval tv;

  tv.tv_sec = 1;							//setting the pipelining timeout to 0
  tv.tv_usec = 0;

  printf("\t\t----------\t\t----------\n");
  printf("\t\t  SERVER  \t\t  STATUS  \n");
  printf("\t\t----------\t\t----------\n");

  for(server_id = 0; server_id < TOTAL_SERVERS; server_id++)
  {
    setsockopt(client_sock[server_id], SOL_SOCKET, SO_RCVTIMEO, (const unsigned char*) &tv,sizeof(struct timeval));
    //inet_ntop(AF_INET, &server_sock_addr[server_id].sin_addr.s_addr, ip4, INET_ADDRSTRLEN);
    //printf("__s_%d___p_%d___a_%s__", client_sock[server_id], ntohs(server_sock_addr[server_id].sin_port), ip4);

    if(connect(client_sock[server_id], (struct sockaddr *) &server_sock_addr[server_id], server_sock_addr_size) == 0)
    {
      table[server_id] = CONNECTION_SUCCESSFUL;
      printf("\t\t  DFS #%u   \t\t    UP  \n", server_id);
    }

    else
    {
      table[server_id] = CONNECTION_UNSUCCESSFUL;
      printf("\t\t  DFS #%u   \t\t   DOWN  \n", server_id);
      //printf("%s", strerror(errno));
    }
  }
  printf("\t\t----------\t\t----------\n");
}

unsigned char fetch_server_attr(unsigned char* user_config_filename, unsigned char* entity, server_attr_t *server)
{
  unsigned char parsed_result[16];
  unsigned char *port_match;

  bzero(parsed_result, sizeof(parsed_result));
  if(search_user_config(user_config_filename, entity, parsed_result) == USER_CONFIG_ENTITY_NOT_FOUND)
    return SERVER_ATTR_NOT_FOUND;

  if((port_match = strstr(parsed_result, ":")) == 0)
    return SERVER_ATTR_NOT_FOUND;

  port_match[0] = '\0';
  port_match = port_match + 1;

  strcpy(server->ip, parsed_result);
  strcpy(server->port, port_match);

  return SERVER_ATTR_FOUND;
}

unsigned char search_user_config(unsigned char *user_config_filename, unsigned char *entity, unsigned char *entity_match)
{
  FILE *user_config_file;

  user_config_file = fopen(user_config_filename, "r");

  if(parse_file(user_config_file, entity, entity_match) != 1)
  {
    fclose(user_config_file);
    return USER_CONFIG_ENTITY_NOT_FOUND;
  }

  fclose(user_config_file);

  return USER_CONFIG_ENTITY_FOUND;
}

unsigned char authenticator(FILE *config_file, unsigned char* username, unsigned char* password, unsigned char* user_config_filename)
{
  unsigned char read_password[32];
  FILE *user_config_file;

  if(parse_file(config_file, username, user_config_filename) == 0)
    return AUTHENTICATION_USER_NOT_FOUND;

  username[strlen(username) - 1] = '\0';

  user_config_file = fopen(user_config_filename, "r");

  if(user_config_file == NULL)
    return AUTHENTICATION_USER_CONFIG_NOT_FOUND;

  bzero(read_password, sizeof(read_password));
  if(parse_file(user_config_file, "password ", read_password) == 0)
  {
    fclose(user_config_file);
    return AUTHENTICATION_INVALID_CONFIG_FILE;
  }

  fclose(user_config_file);

  password[strlen(password) - 1] = '\0';

  if(strcmp(password, read_password) != 0)
    return AUTHENTICATION_INVALID_PASSWORD;

  return AUTHENTICATION_SUCCESSFUL;
}

void xor_crypto(unsigned char* password, unsigned char* input, size_t size)
{
  // unsigned char pwd_length = strlen(password);
	// unsigned char key = 0;
  //
  // for(int i = 0; i < pwd_length; i++)
  //   key = key + (password[i]/pwd_length);
  //
	// for(int i = 0; i < size; i++)
	// 	input[i] = input[i] ^ key;

    int keylen;
    keylen = strlen(password);

    // for(int i = 0; i < pwd_length; i++)
    //   key = key + (password[i]/pwd_length);
    //
    // //printf("\n$$ CIPHER %X $$\n", key);

    for(int i = 0; i < size; i++)
      input[i] = input[i] ^ password[i % keylen];
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
