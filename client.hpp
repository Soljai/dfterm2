#ifndef client_hpp
#define client_hpp

#include "interface_termemu.hpp"
#include "termemu.h"
#include "sockets.hpp"
#include "telnet.hpp"
#include "logger.hpp"
#include "slot.hpp"
#include "dfterm2_configuration.hpp"

namespace dfterm
{

using namespace trankesbel;

class Client;

/* Required by TelnetSession */
class ClientTelnetSession : public TelnetSession
{
    private:
        WP<Client> client;

    public:
        ClientTelnetSession();
        ~ClientTelnetSession();

        bool readRawData(void* data, size_t* size);
        bool writeRawData(const void* data, size_t* size);

        void setClient(WP<Client> client);
};

class Client
{
    private:
        ClientTelnetSession ts;
        SP<Socket> client_socket;
        SP<InterfaceTermemu> interface;
        Terminal buffer_terminal;
        Terminal last_client_terminal;

        /* Slot for game. */
        WP<Slot> slot;
        /* Database access. */
        WP<ConfigurationDatabase> configuration;

        /* In the identifying window, track the index numbers for password fields. */
        int password1_index, password2_index, message_index;

        /* Last time a password was entered. Used for limiting how fast passwords
         * can be tried. */
        uint64_t password_enter_time;

        /* Nickname and whether the client has identified itself. */
        UnicodeString nickname;
        bool identified;

        /* This method handles what happens when client identified.
           (Creates windows etc.) */
        void clientIdentified();

        ui32 chat_window_input_index;

        /* Maximum number of lines in chat history. */
        ui32 max_chat_history;

        SP<Logger> global_chat;
        SP<LoggerReader> global_chat_reader;
        void cycleChat();

        /* Configuring dfterm2 is complex enough to warrant
         * dedicated classes and files. Here's a class handle to them. */
        ConfigurationInterface config_interface;

        /* User handle. */
        SP<User> user;

        /* Nicks go in this window */
        SP<InterfaceElementWindow> nicklist_window;
        /* And chat to this window */
        SP<InterfaceElementWindow> chat_window;
        /* And the game screen to this one */
        SP<Interface2DWindow> game_window;
        /* Configuration window */
        SP<InterfaceElementWindow> config_window;

        /* Identify window. Exists only at start. */
        SP<InterfaceElementWindow> identify_window;

        /* Used to keep nick list up to date. */
        vector<WP<Client> >* clients;
        void updateNicklistWindow();
        /* This one calls updateNicklistWindow for all clients in that vector. */
        void updateNicklistWindowForAll();

        bool do_full_redraw;
        bool packet_pending;
        ui32 packet_pending_index;
        string deltas;

        WP<Client> self;

        /* No copies */
        Client(const Client &c) { };
        Client& operator=(const Client &c) { return (*this); };

        /* No default constructor */
        Client() { };

        /* Create a client and associate a socket with it. */
        Client(SP<Socket> client_socket);

        void setSelf(WP<Client> c) { self = c; ts.setClient(self); };

        /* Callback functions. */
        bool chatRestrictFunction(ui32* keycode, ui32* cursor);
        bool chatSelectFunction(ui32 index);
        bool identifySelectFunction(ui32 index);
        void gameInputFunction(ui32 keycode, bool special_key);

    public:
        static SP<Client> createClient(SP<Socket> client_socket)
        {
            SP<Client> c(new Client(client_socket));
            c->setSelf(WP<Client>(c));
            c->ts.handShake();
            return c;
        }
        /* Destructor */
        ~Client();

        /* Set the configuration database to use. Clients will use a weak reference to the database. 
         * You should set it before you put the client into use. */
        void setConfigurationDatabase(SP<ConfigurationDatabase> configuration_database)
        { setConfigurationDatabase(WP<ConfigurationDatabase>(configuration_database)); };
        void setConfigurationDatabase(WP<ConfigurationDatabase> configuration_database);
        /* Returns the configuration database this client is using. */
        WP<ConfigurationDatabase> getConfigurationDatabase() const;

        /* Returns the socket the client is using */
        SP<Socket> getSocket() { return client_socket; };

        /* Returns true if client connection is active. */
        bool isActive() const;

        /* Sets a slot for this client. */
        void setSlot(SP<Slot> slot)
        { this->slot = slot; };

        /* Gets the slot this client is seeing. */
        WP<Slot> getSlot() const { return slot; };

        /* Sets the global chat. */
        void setGlobalChatLogger(SP<Logger> global_chat);
        SP<Logger> getGlobalChatLogger() const;

        /* Sets the vector of clients that are connected. This is used
           to fill nicklist window, if set. */
        void setClientVector(vector<WP<Client> >* clients);
        /* Tells this client that client list has been updated. 
           No effect if client vector has not been set with above call. */
        void updateClients() { updateNicklistWindow(); };

        /* Cycle the client connection */
        void cycle();
};

}

#endif

