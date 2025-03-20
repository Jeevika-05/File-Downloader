#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<pthread.h>
#include<semaphore.h>
#include<unistd.h>
#include<fcntl.h>

#define MAX_THREADS 8 // max simultaneous threads
#define BUFFER_SIZE 8192 //8KB

SSL *ssl_socket;


typedef struct
{
	char url[512];
	char save_path[256];
	char output_filename[256];
	long file_size;
	int CHUNK_SIZE;
	long downloaded_bytes=0;
	int pause_flag;
	pthread_mutex_t progress_lock;
	sem_t download_sem;
}FileDownload;

typedef struct
{
	int thread_id;
	long start_byte;
	long end_byte;
	FileDownload *file_info;

}DownloadTask;


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


	//request chunk
	char range_header[64];
	sprintf(range_header,"Range : bytes=%ld-%ld\r\n",task->start_byte,task->end_byte);
	send_https_request(range_header);//implement ???

	long total_received=0;
	while(total_received < chunk_size)
	{

		if(file->pause_flag)
		{
			sleep(1);
			continue;
		}


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
	while(1)
	{
		scanf(" %c",&command);
		if(command =='P' || command == 'p')
		{
			file->pause_flag=1;
			printf("Download paused\n");
		}
		else if(command =='R' || command == 'r')
		{
			file->pause_flag=0;
			printf("Download resumed\n");
		}
		else if(command=='Q' || command =='q')
		{
			printf("Download cancelled\n");
			exit(0);
		}
	}
	return NULL;
}
void merge_files(FileDownload *file,int num_chunks)
{
	char final_filepath[512];
	sprintf(final_filepath,"%s/%s",file->save_path,file->output_filename);
	FILE *final_file=fopen(final_filepath,"wb");
	if(!final_file)
	{
		perror("Failed to open final file");
		return;
	}

	char buffer[BUFFER_SIZE];
	for(int i=0;i<num_chunks;i++)
	{
		char filename[256];
		sprintf(filename,"%s/chunk_%d.tmp",file->save_path,i);
		FILE *chunk_file=fopen(filename,"rb");

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
		remove(filename);
	}

	fclose(final_file);
	printf("All chunks merged successfully into %s\n",file->output_filename);
}
void *download_file(void *args)
{
	FileDownload *file =(FileDownload*)args;
	set_chunk_size(file);

	int num_chunks=(file->file_size+file->CHUNK_SIZE -1)/file->CHUNK_SIZE;

	pthread_t threads[MAX_THREADS];
	pthread_t progress_thread,command_thread;

	sem_init(&file->download_sem,0,MAX_THREADS);
	pthread_mutex_init(&file->progress_lock,NULL);

	pthread_create(&progress_thread,NULL,progress_bar,file);
	pthread_create(&command_thread,NULL,command_listener,file);


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
			for(int j=0;j<MAX_THREADS && j<=i;j++)
			{
				pthread_join(threads[j],NULL);
				sem_post(&file->download_sem);
			}
		}
	}
	pthread_join(progress_thread,NULL);
	pthread_join(command_thread,NULL);


	merge_files(file,num_chunks);
	sem_destroy(&file->download_sem);
	return NULL;
}
int main(int argc,char *argv[])
{

	int num_files;
	printf("Enter the no.of files to download:");
	scanf("%d",&num_files);
	getchar();

	FileDownload files[num_files];
	pthread_t file_threads[num_files];


	for(int i=0;i<num_files;i++)
	{
		printf("\nEnter URL for file %d",i+1);
		fgets(files[i].url,sizeof(files[i].url),stdin);

		files[i].url[strcspn(files[i].url,"\n")]=0;

		files[i].file_size=get_filesize();

		pthread_create(&file_threads[i],NULL,download_file,&files[i]);
	}
	for(int i=0;i<num_files;i++)
		pthread_join(file_threads[i]);

	return 0;

}
