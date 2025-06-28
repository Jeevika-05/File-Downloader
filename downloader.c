#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define MAX_THREADS 8 // max simultaneous threads
#define BUFFER_SIZE 8192 //8KB
#define MAX_FILES 10
int num_files;
// Structure for file download info
typedef struct
{
        char url[512];
        char save_path[256];
        char output_filename[256];
        long file_size;
        int CHUNK_SIZE;
        long downloaded_bytes;
        int pause_flag;
        int cancel_flag;
        int num_chunks;
        pthread_mutex_t progress_lock;
        sem_t download_sem;
        pthread_mutex_t  pause_lock;
        pthread_cond_t pause_cond;

}FileDownload;

// Structure for each chunk download task
typedef struct {
    int thread_id;
    long start_byte;
    long end_byte;
    FileDownload *file_info;
} DownloadTask;

typedef struct
{
    FileDownload *files;
    int exit_flag;
    int num_files;
    pthread_mutex_t exit_lock;
} CommandListenerArgs;

int ensure_directory(const char*dir)
{
        struct stat st={0};
        if(stat(dir,&st)==-1)
        {
                if(mkdir(dir,0755)!=0)
                {
                        perror("Failed to create directory");
                        return -1;
                }
        }
        return 0;
}



void delete_chunks(FileDownload *file)
{
    char chunk_filename[512];
    for (int i = 0; i < file->num_chunks; i++)
    {
        snprintf(chunk_filename, sizeof(chunk_filename), "%s/chunk_%d.tmp", file->save_path, i);

        if (access(chunk_filename, F_OK) == 0) // Check if file exists before deleting
        {
            if (unlink(chunk_filename) != 0){
                perror("Error deleting chunk");}
//                printf("Deleted chunk: %s\n", chunk_filename);
        }
    }
}

//------------------------------
// Helper: Initialize SSL context
SSL_CTX *init_ssl_context() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        fprintf(stderr, "SSL_CTX_new() failed.\n");
        exit(1);
    }
    return ctx;
}

//------------------------------
// Helper: Create an SSL socket connection to a given host/port
SSL *create_ssl_socket(const char *hostname, const char *port, int *sockfd, SSL_CTX *ctx)
{
    struct addrinfo hints, *res, *p;
    int rv;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if ((rv = getaddrinfo(hostname, port, &hints, &res)) != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return NULL;
    }

    for (p = res; p != NULL; p = p->ai_next)
    {
        *sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (*sockfd == -1)
            continue;
        if (connect(*sockfd, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(*sockfd);
    }
    if (p == NULL)
    {
        fprintf(stderr, "failed to connect to %s:%s\n", hostname, port);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    SSL *ssl = SSL_new(ctx);
    SSL_set_tlsext_host_name(ssl, hostname);
    SSL_set_fd(ssl, *sockfd);
    if (SSL_connect(ssl) <= 0)
    {
        fprintf(stderr, "SSL_connect() failed.\n");
        ERR_print_errors_fp(stderr);
        close(*sockfd);
        return NULL;
    }
    return ssl;
}

long get_file_size(const char *hostname, const char *path)
{
    long fsize = -1;
    int sockfd;
    SSL_CTX *ctx;
    SSL *ssl;
    char request[512], response[BUFFER_SIZE];
    int bytes_read;
    int max_redirects = 5; // Prevent infinite redirect loops

    // Buffers to hold the modified hostname and path during redirects
    char current_host[256], current_path[256];
    strncpy(current_host, hostname, sizeof(current_host));
    strncpy(current_path, path, sizeof(current_path));

    while (max_redirects--)
    {
        // Initialize SSL context
        ctx = init_ssl_context();

        // Create SSL socket and establish connection
        ssl = create_ssl_socket(current_host, "443", &sockfd, ctx);
        if (!ssl)
        {
            SSL_CTX_free(ctx);
            return -1;
        }

        // Prepare and send HEAD request
        snprintf(request, sizeof(request),
                 "HEAD %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                 "(KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36\r\n"
                 "Range: bytes=0-\r\n"
                 "Connection: close\r\n\r\n",
                 current_path, current_host);

        //printf("sending request:\n%s\n",request);
        SSL_write(ssl, request, strlen(request));

        // Read server response
        bytes_read = SSL_read(ssl, response, sizeof(response) - 1);
        if (bytes_read > 0)
        {
            response[bytes_read] = '\0';
           // printf("Server response:\n%s\n", response);

            // Check for redirects (301, 302, 307, 308)
            if (strstr(response, "HTTP/1.1 301") || strstr(response, "HTTP/1.1 302") ||
                strstr(response, "HTTP/1.1 307") || strstr(response, "HTTP/1.1 308"))
            {
                char *location = strstr(response, "Location:");
                if (location)
                {
                    char new_url[256], new_host[128], new_path[128];
                    sscanf(location, "Location: %255s", new_url);

//                    printf("Redirecting to: %s\n", new_url);

                    // Parse new URL (basic parsing, assumes HTTPS)
                    sscanf(new_url, "https://%127[^/]/%127[^\n]", new_host, new_path);

                    // Cleanup current SSL connection
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    close(sockfd);
                    SSL_CTX_free(ctx);

                    // Update hostname and path buffers
                    strncpy(current_host, new_host, sizeof(current_host));
                    strncpy(current_path, new_path, sizeof(current_path));

                    continue; // Retry with new URL
                }
            }

            // Extract Content-Length
            char *cl = strstr(response, "Content-Length:");
            if (cl)
            {
                sscanf(cl, "Content-Length: %ld", &fsize);
            }
        }
        else
        {
            fprintf(stderr, "Failed to obtain file size from server.\n");
        }

        // Cleanup
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sockfd);
        SSL_CTX_free(ctx);

//        printf("File size: %ld\n", fsize);
        return fsize;
    }

    fprintf(stderr, "Max redirects reached. Aborting.\n");
    return -1;
}


void parse_url(const char *url, char *hostname, char *path)
{
    if (strncmp(url, "https://", 8) == 0)
        url += 8; // Skip "https://"

    char *slash = strchr(url, '/');
    if (slash)
    {
        strncpy(hostname, url, slash - url);
        hostname[slash - url] = '\0'; // Null terminate
        strcpy(path, slash);          // Copy path
    }
    else
    {
        strcpy(hostname, url);
        strcpy(path, "/"); // Default path
    }
}

//------------------------------
// Helper: Send HTTP GET request for a specific chunk and return an SSL pointer
SSL *send_chunk_request(FileDownload *file, long start_byte, long end_byte) {
    int sockfd;
    SSL_CTX *ctx = init_ssl_context();

    // Parse the URL stored in file->url to extract hostname and path.
    // For simplicity, we assume the URL starts with "https://"
    char host[256], path[256];
    const char *p = file->url;
    if (strncmp(p, "https://", 8) == 0)
        p += 8; // skip "https://"
    char *slash = strchr(p, '/');
    if (slash) {
        size_t host_len = slash - p;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
        strcpy(path, slash);
    } else {
        strcpy(host, p);
        strcpy(path, "/");
    }

    SSL *ssl = create_ssl_socket(host, "443", &sockfd, ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    // Build the GET request with the Range header for this chunk
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: CustomDownloader/1.0\r\n"
             "Range: bytes=%ld-%ld\r\n"
             "Connection: close\r\n\r\n",
             path, host, start_byte, end_byte);

    SSL_write(ssl, request, strlen(request));

    // Note: We do not shutdown or free SSL here since we need the connection open
    // The calling function will use the returned SSL pointer to read the response.
    return ssl;
}

//------------------------------
// Function: download_chunk
// Downloads a specific chunk using its own SSL connection.
void *download_chunk(void *args) {
    DownloadTask *task = (DownloadTask*)args;
    FileDownload *file = task->file_info;
    char buffer[BUFFER_SIZE];
    int bytes_received = 0;
    long chunk_size = task->end_byte - task->start_byte + 1;

    char filename[256];
    sprintf(filename, "%s/chunk_%d.tmp", file->save_path, task->thread_id);

    FILE *temp_file = fopen(filename, "wb");
    if (!temp_file) {
        perror("File open error");
        pthread_exit(NULL);
    }

//    printf("Thread %d: Downloading bytes %ld to %ld for %s\n",
//           task->thread_id, task->start_byte, task->end_byte, file->output_filename);

    // Create new SSL connection for this chunk and send HTTP GET with Range header
    SSL *ssl = send_chunk_request(file, task->start_byte, task->end_byte);
    if (!ssl) {
        fprintf(stderr, "Failed to send chunk request.\n");
        fclose(temp_file);
        free(task);
        pthread_exit(NULL);
    }

    // Read from the SSL connection.
    // Skip HTTP headers: Find the end of headers (\r\n\r\n)
    int header_parsed = 0;
    int header_offset = 0;
    while (1) {
        bytes_received = SSL_read(ssl, buffer, sizeof(buffer));
        if (bytes_received <= 0)
            break;
        if (!header_parsed) {
            // Try to locate header termination
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) {
                header_offset = body - buffer + 4;
                header_parsed = 1;
                fwrite(buffer + header_offset, 1, bytes_received - header_offset, temp_file);
            }
        } else {
            fwrite(buffer, 1, bytes_received, temp_file);
        }
    }

    fclose(temp_file);
    // Cleanup SSL connection
    SSL_shutdown(ssl);
    SSL_free(ssl);
    // Note: The underlying socket is closed when SSL is freed if using blocking mode.
    // We should free the SSL_CTX if it's not needed anymore, but in our send_chunk_request,
    // we created a new ctx and never freed it. For a production code, you'd want to manage ctx lifetimes properly.
    // For now, we leave it as is for simplicity.

    pthread_mutex_lock(&file->progress_lock);
    file->downloaded_bytes += chunk_size;  // Ideally, use actual bytes read.
    pthread_mutex_unlock(&file->progress_lock);

//    printf("Thread %d: Completed download for %s\n", task->thread_id, filename);
    free(task);
    return NULL;
}

//ADAPTIVE CHUNK SIZING
void set_chunk_size(FileDownload *file)
{
        if(file->file_size < 10*1024*1024 ) //<10MB
                file->CHUNK_SIZE=256*1024; //256KB

        else if(file->file_size < 100 * 1024 * 1024) //10MB-100MB
                file->CHUNK_SIZE=512*1024; //512KB
        else
                file->CHUNK_SIZE=1024*1024; //1MB
}

void *progress_bar(void *args)
{
        FileDownload *file=(FileDownload*)args;
        char time_str[64];

        while(file->downloaded_bytes < file->file_size && !file->cancel_flag)
        {
                if(!file->pause_flag)
                {
                        time_t now=time(NULL);
                        struct tm *t =localtime(&now);
                        strftime(time_str,sizeof(time_str),"%Y-%m-%d %H:%M:%S",t);

                        float percent =(file->downloaded_bytes / (float)file->file_size)*100;
                        printf("\r[%s] Downloading %s : [%.2f%%] %ld/%ld bytes  ",time_str, file->output_filename, percent,file->downloaded_bytes, file->file_size);
                        fflush(stdout);
                }
                sleep(1);
        }
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);

        printf("\r[%s] Download complete: %s !!                  \n", time_str, file->output_filename);

        return NULL;
}
void *command_listener(void *args)
{
    CommandListenerArgs *cla = (CommandListenerArgs*)args;
    FileDownload *files = cla->files;
    char input_line[256];
    char command;
    int file_index;

    while (1)
    {
        // Check for exit flag first.
        pthread_mutex_lock(&cla->exit_lock);
        if(cla->exit_flag)
        {
            pthread_mutex_unlock(&cla->exit_lock);
            printf("Command listener exiting (select timeout).\n");
            break;
        }
        pthread_mutex_unlock(&cla->exit_lock);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;    // 1 second timeout
        tv.tv_usec = 0;

        int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1)
        {
            perror("select()");
            continue;
        }
        else if (retval == 0)
        {
            // Timeout occurred, no data, loop back and check exit_flag.
            continue;
        }
        else if (FD_ISSET(STDIN_FILENO, &readfds))
        {
            if(fgets(input_line, sizeof(input_line), stdin) != NULL)
            {
                input_line[strcspn(input_line, "\n")] = '\0';
                if (sscanf(input_line, " %c", &command) == 1)
                {
                    if (command == 'P' || command == 'p')
                    {
                        if(sscanf(input_line + 1, "%d", &file_index) == 1 &&
                           file_index >= 0 && file_index < cla->num_files)
                        {
                            pthread_mutex_lock(&files[file_index].pause_lock);
                            files[file_index].pause_flag = 1;
                            pthread_mutex_unlock(&files[file_index].pause_lock);
                            printf("File %d paused\n", file_index);
                        }
                    }
                    else if (command == 'R' || command == 'r')
                    {
                        if(sscanf(input_line + 1, "%d", &file_index) == 1 &&
                           file_index >= 0 && file_index < cla->num_files)
                        {
                            pthread_mutex_lock(&files[file_index].pause_lock);
                            files[file_index].pause_flag = 0;
                            pthread_cond_signal(&files[file_index].pause_cond);
                            pthread_mutex_unlock(&files[file_index].pause_lock);
                            printf("File %d resumed\n", file_index);
                        }
                    }
                    else if (command == 'Q' || command == 'q')
                    {
                        char target[10];
                        if(sscanf(input_line + 1, " %9s", target) == 1)
                        {
                            if (strcmp(target, "ALL") == 0)
                            {
                                printf("Cancelling all downloads...\n");
                                for (int i = 0; i < cla->num_files; i++) {
                                    pthread_mutex_lock(&files[i].pause_lock);
                                    files[i].cancel_flag = 1;
                                    pthread_cond_signal(&files[i].pause_cond);
                                    pthread_mutex_unlock(&files[i].pause_lock);
                                    delete_chunks(&files[i]);
                                }
                                break;
                            }
                            else
                            {
                                file_index = atoi(target);
                                if (file_index >= 0 && file_index < cla->num_files)
                                {
                                    pthread_mutex_lock(&files[file_index].pause_lock);
                                    files[file_index].cancel_flag = 1;
                                    pthread_cond_signal(&files[file_index].pause_cond);
                                    pthread_mutex_unlock(&files[file_index].pause_lock);
                                    delete_chunks(&files[file_index]);
                                    printf("Cancelling File %d...\n", file_index);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

void merge_files(FileDownload *file,int num_chunks)
{
        char final_filepath[512];
        snprintf(final_filepath, sizeof(final_filepath), "%s/%s", file->save_path, file->output_filename);

        FILE *final_file=fopen(final_filepath,"wb");
        printf("final file path is %s\n",final_filepath);

        if(!final_file)
        {
                perror("Failed to open final file");
                return;
        }

        char buffer[BUFFER_SIZE];
        for(int i=0;i<file->num_chunks;i++)
        {
                char chunk_filename[256];
                snprintf(chunk_filename, sizeof(chunk_filename), "%s/chunk_%d.tmp", file->save_path, i);

                FILE *chunk_file=fopen(chunk_filename,"rb");

                if(!chunk_file)
                {
                        perror("Chunk file open error");
                        continue;
                }

                size_t bytes;
                while((bytes = fread(buffer,1,sizeof(buffer),chunk_file))>0)
                {
                        fwrite(buffer,1,bytes,final_file);
                }

                fclose(chunk_file);

        }

        fclose(final_file);
        delete_chunks(file);

//      printf("File %s merged and chunks deleted.\n", file->output_filename);
}

void *download_file(void *args)
{
        FileDownload *file =(FileDownload*)args;
        printf("Downloading file to %s/%s\n",file->save_path,file->output_filename);

        set_chunk_size(file);

        int num_chunks=(file->file_size+file->CHUNK_SIZE -1)/file->CHUNK_SIZE;

        pthread_t threads[MAX_THREADS];
        pthread_t progress_thread,command_thread;

        sem_init(&file->download_sem,0,MAX_THREADS);
        pthread_mutex_init(&file->progress_lock,NULL);
        pthread_mutex_init(&file->pause_lock,NULL);
        pthread_cond_init(&file->pause_cond,NULL);

        pthread_create(&progress_thread,NULL,progress_bar,file);


        for(int i=0;i<num_chunks;i++)
        {
                sem_wait(&file->download_sem);

                DownloadTask *task=malloc(sizeof(DownloadTask));
                task->thread_id=i;
                task->start_byte=i*file->CHUNK_SIZE;
                task->end_byte=(i+1)*file->CHUNK_SIZE -1;

                task->file_info=file;


                if(task->end_byte >= file->file_size )
                        task->end_byte=file->file_size-1;

                pthread_create(&threads[i%MAX_THREADS] , NULL,download_chunk ,task);

                if(i%MAX_THREADS==MAX_THREADS-1|| i==num_chunks-1)
                {
                        int batch_size=(i%MAX_THREADS==MAX_THREADS -1)?MAX_THREADS:(i%MAX_THREADS)+1;

                        for(int j=0;j<batch_size;j++)
                        {
                                pthread_join(threads[j],NULL);
                        }
                        for (int j = 0; j < batch_size; j++) {
                                sem_post(&file->download_sem);
                        }
                }
        }


        pthread_join(progress_thread,NULL);

        file->num_chunks=num_chunks;
        merge_files(file,num_chunks);
        sem_destroy(&file->download_sem);
        pthread_mutex_destroy(&file->pause_lock);
        pthread_cond_destroy(&file->pause_cond);

        return NULL;
}


int main(int argc, char *argv[]) {
    printf("Enter the number of files to download: ");
    scanf("%d", &num_files);
    getchar();  // Consume newline left by scanf

    if (num_files > MAX_FILES) {
        printf("Error: Maximum number of downloads is %d.\n", MAX_FILES);
        return 1;
    }

    char input[256];
    FileDownload files[num_files];
    pthread_t file_threads[num_files];

    const char *default_dir = "/home/kirtana/OS/J_File-Downloader";
    printf("Default download directory is \"%s\". Press Enter to accept or type a different directory: ", default_dir);
    fgets(input, sizeof(input), stdin);
    input[strcspn(input, "\n")] = '\0';  // Remove newline character

    const char *download_dir = (strlen(input) > 0) ? input : default_dir;

    // Ensure the download directory exists
    if (ensure_directory(download_dir) != 0) {
        fprintf(stderr, "Error: Unable to create or access the download directory.\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < num_files; i++) {
        strcpy(files[i].save_path, download_dir);
        files[i].downloaded_bytes = 0;
        files[i].pause_flag = 0;
    }

    for (int i = 0; i < num_files; i++) {
        printf("\nEnter URL for file %d: ", i + 1);
        fgets(files[i].url, sizeof(files[i].url), stdin);
        files[i].url[strcspn(files[i].url, "\n")] = '\0';  // Remove newline character

        printf("Enter output filename for file %d: ", i + 1);
        fgets(files[i].output_filename, sizeof(files[i].output_filename), stdin);
        files[i].output_filename[strcspn(files[i].output_filename, "\n")] = '\0';

        // Extract hostname and path
        char hostname[256], path[1024];
        /*if (*/parse_url(files[i].url, hostname, path); /*!= 0) {
            fprintf(stderr, "Error: Invalid URL format.\n");
            return EXIT_FAILURE;
        }*/


        // Get file size from server
        files[i].file_size = get_file_size(hostname, path);
        if (files[i].file_size < 0) {
            fprintf(stderr, "Error: Unable to determine file size for %s.\n", files[i].url);
            return EXIT_FAILURE;
        }

        // Initialize mutex and condition variable
        pthread_mutex_init(&files[i].pause_lock, NULL);
        pthread_cond_init(&files[i].pause_cond, NULL);

        // Create a thread for downloading
        pthread_create(&file_threads[i], NULL, download_file, &files[i]);
    }
        CommandListenerArgs cla;
        cla.files = files;
        cla.num_files = num_files;
        cla.exit_flag=0;
        pthread_mutex_init(&cla.exit_lock,NULL);
        pthread_t command_thread;
        pthread_create(&command_thread, NULL, command_listener, &cla);

    // Wait for all download threads to complete
    for (int i = 0; i < num_files; i++) {
        pthread_join(file_threads[i], NULL);
    }
        pthread_mutex_lock(&cla.exit_lock);
        cla.exit_flag=1;
        pthread_mutex_unlock(&cla.exit_lock);

        pthread_join(command_thread,NULL);

        pthread_mutex_destroy(&cla.exit_lock);
    return 0;
}
