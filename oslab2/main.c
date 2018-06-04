#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/errno.h>
#include <setjmp.h>

#define BUF_SIZE 1000
#define MTEXT_LEN 1000
#define USER_WR 0600 // 0200+0400, the user can either read or write to the IPC.

struct mymsg{//A standard message structure. Normally, type should be positive.
    long type;
    char mtext[MTEXT_LEN];
};

union semun{//A union used to set value in semctl();
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

static sigjmp_buf jmpbuf;//Jump buf used to store the context at the destination of siglongjmp().
static volatile  sig_atomic_t canjump=0;//This variable  prevent the signal handler jump before the jump buf is set, which can make a segmentation fault.



void start_sig_hdl(int sig){//Signal handler for child. If child received SIGUSR1, jump out of endless loop.
    if (sig==SIGUSR1){
            if (canjump==1)
                siglongjmp(jmpbuf,1);
            else return;
    }
    else return;
}

void child_ready_hdl(int sig){//Signal handler 2 for parent. If parent received SIGUSR2, set its canjump to show that child is ready to jump.
    if (sig==SIGUSR2)
        canjump=1;
    else return;
}


int main() {

    char buff[BUF_SIZE];
    int len=0;
    printf("Input a string:>");
    scanf("%s",buff);
    for (int i=0;buff[i]!='\0';i++) len++;//Get the length of input.

    int mailbox=msgget(IPC_PRIVATE,USER_WR|IPC_CREAT);//Create a mailbox.
    if (mailbox<0){printf("Creating mailbox failed. Exit.");exit(1);}
    else printf("Mailbox id: %d\n",mailbox);

    int sharemem=shmget(IPC_PRIVATE,BUF_SIZE,USER_WR|IPC_CREAT);//Create a shared memory space with size 1000.
    if (sharemem<0){printf("Creating shared memory failed. Exit.");exit(1);}
    else printf("Shared memory id: %d\n",sharemem);

    int sem=semget(IPC_PRIVATE,1,USER_WR|IPC_CREAT);//Create a semaphore to protect the shared memory.
    if (sem<0){printf("Creating sem failed. Exit.");exit(1);}
    else printf("Sem id: %d\n",sem);

    union semun arg;//arg used in semctl.
    arg.val=1;
    semctl(sem,0,SETVAL,arg);//Initialize the semaphore to 1.

    pid_t ppid=0;

    int childQuitStatus=0;//Integer to store child exit status.

    void* none=0;// A void pointer to receive ptr that are not used.


    if (signal(SIGUSR1,start_sig_hdl) ==SIG_ERR){printf("Set signal handler failed. Exit.\n");exit(1);}//Set signal handler.

    if (signal(SIGUSR2,child_ready_hdl)==SIG_ERR){printf("Set parent signal handler failed. Exit.\n");exit(1);}//Set signal handler.

    pid_t child_pid=fork();//Fork here.

    if (child_pid<0) {printf("Fork error. Exit.\n");exit(1);}//This should never happen.

    else if (child_pid==0){//Child.
        pid_t parent=getppid();

        if (sigsetjmp(jmpbuf,1)==0){//Set jump destination.
            canjump=1;kill(parent,SIGUSR2);//Tell parent that child is ready to jump.
            for( ; ; ) pause();//Waiting for parent.
        }else{

            struct mymsg* msgptr=calloc(sizeof(long)+MTEXT_LEN,sizeof(char));//Malloc message buf.
            ssize_t msglength=-1;
            for (; ;){
                msglength=msgrcv(mailbox,msgptr,MTEXT_LEN,0,0);//Block and get the first message in the mailbox.
                if (msglength>=0)break;
                else {printf("Rcv Error %d",errno);exit(1);}//This should never happen.
            }

            printf(msgptr->mtext);//Print child share 1.
            printf("\n");//Flush the buf.

            char* shared=shmat(sharemem,0,SHM_RDONLY);//Mount the shared memory.

            arg.val=0;
            for ( ; ; ) if (semctl(sem,0,GETVAL)>0) break;//Spinning waiting for sem.
            semctl(sem,0,SETVAL,arg);//enter the dangerous zone

            printf(shared);

            arg.val=1;
            semctl(sem,0,SETVAL,arg);//Quit the dangerous zone.

            printf("\n");//Flush the buf
            exit(0);
        }
    }else{
        int share=len/3;
        int rest=len%3;
        printf("Input is divided into 3 part: %d, %d, %d\n",share+rest,share,share);

        char output_buff[BUF_SIZE];
        size_t end1=0;
        for (size_t i=0;i<len-2*share;i++){
            output_buff[i]=buff[i];
            end1++;
        }

        output_buff[end1]='\0';

        printf("Your input is:\n");
        printf("%s",output_buff);//Print parent's share.
        printf("\n");//Flush the buff

        struct mymsg *msg=calloc(sizeof(long)+MTEXT_LEN,sizeof(char));//Malloc message;
        msg->type=1;
        size_t end2=end1;

        for(size_t i=end1;i<len-share;i++){//Writing message.
            msg->mtext[i-end1]=buff[i];
            end2++;
        }
        msg->mtext[end2]='\0';//Set the end of str.

        if (msgsnd(mailbox,msg,MTEXT_LEN,IPC_NOWAIT)==-1){printf("Send message failed.%d Exit.\n",errno);exit(1);};//Send message with share2.

        char* shared=shmat(sharemem,0,SHM_W);//Mount shared memory.

        if (shared==(char*)-1) {printf("Mount shared memory failed. Exit.\n");exit(1);};

        //while (semctl(sem,0,GETVAL)==0) sleep(1);

        arg.val=0;
        for ( ; ; ) if (semctl(sem,0,GETVAL)>0) break;//Spinning waiting for sem.
        semctl(sem,0,SETVAL,arg);//enter the dangerous zone

        for (size_t i=end2;i<len;i++){
            shared[i-end2]=buff[i];//Move the left part to the shared memory;
        }

        shared[len-end2]='\0';

        arg.val=1;
        semctl(sem,0,SETVAL,arg);//Quit the dangerous zone.

        while(canjump==0) sleep(1);//Spinning waiting for child ready.

        kill(child_pid,SIGUSR1);//Send signal to start child.

        waitpid(child_pid,&childQuitStatus,0);//Wait on child.
        printf("Child %d quit with status %d, Start to delete IPCs\n",child_pid,childQuitStatus);
        if (WIFSIGNALED(childQuitStatus))
            printf(" - abnormal termination,signal number=%d\n",WTERMSIG(childQuitStatus));

        msgctl(mailbox,IPC_RMID,none);
        semctl(sem,0,IPC_RMID);
        shmctl(sharemem,IPC_RMID,none);//Delete all IPC.
        printf("...\nAll IPCs deleted. Parent now change its image to /bin/id:\n\n");

        char* exec_path="/bin/id";//The executable to run.
        execl(exec_path,"0",(char*)0);//Change progress image.
    }
}