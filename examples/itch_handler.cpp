
/* a client in the unix domain */

#include <OptionParser.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>

/* ############################################################################################################################################# */

#define VERSION "1.0.4.0"

#define MSG_SIZE 256 // Buffer size for messages on socket stream (bytes)
#define MSG_SIZE_SMALL 64 // Buffer size for small messages on socket stream (bytes)
#define MSG_SIZE_LARGE 1024 // Buffer size for large messages on socket stream (bytes)

/* ############################################################################################################################################# */

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

// Check if descriptor is ready for read (non-blocking)
inline int SelectReadBlocking(int fd)
{
    fd_set fdset; // Single descriptor set
    FD_ZERO(&fdset); FD_SET(fd, &fdset);
    struct timeval tv = {1, 0}; // Timeout zero (to prevent blocking)
    return select(fd + 1, &fdset, NULL, NULL, &tv);
}

// Read stream on Unix socket (non-blocking)
int ReadSocketStream(int sockfd, std::string* dest)
{
    // Always clear string
    (*dest).clear();

    // Check if data is available
    int rdy = SelectReadBlocking(sockfd);
    if (rdy <= 0) return rdy;

    // Read stream to string
    char buffer[MSG_SIZE_LARGE]; // Read MSG_SIZE bytes
    if (read(sockfd, buffer, MSG_SIZE_LARGE) <= 0) return -1;
    (*dest).append(buffer, strcspn(buffer, "\0")); // buffer is copied to string until the first \0 char is found

    return 1;
}

// Read stream on Unix socket (non-blocking)
int ReadSocketStreamSmall(int sockfd, std::string* dest)
{
    // Always clear string
    (*dest).clear();

    // Check if data is available
    int rdy = SelectReadBlocking(sockfd);
    if (rdy <= 0) return rdy;

    // Read stream to string
    char buffer[MSG_SIZE_SMALL]; // Read MSG_SIZE bytes
    if (read(sockfd, buffer, MSG_SIZE_SMALL) <= 0) return -1;
    (*dest).append(buffer, strcspn(buffer, "\0")); // buffer is copied to string until the first \0 char is found

    return 1;
}

/* ############################################################################################################################################# */

int main(int argc, char *argv[])
{
    // Parse input args 
    auto parser = optparse::OptionParser().version(VERSION);
    parser.add_option("-p", "--path").dest("path").help("socket path");
    optparse::Values options = parser.parse_args(argc, argv);

    // Print help
    if (options.get("help"))
    {
        parser.print_help();
        return 0;
    }

    // Check for Path input
    if (!options.is_set("path")) error("no path provided for socket");

    struct sockaddr_un  serv_addr;
    bzero((char *)&serv_addr,sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strcpy(serv_addr.sun_path, options.get("path"));
    int servlen = strlen(serv_addr.sun_path) + sizeof(serv_addr.sun_family);

    // Create socket
    int sockfd;
    if ((sockfd = socket(AF_UNIX, SOCK_STREAM,0)) < 0) error("error creating socket");

    // Connect socket
    if (connect(sockfd, (struct sockaddr *) &serv_addr, servlen) < 0) error("error connecting");

    // Set Consts
    char buffer[MSG_SIZE];

    // Set Variables
    char* input;
    ssize_t size;

    /* ############################################################################################################################################# */
    
    // Send message
    while (1)
    {
        bzero(buffer, MSG_SIZE);
        input = fgets(buffer, MSG_SIZE, stdin);
        buffer[strcspn(buffer, "\r\n")] = 0;
        size = write(sockfd, buffer, MSG_SIZE);

        std::string instr = buffer;
        if (instr.find("add ") != std::string::npos)
        {
            std::string result;
            int rdy = ReadSocketStreamSmall(sockfd, &result);
            if (rdy < 0) {};
            std::cout << result << std::endl;
        }
        if (instr.find("get book") != std::string::npos)
        {
            std::string result;
            int rdy = ReadSocketStream(sockfd, &result);
            if (rdy < 0) {};
            std::cout << result << std::endl;
        }

        if ((input + size) < 0) {}
    }

    // Close socket
    close(sockfd);
    return 0;
}

/* ############################################################################################################################################# */