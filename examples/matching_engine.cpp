/* a server in the unix domain */

#include "trader/matching/market_manager.h"
#include <OptionParser.h>

#include "system/stream.h"
#include "filesystem/file.h"

#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include <sys/un.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <string>
#include <vector>
#include <regex>
#include <iostream>
#include <iomanip>

extern "C" {
    #include <sqlite3/sqlite3.h>
}

using namespace CppCommon;
using namespace CppTrader::Matching;

/* ############################################################################################################################################# */

/* Preprocessed */

#define VERSION "2.2.1.5" // Program version

#define MSG_SIZE 256 // Buffer size for messages on socket stream (bytes)
#define MSG_SIZE_SMALL 64 // Buffer size for small messages on socket stream (bytes)
#define MSG_SIZE_LARGE 1024 // Buffer size for large messages on socket stream (bytes)

#define MAX_CLIENTS 64 // Max number of simultaneous clients connected to socket

/* ############################################################################################################################################# */

/* Constants */

const uint64_t SYMBOL_ID = 1; // Symbol Id for the Order Book

const std::string STATUS_RUN = "RUNNING"; // Daemon status (RUN)
const std::string STATUS_GSTOP = "GRACEFULLY_STOPPED"; // Daemon status (GSTOP)
const std::string STATUS_ABEND = "ABEND"; // Daemon status (ABEND)

const std::string EMPTY_STR = ""; // Empty String
const std::string NULL_STR = "NULL"; // Null String

const std::string CSV_SEP = ","; // CSV separator
const std::string CSV_EOL = "\n"; // CSV end of line

// Enums Mapping
const char* LEVEL_TYPES[] = {"BID","ASK"};
const char* ORDER_SIDES[] = {"BUY","SELL"};
const char* ORDER_TYPES[] = {"MARKET","LIMIT","STOP","STOP_LIMIT","TRAILING_STOP","TRAILING_STOP_LIMIT"};
const char* ORDER_TIFS[] = {"GTC","IOC","FOK","AON"};

// Order CSV Header
const std::string CSV_HEADER_FOR_ORDER = (
    "Id" + CSV_SEP +
    "SymbolId" + CSV_SEP +
    "Type" + CSV_SEP +
    "Side" + CSV_SEP +
    "Price" + CSV_SEP +
    "StopPrice" + CSV_SEP +
    "Quantity" + CSV_SEP +
    "TimeInForce" + CSV_SEP +
    "MaxVisibleQuantity" + CSV_SEP +
    "Slippage" + CSV_SEP +
    "TrailingDistance" + CSV_SEP +
    "TrailingStep" + CSV_SEP +
    "ExecutedQuantity" + CSV_SEP +
    "LeavesQuantity"
);

// Order Book CSV Header
const std::string CSV_HEADER_FOR_BOOK = (
    "Group" + CSV_SEP +
    "LevelType" + CSV_SEP +
    "LevelPrice"
);

// Create Orders Table Query
const std::string QUERY_CREATE_TABLE_ORDERS = (EMPTY_STR +
    "CREATE TABLE IF NOT EXISTS orders (" +
        "Id INT PRIMARY KEY NOT NULL" + CSV_SEP +
        "SymbolId TINYINT NOT NULL" + CSV_SEP +
        "Type TINYINT NOT NULL" + CSV_SEP +
        "Side TINYINT NOT NULL" + CSV_SEP +
        "Price INT NOT NULL" + CSV_SEP +
        "StopPrice INT NOT NULL" + CSV_SEP +
        "Quantity INT NOT NULL" + CSV_SEP +
        "TimeInForce TINYINT NOT NULL" + CSV_SEP +
        "MaxVisibleQuantity INT" + CSV_SEP +
        "Slippage INT" + CSV_SEP +
        "TrailingDistance INT" + CSV_SEP +
        "TrailingStep INT" + CSV_SEP +
        "ExecutedQuantity INT NOT NULL" + CSV_SEP +
        "LeavesQuantity INT NOT NULL" + CSV_SEP +
        "Info CHAR(300) NOT NULL" +
    ")"
);

// Create Latest Table Query
const std::string QUERY_CREATE_TABLE_LATEST = (
    "CREATE TABLE IF NOT EXISTS latest (Id INT NOT NULL)"
);

// Populate Latest Table Query
const std::string QUERY_INSERT_INTO_LATEST = (EMPTY_STR +
    "INSERT INTO latest (Id) SELECT 0 WHERE NOT EXISTS (" +
        "SELECT * FROM latest" +
    ")"
);

/* ############################################################################################################################################# */

/* Helper Functions */

// Convert Objects to string via operator<<
template <typename T>
inline std::string sstos(const T* input)
{
    std::stringstream ss;
    ss << (*input);
    return ss.str();
}

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

/* Command Context */

class MyMarketHandler;

namespace Context {

    struct Connection
    {
        sqlite3* sqlite_ptr = NULL; // Connection to SQLite Database
        int sockfd = 0; // File Descriptor for current Connection
    };

    struct Order
    {
        int id = 0; // Order Id
        std::string info = EMPTY_STR; // Order Info
    };

    struct Command
    {
        std::string input = EMPTY_STR; // Command Input
        std::string response = EMPTY_STR; // Command Response
        int response_size = MSG_SIZE_SMALL; // Response Size
    };

    struct Market
    {
        MarketManager* market_ptr = NULL; // Pointer to Market Manager
        MyMarketHandler* handler_ptr = NULL; // Pointer to Market Handler
        std::vector<int> changes = {}; // List of changed orders
        std::map<int, std::string> info = {}; // Info

        // Methods
        std::vector<int>::iterator ChangesInsert(int id);
        std::map<int, std::string>::iterator InfoInsert(int id, std::string text);
        std::map<int, std::string>::iterator InfoErase(int id);
        std::map<int, std::string>::iterator InfoFindID(std::string text);
    };

    /* ############################################################################################################################################# */

    // Add entry to Changes list
    std::vector<int>::iterator Market::ChangesInsert(int id)
    {
        std::vector<int>::iterator it = find(changes.begin(), changes.end(), id);
        if (it == changes.end()) changes.push_back(id);
        return find(changes.begin(), changes.end(), id);
    }

    // Add entry to Info map
    std::map<int, std::string>::iterator Market::InfoInsert(int id, std::string text)
        { return info.insert(std::make_pair(id, text)).first; }
    
    // Delete entry from Info map
    std::map<int, std::string>::iterator Market::InfoErase(int id)
        { return info.erase(info.find(id)); }

    // Get OrderID from Info text
    std::map<int, std::string>::iterator Market::InfoFindID(std::string text)
    {
        return std::find_if(
            info.begin(),
            info.end(),
            [&](const std::pair<int, std::string> &pair)
                { return pair.second == text; }
        );
    }

    /* ############################################################################################################################################# */

    // Context struct
    struct Ctx
    {
        bool enable = false; // Enable operation with context
        Connection connection = Connection();
        Market market = Market();
        Order order = Order();
        Command command = Command();
        
    };

    // Get Context
    Ctx* Get()
    {
        static Ctx ctx;
        return &ctx;
    }

    // Set Context
    inline void Set(Ctx& value)
    {
        Ctx* ctx = Get();
        (*ctx) = value;
    }

    // Clear Context
    inline void Clear()
    {
        Ctx ctx;
        Set(ctx);
    }
}

/* ############################################################################################################################################# */

void chld_handler(int signum)
{
    signal(SIGCHLD, chld_handler);
	int wstat;
    wait3(&wstat, WNOHANG, NULL);
}

// Change current process to Daemon
void Daemonize(const char* root)
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
    signal(SIGCHLD, chld_handler);
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

    // Close all open file descriptors
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
inline int SelectVector(std::vector<int>* fdvec)
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
inline int CloseVector(std::vector<int>* fdvec)
{
    int code = 0;
    for (auto it = (*fdvec).rbegin(); it != (*fdvec).rend(); ++it)
        { code += close(*it); } // Do reverse iteration on the vector
    if (code < 0) return -1;
    else return 0; // Return close() code
}

/* ############################################################################################################################################# */

// Read stream on Unix socket (non-blocking)
inline int ReadSocketStream(int sockfd, int size, std::string* dest)
{
    // Always clear string
    (*dest).clear();

    // Check if data is available
    int rdy = SelectReadNonBlocking(sockfd);
    if (rdy <= 0) return rdy;

    // Read stream to string
    char buffer[MSG_SIZE_LARGE];
    if (read(sockfd, buffer, size) <= 0) return -1;
    (*dest).append(buffer, strcspn(buffer, "\0")); // buffer is copied to string until the first \0 char is found

    return 1;
}

// Write to stream on Unix socket
inline int WriteSocketStream(int sockfd, int size, std::string* data)
{
    // Check if write is available
    int rdy = SelectWrite(sockfd);
    if (rdy <= 0) return rdy;

    // Write string to stream
    char buffer[MSG_SIZE_LARGE];
    strcpy(buffer, (*data).c_str() + '\0'); // string is copied to buffer with a trailing \0 char 
    if (write(sockfd, buffer, size) <= 0) return -1;

    return 1;
}

/* ############################################################################################################################################# */

// Accept connection on Unix socket (non-blocking)
inline int AcceptConnection(int sockfd)
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

// Parse Order to CSV
inline std::string ParseOrder(const Order& order)
{
    auto ctx = Context::Get();

    // First get info/transaction ID variable
    std::string info;
    std::map<int, std::string>::iterator info_it = (*ctx).market.info.find(order.Id);
    if (info_it != (*ctx).market.info.end()) { info = info_it->second; }
    else error("Error at 'ParseOrder': could not find 'info' for order: " + sstos(&order));
    
    info = std::regex_replace(info, std::regex("\""), "\\\"");

    return (
        std::to_string(order.Id) + CSV_SEP +
        std::to_string(order.SymbolId) + CSV_SEP +
        ORDER_TYPES[(int)order.Type] + CSV_SEP +
        ORDER_SIDES[(int)order.Side] + CSV_SEP +
        std::to_string(order.Price) + CSV_SEP +
        std::to_string(order.StopPrice) + CSV_SEP +
        std::to_string(order.Quantity) + CSV_SEP +
        ORDER_TIFS[(int)order.TimeInForce] + CSV_SEP +
        ((order.IsHidden() || order.IsIceberg())
            ? std::to_string(order.MaxVisibleQuantity)
            : NULL_STR
        ) + CSV_SEP +
        ((order.IsSlippage())
            ? std::to_string(order.Slippage)
            : NULL_STR
        ) + CSV_SEP +
        ((order.IsTrailingStop() || order.IsTrailingStopLimit())
            ? std::to_string(order.TrailingDistance) + CSV_SEP +
                std::to_string(order.TrailingStep)
            : NULL_STR + CSV_SEP + NULL_STR
        ) + CSV_SEP +
        std::to_string(order.ExecutedQuantity) + CSV_SEP +
        std::to_string(order.LeavesQuantity) + CSV_SEP +
        "\"" + info + "\""
    );
}

/* ############################################################################################################################################# */

// Parse OrderBook::Levels to CSV
std::string ParseOrderBookLevels(MarketManager* market, OrderBook::Levels levels, const char* group)
{
    std::string csv;
    std::string level_props;
    
    // Loop over Levels orders
    for (LevelNode level : levels)
    {
        // Get Level properties
        level_props = (
            group + CSV_SEP +
            LEVEL_TYPES[(int)level.Type] + CSV_SEP +
            std::to_string(level.Price)
        );
        for (OrderNode node : level.OrderList)
        {
            const Order* order = (*market).GetOrder(node.Id);
            csv.append(
                level_props + CSV_SEP + // Insert level properties
                ParseOrder(*order) + CSV_EOL // Insert Order properties
            );
        }
    }

    return csv;
}

/* ############################################################################################################################################# */

// Parse OrderBook to CSV
std::string ParseOrderBook(MarketManager* market, const OrderBook* order_book_ptr)
{
    // Insert header
    std::string csv;
    csv.append(
        CSV_HEADER_FOR_BOOK + CSV_SEP +
        CSV_HEADER_FOR_ORDER + CSV_SEP +
        "Info" + CSV_EOL
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

// Generate Query to insert Order into SQLite
inline std::string InsertQueryFromOrder(const Order& order, std::string info)
{
    return (
        "INSERT INTO orders (" + CSV_HEADER_FOR_ORDER + ",Info) VALUES (" +
            std::to_string((int)order.Id) + CSV_SEP +
            std::to_string((int)SYMBOL_ID) + CSV_SEP +
            std::to_string((int)order.Type) + CSV_SEP +
            std::to_string((int)order.Side) + CSV_SEP +
            std::to_string((int)order.Price) + CSV_SEP +
            std::to_string((int)order.StopPrice) + CSV_SEP +
            std::to_string((int)order.Quantity) + CSV_SEP +
            std::to_string((int)order.TimeInForce) + CSV_SEP +
            std::to_string((int)order.MaxVisibleQuantity) + CSV_SEP +
            std::to_string((int)order.Slippage) + CSV_SEP +
            std::to_string((int)order.TrailingDistance) + CSV_SEP +
            std::to_string((int)order.TrailingStep) + CSV_SEP +
            std::to_string((int)order.ExecutedQuantity) + CSV_SEP +
            std::to_string((int)order.LeavesQuantity) + CSV_SEP +
            "'" + info + "'" +
        ")"
    );
}

// Generate Query to update Order into SQLite
inline std::string UpdateQueryFromOrder(const Order& order)
{
    return (EMPTY_STR +
        "UPDATE orders SET " +
            "Type=" + std::to_string((int)order.Type) + CSV_SEP +
            "Side=" + std::to_string((int)order.Side) + CSV_SEP +
            "Price=" + std::to_string((int)order.Price) + CSV_SEP +
            "StopPrice=" + std::to_string((int)order.StopPrice) + CSV_SEP +
            "Quantity=" + std::to_string((int)order.Quantity) + CSV_SEP +
            "TimeInForce=" + std::to_string((int)order.TimeInForce) + CSV_SEP +
            "MaxVisibleQuantity=" + std::to_string((int)order.MaxVisibleQuantity) + CSV_SEP +
            "Slippage=" + std::to_string((int)order.Slippage) + CSV_SEP +
            "TrailingDistance=" + std::to_string((int)order.TrailingDistance) + CSV_SEP +
            "TrailingStep=" + std::to_string((int)order.TrailingStep) + CSV_SEP +
            "ExecutedQuantity=" + std::to_string((int)order.ExecutedQuantity) + CSV_SEP +
            "LeavesQuantity=" + std::to_string((int)order.LeavesQuantity) +
        " WHERE " +
            "Id=" + std::to_string((int)order.Id)
    );
}

// Generate new Order from result of Query
inline Order OrderFromQuery(sqlite3_stmt* row)
{
    Order order = Order(
        sqlite3_column_int(row, 0), // Id
        SYMBOL_ID, // Symbol
        OrderType(sqlite3_column_int(row, 2)), // Type
        OrderSide(sqlite3_column_int(row, 3)), // Side
        sqlite3_column_int(row, 4), // Price
        sqlite3_column_int(row, 5), // Stop Price
        sqlite3_column_int(row, 6), // Quantity
        OrderTimeInForce(sqlite3_column_int(row, 7)), // Time In Force
        sqlite3_column_int(row, 8), // Max Visible Quantity
        sqlite3_column_int(row, 9), // Slippage
        sqlite3_column_int(row, 10), // Trailing Distance
        sqlite3_column_int(row, 11) // Trailing Step
    );
    order.ExecutedQuantity = sqlite3_column_int(row, 12); // Executed Quantity
    order.LeavesQuantity = sqlite3_column_int(row, 13); // Leaves Quantity

    return order;
}

/* ############################################################################################################################################# */

// Populate SQLite Database
void PopulateDatabase(sqlite3* db)
{
    // Create Tables in SQLite
    static const std::string query = (
        QUERY_CREATE_TABLE_LATEST + "; " +
        QUERY_INSERT_INTO_LATEST + "; " +
        QUERY_CREATE_TABLE_ORDERS + ";"
    );
    char* err;
    int rdy = sqlite3_exec(db, query.c_str(), NULL, NULL, &err);
    if (rdy != SQLITE_OK)
    { error("sqlite error(1): " + sstos(&err)); exit(1); };
}

// Get Latest Id from Database
int GetLatestId(sqlite3* db) 
{
    sqlite3_stmt* result;
    static const char* query = "SELECT * FROM latest";

    // Prepare query
    int rdy = sqlite3_prepare(db, query, -1, &result, NULL);
    if (rdy != SQLITE_OK)
    {
        const char* err = sqlite3_errmsg(db);
        error("sqlite error(2): " + sstos(&err));
        exit(1);
    };

    // Get Latest value
    int lts = 0;
    while (sqlite3_step(result) == SQLITE_ROW)
    { lts = sqlite3_column_int(result, 0); };
    return lts;
}

// Populate Local Order Book from SQLite
void PopulateBook(MarketManager* market, sqlite3* db, const char* name)
{
    // Add Symbol
    Symbol symbol(SYMBOL_ID, name);
    ErrorCode err = (*market).AddSymbol(symbol);
    if (err != ErrorCode::OK)
    { error("Failed AddSymbol: " + sstos(&err)); exit(1); };

    // Add Book
    err = (*market).AddOrderBook(symbol);
    if (err != ErrorCode::OK)
    { error("Failed AddOrderBook: " + sstos(&err)); exit(1); };

    // Prepare query
    sqlite3_stmt* result;
    const char* query = "SELECT * FROM orders";
    int rdy = sqlite3_prepare(db, query, -1, &result, NULL);
    if (rdy != SQLITE_OK)
    {
        const char* _err = sqlite3_errmsg(db);
        error("sqlite error(3): " + sstos(&_err));
        exit(1);
    };

    auto ctx = Context::Get();

    // Get Orders
    while (sqlite3_step(result) == SQLITE_ROW)
    {
        // Add Order
        Order order = OrderFromQuery(result);
        const unsigned char* info = sqlite3_column_text(result, 14);
        (*ctx).order.info = std::string((char const*)info);
        (*ctx).order.id = (int)order.Id;
        err = (*market).AddOrder(order);
        if (err != ErrorCode::OK)
        { error("Failed AddOrder: " + sstos(&err)); exit(1); };
    };

    (*ctx).order = Context::Order();
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
    MyMarketHandler(int lts)
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
          _lts_order_id(lts)
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

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Add Symbol
        log("Add symbol: " + sstos(&symbol));

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddSymbol 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onDeleteSymbol(const Symbol& symbol) override
    {
        ++_updates; --_symbols;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Delete Symbol
        log("Delete symbol: " + sstos(&symbol)); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteSymbol 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onAddOrderBook(const OrderBook& order_book) override
    {
        ++_updates; ++_order_books; _max_order_books = std::max(_order_books, _max_order_books);

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Add Order Book
        log("Add order book: " + sstos(&order_book)); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onUpdateOrderBook(const OrderBook& order_book, bool top) override
    {
        _max_order_book_levels = std::max(std::max(order_book.bids().size(), order_book.asks().size()), _max_order_book_levels);

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Update Order Book
        log("Update order book: " + sstos(&order_book) + (top ? " - Top of the book!" : "")); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server UpdateOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onDeleteOrderBook(const OrderBook& order_book) override
    {
        ++_updates; --_order_books;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Delete Order Book
        log("Delete order book: " + sstos(&order_book)); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteOrderBook 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onAddLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Add Level
        log("Add level: " + sstos(&level) + (top ? " - Top of the book!" : "")); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server AddLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onUpdateLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates; _max_order_book_orders = std::max(level.Orders, _max_order_book_orders);

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Update Level
        log("Update level: " + sstos(&level) + (top ? " - Top of the book!" : "")); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server UpdateLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onDeleteLevel(const OrderBook& order_book, const Level& level, bool top) override
    {
        ++_updates;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Log Delete Leve
        log("Delete level: " + sstos(&level) + (top ? " - Top of the book!" : "")); 

        /*
        // Send to server
        std::string cmd = "/home/sysop/books/scripts/server DeleteLevel 123";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + strerror(errno)); }
        */
    }

    void onAddOrder(const Order& order) override
    {
        ++_updates; ++_orders; _max_orders = std::max(_orders, _max_orders); ++_add_orders;

        // Update Unique Id Record
        _lts_order_id = std::max((size_t)order.Id, _lts_order_id);

        auto ctx = Context::Get();

        // Check if Id is Sync
        if ((int)order.Id != (*ctx).order.id)
        {
            error("Error at 'onAddOrder' callback: id out of sync");
            return;
        }

        // Store Order Info
        (*ctx).market.InfoInsert(order.Id, (*ctx).order.info);

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        bool success = true;

        std::string id = std::to_string((int)order.Id);

        sqlite3* db = (*ctx).connection.sqlite_ptr;
        char* err;

        // Add order to SQLite
        const std::string query = (EMPTY_STR +
            "BEGIN; " +
            "UPDATE latest SET Id=" + id + "; " +
            InsertQueryFromOrder(order, (*ctx).order.info) + "; " +
            "COMMIT;"
        );
        int rdy = sqlite3_exec(db, query.c_str(), NULL, NULL, &err);
        if (rdy != SQLITE_OK)
        {
            error("sqlite error(4): " + sstos(&err));
            success = false;
        };

        // Log Add Order
        log("Add order: " + sstos(&order));

        /*
        // Send to server with system()
        std::string cmd = "/home/sysop/books/BTC_TUSD/server AddOrder " + id + ":" + (*ctx).order.info;
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0) { error("Error doing system call " + std::string(strerror(errno))); }
        */

        // Set response to client
        if (success) (*ctx).command.response = id;
    }

    /* Update orders when half filled */
    void onUpdateOrder(const Order& order) override
    {
        ++_updates; ++_update_orders;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        sqlite3* db = (*ctx).connection.sqlite_ptr;
        char* err;

        const std::string query = UpdateQueryFromOrder(order);
        int rdy = sqlite3_exec(db, query.c_str(), NULL, NULL, &err);
        if (rdy != SQLITE_OK)
        { error("sqlite error(6): " + sstos(&err)); };

        // Get info/transaction ID
        std::string info;
        std::map<int, std::string>::iterator info_it = (*ctx).market.info.find(order.Id);
        if (info_it != (*ctx).market.info.end()) { info = info_it->second; }
        else error("Error at 'onUpdateOrder' callback: could not find 'info' for order: " + sstos(&order));

        /*
        // *** Send to server with system()
        // NOT NEEDED BECAUSE I CAN CALCULATE PARTIAL TRADES
        // BUT CAN BE USED IN FUTURE TO CROSS CHECK THESE PARTIAL CALCS
        std::string cmd = "/home/sysop/books/BTC_TUSD/execute_processor UpdateOrder '" + sstos(&order) + ":" + info + "' > execute_processor.txt 2>&1 &";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0 && iCallResult != -1) { error("Error doing system call (AC862): " + std::string(strerror(errno)) + " " + std::to_string(iCallResult)); }
        // *** Send to server end
        */
    }

    void onDeleteOrder(const Order& order) override
    {
        ++_updates; --_orders; ++_delete_orders;

        auto ctx = Context::Get();

        // First get info/transaction ID variable
        std::string info;
        std::map<int, std::string>::iterator info_it = (*ctx).market.info.find(order.Id);
        if (info_it != (*ctx).market.info.end()) { info = info_it->second; }
        else error("Error at 'onDeleteOrder' callback: could not find 'info' for order: " + sstos(&order));

        // Delete Order Info
        (*ctx).market.InfoErase(order.Id);

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        bool success = true;

        std::string id = std::to_string((int)order.Id);

        sqlite3* db = (*ctx).connection.sqlite_ptr;
        char* err;

        // Delete order from SQLite
        const std::string query = (
            "DELETE FROM orders WHERE Id=" + id
        );
        int rdy = sqlite3_exec(db, query.c_str(), NULL, NULL, &err);
        if (rdy != SQLITE_OK)
        {
            error("sqlite error(5): " + sstos(&err));
            success = false;
        };

        // Log Deleted Order
        log("Delete order: " + sstos(&order) + " and info " + info);

        // Check if order was deleted by user
        std::string command = (*ctx).command.input;
        bool user_cmd = command.find("delete order") != std::string::npos;

        // Execute child callbacks
        if (user_cmd) onDeleteOrderCommand(order, success);
        else onDeleteExecutedOrder(order, id, info);
    }

    void onDeleteOrderCommand(const Order& order, bool success)
    {
        auto ctx = Context::Get();
        
        // Set response to client
        if (success) (*ctx).command.response = "OK";
    }

    void onDeleteExecutedOrder(const Order& order, std::string id, std::string info)
    {
        /*
        // *** Send to server
        std::string cmd = "/home/sysop/books/BTC_TUSD/execute_processor DeleteOrder '" + id + ":" + sstos(&order) + ":" + info + "' > execute_processor.txt 2>&1 &";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0 && iCallResult != -1) { error("Error doing system call (BB332): " + std::string(strerror(errno)) + " " + std::to_string(iCallResult)); }
        // *** Send to server end
        */
    }

    void onExecuteOrder(const Order& order, uint64_t price, uint64_t quantity) override
    {
        ++_updates; ++_execute_orders;

        auto ctx = Context::Get();

        // Check if operation is enabled
        if (!(*ctx).enable) return;

        // Add Order to changes if not already added
        (*ctx).market.ChangesInsert(order.Id);

        // First get info/transaction ID variable
        std::string info;
        std::map<int, std::string>::iterator info_it = (*ctx).market.info.find(order.Id);
        if (info_it != (*ctx).market.info.end()) { info = info_it->second; }
        else error("Error at 'onExecuteOrder' callback: could not find 'info' for order: " + sstos(&order));
        
        // Log Executed Order
     	log("Execute order: " + sstos(&order) + " with price " + sstos(&price) + " and quantity " + sstos(&quantity) + " and info " + info);
        
        /*
        // *** Send to server with system()
        std::string cmd = "/home/sysop/books/BTC_TUSD/execute_processor ExecuteOrder '" + sstos(&price) + "@" + sstos(&quantity) + ":" + sstos(&order) + ":" + info + "' > execute_processor.txt 2>&1 &";
        int iCallResult = system(cmd.c_str());
        if (iCallResult < 0 && iCallResult != -1) { error("Error doing system call (AA832): " + std::string(strerror(errno)) + " " + std::to_string(iCallResult)); }
        // *** Send to server end
        */
    }
};

/* ############################################################################################################################################# */

/* Symbols */

void AddSymbol(MarketManager* market, const std::string& command)
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

        ErrorCode result = (*market).AddSymbol(symbol);
        if (result != ErrorCode::OK)
            error("Failed 'add symbol' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add symbol' command: " + command);
}

void DeleteSymbol(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^delete symbol (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        ErrorCode result = (*market).DeleteSymbol(id);
        if (result != ErrorCode::OK)
            error("Failed 'delete symbol' command: " + sstos(&result));

        return;
    }

    error("Invalid 'delete symbol' command: " + command);
}

/* ############################################################################################################################################# */

/* Books */

void AddOrderBook(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        char name[8];
        std::memset(name, 0, sizeof(name));

        Symbol symbol(id, name);

        ErrorCode result = (*market).AddOrderBook(symbol);
        if (result != ErrorCode::OK)
            error("Failed 'add book' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add book' command: " + command);
}

void DeleteOrderBook(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^delete book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);

        ErrorCode result = (*market).DeleteOrderBook(id);
        if (result != ErrorCode::OK)
            error("Failed 'delete book' command: " + sstos(&result));

        return;
    }

    error("Invalid 'delete book' command: " + command);
}

// Get OrderBook in CSV format
void GetOrderBook(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^get book (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t symbol_id = std::stoi(match[1]);
    
        const OrderBook* order_book_ptr = (*market).GetOrderBook(symbol_id);
        if (order_book_ptr == NULL)
            error("Failed 'get book' command: Book not found");
        else
        {
            // Get CSV
            std::string res = ParseOrderBook(market, order_book_ptr);

            // Set response to client
            auto ctx = Context::Get();
            (*ctx).command.response = res;
            (*ctx).command.response_size = MSG_SIZE_LARGE;
        }
        
        return;
    }

    error("Invalid 'get book' command: " + command);
}

/* ############################################################################################################################################# */

/* Orders: Modify */

void ReduceOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^reduce order (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t quantity = std::stoi(match[2]);

        ErrorCode result = (*market).ReduceOrder(id, quantity);
        if (result != ErrorCode::OK)
            error("Failed 'reduce order' command: " + sstos(&result));

        return;
    }

    error("Invalid 'reduce order' command: " + command);
}

void ModifyOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^modify order (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_price = std::stoi(match[2]);
        uint64_t new_quantity = std::stoi(match[3]);

        ErrorCode result = (*market).ModifyOrder(id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            error("Failed 'modify order' command: " + sstos(&result));

        return;
    }

    error("Invalid 'modify order' command: " + command);
}

void MitigateOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^mitigate order (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_price = std::stoi(match[2]);
        uint64_t new_quantity = std::stoi(match[3]);

        ErrorCode result = (*market).MitigateOrder(id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            error("Failed 'mitigate order' command: " + sstos(&result));

        return;
    }

    error("Invalid 'mitigate order' command: " + command);
}

void ReplaceOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^replace order (\\d+) (\\d+) (\\d+) (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint64_t id = std::stoi(match[1]);
        uint64_t new_id = std::stoi(match[2]);
        uint64_t new_price = std::stoi(match[3]);
        uint64_t new_quantity = std::stoi(match[4]);

        ErrorCode result = (*market).ReplaceOrder(id, new_id, new_price, new_quantity);
        if (result != ErrorCode::OK)
            error("Failed 'replace order' command: " + sstos(&result));

        return;
    }

    error("Invalid 'replace order' command: " + command);
}

void DeleteOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^delete order (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        // Set default response
        (*ctx).command.response = "FAIL";
        
        std::string info = match[1];

        // Get Order ID from Info
        int id;
        std::map<int, std::string>::iterator info_it = (*ctx).market.InfoFindID(info);
        if (info_it != (*ctx).market.info.end()) { id = info_it->first; }
        else {
            error("Failed 'delete order' command: ORDER_NOT_FOUND");
            return;
        };
        
        ErrorCode result = (*market).DeleteOrder(id);
        if (result != ErrorCode::OK)
            error("Failed 'delete order' command: " + sstos(&result));

        return;
    }

    error("Invalid 'delete order' command: " + command);
}


// Get Order in CSV format
void GetOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^get order (\\d+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        uint32_t id = std::stoi(match[1]);
    
        const Order* order_ptr = (*market).GetOrder(id);
        if (order_ptr == NULL)
            error("Failed 'get order' command: Order not found");
        else
        {
            // Get CSV
            std::string res = (
                CSV_HEADER_FOR_ORDER + CSV_EOL +
                ParseOrder(*order_ptr) + CSV_EOL
            );

            // Set response to client
            auto ctx = Context::Get();
            (*ctx).command.response = res;
            (*ctx).command.response_size = MSG_SIZE;
        }
        
        return;
    }

    error("Invalid 'get order' command: " + command);
}

/* ############################################################################################################################################# */

/* Orders: Add */

void AddMarketOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add market (buy|sell) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t quantity = std::stoi(match[2]);
        (*ctx).order.info = match[3];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyMarket(id, SYMBOL_ID, quantity);
        else if (match[1] == "sell")
            order = Order::SellMarket(id, SYMBOL_ID, quantity);
        else
        {
            error("Invalid market order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add market' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add market' command: " + command);
}

void AddSlippageMarketOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add slippage market (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t quantity = std::stoi(match[2]);
        uint64_t slippage = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyMarket(id, SYMBOL_ID, quantity, slippage);
        else if (match[1] == "sell")
            order = Order::SellMarket(id, SYMBOL_ID, quantity, slippage);
        else
        {
            error("Invalid market order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add slippage market' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add slippage market' command: " + command);
}

void AddLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add limit (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, SYMBOL_ID, price, quantity);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, SYMBOL_ID, price, quantity);
        else
        {
            error("Invalid limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add limit' command: " + command);
}

void AddIOCLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add ioc limit (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::IOC);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::IOC);
        else
        {
            error("Invalid limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add ioc limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add ioc limit' command: " + command);
}

void AddFOKLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add fok limit (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::FOK);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::FOK);
        else
        {
            error("Invalid limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add fok limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add fok limit' command: " + command);
}

void AddAONLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add aon limit (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::AON);
        else if (match[1] == "sell")
            order = Order::SellLimit(id, SYMBOL_ID, price, quantity, OrderTimeInForce::AON);
        else
        {
            error("Invalid limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add aon limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add aon limit' command: " + command);
}

void AddStopOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add stop (buy|sell) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t stop_price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        (*ctx).order.info = match[4];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyStop(id, SYMBOL_ID, stop_price, quantity);
        else if (match[1] == "sell")
            order = Order::SellStop(id, SYMBOL_ID, stop_price, quantity);
        else
        {
            error("Invalid stop order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add stop' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add stop' command: " + command);
}

void AddStopLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add stop-limit (buy|sell) (\\d+) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t stop_price = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);
        (*ctx).order.info = match[5];

        Order order;
        if (match[1] == "buy")
            order = Order::BuyStopLimit(id, SYMBOL_ID, stop_price, price, quantity);
        else if (match[1] == "sell")
            order = Order::SellStopLimit(id, SYMBOL_ID, stop_price, price, quantity);
        else
        {
            error("Invalid stop-limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add stop-limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add stop-limit' command: " + command);
}

void AddTrailingStopOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add trailing stop (buy|sell) (\\d+) (\\d+) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t stop_price = std::stoi(match[2]);
        uint64_t quantity = std::stoi(match[3]);
        int64_t trailing_distance = std::stoi(match[4]);
        int64_t trailing_step = std::stoi(match[5]);
        (*ctx).order.info = match[6];

        Order order;
        if (match[1] == "buy")
            order = Order::TrailingBuyStop(id, SYMBOL_ID, stop_price, quantity, trailing_distance, trailing_step);
        else if (match[1] == "sell")
            order = Order::TrailingSellStop(id, SYMBOL_ID, stop_price, quantity, trailing_distance, trailing_step);
        else
        {
            error("Invalid stop order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add trailing stop' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add trailing stop' command: " + command);
}

void AddTrailingStopLimitOrder(MarketManager* market, const std::string& command)
{
    static std::regex pattern("^add trailing stop-limit (buy|sell) (\\d+) (\\d+) (\\d+) (\\d+) (\\d+) (.+)$");
    std::smatch match;

    if (std::regex_search(command, match, pattern))
    {
        auto ctx = Context::Get();

        uint64_t id = (*ctx).order.id;
        uint64_t stop_price = std::stoi(match[2]);
        uint64_t price = std::stoi(match[3]);
        uint64_t quantity = std::stoi(match[4]);
        int64_t trailing_distance = std::stoi(match[5]);
        int64_t trailing_step = std::stoi(match[6]);
        (*ctx).order.info = match[7];

        Order order;
        if (match[1] == "buy")
            order = Order::TrailingBuyStopLimit(id, SYMBOL_ID, stop_price, price, quantity, trailing_distance, trailing_step);
        else if (match[1] == "sell")
            order = Order::TrailingSellStopLimit(id, SYMBOL_ID, stop_price, price, quantity, trailing_distance, trailing_step);
        else
        {
            error("Invalid stop-limit order side: " + sstos(&match[1]));
            return;
        }

        ErrorCode result = (*market).AddOrder(order);
        if (result != ErrorCode::OK)
            error("Failed 'add trailing stop-limit' command: " + sstos(&result));

        return;
    }

    error("Invalid 'add trailing stop-limit' command: " + command);
}

/* ############################################################################################################################################# */

/* Execute Command */

void UpdateOrders()
{
    auto ctx = Context::Get();

    // Generate all update queries
    std::string updates;
    for (int id : (*ctx).market.changes)
    {
        const Order* order = (*(*ctx).market.market_ptr).GetOrder(id);
        if (order == NULL) continue;
        updates.append(UpdateQueryFromOrder(*order) + "; ");
    }

    if (!updates.empty())
    {
        sqlite3* db = (*ctx).connection.sqlite_ptr;
        char* err;

        const std::string query = "BEGIN; " + updates + "COMMIT;";
        int rdy = sqlite3_exec(db, query.c_str(), NULL, NULL, &err);
        if (rdy != SQLITE_OK)
        { error("sqlite error(7): " + sstos(&err)); };
    }

    // Clear changes
    (*ctx).market.changes.clear();
}

void Execute()
{
    // Get Context
    auto ctx = Context::Get();
    std::string command = (*ctx).command.input;
    MarketManager* market = (*ctx).market.market_ptr;

    // Exit
    if      (command == "exit") (*ctx).enable = false;
    // Matching
    else if (command == "enable matching") (*market).EnableMatching();
    else if (command == "disable matching") (*market).DisableMatching();
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
    else if (command.find("get order") != std::string::npos) GetOrder(market, command);
    // Prepare to Add Order
    else if (command.find("add ") != std::string::npos)
    {
        // Set default response
        (*ctx).command.response = "FAIL";

        // Set new Order Id
        (*ctx).order.id = (*(*ctx).market.handler_ptr).lts_order_id() + 1;

        // Orders: Add
        if      (command.find("add market") != std::string::npos) AddMarketOrder(market, command);
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

    // Update changed orders
    if (!(*ctx).market.changes.empty()) UpdateOrders();
}

/* ############################################################################################################################################# */

/* Send Response */

void SendResponseIncremental(int sockfd, int response_size, std::string response)
{
    // Calculate pages
    int content_size = response.size() + 15; // Add prefix characters
    int pages = (int)content_size / response_size;
    int rem = content_size % response_size;
    if (rem > 0) ++pages; // Add last page
    if ((content_size + pages) > (response_size * pages)) ++pages; // Add terminator characters

    std::stringstream ss_pages;
    ss_pages << std::setw(4) << std::setfill('0') << pages;
    response = "PAGES >> " + ss_pages.str() + '\n' + response; // Add prefix

    // Send multiple responses
    std::string::iterator it = response.begin();
    std::string::iterator it_end = it;
    while (true) {
        if (it >= response.end()) break;
        if (it_end >= response.end()) break;

        it_end = it + response_size - 1; // End of page
        if (it_end > response.end()) it_end = response.end();

        std::string page(it, it_end); // Get substring

        int rdy = WriteSocketStream(sockfd, response_size, &page);
        if (rdy < 0) error("Failed sending response to client");
        
        it += response_size - 1; // Go to next page
    };
}

void SendResponse()
{
    auto ctx = Context::Get();

    // Send response to client
    int sockfd = (*ctx).connection.sockfd;
    std::string response = (*ctx).command.response;
    int response_size = (*ctx).command.response_size;
    int content_size = response.size() + 1;

    // Send multiple responses
    if (content_size > response_size) {
        SendResponseIncremental(sockfd, response_size, response);
    } else {
        // Send single response
        int rdy = WriteSocketStream(sockfd, response_size, &response);
        if (rdy < 0) error("Failed sending response to client");
    };
}

/* ############################################################################################################################################# */

int main(int argc, char** argv)
{
    /* CLI */

    // Parse input args
    optparse::OptionParser parser = optparse::OptionParser();
    parser.version(VERSION);
    parser.add_option("-n", "--name").dest("name").help("Daemon name");
    parser.add_option("-p", "--path").dest("path").help("Daemon root folder");
    optparse::Values options = parser.parse_args(argc, argv);

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
    Path sqlite_path = root / Path(name + ".db");

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

    // Set Stdout and Stderr to log and err files
    if (freopen(log_path.string().c_str(), "a+", stdout) == NULL) exit(1);
    if (freopen(err_path.string().c_str(), "a+", stderr) == NULL) exit(1);

    log("switched to daemon");

    // Initialize Context
    Context::Clear();

    // Connect to SQLite
    sqlite3* db;
    rdy = sqlite3_open(sqlite_path.string().c_str(), &db);
    if (rdy != SQLITE_OK)
    { error("error connecting to sqlite"); exit(1); };

    // Setup SQLite
    PopulateDatabase(db); // Create DB Tables
    int lts = GetLatestId(db); // Get Latest Order Id

    log("connected to sqlite");

    // Initiate MarketManager
    MyMarketHandler market_handler(lts);
    MarketManager market(market_handler);
    PopulateBook(&market, db, name.c_str()); // Fill order book from DB
    market.EnableMatching(); // Enable matching
    
    // Create socket
    int sockfd = UnixSocket(socket_path.string().c_str(), MAX_CLIENTS);
    if (sockfd == -1) { error("error creating socket"); exit(1); };
    if (sockfd == -2) { error("error binding socket"); exit(1); };
    if (sockfd == -3) { error("error listening on socket"); exit(1); };

    log("listening on socket...");

    // Update status file
    File::WriteAllText(status_path, STATUS_RUN);

    /* ############################################################################################################################################# */

    /* LOOP */

    auto ctx = Context::Get();

    // Set Constants
    (*ctx).market.market_ptr = &market;
    (*ctx).market.handler_ptr = &market_handler;
    (*ctx).connection.sqlite_ptr = db;
    
    std::vector<int> connections = {sockfd}; // Connection vector
    std::vector<int>::iterator it; // Connection iterator
    std::string message;
    int connfd;

    (*ctx).enable = true; // Run condition

    // Handle connections
    while ((*ctx).enable)
    {
        try
        {
            // Wait for new connection or message
            rdy = SelectVector(&connections);
            if (rdy < 0) error("error waiting for connections");

            // Accept new connection (if available)
            connfd = AcceptConnection(sockfd);
            if (connfd < 0) error("error accepting connetion");
            if (connfd > 0) connections.push_back(connfd); // Add connection to vector

            // Read messages from all clients (if available)
            it = ++connections.begin();
            while ((it < connections.end()) && (*ctx).enable)
            {
                rdy = ReadSocketStream(*it, MSG_SIZE, &message); // Read message
                if (rdy < 0) // Connection closed
                {
                    close(*it);
                    it = connections.erase(it); // Remove connection from vector
                }
                if (rdy > 0) // Message recieved
                {
                    // Update Context
                    (*ctx).connection.sockfd = *it;
                    (*ctx).command.input = message;
                    (*ctx).command.response = NULL_STR;

                    // Execute command
                    Execute();

                    // Send response to client
                    SendResponse();

                    // Clear Context
                    (*ctx).connection.sockfd = 0;
                    (*ctx).order = Context::Order();
                    (*ctx).command = Context::Command();
                }
                ++it; // Update iterator
            }
        }
        // Catch any error
        catch (std::exception const& e)
        {
            error(e.what());
            (*ctx).enable = false;
        }
        catch (...)
        {
            error("unknown error occurred");
            (*ctx).enable = false;
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
