#include <iostream>
#include <cstring>
#include <cstdint>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <pthread.h>
#include <algorithm>
#include <vector>
#include <list>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <errno.h>
#include <unistd.h>
#include <lemon/bfs.h>
#include <lemon/lgf_reader.h>
#include <lemon/path.h>
#include <lemon/smart_graph.h>
#include "csv.h"

using namespace lemon;
using namespace std;

int server_start_listen();
void do_command(char *command, int client);
int server_establish_connection(int server_fd);
int server_send(int fd, string data);
void *tcp_server_read(void *arg);
void mainloop(int server_fd);

// Server constants
const char *PORT = "8888";      // port numbers 1-1024 are probably reserved by your OS
const int MAXLEN = 1024;        // Max length of a message.
const int MAXFD = 32;           // Maximum file descriptors to use. Equals maximum clients.
const int BACKLOG = 8;          // Number of connections that can wait in que before they be accept()ted

// This needs to be declared volatile because it can be altered by an other thread. Meaning the compiler cannot
// optimise the code, because it's declared that not only the program can change this variable, but also external
// programs. In this case, a thread.
volatile fd_set the_state;

pthread_mutex_t mutex_state = PTHREAD_MUTEX_INITIALIZER;

SmartDigraph g;
SmartDigraph::Node genesis = INVALID;
SmartDigraph::NodeMap<string> address(g);
SmartDigraph::ArcMap<string> tx_hash(g);

unordered_map<string, int> clusters;

vector<string> tokenize(string const &input)
{
    istringstream str(input);
    istream_iterator<string> cur(str), end;
    return vector<string>(cur, end);
}

int main()
{
    try
    {
        digraphReader(g, "../grapher/tx_graph.lgf").  // read the directed graph into g
        arcMap("tx_hash", tx_hash).                   // read the 'tx_hash' arc map into tx_hash
        nodeMap("label", address).
        node("source", genesis).                      // read 'source' node to genesis
        run();
    }
    catch (Exception &error)
    {
        cerr << "Error: " << error.what() << endl;
        return -1;
    }

    cerr << "Graph loaded from 'tx_graph.lgf'." << endl;
    cerr << "Number of nodes: " << countNodes(g) << endl;
    cerr << "Number of arcs: " << countArcs(g) << endl;

    io::CSVReader<3> in("../clusterizer/clusters.csv");
    in.read_header(io::ignore_extra_column, "address", "cluster");
    string address; int cluster;
    while (in.read_row(address, cluster))
    {
        clusters.insert(make_pair<string, int>(address, cluster));
    }

    int server_fd = server_start_listen();

    cerr << "Server started." << endl;

    if (server_fd == -1)
    {
        cerr << "An error occured. Closing program." << endl;
        return 1;
    }

    mainloop(server_fd);

    return 0;
}

void print_cluster(int client, int cluster)
{
    server_send(client, "BEGIN\n");

    for (auto &it : clusters)
    {
        if (it->second == cluster)
        {
            server_send(client, it->first + "\n");
        }
    }

    server_send(client, "END\n");
}

string find_path(string from, string to)
{
    Path<SmartDigraph> p;
    SmartDigraph::Node s = INVALID, t = INVALID;

    int d;
    ostringstream oss;

    for (SmartDigraph::NodeIt n(g); (s == INVALID || t == INVALID) && n != INVALID; ++n)
    {
        if (s == INVALID && address[n] == from)
            s = n;
        if (t == INVALID && address[n] == to)
            t = n;
    }

    if (s == INVALID || t == INVALID)
        return "";

    bool reached = bfs(g).path(p).dist(d).run(s, t);

    if (reached)
    {
        oss << d << endl;
    }
    else
    {
        cerr << "No paths from source to destination." << endl;
        return "";
    }

    oss << from;

    for (Path<SmartDigraph>::ArcIt a(p); a != INVALID; ++a)
    {
        oss << ">" << address[g.target(a)];
    }

    oss << endl;

    bool first = true;
    for (Path<SmartDigraph>::ArcIt a(p); a != INVALID; ++a)
    {
        if (first)
        {
            first = false;
        }
        else
        {
            oss << ">";
        }
        oss << tx_hash[a];
    }

    oss << endl;

    return oss.str();
}

unordered_set<string> find_successors(string from)
{
    SmartDigraph::Node s = INVALID;
    unordered_set<string> successors;

    for (SmartDigraph::NodeIt n(g); (s == INVALID) && n != INVALID; ++n)
    {
        if (address[n] == from)
            s = n;
    }

    for (SmartDigraph::OutArcIt a(g, s); a != INVALID && s != INVALID; ++a)
    {
        successors.insert(address[g.target(a)]);
    }

    return successors;
}

unordered_set<string> find_predecessors(string from)
{
    SmartDigraph::Node s = INVALID;
    unordered_set<string> predecessors;

    for (SmartDigraph::NodeIt n(g); (s == INVALID) && n != INVALID; ++n)
    {
        if (address[n] == from)
            s = n;
    }

    for (SmartDigraph::InArcIt a(g, s); a != INVALID && s != INVALID; ++a)
    {
        predecessors.insert(address[g.source(a)]);
    }

    return predecessors;
}


int server_start_listen()
{
    struct addrinfo hostinfo, *res;

    int sock_fd;

    int server_fd; // the fd the server listens on
    int ret;
    int yes = 1;

    // first, load up address structs with getaddrinfo():

    memset(&hostinfo, 0, sizeof(hostinfo));

    hostinfo.ai_family = AF_INET;
    hostinfo.ai_socktype = SOCK_STREAM;
    hostinfo.ai_flags = AI_PASSIVE;

    getaddrinfo(NULL, PORT, &hostinfo, &res);

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    //if(server_fd < 0) throw some error;

    //prevent "Error Address already in use"
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
    // if(ret < 0) throw some error;

    ret = ::bind(server_fd, res->ai_addr, res->ai_addrlen);

    if (ret != 0)
    {
        cerr << "Error:" << strerror(errno) << endl;
        return -1 ;
    }

    ret = listen(server_fd, BACKLOG);
    //if(ret < 0) throw some error;

    return server_fd;
}

int server_establish_connection(int server_fd)
// This function will establish a connection between the server and the
// client. It will be executed for every new client that connects to the server.
// This functions returns the socket filedescriptor for reading the clients data
// or an error if it failed.
{
    char ipstr[INET_ADDRSTRLEN];
    int port;

    int new_sd;
    struct sockaddr_storage remote_info ;
    socklen_t addr_size;

    addr_size = sizeof(addr_size);
    new_sd = accept(server_fd, (struct sockaddr *) &remote_info, &addr_size);
    //if (fd < 0) throw some error here;

    getpeername(new_sd, (struct sockaddr *)&remote_info, &addr_size);

    struct sockaddr_in *s = (struct sockaddr_in *)&remote_info;
    port = ntohs(s->sin_port);
    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);

    std::cerr << "Connection accepted from "  << ipstr <<  " using port " << port << endl;

    return new_sd;
}

int server_send(int fd, string data)
// This function will send data to the clients fd.
// data contains the message to be send
{
    int ret;

    ret = send(fd, data.c_str(), strlen(data.c_str()), 0);
    //if(ret != strlen(data.c_str()) throw some error;
    return 0;
}

void *tcp_server_read(void *arg)
// This function runs in a thread for every client, and reads incoming data.
{
    intptr_t rfd;

    char buf[MAXLEN];
    int buflen;
    int wfd;

    rfd = (intptr_t)arg;
    for (;;)
    {
        buflen = read(rfd, buf, sizeof(buf));
        if (buflen <= 0)
        {
            cerr << "Client disconnected. Clearing fd " << rfd << endl ;
            pthread_mutex_lock(&mutex_state);
            FD_CLR(rfd, &the_state);
            pthread_mutex_unlock(&mutex_state);
            close(rfd);
            pthread_exit(NULL);
        }

        do_command(buf, rfd);

    }
    return NULL;
}

void mainloop(int server_fd)
// This loop will wait for a client to connect. When the client connects, it creates a
// new thread for the client and starts waiting again for a new client.
{
    string welcome_msg = "200 BitIodine 1.0.0 READY\n";

    pthread_t threads[MAXFD];

    FD_ZERO(&the_state); // FD_ZERO clears all the filedescriptors in the file descriptor set fds.

    while (1) // start looping here
    {
        intptr_t rfd;
        void *arg;

        // if a client is trying to connect, establish the connection and create a fd for the client.
        rfd = server_establish_connection(server_fd);

        if (rfd >= 0)
        {
            cerr << "Client connected. Using file descriptor " << rfd << endl;
            if (rfd > MAXFD)
            {
                cerr << "Too many clients are trying to connect." << endl;
                close(rfd);
                continue;
            }

            pthread_mutex_lock(&mutex_state);  // Make sure no 2 threads can create a fd simultanious.

            FD_SET(rfd, &the_state);  // Add a file descriptor to the FD-set.

            pthread_mutex_unlock(&mutex_state); // End the mutex lock

            arg = (void *) rfd;

            server_send(rfd, welcome_msg); // send a welcome message/instructions.

            // now create a thread for this client to intercept all incoming data from it.
            pthread_create(&threads[rfd], NULL, tcp_server_read, arg);
        }
    }
}

void do_command(char *command_c, int client)
{
    string command = command_c;
    string output;
    vector<string> tokens = tokenize(command);

    if (tokens[0] == "SHORTEST_PATH_A2A")
    {
        if (tokens.size() < 3)
        {
            server_send(client, "500 Arguments error.\n");
            return;
        }

        output = find_path(tokens[1], tokens[2]);

        if (output != "")
        {
            server_send(client, "BEGIN\n");
            server_send(client, output);
            server_send(client, "END\n");
        }
        else
        {
            server_send(client, "500 No path.\n");
        }
        return;
    }
    else if (tokens[0] == "SUCCESSORS")
    {
        if (tokens.size() < 2)
        {
            server_send(client, "500 Arguments error.\n");
            return;
        }

        server_send(client, "BEGIN\n");

        unordered_set<string> successors = find_successors(tokens[1]);

        if (!successors.empty())
        {
            for (auto itr = successors.begin(); itr != successors.end(); ++itr)
            {
                output += (*itr) + ",";
            }

            server_send(client, output.substr(0, output.size() - 1) + "\n");
            server_send(client, "END\n");
        }
        else
            server_send(client, "500 No successors.\n");
        return;
    }
    else if (tokens[0] == "PREDECESSORS")
    {
        if (tokens.size() < 2)
        {
            server_send(client, "500 Arguments error.\n");
            return;
        }

        server_send(client, "BEGIN\n");

        unordered_set<string> predecessors = find_predecessors(tokens[1]);

        if (!predecessors.empty())
        {
            for (auto itr = predecessors.begin(); itr != predecessors.end(); ++itr)
            {
                output += (*itr) + ",";
            }

            server_send(client, output.substr(0, output.size() - 1) + "\n");
            server_send(client, "END\n");
        }
        else
            server_send(client, "500 No predecessors.\n");
        return;
    }
    else if (tokens[0] == "QUIT")
    {
        exit(0);
    }
    else if (tokens[0] == "PRINT_CLUSTER")
    {
        int cluster;

        if (tokens.size() < 2)
        {
            server_send(client, "500 Arguments error.\n");
            return;
        }

        print_cluster(client, tokens[1]);

        return;
    }
    else if (tokens[0] == "PRINT_NEIGHBORS")
    {
        int cluster;

        if (tokens.size() < 2)
        {
            server_send(client, "500 Arguments error.\n");
            return;
        }

        unordered_map<string, int>::const_iterator got = clusters.find(tokens[1]);

        if (got == clusters.end())
        {
            server_send(client, "500 Address not present in any cluster.\n");
            return;
        }
        else
        {
            cluster = got->second;
        }

        print_cluster(client, cluster);

        return;
    }
    else
    {
        server_send(client, "404 COMMAND NOT FOUND\n");
    }
}