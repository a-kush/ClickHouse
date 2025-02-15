// WORKING VERSION

#include <AggregateFunctions/registerAggregateFunctions.h>
#include <Formats/registerFormats.h>
#include <Functions/registerFunctions.h>
#include <IO/UseSSL.h>
#include "../client/Client.h"

#include <sys/fcntl.h>
#include <sys/signal.h>

#include <csignal>
#include "AggregateFunctions/AggregateFunctionFactory.h"
#include "AggregateFunctions/IAggregateFunction.h"
#include "IO/WriteBuffer.h"
#include "IO/WriteBufferFromFileDescriptor.h"
#include "IO/WriteBufferFromString.h"
#include "Top.h"
#include "config.h"

#include <chrono>
#include <string>
#include <thread>

#include <cmath>
#include <format>
#include <unordered_map>

#include <fcntl.h>


#ifndef __clang__
#    pragma GCC optimize("-fno-var-tracking-assignments")
#endif


#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-declarations"

namespace DB
{

//------------COLORS-------------//


namespace C // color_pairs
{
    enum
    {
        BLACK_GREEN = 1,
        BLACK_CYAN = 2,
        BLACK_YELLOW = 3
    };
}

void init_colors()
{
    init_pair(C::BLACK_GREEN, COLOR_BLACK, COLOR_GREEN);
    init_pair(C::BLACK_CYAN, COLOR_BLACK, COLOR_CYAN);
    init_pair(C::BLACK_YELLOW, COLOR_BLACK, COLOR_YELLOW);
}


//-------------------------------//

//-----------NCURSES-------------//

bool is_ncurses_mode = false;

const int TABLE_START_Y = 8;

[[noreturn]] void handler_sigint(int signal)
{
    (void)signal;
    if (is_ncurses_mode)
    {
        endwin();
        is_ncurses_mode = false;
    }
    _exit(signal);
}


void Top::initNcurses()
{
    struct sigaction act = {};
    act.sa_handler = handler_sigint;

    initscr();
    sigaction(SIGINT, &act, nullptr); // must be after initscr() according to curses documentation
    noecho(); // user doesn't see what he presses
    cbreak(); // disable line buffering

    if (has_colors() == FALSE)
    {
        perror("Terminal doesn't support colors.\n");
        exit(1);
    }
    if (can_change_color() == FALSE)
    {
        perror("Changing colors isn't allowed.\n");
        exit(1);
    }
    if (start_color() == ERR)
    {
        perror("Color table cannot be allocated.\n");
        exit(1);
    }
    init_colors();
    curs_set(0);
    is_ncurses_mode = true;

    int startx = 0;
    int starty = 0;
    int width = COLS;
    int height = TABLE_START_Y;
    top_win = newwin(height, width, starty, startx);

    startx = 0;
    starty = TABLE_START_Y;
    width = COLS;
    height = LINES - 1 - starty;
    table_win_height = height;
    table_win = newwin(height, width, starty, startx);
    table_start_row = 1; // not counting header
    table_end_row = height - 1;

    starty = LINES - 1;
    height = 1;
    bottom_win = newwin(height, width, starty, startx);
    keypad(bottom_win, TRUE);
    nodelay(bottom_win, TRUE); // wgetch becomes non-blocking, needed for arrow keys navigation

    highlight = 1;
}

void resetWin(WINDOW * win)
{
    wclear(win);
    wmove(win, 0, 0);
}

//-------------------------------//

namespace P
{ // progress constants
    enum
    {
        READ_PROGRESS = 0,
        READ_BYTES = 1,
        WROTE_BYTES = 2,
        MEMORY_USAGE = 3,
        MEMORY_USAGE_PERCENT = 4,
        ELAPSED = 5,
        KILL = 6
    };
}

namespace BYTE
{
    float MB = 1024 * 1024;
    float GB = MB * 1024; // in bytes
    float TB = GB * 1024;
}


namespace Q
{ // Queries
    enum
    {
        METRIC = 0,
        ASYNC_METRIC = 1,
        EVENT = 2,
        PROCESS = 3
    };


    std::vector<String> h_process = {"r_progr", "read", "wrote", "RAM", "RAM%%", "time", "kill", "user", "query"};


    String epilogue = " INTO OUTFILE 'top/out.txt' FORMAT TabSeparated";

    String q_processes_base = "SELECT read_rows / total_rows_approx, read_bytes, written_bytes, memory_usage, ROUND(memory_usage / (SELECT "
                              "SUM(memory_usage) FROM system.processes) * 100, 1) "
                              ", ROUND(elapsed, 0), is_cancelled, user, query FROM system.processes ";
    String q_processes = q_processes_base + epilogue;

    //------------SORTING QUERIES---------------//

    String q_processes_ram_sort_inc = q_processes_base + " ORDER BY memory_usage" + epilogue;
    String q_processes_ram_sort_desc = q_processes_base + " ORDER BY memory_usage DESC" + epilogue;

    String q_processes_read_sort_inc = q_processes_base + " ORDER BY read_bytes" + epilogue;
    String q_processes_read_sort_desc = q_processes_base + " ORDER BY read_bytes DESC" + epilogue;

    String q_processes_write_sort_inc = q_processes_base + " ORDER BY written_bytes" + epilogue;
    String q_processes_write_sort_desc = q_processes_base + " ORDER BY written_bytes DESC" + epilogue;

    String q_processes_elapsed_sort_inc = q_processes_base + " ORDER BY elapsed" + epilogue;
    String q_processes_elapsed_sort_desc = q_processes_base + " ORDER BY elapsed DESC" + epilogue;

    int ram_sort = 0, read_sort = 0, write_sort = 0, elapsed_sort = 0;

    struct SortingStrings
    {
        std::string_view inc;
        std::string_view dec;
    };

    std::unordered_map<char, int *> sort_map_mods = {{'m', &ram_sort}, {'r', &read_sort}, {'w', &write_sort}, {'t', &elapsed_sort}};
    std::unordered_map<char, SortingStrings> sort_map_queries
        = {{'m', {q_processes_ram_sort_inc, q_processes_ram_sort_desc}},
           {'r', {q_processes_read_sort_inc, q_processes_read_sort_desc}},
           {'w', {q_processes_write_sort_inc, q_processes_write_sort_desc}},
           {'t', {q_processes_elapsed_sort_inc, q_processes_elapsed_sort_desc}}};
    std::unordered_map<char, int> sort_map_ind = {{'m', P::MEMORY_USAGE}, {'r', P::READ_BYTES}, {'w', P::WROTE_BYTES}, {'t', P::ELAPSED}};
    //stopped HERE

    int sort_highlight = -1;


    //-----------------------------------------//

    struct MetricAndType
    {
        String name;
        int type;
    };


    std::vector<MetricAndType> h_top
        = {{"Query", METRIC},
           {"Merge", METRIC},
           {"Move", METRIC},
           {"DelayedInserts", METRIC},
           {"DistributedFilesToInsert", METRIC},
           {"TCPConnection", METRIC},
           {"HTTPConnection", METRIC},
           {"MaxPartCountForPartition", ASYNC_METRIC},
           {"TotalPartsOfMergeTreeTables", ASYNC_METRIC},
           {"TotalRowsOfMergeTreeTables", ASYNC_METRIC},
           {"ReplicasMaxQueueSize", ASYNC_METRIC},
           {"ReplicasSumQueueSize", ASYNC_METRIC},
           {"ReadonlyReplica", METRIC},
           {"Uptime", ASYNC_METRIC},
           {"GlobalThread", METRIC},
           {"GlobalThreadActive", METRIC}};

    String q_top_metrics_base = "SELECT metric, value FROM system.metrics WHERE ";
    String q_top_async_metrics_base = "SELECT metric, value FROM system.asynchronous_metrics WHERE ";


    String q_top_metrics = q_top_metrics_base; // to be inited
    String q_top_async_metrics = q_top_async_metrics_base; // to be inited

    String help_bar_str = "F1 Help  q Quit";

    void initQueries()
    // creates top window queries from vector of wanted metrics
    {
        for (size_t i = 0; i < h_top.size(); ++i)
        {
            auto & elem = h_top[i];
            if (elem.type == METRIC)
            {
                q_top_metrics += "metric == '" + elem.name + "' OR ";
            }
            else if (elem.type == ASYNC_METRIC)
            {
                q_top_async_metrics += "metric == '" + elem.name + "' OR ";
            }
        }
        q_top_metrics.erase(q_top_metrics.size() - 3);
        q_top_async_metrics.erase(q_top_async_metrics.size() - 3);

        q_top_metrics += epilogue;
        q_top_async_metrics += epilogue;
    }
}


//--------------GENERAL--------------------//

/* I think it should be optimised by not using "INTO OUTFILE", but I can't change std_out field of clickhouse-client
(= operator deleted) *. I tried reading from std.out.buffer() but for some reason on the end of it sometimes appear
unwanted symbols. I suppose you shouldn't be iteratring through it's buffer and possibly mixing ncurses output and
iostream is a bad idea. Maybe I should play with ncurses output descriptor.

Function processes query and gets result in a signle String. (could be optimised by finding all ASTs at the beginning)
(TabSeparated)
*/

String Top::queryToString(String & query)
{
    remove("top/out.txt");

    processQueryText(query);
    String str;
    char c;
    ReadBufferFromFile read_buffer("top/out.txt");
    while (true)
    {
        int status = read_buffer.read(c);
        if (status == read_buffer.eof())
        {
            break;
        }
        str.push_back(c);
    }
    read_buffer.close();
    return str;
}

/* Help bar at the bottom of screen */
void Top::printHelpBar()
{
    wmove(bottom_win, 0, 0);
    wattron(bottom_win, COLOR_PAIR(C::BLACK_CYAN));
    wprintw(bottom_win, Q::help_bar_str.data());
    wattroff(bottom_win, COLOR_PAIR(C::BLACK_CYAN));
    wrefresh(bottom_win);
}

void Top::showHelpScreen() // can be done in different window and just refreshing to show
{
    resetWin(stdscr);

    wprintw(
        stdscr,
        "Pressing next keys sorts process table, press again to switch from"
        "\nincreasing to decreasing order and vice versa:"
        "\n'm' by RAM usage."
        "\n'r' by amount of read bytes."
        "\n'w' by amount of written bytes."
        "\n't' by time elapsed."
        "\n\nUse arrow keys to navigate through process table."
        "\nUse enter to show full information for selected query."
        "\n\nPress any key to return.");

    wrefresh(stdscr);

    wgetch(stdscr);

    printHelpBar();
}

/* Allows user to control the app*/
bool Top::tryKeyboard()
{
    int ch = wgetch(bottom_win);
    switch (ch)
    {
        case '\n':
            printLineDescription();
            return true;
        case 'm':
        case 'r':
        case 'w':
        case 't':
            setSortedQuery(ch);
            return true;
        case KEY_F(1):
            showHelpScreen();
            return true;
        case KEY_UP:
            --highlight;
            return true;
        case KEY_DOWN:
            ++highlight;
            return true;
        case 'q':
            handler_sigint(0);
        default:
            return false;
    }
}

/* Needed so we would'nt update the screen to often */
int Top::sleepTryKeyboard()
{
    for (int i = 0; i < 10; ++i)
    {
        if (tryKeyboard())
        {
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

//-------------------------------//

//-----------PROGRESS TABLE-------------//

void parseBytes(String & data)
{
    float bytes = std::stof(data);
    float value;
    String res;
    if (bytes < BYTE::GB)
    { // always round up
        value = bytes / BYTE::MB;
        res = std::format("{:.0f}", value) + "MB";
    }
    else if (bytes < BYTE::TB)
    {
        value = bytes / BYTE::GB;
        res = std::format("{:.2f}", value) + "GB";
    }
    else
    {
        value = bytes / BYTE::TB;
        res = std::format("{:.2f}", value) + "TB";
    }
    data = res;
}

void parseSeconds(String & data)
{
    long long seconds = std::stoll(data);
    long long hours = seconds / 3600;
    long long minutes = (seconds % 3600) / 60;
    seconds %= 60;

    String res = std::format("{}:{}:{}", hours, minutes, seconds);
    data = res;
}

void parseKill(String & data)
{
    if (data == "1")
    {
        data = "YES";
    }
    else
    {
        data = "NO";
    }
}
/* Fills progress bar with '|' symbols and append spaces on the remaining part */
void Top::addProgressbar()
{
    int length = static_cast<int>(COLS * 0.2);
    for (size_t i = 1; i < process_table.size(); ++i)
    {
        auto & vec = process_table[i];
        double progress = std::stod(vec[P::READ_PROGRESS]);

        if (std::isinf(progress) || std::isnan(progress)) // very necessary part to check.
        {
            progress = 0;
            vec[P::READ_PROGRESS] = "NO";
            continue;
        }


        int j = 1;
        while (static_cast<double>(j) / length < progress)
        {
            ++j;
        }
        vec[P::READ_PROGRESS] = String(j, '|') + String(length - j, ' ');
    }
}


void Top::reformatProcessTable()
{
    addProgressbar();
    for (size_t i = 1; i < process_table.size(); ++i)
    {
        auto & vec = process_table[i];
        parseBytes(vec[P::READ_BYTES]);
        parseBytes(vec[P::WROTE_BYTES]);
        parseBytes(vec[P::MEMORY_USAGE]);
        parseSeconds(vec[P::ELAPSED]);
        parseKill(vec[P::KILL]);
    }
}

/* Turns system.processes query result (string) into 2d vector (table) */
void Top::parseMetric(String & str)
{
    process_table.clear();

    process_table.push_back(Q::h_process);

    std::vector<String> cur_vec;
    String cur_str;
    for (auto sym : str)
    {
        if (sym == '\t' || sym == '\n')
        {
            if (!cur_str.empty())
            {
                cur_vec.push_back(cur_str);
                cur_str.clear();
            }
            if (sym == '\n')
            {
                process_table.push_back(cur_vec);
                cur_vec.clear();
            }
        }
        else
        {
            cur_str.push_back(sym);
        }
    }
    if (!cur_str.empty())
    {
        cur_vec.push_back(cur_str);
        process_table.push_back(cur_vec);
    }
    reformatProcessTable();
}

/* Prints one row of table (which is 2d vector)*/
void Top::printLine(int ind, std::vector<int> & indents, bool is_header)
{
    std::vector<String> vec = process_table[ind];
    for (size_t j = 0; j < vec.size(); ++j)
    {
        String str = vec[j].substr(0, COLS - getcurx(table_win) - 1);

        if (!is_header && j == 0)
        {
            if (highlight == ind)
            {
                wattroff(table_win, COLOR_PAIR(C::BLACK_CYAN));
            }
            wattron(table_win, COLOR_PAIR(C::BLACK_YELLOW));
            wprintw(table_win, str.data());
            wattroff(table_win, COLOR_PAIR(C::BLACK_YELLOW));
            if (highlight == ind)
            {
                wattron(table_win, COLOR_PAIR(C::BLACK_CYAN));
            }
        }
        else
        {
            if (is_header && static_cast<int>(j) == Q::sort_highlight)
            {
                wattroff(table_win, COLOR_PAIR(C::BLACK_GREEN));

                wattron(table_win, COLOR_PAIR(C::BLACK_CYAN));
                wprintw(table_win, str.data());
                wattroff(table_win, COLOR_PAIR(C::BLACK_CYAN));

                wattron(table_win, COLOR_PAIR(C::BLACK_GREEN));
            }
            else
            {
                wprintw(table_win, str.data());
            }
        }

        int xdif = (indents[j] + 1 - static_cast<int>(str.size())); // + 1 for a delimeter (a space)
        if (is_header && j == P::MEMORY_USAGE_PERCENT)
        { // fixes %% problem in RAM%%
            ++xdif;
        }
        if (j != vec.size() - 1)
        {
            wprintw(table_win, String(xdif, ' ').data());
        }
        else
        {
            wprintw(table_win, String(COLS - getcurx(table_win) - 1, ' ').data());
        }
    }
    wmove(table_win, getcury(table_win) + 1, 0);
}

/* Calculates column indents so table would look as expected no matter lengths of values */
std::vector<int> get_indents(std::vector<std::vector<String>> & a)
{
    if (a.empty())
    {
        perror("Passed Vector Is Empty");
        _exit(1);
    }
    std::vector<int> indents(a[0].size());

    bool is_header = true;
    for (auto & vec : a)
    {
        for (size_t i = 0; i < vec.size(); ++i)
        {
            indents[i] = std::max(indents[i], static_cast<int>(vec[i].size()));
            if (is_header && i == P::MEMORY_USAGE_PERCENT)
            { // fixes problem with %% character in "RAM%%" (which is counted twice)
                --indents[i];
            }
        }
        is_header = false;
    }

    return indents;
}

/* We print only those rows which fit on screen but alwo allow scrolling:
  highlighted row helps understand what part of table we want to show */
void Top::printProcessTable()
{
    resetWin(table_win);
    auto indents = get_indents(process_table);

    wattron(table_win, COLOR_PAIR(C::BLACK_GREEN)); // printing header
    printLine(0, indents, true);
    wattroff(table_win, COLOR_PAIR(C::BLACK_GREEN));

    if (highlight < 1)
    {
        highlight = 1;
    }
    int cur_size = static_cast<int>(process_table.size());
    if (highlight >= cur_size) // cur_size is always >= 2
    {
        highlight = cur_size - 1;
    }

    table_end_row = table_start_row + table_win_height - 1; // -2 because we are allowed Lines - 2 space (bcof header)
    if (highlight >= table_end_row)
    { // scrolling down
        ++table_end_row;
        ++table_start_row;
    }
    if (highlight < table_start_row)
    { // scrolling up
        --table_end_row;
        --table_start_row;
    }
    table_end_row = std::min(table_end_row, cur_size);


    for (int i = table_start_row; i < table_end_row; ++i) // printing data
    {
        if (i == highlight)
        {
            wattron(table_win, COLOR_PAIR(C::BLACK_GREEN)); // printing header
            printLine(i, indents, false);
            wattroff(table_win, COLOR_PAIR(C::BLACK_GREEN));
        }
        else
        {
            printLine(i, indents, false);
        }
    }
}

/* Print full info from system.processes about highlighted query 
(Could be improved by formatting query output)*/
void Top::printLineDescription()
{
    resetWin(stdscr);

    std::vector<String> cur_header = Q::h_process;

    for (size_t i = 0; i < cur_header.size(); ++i)
    {
        String name = cur_header[i];
        String value = process_table[highlight][i];

        wattron(stdscr, COLOR_PAIR(C::BLACK_GREEN));
        wprintw(stdscr, "%s", name.data());
        wattroff(stdscr, COLOR_PAIR(C::BLACK_GREEN));

        wprintw(stdscr, ": %s\n", value.data());
    }
    wrefresh(stdscr);

    wgetch(stdscr); // press any key to return
}

/* Allows user to sort table by some columns in both orders */
void Top::setSortedQuery(char option)
{
    for (auto & elem : Q::sort_map_mods)
    {
        if (elem.first != option)
        {
            *elem.second = 0;
        }
    }

    int * cur_val = Q::sort_map_mods[option];
    if (*cur_val != 1)
    {
        this->process_query = Q::sort_map_queries[option].inc;
        *cur_val = 1;
    }
    else
    {
        this->process_query = Q::sort_map_queries[option].dec;
        *cur_val = -1;
    }
    Q::sort_highlight = Q::sort_map_ind[option];
}


//-------------------------------//

//-------------TOP-TABLE------------------//

/* Prints some impornant metrics (should add some more metrics)*/
void Top::printTop()
{
    resetWin(top_win);

    int ind = 0;
    for (auto & metric : Q::h_top)
    {
        if (ind >= TABLE_START_Y)
        {
            wmove(top_win, ind - TABLE_START_Y, COLS / 2);
        }
        wprintw(top_win, "%s: %s\n", metric.name.data(), top_data[metric.name].data());

        ++ind;
    }
}

/* Processes only "metric, value". Could be extended to description and then description should be shown with F1 key */
void Top::parseTopQuery(String & str)
{
    String cur_str;
    String cur_metric;
    for (auto sym : str)
    {
        if (sym == '\t' || sym == '\n')
        {
            if (cur_metric.empty())
            {
                cur_metric = cur_str;
            }
            else
            {
                top_data[cur_metric] = cur_str;
                cur_metric.clear();
            }
            cur_str.clear();
        }
        else
        {
            cur_str.push_back(sym);
        }
    }
    if (!cur_str.empty())
    {
        top_data[cur_metric] = cur_str;
    }
}

//-----------TOP AND PROCESS TABLE WRAPPERS--------//

int Top::makeProcessTable()
{
    String str = queryToString(this->process_query);
    if (tryKeyboard())
    {
        return 0;
    }

    parseMetric(str);
    if (tryKeyboard())
    {
        return 0;
    }

    printProcessTable();
    if (tryKeyboard())
    {
        return 0;
    }
    return 1;
}


int Top::makeTop()
{
    String str = queryToString(Q::q_top_metrics) + "\n" + queryToString(Q::q_top_async_metrics);

    if (tryKeyboard())
    {
        return 0;
    }
    parseTopQuery(str);

    if (tryKeyboard())
    {
        return 0;
    }

    printTop();

    return 1;
}

//---------------------------------------//

//---------------RUNNING-------------------//

[[noreturn]] void Top::go()
{
    initNcurses();
    Q::initQueries();
    printHelpBar();
    this->process_query = Q::q_processes;

    while (true)
    {
        if (!makeProcessTable())
        {
            continue;
        }

        if (!makeTop())
        {
            continue;
        }
        printHelpBar();
        wrefresh(table_win);
        wrefresh(top_win);

        sleepTryKeyboard();
    }
}


[[noreturn]] void Top::start()
{
    // part of rewritten client function
    initialize(*this);

    UseSSL uqse_ssl;
    MainThreadStatus::getInstance();
    setupSignalHandler();

    std::cout << std::fixed << std::setprecision(3);
    std::cerr << std::fixed << std::setprecision(3);

    registerFormats();
    registerFunctions();
    registerAggregateFunctions();

    processConfig();

    connect();

    connection->setDefaultDatabase(connection_parameters.default_database);

    // my code
    is_interactive = false;
    go();
}
}

int mainEntryClickHouseTop(int argc, char ** argv)
{
    DB::Top client;
    client.init(argc, argv);
    client.start();
}
