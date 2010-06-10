#include "state.hpp"
#include <iostream>
#include <sstream>
#include "nanoclock.hpp"

using namespace dfterm;
using namespace std;

static bool state_initialized = false;

State::State()
{
    state_initialized = true;
    global_chat = SP<Logger>(new Logger);
    ticks_per_second = 20;
};

State::~State()
{
    state_initialized = false;
};

SP<State> State::createState()
{
    if (state_initialized) return SP<State>();
    return SP<State>(new State);
};

bool State::setDatabase(UnicodeString database_file)
{
    string r;
    database_file.toUTF8String(r);
    return setDatabaseUTF8(r);
}

bool State::setDatabaseUTF8(string database_file)
{
    /* Configuration */
    configuration = SP<ConfigurationDatabase>(new ConfigurationDatabase);
    OpenStatus d_result = configuration->openUTF8(database_file);
    if (d_result == Failure)
    {
        stringstream ss;
        ss << "Failed to open database file " << database_file;
        admin_logger->logMessageUTF8(ss);
        return false;
    }
    if (d_result == OkCreatedNewDatabase)
    {
        admin_logger->logMessageUTF8("Created a new database from scratch. You should add an admin account to configure dfterm2.");
        admin_logger->logMessageUTF8("You need to use the command line tool dfterm2_configure for that. Close dfterm2 and then");
        admin_logger->logMessageUTF8("add an account like this: ");
        admin_logger->logMessageUTF8("dfterm2_configure --adduser (user name) (password) admin");
        admin_logger->logMessageUTF8("For example:");
        admin_logger->logMessageUTF8("dfterm2_configure --adduser Adeon s3cr3t_p4ssw0rd admin");
        admin_logger->logMessageUTF8("This will create a new admin account for you.");
        admin_logger->logMessageUTF8("If you are not using the default database (if you don't know then you are using it), use");
        admin_logger->logMessageUTF8("the --database switch to modify the correct database.");
        return false;
    }

    return true;
}

bool State::addTelnetService(SocketAddress address)
{
    stringstream ss;
    SP<Socket> s(new Socket);
    bool result = s->listen(address);
    if (!result)
    {
        ss << "Listening failed. " << s->getError();
        admin_logger->logMessageUTF8(ss);
        return false;
    }
    admin_logger->logMessageUTF8("Telnet service started. ");
    listening_sockets.insert(s);

    return true;
}

void State::setTicksPerSecond(uint64_t ticks_per_second)
{
    this->ticks_per_second = ticks_per_second;
}

void State::loop()
{
    /* Use these for timing ticks */
    uint64_t start_time;
    const uint64_t tick_time = 1000000000 / ticks_per_second;

    bool close = false;

    while(listening_sockets.size() > 0 && !close)
    {
        start_time = nanoclock();

        bool update_nicklists = false;
        /* Prune inactive clients */
        unsigned int i2, len = clients.size();
        for (i2 = 0; i2 < len; i2++)
        {
            if (!clients[i2]->isActive())
            {
                clients.erase(clients.begin() + i2);
                clients_weak.erase(clients_weak.begin() + i2);
                len--;
                i2--;
                admin_logger->logMessageUTF8("Pruned an inactive connection.");
                update_nicklists = true;
                continue;
            }
        }

        /* Check for incoming connections. */
        SP<Socket> new_connection(new Socket);
        set<SP<Socket> >::iterator listener;
        for (listener = listening_sockets.begin(); listener != listening_sockets.end(); listener++)
        {
            bool got_connection = (*listener)->accept(new_connection.get());
            if (got_connection)
            {
                SP<Client> new_client = Client::createClient(new_connection);
                new_client->setConfigurationDatabase(configuration);
                new_client->setGlobalChatLogger(global_chat);
                clients.push_back(new_client);
                clients_weak.push_back(new_client);
                new_client->setClientVector(&clients_weak);
                admin_logger->logMessageUTF8("Got new connection.");
                update_nicklists = true;
            }
        }

        /* Read and write from and to connections */
        len = clients.size();
        for (i2 = 0; i2 < len; i2++)
        {
            if (update_nicklists) clients[i2]->updateClients();
            clients[i2]->cycle();
            if (clients[i2]->shouldShutdown()) close = true;
        }

        /* Ticky wait. */
        uint64_t end_time = nanoclock();
        if (end_time - start_time < tick_time)
            nanowait( tick_time - (end_time - start_time) );
        flush_messages();
    }
    flush_messages();
}
