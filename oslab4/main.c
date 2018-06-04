#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <fcntl.h>


#define MAXSLEEP 128

#define MAX_FILENAME_SIZE 500
#define MAX_FILE_SIZE 50000

#define QLEN 10

#ifndef  HOST_NAME_MAX
#define HOST_NAME_MAX 256
#endif

struct FileDescriptor{
    unsigned len;
    char filename[MAX_FILENAME_SIZE];
    char data[];
};

struct CtlMsg{
    unsigned len;
    int accept;
};



int connect_retry(int domain,int type,int protocol,const struct sockaddr *addr,socklen_t alen){// A extension of connect() with socket creating and error handler.
    int fd;
    unsigned numsec;
    for (numsec=1;numsec<=MAXSLEEP;numsec<<=1){//exponential backoff.
        if((fd=socket(domain,type,protocol))<0)
            return(-1);//Fatal error: Unable to create a socket.
        if (connect(fd,addr,alen)==0)
            return (fd);//Success, return fd.
        close(fd);//Always close socket if failed to keep it transplantable.

        if (numsec<=MAXSLEEP/2)
            printf("Failed to connect to server. retry after %d second(s)\n",numsec);
            sleep(numsec);//Sleep to prevent network congestion.
    }
    return(-1);// timeout
}

int initserver(int type,const struct sockaddr *addr,socklen_t alen,int qlen){//Create a socket, bind it to the addr, and start listening.
    int fd;
    int err=0;

    if((fd=socket(addr->sa_family,type,0))<0)
        return(-1);//Fatal error: Unable to create a socket.
    if(bind(fd,addr,alen)<0)
        goto errout;//Fatal error: Unable to bind socket with specified address.

    if (type==SOCK_STREAM||type==SOCK_SEQPACKET)
        if(listen(fd,qlen)<0)
            goto errout;//Fatal error: Unable to start listen;

    return (fd);



    errout:
        err=errno;
        close(fd);
        errno=err;
        return(-1);
}

void serve(int sockfd){
    int clfd;

    for(;;){
        if((clfd=accept(sockfd,NULL,NULL))<0){// Accept new connection from client.
            printf("Accept error %d, quit.\n",errno);
            exit(1);
        }else{
            printf("Connection accepted.\n");

            struct CtlMsg* msgp=malloc(sizeof(struct CtlMsg));
            ssize_t rec_len;
            if ((rec_len=recv(clfd,msgp,sizeof(struct CtlMsg),0))<=0)// Receive control message.
                {printf("Receive control message failed. Last error:%d,\n\n",errno);exit(1);}

            size_t file_size=msgp->len;// Get size from control message.
            msgp->accept=1;

            printf("Server received control message of size %d. %d bytes required.\n",(int)rec_len,(int)file_size);

            struct FileDescriptor *file=malloc(file_size);// alloc space for receiving file.

            if(send(clfd,msgp, sizeof(struct CtlMsg),0)!= sizeof(struct CtlMsg)){
                printf("Response to client failed. Abort connection. Message: %s\n\n",strerror(errno));
                free(file);
                free(msgp);
                close(clfd);
                continue;
            }

            ssize_t recv_size;
            if((recv_size=recv(clfd,file,file_size,MSG_WAITALL)<0)){ // Receive data.
                printf("Receive less or more data: %d than expected: %d. Abort.\n\n",(int)file_size,(int)recv_size);
                free(file);
                free(msgp);
                close(clfd);
                continue;
            }

            printf("Receive success. File name：%s\n",file->filename);

            size_t real_file_size=(file->len)-sizeof(long)-sizeof(char)*MAX_FILENAME_SIZE;

            FILE* rfile=fopen(file->filename,"wb");

            size_t written_size;
            if((written_size=fwrite(file->data,1,real_file_size,rfile))<0)// Write data to file.
                {printf("File write error.Last error: %s",strerror(errno));exit(1);}

            printf("Receive %d bytes success. Write %d bytes to file",(int)file_size,(int)written_size);

            fclose(rfile);

            free(file);

            free(msgp);

            printf("\nReceive complete. Last error:%s\n\n",strerror(errno));

            close(clfd);

        }
    }


}



int main(int argc,char* argv[]) {

    int sockfd, err,n;

    char abuf[INET_ADDRSTRLEN];

    if (argc<2) {printf("arg wrong. see -h.\n");exit(1);}
    if (strcmp(argv[1],"-c")==0){ // Client Mode.
        if (argc!=5){printf("Missing of additional arg for client.see -h.\n");exit(1);}

        int slash_index=-1;
        int i=0;
        for (i=0;argv[4][i]!='\0';i++)
            if (argv[4][i]=='/')slash_index=i;
        char* relative_name=(slash_index==-1)?argv[4]:argv[4]+slash_index+1;

        printf("Relative name: %s\n",relative_name);

        FILE * pFile;
        long lSize; // File Size.
        char * fbuffer; // File buffer.
        size_t result;


        if ((pFile = fopen (argv[4], "rb" ))==NULL) //Open read-only binary file.
            {printf("Failed to open file. Error message: %s\n\n",strerror(errno));exit (1); }

        fseek (pFile , 0 , SEEK_END);
        lSize = ftell (pFile);//Get the size of file.
        rewind (pFile);

        if ((fbuffer = (char*) malloc (sizeof(char)*lSize)) == NULL)//File buffer
            {printf("Memory alloc error. Message: %s\n\n",strerror(errno));fclose(pFile);exit (1);}

        if ((result = fread (fbuffer,1,lSize,pFile)) != lSize)//Copy file to the buff.
            {printf("Reading error. Message: %s\n\n",strerror(errno));fclose(pFile);exit (1);}

        fclose(pFile);

        struct sockaddr_in sin;// Structure to identify the socket address.

        sin.sin_family=AF_INET;// IPv4 Address Family.

        if(inet_pton(AF_INET,argv[2],&(sin.sin_addr))!=1){printf("Invalid IP address: %s\n\n",argv[2]);exit(1);}// Set IP

        sin.sin_port=htons((uint16_t)atoi(argv[3])); // Set Port

        printf("Preparing Connection to: %s, port: %d\n",inet_ntoa(sin.sin_addr),ntohs(sin.sin_port));


        if((sockfd=connect_retry(AF_INET,SOCK_STREAM,0,(struct sockaddr*)&sin, sizeof(struct sockaddr)))<0){// Try to connect to server.
            err=errno;
            printf("Error occured when connecting to server: %s\n\n",strerror(err));
            exit(1);}
        else{ // Connection Success.
            printf("Connection to server success.\n");

            struct FileDescriptor* file=malloc(sizeof(long)+ sizeof(char)*MAX_FILENAME_SIZE+sizeof(char)*lSize);
            sprintf(file->filename,relative_name);
            memcpy(file->data,fbuffer,sizeof(char)*lSize);
            file->len=sizeof(long)+ sizeof(char)*MAX_FILENAME_SIZE+sizeof(char)*lSize;// Initialize the structure FileDescriptor.

            struct CtlMsg* msgp=malloc(sizeof(struct CtlMsg));
            msgp->len=file->len;  // Prepare control message.

            if (send(sockfd,msgp,sizeof(struct CtlMsg),0)!= sizeof(struct CtlMsg)) // Send control message to server.
                {printf("Sending control message failed. Last error:%s\n\n"),strerror(errno);close(sockfd);exit(1);}

            if (recv(sockfd,msgp,sizeof(struct CtlMsg),0)!= sizeof(struct CtlMsg)) // Receive control message from server.
                {printf("Receving server message failed. Last error:%s\n\n"),strerror(errno);close(sockfd);exit(1);}

            if (msgp->accept==1){// If the server accept the transfer.
                printf("Transfer permission get.\n");
                ssize_t send_size;
                if((send_size=send(sockfd,file,file->len,MSG_MORE))!=file->len) // Send file.
                {printf("Failed when transferring file. File Len: %d Send size: %d. Last error:%s\n\n",(int)file->len,(int)send_size,strerror(errno));close(sockfd);exit(1);};}
            else{
                printf("Receive Permission failed. Please contact the administrator of server.\n\n");
                close(sockfd);
                exit(1);
            }
            close(sockfd);


        }
        printf("Send complete.Quit.\n\n");
        return 0;
    }else if (strcmp(argv[1],"-s")==0){ // Server Mode
        if (argc!=4) {printf("Misssing or more args. see -h.\n ");exit(1);}

        struct sockaddr_in sin; // Socket Address Descriptor.

        sin.sin_family=AF_INET; // Using IPv4 Address Family.

        if(inet_pton(AF_INET,argv[2],&(sin.sin_addr))!=1){printf("Invalid IP address: %s\n",argv[2]);exit(1);}

        sin.sin_port=htons((uint16_t)atoi(argv[3]));

        printf("Receive: %s, port: %d\n",inet_ntoa(sin.sin_addr),ntohs(sin.sin_port));

        if((sockfd=initserver(SOCK_STREAM,(struct sockaddr*)&sin,sizeof(struct sockaddr_in),QLEN))>=0){ // Bind socket with addr
            printf("Start serving\n");
            serve(sockfd);
            exit(0);
        }else{
            printf("Failed to bind socket. Error message: %s",strerror(errno));
            exit(1);
        }

    }else if(strcmp(argv[1],"-h")==0){ // Help Mode.
        printf("Usage:\n");
        printf("  Server: -s [Our IP Address] [Our Port]\n");
        printf("  Client: -c [Server IP Address] [Server Port] [Full File Name]\n");
    }
    else{
        printf("Unknown option:");
        printf(argv[1]);
        printf(" See -h\n");
    return 0;}
}