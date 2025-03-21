//
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<pthread.h>
#include<semaphore.h>
#include<unistd.h>
#include<errno.h>

#define MAX_THREADS 8 // max simultaneous threads
#define BUFFER_SIZE 8192 //8KB
#define MAX_FILES 10

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
typedef struct
{
	int thread_id;
	long start_byte;
	long end_byte;
	FileDownload *file_info;

}DownloadTask;


void delete_chunks(FileDownload *file)
{
	char chunk_filename[512];
	for(int i=0;i<file->num_chunks;i++)
	{
		snprintf(chunk_filename,sizeof(chunk_filename),"%s/chunk_%d.tmp",file->save_path,i);

		if(unlink(chunk_filename)==0)
			printf("Deleted chunk :%s\n",chunk_filename);
		else
			perror("Error deleting chunk");
	}
}
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

//SEND HTTPS REQUEST
//kirtana

void *download_chunk(void *args)
{
	DownloadTask *task = (DownloadTask*)args;
	FileDownload *file=task->file_info;

	char buffer[BUFFER_SIZE];
	int bytes_received=0;
	long chunk_size = task->end_byte - task->start_byte +1 ;

	char filename[256];
	sprintf(filename,"%s/chunk_%d.tmp",file->save_path,task->thread_id);

	FILE *temp_file = fopen(filename,"wb");
	if(!temp_file)
	{
		perror("File open error");
		pthread_exit(NULL);
	}

	printf("Thread %d:Downloading bytes %ld to %ld for %s\n",task->thread_id,task->start_byte,task->end_byte,file->output_filename);
/*	//create a new ssl connection for each thread....

	//request chunk
	char range_header[64];
	sprintf(range_header,"Range : bytes=%ld-%ld\r\n",task->start_byte,task->end_byte);
	send_https_request(range_header);//implement ???
//modify logic
*/

	long total_received=0;
	while(total_received < chunk_size)
	{

		pthread_mutex_lock(&file->pause_lock);
		while(file->pause_flag)
		{
			pthread_cond_wait(&file->pause_cond,&file->pause_lock);
		}
		pthread_mutex_unlock(&file->pause_lock);


		bytes_received =SSL_read(ssl_socket,buffer,sizeof(buffer));
		if(bytes_received <=0) break; //connection closed

		fwrite(buffer,1,bytes_received,temp_file);

		pthread_mutex_lock(&file->progress_lock);
		file->downloaded_bytes += bytes_received;
		pthread_mutex_unlock(&file->progress_lock);

		total_received+=bytes_received;
	}

	fclose(temp_file);

	printf("Thread %d:Completed download for %s\n",task->thread_id,file->output_filename);
	free(task);
	return NULL;
}
void *progress_bar(void *args)
{
	FileDownload *file=(FileDownload*)args;

	while(file->downloaded_bytes < file->file_size)
	{
		if(!file->pause_flag)
		{
			float percent =(file->downloaded_bytes / (float)file->file_size)*100;
			printf("\rDownloading %s :[%.2f%%] %ld/%ld bytes",file->output_filename,percent,file->downloaded_bytes,file->file_size);
			fflush(stdout);
		}
		sleep(1);
	}
	printf("Download complete : %s !!\n",file->output_filename);
	return NULL;
}
void *command_listener(void *args)
{
	char command;
	FileDownload *files=(FileDownload*)args;
	int file_index;

	while(1)
	{
		printf("Enter command (P <index>, R <index>, Q <index>/ALL): ");
		scanf(" %c",&command);
		if(command =='P' || command == 'p')
		{
			scanf("%d", &file_index);
            		if (file_index >= 0)
			{
                		files[file_index].pause_flag = 1;
                		printf("File %d paused\n", file_index);
            		}
		}

		else if (command == 'R' || command == 'r')
		{
            		scanf("%d", &file_index);
            		if (file_index >= 0) {
                		pthread_mutex_lock(&files[file_index].pause_lock);
                		files[file_index].pause_flag = 0;
                		pthread_cond_broadcast(&files[file_index].pause_cond);
                		pthread_mutex_unlock(&files[file_index].pause_lock);
                		printf("File %d resumed\n", file_index);
            		}
        	}
		else if(command=='Q' || command =='q')
		{
			char target[10];
        	    	scanf("%s", target);
            		if (strcmp(target, "ALL") == 0)
			{
                		printf("Cancelling all downloads...\n");
                		for (int i = 0; i < MAX_FILES; i++) {
                    			files[i].cancel_flag = 1;
                    			delete_chunks(&files[i]);
                		}
               			 exit(0);
			}
			else
			{
                		file_index = atoi(target);
                		if (file_index >= 0) {
                    			printf("Cancelling File %d...\n", file_index);
                    			files[file_index].cancel_flag = 1;
                    			delete_chunks(&files[file_index]);
                		}
		}

	return NULL;
}
void merge_files(FileDownload *file,int num_chunks)
{
	char final_filepath[512];
	snprintf(final_filepath, sizeof(final_filepath), "%s/%s", file->save_path, file->output_filename);

	FILE *final_file=fopen(final_filepath,"wb");
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

	printf("File %s merged and chunks deleted.\n", file->output_filename);
}
void *download_file(void *args)
{
	FileDownload *file =(FileDownload*)args;
	file->num_chunks = (file->file_size + file->CHUNK_SIZE - 1) / file->CHUNK_SIZE;

	printf("Downloading file to %s/%s\n",file->save_path,file->output_filename);

	set_chunk_size(file);


	pthread_t threads[MAX_THREADS];
	pthread_t progress_thread,command_thread;

	sem_init(&file->download_sem,0,MAX_THREADS);
	pthread_mutex_init(&file->progress_lock,NULL);
	pthread_mutex_init(&file->pause_lock,NULL);
	pthread_cond_init(&file->pause_cond,NULL);

	pthread_create(&progress_thread,NULL,progress_bar,file);
	pthread_create(&command_thread,NULL,command_listener,file);


	for(int i=0;i<file->num_chunks;i++)
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
				sem_post(&file->download_sem);
			}
		}
	}
	pthread_join(progress_thread,NULL);
	pthread_join(command_thread,NULL);


	merge_files(file,file->num_chunks);
	sem_destroy(&file->download_sem);
	pthread_mutex_destroy(&file->pause_lock);
	pthread_cond_destroy(&file->pause_cond);

	return NULL;
}
int main(int argc,char *argv[])
{

	int num_files;
	printf("Enter the no.of files to download:");
	scanf("%d",&num_files);
	getchar();

 	if (num_files > MAX_FILES) {
        	printf("Error: Maximum number of downloads is %d.\n", MAX_FILES);
        	return 1;
    	}
	char input[256];


	FileDownload files[num_files];
	pthread_t file_threads[num_files];



	const char*default_dir="/home/user/Downloads";
	printf("Default download directory is \"%s\".Press enter to accept or type a different directory:",default_dir);
	fgets(input,sizeof(input),stdin);

	input[strcspn(input,"\n")]='\0';

	const char *download_dir=(strlen(input)>0)?input : default_dir;

	if(ensure_directory(download_dir)!=0)
	{
		fprintf(stderr,"Error:Unable to create or access the download  directory..");
		exit(EXIT_FAILURE);
	}

	for(int i=0;i<num_files;i++)
	{
		strcpy(files[i].save_path,download_dir);
		files[i].downloaded_bytes=0;
		files[i].pause_flag=0;
	}

	for(int i=0;i<num_files;i++)
	{
		printf("\nEnter URL for file %d",i+1);
		fgets(files[i].url,sizeof(files[i].url),stdin);

		files[i].url[strcspn(files[i].url,"\n")]='\0';

		printf("Enter output filename for file %d:",i+1);
		fgets(files[i].output_filename,sizeof(files[i].output_filename),stdin);
		files[i].output_filename[strcspn(files[i].output_filename,"\n")]='\0';

		files[i].file_size=get_filesize();

		pthread_create(&file_threads[i],NULL,download_file,&files[i]);

		pthread_mutex_init(&files[i].pause_lock, NULL);
        	pthread_cond_init(&files[i].pause_cond, NULL);
	}
	for(int i=0;i<num_files;i++)
		pthread_join(file_threads[i]);

	return 0;

}
