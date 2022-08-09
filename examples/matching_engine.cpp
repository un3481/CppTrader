/* a server in the unix domain */

#include "trader/matching/market_manager.h"

#include "system/stream.h"
#include "filesystem/file.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/un.h>

#include <string>
#include <vector>
#include <regex>
#include <iostream>
#include <OptionParser.h>

using namespace CppCommon;
using namespace CppTrader::Matching;

/* ############################################################################################################################################# */
// Constants

#define VERSION "2.0.2.0" // Program version

#define MSG_SIZE 1024 // Buffer size for messages on socket stream (bytes)
#define MSG_SIZE_LARGE 8192 // Buffer size for large messages on socket stream (bytes)

#define MAX_CLIENTS 64 // Max number of simultaneous clients connected to socket

#define STATUS_RUN "RUNNING" // Daemon status (RUN)
#define STATUS_GSTOP "GRACEFULLY_STOPPED" // Daemon status (GSTOP)
#define STATUS_ABEND "ABEND" // Daemon status (ABEND)

#define CSV_SEP "," // CSV separator
#define CSV_EOL "\n" // CSV end of line

static const std::string _str = "";

/* ############################################################################################################################################# */

// Get Timestamp for Logs
inline std::string now()
{
    time_t ct = time(0);
    struct tm  tstruct;
    char buf[80];
    tstruct = *localtime(&ct);
    strftime(buf, sizeof(buf), "%Y-%m-%d %X", &tstruct);
    return std::string(buf);
}

// Log
inline void log(const std::string& msg)
    { std::cout << now() << '\t' << msg << std::endl; }

// Log Error
inline void error(const std::string& msg)
    { std::cerr << now() << '\t' << msg << std::endl; }

// Error during the CLI step
inline void CliError(const char *msg)
    { perror(msg); exit(1); }


/* ############################################################################################################################################# */

// Change current process to Daemon
static void Daemonize(const char* root)
{
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    if (chdir(root) < 0) { error("error changing root directory"); exit(1); };

    /* Close all open file descriptors */
    for (int fd = sysconf(_SC_OPEN_MAX); fd >= 0; fd--)
    { close(fd); }
}

/* ############################################################################################################################################# */

// Create Unix socket
int UnixSocket(const char* path, int clients)
{
    // Set Variables
    struct sockaddr_un sock_addr;
    int sockfd, addr_len;

    // Set Address
    bzero((char *) &sock_addr, sizeof(sock_addr));
    sock_addr.sun_family = AF_UNIX;
    strcpy(sock_addr.sun_path, path);
    addr_len = strlen(sock_addr.sun_path) + sizeof(sock_addr.sun_family);

    // Create socket
    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;

    // Bind socket
    if (bind(sockfd, (struct sockaddr *)&sock_addr, addr_len) < 0) return -2;

    // Listen for connection
    if (listen(sockfd, clients) < 0) return -3;

    // Return file descriptor
    return sockfd;
}

// Connect to Unix socket
int ConnectUnixSocket(const char* path)
{
    // Set Variables
    struct sockaddr_un sock_addr;
    int sockfd, addr_len;

    // Set Address
    bzero((char *) &sock_addr, sizeof(sock_addr));
    sock_addr.sun_family = AF_UNIX;
    strcpy(sock_addr.sun_path, path);
    addr_len = strlen(sock_addr.sun_path) + sizeof(sock_addr.sun_family);

    // Create socket
    if ((sockfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) return -1;

    // Connect socket
    if (connect(sockfd, (struct sockaddr *) &sock_addr, addr_len) < 0) return -2;

    return sockfd;
}

/* ############################################################################################################################################# */

// Check if descriptor is ready for read (non-blocking)
inline int SelectReadNonBlocking(int fd)
{
    fd_set fdset; // Single descriptor set
    FD_ZERO(&fdset); FD_SET(fd, &fdset);
    struct timeval tv = {0, 0}; // Timeout zero (to prevent blocking)
    return select(fd + 1, &fdset, NULL, NULL, &tv);
}

// Check if descriptor is ready for write
inline int SelectWrite(int fd)
{
    fd_set fdset; // Single descriptor set
    FD_ZERO(&fdset); FD_SET(fd, &fdset);
    struct timeval tv = {1, 0}; // Timeout of 1 second
    return select(fd + 1, NULL, &fdset, NULL, &tv);
}

/* ############################################################################################################################################# */

// Apply select() on vector of file descriptors
int SelectVector(std::vector<int>* fdvec)
{
    int maxfd = 0; // Max descriptor number
    fd_set fdset; // Descriptor set
    FD_ZERO(&fdset);
    for (int fd : *fdvec)
    {
        FD_SET(fd, &fdset); // Add descriptor to set
        if (fd > maxfd) maxfd = fd;
    }
    if (maxfd == 0) return 0; // Vector empty
    return select(maxfd + 1, &fdset, NULL, NULL, NULL);
}

// Apply close() on vector of file descriptors
int CloseVector(std::vector<int>* fdvec)
{
    int code = 0;
    for (auto it = (*fdvec).rbegin(); it != (*fdvec).rend(); ++it)
        { code += close(*it); } // Do reverse iteration on the vector
    if (code < 0) return -1;
    else return 0; // Return close() code
}

/* ############################################################################################################################################# */

// Read stream on Unix socket (non-blocking)
int ReadSocketStream(int sockfd, std::string* dest)
{
    // Always clear string
    (*dest).clear();

    // Check if data is available
    int rdy = SelectReadNonBlocking(sockfd);
    if (rdy <= 0) return rdy;

    // Read stream to string
    char buffer[MSG_SIZE]; // Read MSG_SIZE bytes
    if (read(sockfd, buffer, sizeof(buffer)) <= 0) return -1;
    (*dest).append(buffer, strcspn(buffer, "\0")); // buffer is copied to string until the first \0 char is found

    return 1;
}

// Write to stream on Unix socket
int WriteSocketStream(int sockfd, std::string* data)
{
    // Check if write is available
    int rdy = SelectWrite(sockfd);
    if (rdy <= 0) return rdy;

    // Write string to stream
    char buffer[MSG_SIZE_LARGE]; // Write MSG_SIZE_LARGE bytes
    strcpy(buffer, (*data).c_str() + '\0'); // string is copied to buffer with a trailing \0 char 
    if (write(sockfd, buffer, sizeof(buffer)) <= 0) return -1;

    return 1;
}

/* ############################################################################################################################################# */

// Accept connection on Unix socket (non-blocking)
int AcceptConnection(int sockfd)
{
    // Check if connection is available
    int rdy = SelectReadNonBlocking(sockfd);
    if (rdy <= 0) return rdy;

    struct sockaddr_un sock_addr;
    socklen_t sock_len = sizeof(sock_addr);

    // Accept connection
    return accept(sockfd, (struct sockaddr *)&sock_addr, &sock_len);
}

/* ############################################################################################################################################# */

// Order CSV Header
static const std::string OrderCSVHeader = (_str +
    "Id" + CSV_SEP +
    "SymbolId" + CSV_SEP +
    "Type" + CSV_SEP +
    "Side" + CSV_SEP +
    "Price" + CSV_SEP +
    "StopPrice" + CSV_SEP +
    "Quantity" + CSV_SEP +
    "ExecutedQuantity" + CSV_SEP +
    "LeavesQuantity" + CSV_SEP +
    "TimeInForce" + CSV_SEP +
    "TrailingDistance" + CSV_SEP +
    "TrailingStep" + CSV_SEP +
    "MaxVisibleQuantity" + CSV_SEP +
    "Slippage"
);

// Parse Order to CSV
inline std::string ParseOrder(const Order& order)
{
    static const char* OrderSides[] = {"BUY","SELL"};
    static const char* OrderTypes[] = {"MARKET","LIMIT","STOP","STOP_LIMIT","TRAILING_STOP","TRAILING_STOP_LIMIT"};
    static const char* OrderTIFs[] = {"GTC","IOC","FOK","AON"};
    static const std::string NullField = "NULL";
    std::string csv;

    csv.append(
        std::to_string(order.Id) + CSV_SEP +
        std::to_string(order.SymbolId) + CSV_SEP +
        OrderTypes[(int)order.Type] + CSV_SEP +
        OrderSides[(int)order.Side] + CSV_SEP +
        std::to_string(order.Price) + CSV_SEP +
        std::to_string(order.StopPrice) + CSV_SEP +
        std::to_string(order.Quantity) + CSV_SEP +
        std::to_string(order.ExecutedQuantity) + CSV_SEP +
        std::to_string(order.LeavesQuantity) + CSV_SEP +
        OrderTIFs[(int)order.TimeInForce] + CSV_SEP
    );
    if (order.IsTrailingStop() || order.IsTrailingStopLimit()) csv.append(
        std::to_string(order.TrailingDistance) + CSV_SEP +
        std::to_string(order.TrailingStep) + CSV_SEP
    );
    else csv.append(NullField + CSV_SEP + NullField + CSV_SEP);
    if (order.IsHidden() || order.IsIceberg()) csv.append(std::to_string(order.MaxVisibleQuantity) + CSV_SEP);
    else csv.append(NullField + CSV_SEP);
    if (order.IsSlippage()) csv.append(std::to_string(order.Slippage));
    else csv.append(NullField);
    
    return csv;
}

/* ############################################################################################################################################# */

// Order Book CSV Header
static const std::string OrderBookCSVHeader = (_str +
    "Group" + CSV_SEP +
    "LevelType" + CSV_SEP +
    "LevelPrice"
);

// Parse OrderBook::Levels to CSV
std::string ParseOrderBookLevels(MarketManager& market, OrderBook::Levels levels, const char* group)
{
    static const char* LevelTypes[] = {"BID","ASK"};
    static const std::string empty = "";
    std::string csv;
    
    // Loop over Levels orders
    for (auto level : levels) {
        // Get Level properties
        const std::string level_props = (empty +
            group + CSV_SEP +
            LevelTypes[(int)level.Type] + CSV_SEP +
            std::to_string(level.Price) + CSV_SEP
        );
        for (auto node : level.OrderList) {
            auto order = market.GetOrder(node.Id);
            csv.append(level_props); // Insert level properties
            csv.append(ParseOrder(*order)); // Insert Order properties
            csv.append(CSV_EOL);
        }
    }

    return csv;
}

/* ############################################################################################################################################# */

// Parse OrderBook to CSV
std::string ParseOrderBook(MarketManager& market, const OrderBook* order_book_ptr)
{
    // Insert header
    std::string csv;
    csv.append(
        OrderBookCSVHeader + CSV_SEP +
        OrderCSVHeader + CSV_EOL
    );

    // Parse Levels
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).bids(), "BIDS"));
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).asks(), "ASKS"));
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).buy_stop(), "BUY_STOP"));
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).sell_stop(), "SELL_STOP"));
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).trailing_buy_stop(), "TRAILING_BUY_STOP"));
    csv.append(ParseOrderBookLevels(market, (*order_book_ptr).trailing_sell_stop(), "TRAILING_SELL_STOP"));

    return csv;
}

/* ############################################################################################################################################# */

/* Command Context */

namespace CommandCtx {

    // Context struct
    struct Context
    {
        int Connection; // File Descriptor for current Connection
        std::string Command; // Command text
        MarketManager* market_ptr; // Pointer to Market Manager
        MyMarketHandler* market_handler_ptr; // Pointer to Market Handler
        uint64_t OrderId; // Order Id
        std::string TextId; // Order TextId
    };

    // Static context
    Context& _ctx()
    {
        static Context ctx;
        return ctx;
    }

    // Get Context
    Context Get()
    {
        auto ctx = _ctx();
        return Context(ctx);
    }

    // Set Context
    void Set(Context& value)
    {
        auto ctx = _ctx();
        auto new_ctx = Context(value);
        ctx = new_ctx;
    }

    // Clear Context
    void Clear()
    {
        Context ctx;
        Set(ctx);
    }
}

/* ############################################################################################################################################# */

/* Custom Market Handler */

class MyMarketHandler : public MarketHandler
{

private:
    size_t _updates;
    size_t _symbols;
    size_t _max_symbols;
    size_t _order_books;
    size_t _max_order_books;
    size_t _max_order_book_levels;
    size_t _max_order_book_orders;
    size_t _orders;
    size_t _max_orders;
    size_t _add_orders;
    size_t _update_orders;
    size_t _delete_orders;
    size_t _execute_orders;
    size_t _lts_order_id;

public:
    MyMarketHandler()
        : _updates(0),
          _symbols(0),
          _max_symbols(0),
          _order_books(0),
          _max_order_books(0),
          _max_order_book_levels(0),
          _max_order_book_orders(0),
          _orders(0),
          _max_orders(0),
          _add_orders(0),
          _update_orders(0),
          _delete_orders(0),
          _execute_orders(0),
          _lts_order_id(0)
    {}

    size_t updates() const { return _updates; }
    size_t max_symbols() const { return _max_symbols; }
    size_t max_order_books() const { return _max_order_books; }
    size_t max_order_book_levels() const { return _max_order_book_levels; }
    size_t max_order_book_orders() const { return _max_order_book_orders; }
    size_t max_orders() const { return _max_orders; }
    size_t add_orders() const { return _add_orders; }
    size_t update_orders() const { return _update_orders; }
    size_t delete_orders() const { return _delete_orders; }
    size_t execute_orders() const { return _execute_orders; }
    size_t lts_order_id() const { return _lts_order_id; }

/* ############################################################################################################################################# */

/* Handler Callbacks */

protected:
    void onAddSymbol(const Symbol& symbol) override
    {
        ++_updates; ++_symbols; _max_symbols = std::max(_symbols, _max_symbols);

        // Log Add Symbol
        std::cout << now() << '\t' << "Add symbol: " << symbol << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddSymbol 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onDeleteSymbol(const Symbol& symbol) override
    {
        ++_updates; --_symbols;

        // Log Delete Symbol
        std::cout << now() << '\t' << "Delete symbol: " << symbol << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteSymbol 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onAddOrderBook(const OrderBook& order_book) override
    {
        ++_updates; ++_order_books; _max_order_books = std::max(_order_books, _max_order_books);

        // Log Add Order Book
        std::cout << now() << '\t' << "Add order book: " << order_book << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onUpdateOrderBook(const OrderBook& order_book, bool top) override
    {
        _max_order_book_levels = std::max(std::max(order_book.bids().size(), order_book.asks().size()), _max_order_book_levels);

        // Log Update Order Book
        std::cout << now() << '\t' << "Update order book: " << order_book << (top ? " - Top of the book!" : "") << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server UpdateOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onDeleteOrderBook(const OrderBook& order_book) override
    {
        ++_updates; --_order_books;

        // Log Delete Order Book
        std::cout << now() << '\t' << "Delete order book: " << order_book << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onAddLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates;

        // Log Add Level
        std::cout << now() << '\t' << "Add level: " << level << (top ? " - Top of the book!" : "") << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onUpdateLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates; _max_order_book_orders = std::max(level.Orders, _max_order_book_orders);

        // Log Update Level
        std::cout << now() << '\t' << "Update level: " << level << (top ? " - Top of the book!" : "") << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server UpdateLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onDeleteLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates;

        // Log Delete Leve
        std::cout << now() << '\t' << "Delete level: " << level << (top ? " - Top of the book!" : "") << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onAddOrder(const Order& order) override
    {
        ++_updates; ++_orders; _max_orders = std::max(_orders, _max_orders); ++_add_orders;

        // Update Unique Id Record
        _lts_order_id = std::max((size_t)order.Id, _lts_order_id);

        // Update SQLite
        uint64_t ctx_id = CommandCtx::Get().OrderId;
        if (ctx_id == order.Id)
        {
            // SQLite::AddOrder(order);
        }

        // Log Add Order
        std::cout << now() << '\t' << "Add order: " << order << std::endl;

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddOrder 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onUpdateOrder(const Order& order) override
    {
        ++_updates; ++_update_orders;

        // Log Order Update
        std::cout << now() << '\t' << "Update order: " << order << std::endl; 

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server UpdateOrder 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onDeleteOrder(const Order& order) override
    {
        ++_updates; --_orders; ++_delete_orders;

        // Log Deleted Order
        std::cout << now() << '\t' << "Delete order: " << order << std::endl;

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteOrder 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }

    void onExecuteOrder(const Order& order, uint64_t price, uint64_t quantity) override
    {
        ++_updates; ++_execute_orders;

        // Log Executed Order
     	std::cout << now() << '\t' << "Execute order: " << order << " with price " << price << " and quantity " << quantity << std::endl;
 
     	// std::string csv;
        // csv.append(OrderCSVHeader + CSV_SEP + "Price" + CSV_SEP + "Quantity" + CSV_EOL);
        // csv.append(ParseOrder(order) + CSV_SEP + std::to_string(price) + CSV_SEP + std::to_string(quantity) + CSV_EOL);

        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server ExecuteOrder 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { /* std::cout << "Error doing system call " << strerror(errno) << '\n'; */ }
    }
};

/* ############################################################################################################################################# */

/* Symbols */

void AddSymbol(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add symbol (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        char name[8];
        std::string sname = match[2];
        std::memcpy(name, sname.data(), std::min(sname.size(), sizeof(name)));

        Symbol symbol(id, name);

        ErrorCode result = market.AddSymbol(symbol);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add symbol' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add symbol' command: " + command);
}

void DeleteSymbol(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^delete symbol (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        ErrorCode result = market.DeleteSymbol(id);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'delete symbol' command: " << result << std::endl;

        return;
    }

    error("Invalid 'delete symbol' command: " + command);
}

/* ############################################################################################################################################# */

/* Books */

void AddOrderBook(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        char name[8];
        std::memset(name, 0, sizeof(name));

        Symbol symbol(id, name);

        ErrorCode result = market.AddOrderBook(symbol);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add book' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add book' command: " + command);
}

void DeleteOrderBook(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^delete book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        ErrorCode result = market.DeleteOrderBook(id);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'delete book' command: " << result << std::endl;

        return;
    }

    error("Invalid 'delete book' command: " + command);
}

// Get OrderBook in CSV format
void GetOrderBook(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^get book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);

        const OrderBook* order_book_ptr = market.GetOrderBook(id);

        if (order_book_ptr == NULL)
            std::cerr << now() << '\t' << "Failed 'get book' command" << std::endl;
        else
        {
            // Get CSV
            std::string csv = ParseOrderBook(market, order_book_ptr);

            // Send data back to client
            int connfd = CommandCtx::Get().Connection;
            int rdy = WriteSocketStream(connfd, &csv);
            if (rdy < 0)
                std::cerr << now() << '\t' << "failed sending response of 'get book' command" << std::endl;
        }
        
        return;
    }

    error("Invalid 'get book' command: " + command);
}

/* ############################################################################################################################################# */

/* Orders: Modify */

void ReduceOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^reduce order (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t quantity = std::stoi(match[2]);

        ErrorCode result = market.ReduceOrder(id, quantity);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'reduce order' command: " << result << std::endl;

        return;
    }

    error("Invalid 'reduce order' command: " + command);
}

void ModifyOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^modify order (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_price = std::stoi(match[2]);
        uint64_t new_quantity = std::stoi(match[3]);

        ErrorCode result = market.ModifyOrder(id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'modify order' command: " << result << std::endl;

        return;
    }

    error("Invalid 'modify order' command: " + command);
}

void MitigateOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^mitigate order (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_price = std::stoi(match[2]);
        uint64_t new_quantity = std::stoi(match[3]);

        ErrorCode result = market.MitigateOrder(id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'mitigate order' command: " << result << std::endl;

        return;
    }

    error("Invalid 'mitigate order' command: " + command);
}

void ReplaceOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^replace order (\\d+) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_id = std::stoi(match[2]);
        uint64_t new_price = std::stoi(match[3]);
        uint64_t new_quantity = std::stoi(match[4]);

        ErrorCode result = market.ReplaceOrder(id, new_id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'replace order' command: " << result << std::endl;

        return;
    }

    error("Invalid 'replace order' command: " + command);
}

void DeleteOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^delete order (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);

        ErrorCode result = market.DeleteOrder(id);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'delete order' command: " << result << std::endl;

        return;
    }

    error("Invalid 'delete order' command: " + command);
}

/* ############################################################################################################################################# */

/* Orders: Add */

void AddMarketOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add market (buy|sell) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyMarket(id, symbol_id, quantity);
        else if (match[1] == "sell")
            order = Order::SellMarket(id, symbol_id, quantity);
        else
        {
            std::cerr << now() << '\t' << "Invalid market order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add market' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add market' command: " + command);
}

void AddSlippageMarketOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add slippage market (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        uint64_t slippage = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyMarket(id, symbol_id, quantity, slippage);
        else if (match[1] == "sell")
            order = Order::SellMarket(id, symbol_id, quantity, slippage);
        else
        {
            std::cerr << now() << '\t' << "Invalid market order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add slippage market' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add slippage market' command: " + command);
}

void AddLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add limit (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, symbol_id, price, quantity);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, symbol_id, price, quantity);
        else
        {
            std::cerr << now() << '\t' << "Invalid limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add limit' command: " + command);
}

void AddIOCLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add ioc limit (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, symbol_id, price, quantity, OrderTimeInForce::IOC);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, symbol_id, price, quantity, OrderTimeInForce::IOC);
        else
        {
            std::cerr << now() << '\t' << "Invalid limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add ioc limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add ioc limit' command: " + command);
}

void AddFOKLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add fok limit (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, symbol_id, price, quantity, OrderTimeInForce::FOK);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, symbol_id, price, quantity, OrderTimeInForce::FOK);
        else
        {
            std::cerr << now() << '\t' << "Invalid limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add fok limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add fok limit' command: " + command);
}

void AddAONLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add aon limit (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, symbol_id, price, quantity, OrderTimeInForce::AON);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, symbol_id, price, quantity, OrderTimeInForce::AON);
        else
        {
            std::cerr << now() << '\t' << "Invalid limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add aon limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add aon limit' command: " + command);
}

void AddStopOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add stop (buy|sell) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t stop_price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyStop(id, symbol_id, stop_price, quantity);
        else if (match[1] == "sell")
            order = Order::SellStop(id, symbol_id, stop_price, quantity);
        else
        {
            std::cerr << now() << '\t' << "Invalid stop order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add stop' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add stop' command: " + command);
}

void AddStopLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add stop-limit (buy|sell) (\\d+) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t stop_price = std::stoi(match[3]);
        uint64_t price = std::stoi(match[4]);
        uint64_t quantity = std::stoi(match[5]);

        Order order;
        if (match[1] == "buy")
            order = Order::BuyStopLimit(id, symbol_id, stop_price, price, quantity);
        else if (match[1] == "sell")
            order = Order::SellStopLimit(id, symbol_id, stop_price, price, quantity);
        else
        {
            std::cerr << now() << '\t' << "Invalid stop-limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add stop-limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add stop-limit' command: " + command);
}

void AddTrailingStopOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add trailing stop (buy|sell) (\\d+) (\\d+) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t stop_price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);
        int64_t trailing_distance = std::stoi(match[5]);
        int64_t trailing_step = std::stoi(match[6]);

        Order order;
        if (match[1] == "buy")
            order = Order::TrailingBuyStop(id, symbol_id, stop_price, quantity, trailing_distance, trailing_step);
        else if (match[1] == "sell")
            order = Order::TrailingSellStop(id, symbol_id, stop_price, quantity, trailing_distance, trailing_step);
        else
        {
            std::cerr << now() << '\t' << "Invalid stop order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add trailing stop' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add trailing stop' command: " + command);
}

void AddTrailingStopLimitOrder(MarketManager& market, const std::string& command)
{
    static std::regex pattern("^add trailing stop-limit (buy|sell) (\\d+) (\\d+) (\\d+) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = CommandCtx::Get().OrderId;
        uint32_t symbol_id = std::stoi(match[2]);
        uint64_t stop_price = std::stoi(match[3]);
        uint64_t price = std::stoi(match[4]);
        uint64_t quantity = std::stoi(match[5]);
        int64_t trailing_distance = std::stoi(match[6]);
        int64_t trailing_step = std::stoi(match[7]);

        Order order;
        if (match[1] == "buy")
            order = Order::TrailingBuyStopLimit(id, symbol_id, stop_price, price, quantity, trailing_distance, trailing_step);
        else if (match[1] == "sell")
            order = Order::TrailingSellStopLimit(id, symbol_id, stop_price, price, quantity, trailing_distance, trailing_step);
        else
        {
            std::cerr << now() << '\t' << "Invalid stop-limit order side: " << match[1] << std::endl;
            return;
        }

        ErrorCode result = market.AddOrder(order);
        if (result != ErrorCode::OK)
            std::cerr << now() << '\t' << "Failed 'add trailing stop-limit' command: " << result << std::endl;

        return;
    }

    error("Invalid 'add trailing stop-limit' command: " + command);
}

/* ############################################################################################################################################# */

/* Execute Command */

void Execute(MarketManager& market, const std::string& command)
{
    // Matching
    if (command == "enable matching") market.EnableMatching();
    else if (command == "disable matching") market.DisableMatching();
    // Symbols
    else if (command.find("add symbol") != std::string::npos) AddSymbol(market, command);
    else if (command.find("delete symbol") != std::string::npos) DeleteSymbol(market, command);
    // Books
    else if (command.find("add book") != std::string::npos) AddOrderBook(market, command);
    else if (command.find("delete book") != std::string::npos) DeleteOrderBook(market, command);
    else if (command.find("get book") != std::string::npos) GetOrderBook(market, command);
    // Orders: Modify
    else if (command.find("reduce order") != std::string::npos) ReduceOrder(market, command);
    else if (command.find("modify order") != std::string::npos) ModifyOrder(market, command);
    else if (command.find("mitigate order") != std::string::npos) MitigateOrder(market, command);
    else if (command.find("replace order") != std::string::npos) ReplaceOrder(market, command);
    else if (command.find("delete order") != std::string::npos) DeleteOrder(market, command);
    // Orders: Add
    else if (command.find("add ") != std::string::npos)
    {
        // Get Latest Order Id Registered
        auto ctx = CommandCtx::Get();
        auto handler_ptr = ctx.market_handler_ptr;
        ctx.OrderId = (*handler_ptr).lts_order_id() + 1; // Update Order Id field
        CommandCtx::Set(ctx); // Set new context

        if (command.find("add market") != std::string::npos) AddMarketOrder(market, command);
        else if (command.find("add slippage market") != std::string::npos) AddSlippageMarketOrder(market, command);
        else if (command.find("add limit") != std::string::npos) AddLimitOrder(market, command);
        else if (command.find("add ioc limit") != std::string::npos) AddIOCLimitOrder(market, command);
        else if (command.find("add fok limit") != std::string::npos) AddFOKLimitOrder(market, command);
        else if (command.find("add aon limit") != std::string::npos) AddAONLimitOrder(market, command);
        else if (command.find("add stop-limit") != std::string::npos) AddStopLimitOrder(market, command);
        else if (command.find("add stop") != std::string::npos) AddStopOrder(market, command);
        else if (command.find("add trailing stop-limit") != std::string::npos) AddTrailingStopLimitOrder(market, command);
        else if (command.find("add trailing stop") != std::string::npos) AddTrailingStopOrder(market, command);
    }
}

/* ############################################################################################################################################# */

int main(int argc, char** argv)
{
    /* CLI */

    // Parse input args 
    auto parser = optparse::OptionParser().version(VERSION);
    parser.add_option("-n", "--name").dest("name").help("Daemon name");
    parser.add_option("-p", "--path").dest("path").help("Daemon root folder");
    auto options = parser.parse_args(argc, argv);

    // Print help
    if (options.get("help"))
        { parser.print_help(); return 0; }

    // Check for inputs
    if (!options.is_set("name")) CliError("no name provided for daemon");
    if (!options.is_set("path")) CliError("no root folder provided");

    // Check root folder
    Path root = Path(options.get("path"));
    if (!root.IsDirectory()) CliError("invalid path provided as root folder");
    else root = root.absolute();
 
    // Set process name
    std::string name;
    name.append(options.get("name"));

    // Set filepaths
    Path log_path = root / Path(name + ".log");
    Path err_path = root / Path(name + ".err");
    Path status_path = root / Path(name + ".status");
    Path socket_path = root / Path(name + ".sock");

    /* ############################################################################################################################################# */

    /* SETUP */

    // Setup status file
    const std::string status_text = File::ReadAllText(status_path);
    bool status = socket_path.IsExists() || (status_text != STATUS_GSTOP);
    
    bool socket_in_use = true;    
    int rdy = ConnectUnixSocket(socket_path.string().c_str());
    if (rdy < 0) socket_in_use = false;
    else close(rdy);
        
    if (socket_in_use && (status_text == STATUS_RUN)) CliError("SOCKET_IN_USE");
    if (!socket_in_use && status)
    {
        File::WriteAllText(status_path, STATUS_ABEND);
        Path::Remove(socket_path);
    }

    // Change process to Daemon
    Daemonize(root.string().c_str());

    log("switched to daemon");

    // Set Stdout and Stderr to log and err files
    if (freopen(log_path.string().c_str(), "a+", stdout) == NULL)
        { error("error opening log file"); exit(1); };
    
    if (freopen(err_path.string().c_str(), "a+", stderr) == NULL)
        { error("error opening err file"); exit(1); };

    // Create socket
    int sockfd = UnixSocket(socket_path.string().c_str(), MAX_CLIENTS);
    if (sockfd == -1) { error("error creating socket"); exit(1); };
    if (sockfd == -2) { error("error binding socket"); exit(1); };
    if (sockfd == -3) { error("error listening on socket"); exit(1); };

    log("listening...");

    // Initiate MarketManager
    MyMarketHandler market_handler;
    MarketManager market(market_handler);
    market.EnableMatching();

    // Update status file
    File::WriteAllText(status_path, STATUS_RUN);

    /* ############################################################################################################################################# */

    /* LOOP */

    std::vector<int> connections = {sockfd}; // Connection vector
    auto it = connections.begin(); // Connection iterator
    int newfd, connfd;
    bool enable = true; // Run condition
    std::string message;

    // Handle connections
    while (enable)
    {
        try
        {
            // Wait for new connection or message
            rdy = SelectVector(&connections);
            if (rdy < 0) error("error waiting for connections");

            // Accept new connection (if available)
            newfd = AcceptConnection(sockfd);
            if (newfd < 0) error("error accepting connetion");
            if (newfd > 0) connections.push_back(newfd); // Add connection to vector

            // Read messages from all clients (if available)
            it = ++connections.begin();
            while ((it < connections.end()) && enable)
            {
                connfd = *it;
                CommandCtx::Clear(); // Clear Current Context
                rdy = ReadSocketStream(connfd, &message); // Read message
                if (rdy < 0) // Connection closed
                {
                    close(connfd);
                    it = connections.erase(it); // Remove connection from vector
                }
                if (rdy > 0) // Message recieved
                {
                    if (message == "exit") enable = false;

                    // Set Context
                    CommandCtx::Context ctx = {
                        Connection: connfd,
                        Command: message,
                        market_ptr: &market,
                        market_handler_ptr: &market_handler
                    };
                    CommandCtx::Set(ctx);

                    // Call Matching engine
                    Execute(market, message);
                }
                ++it; // Update iterator
            }
        }
        // Catch any error
        catch (std::exception const& e)
        {
            error(e.what());
            enable = false;
        }
        catch (...)
        {
            error("unknown error occurred");
            enable = false;
        }
    }

    /* ############################################################################################################################################# */

    /* SHUTDOWN */

    // Graceful shutdown
    CloseVector(&connections); // Close sockets
    unlink(socket_path.string().c_str());
    market.DisableMatching();

    // Update status file
    File::WriteAllText(status_path, STATUS_GSTOP);

    log("graceful shutdown");

    return 0;
}

/* ############################################################################################################################################# */
