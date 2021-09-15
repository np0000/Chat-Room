#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#define USER_LIMIT 5
#define USER_NAME_LIMIT 10
#define BUFFER_SIZE 64
#define PROCESS_NUMBER 8

// ip 172.17.0.10 port 1790

class client_data
{
public:
    int active;
    char nickname[USER_NAME_LIMIT];
    char buf[BUFFER_SIZE];

    void init() {}
    void process() {}
};

class process
{
public:
    process() : m_pid( -1 ){}

public:
    pid_t m_pid;
    int room = -1;
    int m_pipefd[2];
};

template< typename T >
class processpool
{
private:
    // private instructor
    processpool( int listenfd, int process_number = PROCESS_NUMBER );
public:
    static processpool< T >* create( int listenfd, int process_number = 8 )
    {
        if( !m_instance )
        {
            m_instance = new processpool< T >( listenfd, process_number );
        }
        return m_instance;
    }
    ~processpool()
    {
        delete [] m_sub_process;
    }
    void run();

private:
    void setup_sig_pipe();
    void run_parent();
    void run_child();

private:
    static const int MAX_PROCESS_NUMBER = 16;
    static const int USER_PER_PROCESS = 65536;
    static const int MAX_EVENT_NUMBER = 10000;
    int m_process_number;
    int m_idx; // index of child process
    int m_epollfd; // fd of epoll table
    int m_listenfd; // fd of socket
    int m_stop;
    process* m_sub_process; // array of child processes
    static processpool< T >* m_instance; // singleton pattern one per process
};

// init static members
template< typename T >
processpool< T >* processpool< T >::m_instance = NULL;

// one signal pipe per process. 
// all I/O and signals will come from sig_pipefd[1]
static int sig_pipefd[2]; 

static int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// add fd to epoll table
static void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

static void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    //close( fd );
}

// send signal through sig_pipe[1]
// 将事件源统一到sig_pipe
static void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// set handler for signal sig
static void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

template< typename T >
processpool< T >::processpool( int listenfd, int process_number ) 
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( ( process_number > 0 ) && ( process_number <= MAX_PROCESS_NUMBER ) );

    m_sub_process = new process[ process_number ];
    assert( m_sub_process );

    for( int i = 0; i < process_number; ++i )
    {
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd );
        assert( ret == 0 );

        m_sub_process[i].m_pid = fork();
        assert( m_sub_process[i].m_pid >= 0 );
        // parent side is m_pipefd[0]
        if( m_sub_process[i].m_pid > 0 )
        {
            close( m_sub_process[i].m_pipefd[1] );
            continue;
        }
        else
        {
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i;
            break;
        }
    }
}

// called first by both parent and children when run()
// 建立sig_pipe以统一事件源
template< typename T >
void processpool< T >::setup_sig_pipe()
{
    m_epollfd = epoll_create( 5 );
    assert( m_epollfd != -1 );

    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] );
    addfd( m_epollfd, sig_pipefd[0] );

    addsig( SIGCHLD, sig_handler );
    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    addsig( SIGUSR1, sig_handler );
    addsig( SIGPIPE, SIG_IGN );
}

template< typename T >
void processpool< T >::run()
{
    if( m_idx != -1 )
    {
        run_child();
        return;
    }
    run_parent();
}

template< typename T >
void processpool< T >::run_child()
{
    setup_sig_pipe();

    int pipefd = m_sub_process[m_idx].m_pipefd[ 1 ];
    addfd( m_epollfd, pipefd ); // listen to get signal from parent

    char write_buf[BUFFER_SIZE];

    epoll_event events[ MAX_EVENT_NUMBER ];
    T* users = new T [ USER_PER_PROCESS ];
    assert( users );
    int number = 0;
    int ret = -1;
    int user_counter = 0;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        // process signals
        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            // new connection
            // informed by parent
            if( ( sockfd == pipefd ) && ( events[i].events & EPOLLIN ) )
            {
                iovec iov[1];  
                msghdr msg;  
                char name_buf[20];

                //指定缓冲区
                iov[0].iov_base = name_buf;
                iov[0].iov_len = 20;
                msg.msg_name = nullptr;
                msg.msg_namelen = 0;
                msg.msg_iov = iov;
                msg.msg_iovlen = 1;

                //辅助数据
                cmsghdr cm;
                msg.msg_control = &cm;
                msg.msg_controllen = CMSG_LEN(sizeof(int));

                ret = recvmsg(sockfd, &msg, 0);

                if( ret < 0 ) 
                {
                    continue;
                }
                else
                {
                    int connfd = *(int*)CMSG_DATA(&cm);
                    if ( connfd < 0 )
                    {
                        printf( "errno is: %d\n", errno );
                        continue;
                    }
                    // listen client tcp connection
                    // one connection will only be processed by one process
                    addfd( m_epollfd, connfd );
                    strcpy(users[connfd].nickname, name_buf);
                    users[connfd].active = true;
                    user_counter++;

                    strcpy(write_buf, "[SYSTEM] ");
                    strcat(write_buf, users[connfd].nickname);
                    strcat(write_buf, " has entered.\n");

                    for( int j = PROCESS_NUMBER; j < user_counter + PROCESS_NUMBER; ++j )
                    {
                        if (!users[j].active){
                            continue;
                        }
                        if( j == connfd )
                        {
                            const char* temp = "[SYSTEM] You have entered the room.\n";
                            send(j, temp, strlen(temp), 0);
                            continue;
                        }
                        send(j, write_buf, strlen(write_buf), 0);
                    }
                }
            }
            // signals
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            // wait for all childr processes
                            // if the task creates child process
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            // a client left
            else if( events[i].events & EPOLLRDHUP )
            {
                close(sockfd);
                users[sockfd].active = false;
                user_counter--;

                strcpy(write_buf, "[SYSTEM] ");
                strcat(write_buf, users[sockfd].nickname);
                strcat(write_buf, " has left.\n");

                for( int j = PROCESS_NUMBER; j < user_counter + PROCESS_NUMBER + 1; ++j )
                {
                    if (!users[j].active){
                        continue;
                    }
                    send(j, write_buf, strlen(write_buf), 0);
                }
                // printf( "a client left\n" );
                if (user_counter == 0){
                    // printf("all client has left\n");
                    kill(0, SIGUSR1);
                }
            }
            // data comes from client
            else if( events[i].events & EPOLLIN )
            {
                int connfd = sockfd;
                memset( users[connfd].buf, '\0', BUFFER_SIZE );
                ret = recv( connfd, users[connfd].buf, BUFFER_SIZE-1, 0 );
                printf( "get %d bytes of client data %s from %d\n", ret, users[connfd].buf, connfd );
                if( ret < 0 )
                {
                    if( errno != EAGAIN )
                    {
                        close( connfd );
                        user_counter--;
                    }
                }
                else if( ret == 0 )
                {
                    printf( "code should not come to here\n" );
                }
                else
                {
                    time_t t = time( 0 );
                    char tmp_buf[20];
                    strftime(tmp_buf, 20, "%H:%M:%S", localtime(&t));
                    strcpy(write_buf, "[");
                    strcat(write_buf, tmp_buf);
                    strcat(write_buf, "] ");
                    strcat(write_buf, users[connfd].nickname);
                    strcat(write_buf, ": ");
                    strcat(write_buf, users[connfd].buf);

                    // send(connfd, write_buf, strlen(write_buf), 0);

                    for( int j = PROCESS_NUMBER; j < user_counter + PROCESS_NUMBER; ++j )
                    {
                        send(j, write_buf, strlen(write_buf), 0);
                    }
                }
            }
            else
            {
                continue;
            }
        }
    }

    delete [] users;
    users = NULL;
    close( pipefd );
    close( m_epollfd );
}

template< typename T >
void processpool< T >::run_parent()
{
    setup_sig_pipe();

    // parent listen socket
    addfd( m_epollfd, m_listenfd );

    char buf[20];

    epoll_event events[ MAX_EVENT_NUMBER ];
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            // new client
            if( sockfd == m_listenfd )
            {
                // establish tcp connection with client
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( m_listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    printf( "errno is: %d\n", errno );
                    continue;
                }
                // listen client tcp connection
                // one connection will only be processed by one process
                addfd( m_epollfd, connfd );
            }
            // parent's signals
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            // child stopped
                            case SIGCHLD:
                            {
                                pid_t pid;
                                int stat;
                                // find the stopped child and close channel
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    for( int i = 0; i < m_process_number; ++i )
                                    {
                                        if( m_sub_process[i].m_pid == pid )
                                        {
                                            printf( "child %d join\n", i );
                                            close( m_sub_process[i].m_pipefd[0] );
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                // if all children stopped, stop server
                                m_stop = true;
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    if( m_sub_process[i].m_pid != -1 )
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGUSR1:{
                                printf("room %d is empty and recycled\n", m_sub_process[i].room);
                                m_sub_process[i].room = -1;
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                printf( "kill all the clild now\n" );
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if( pid != -1 )
                                    {
                                        kill( pid, SIGTERM );
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            // client pick room number
            else if (events[i].events & EPOLLIN){
                int connfd = sockfd;
                int room = -1;
                memset(buf, '\0', 20);
                ret = recv(connfd, &room, sizeof(room), 0);
                ret = recv(connfd, buf, 20, 0);
                if (ret < 0){
                    if (errno != EAGAIN)
                    {
                        removefd(m_epollfd, connfd);
                        close(connfd);
                    }
                }
                else if (ret == 0){
                    printf("a user left before entering room\n");
                    removefd(m_epollfd, connfd);
                    close(connfd);
                }
                else
                {
                    printf("user %s choose room %d\n", buf, room);
                    
                    // find proper child process to handle
                    int child_idx = -1;
                    for (int i = 0; i < m_process_number; i++){
                        if (m_sub_process[i].room == room){
                            child_idx = i;
                            break;
                        }
                    }
                    if (child_idx == -1){
                        for (int i = 0; i < m_process_number; i++){
                            if (m_sub_process[i].room == -1){
                                child_idx = i;
                                break;
                            }
                        }
                        printf( "make child %d room %d\n", child_idx, room);
                    }
                    if (child_idx == -1){
                        m_stop = true;
                        break;
                    }
                    
                    m_sub_process[child_idx].room = room;

                    // send message to child
                    iovec iov[1];
                    msghdr msg;

                    //指定缓冲区
                    iov[0].iov_base = buf;
                    iov[0].iov_len = 20;
                    msg.msg_name = nullptr;
                    msg.msg_namelen = 0;
                    msg.msg_iov = iov;
                    msg.msg_iovlen = 1;
                    
                    //辅助数据
                    cmsghdr cm;
                    cm.cmsg_len = CMSG_LEN(sizeof(int));
                    cm.cmsg_level = SOL_SOCKET;         //发起协议
                    cm.cmsg_type = SCM_RIGHTS;          //协议类型
                    *(int*)CMSG_DATA(&cm) = connfd; //设置待发送描述符

                    //设置辅助数据
                    msg.msg_control = &cm;
                    msg.msg_controllen = CMSG_LEN(sizeof(int));

                    sendmsg(m_sub_process[i].m_pipefd[0], &msg, 0);
                    removefd(m_epollfd, connfd);
                    close(connfd);
                }
            }
            else
            {
                continue;
            }
        }
    }

    close( m_epollfd );
}

int main(int argc, char* argv[]){
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    auto pool = processpool<client_data>::create(listenfd);
    if (pool){
        pool->run();
        delete pool;
    }
    close(listenfd);
    return 0;
}