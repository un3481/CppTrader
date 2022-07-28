
/* a client in the unix domain */

#include <OptionParser.h>

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>

void error(const char *msg)
{
    perror(msg);
    exit(0);
}

int main(int argc, char *argv[])
{
    // Parse input args 
    auto parser = optparse::OptionParser().version("1.0.0.0");
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
    const uint CMDSIZE = 1024; // The size of a Command is set to 1024 bytes
    char buffer[CMDSIZE];

    // Set Variables
    char* input;
    ssize_t size;
    
    // Send message
    while (1)
    {
        bzero(buffer, sizeof(buffer));
        input = fgets(buffer, sizeof(buffer), stdin);
        buffer[strcspn(buffer, "\r\n")] = 0;
        size = write(sockfd, buffer, sizeof(buffer));
        if ((input + size) < 0) {}
    }

    // Close socket
    close(sockfd);
    return 0;
}
