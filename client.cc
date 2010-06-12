#include "client.hpp"
#include <iostream>
#include <algorithm>
#include "cp437_to_unicode.hpp"
#include <openssl/rand.h>
#include "nanoclock.hpp"

using namespace std; 
using namespace trankesbel;
using namespace dfterm;

void seedRNG()
{
    int rng_counter = 100000;
    /* Seed random number generator a bit */
    while(!RAND_status() && rng_counter > 0)
    {
        rng_counter--;

        #ifdef __WIN32
        #define WIN32_LEAN_AND_MEAN
        #include <windows.h>

        RAND_screen();
        DWORD t = GetTickCount();
        RAND_ADD((void*) &t, sizeof(t), sizeof(t) / 2);
        #endif

        struct timeval tv;
        gettimeofday(&tv, NULL);
        RAND_add((void*) &tv, sizeof(tv), sizeof(tv) / 4);
    }
}

void makeRandomBytes(unsigned char* output, int output_size)
{
    if (!RAND_status()) seedRNG();

    if (!RAND_bytes(output, output_size))
    {
        seedRNG();
        if (!RAND_bytes(output, output_size))
            RAND_pseudo_bytes(output, output_size);
    }
}

ClientTelnetSession::ClientTelnetSession() : TelnetSession()
{
};

ClientTelnetSession::~ClientTelnetSession()
{
};

void ClientTelnetSession::setClient(WP<Client> client)
{
    this->client = client;
}

bool ClientTelnetSession::writeRawData(const void* data, size_t* size)
{
    size_t max_size = (*size);

    (*size) = 0;
    SP<Client> spc = client.lock();
    if (!spc) return false;

    SP<Socket> s = spc->getSocket();
    if (!s) return false;

    if (!s->active()) return false;
    if (max_size == 0) return true;

    size_t result = s->send(data, max_size);
    if (result > 0)
    {
        (*size) = result;
        return true;
    }
    if (!s->active()) return false;

    return true;
}

bool ClientTelnetSession::readRawData(void* data, size_t* size)
{
    size_t max_size = (*size);

    (*size) = 0;
    SP<Client> spc = client.lock();
    if (!spc) return false;

    SP<Socket> s = spc->getSocket();
    if (!s) return false;

    if (!s->active()) return false;
    if (max_size == 0) return true;

    size_t result = s->recv(data, max_size);
    if (result > 0)
    {
        (*size) = result;
        return true;
    }
    if (!s->active()) return false;

    return true;
}



Client::Client(SP<Socket> client_socket)
{
    config_interface = SP<ConfigurationInterface>(new ConfigurationInterface);

    this->client_socket = client_socket;
    packet_pending = false;
    packet_pending_index = 0;
    do_full_redraw = false;

    identified = false;
    clients = (vector<WP<Client> >*) 0;

    interface = SP<InterfaceTermemu>(new InterfaceTermemu);
    interface->initialize();

    /* Register CP437 to unicode maps */
    initCharacterMappings();
    ui32 i1 = 0;
    for (i1 = 0; i1 < 256; i1++)
    {
        CursesElement ce(mapCharacter(i1), White, Black, false);
        interface->mapElementIdentifier(i1, (void*) &ce, sizeof(ce));
    }

    user = SP<User>(new User);
    config_interface->setInterface(interface);

    identify_window = interface->createInterfaceElementWindow();
    identify_window->setHint("chat");
    identify_window->setTitleUTF8("Enter your nickname for this session");
    ui32 index = identify_window->addListElementUTF8("", "nickname", true, true);
    identify_window->modifyListSelectionIndex(index);
    identify_window->setListCallback(bind(&Client::identifySelectFunction, this, _1));

    max_chat_history = 500;

    password_enter_time = nanoclock();

    /* Hide cursor */
    ts.sendPacket("\x1b[?25l", 6);
}

Client::~Client()
{
}

void Client::setState(WP<State> state)
{
    this->state = state;
    config_interface->setState(state);
}

bool Client::isActive() const
{
    return client_socket->active();
}

void Client::cycleChat()
{
    if (!global_chat) return;
    if (!global_chat_reader) return;

    while(1)
    {
        bool message;
        UnicodeString us = global_chat_reader->getLogMessage(&message);
        if (!message) break;

        UnicodeString chat_str = chat_window->getListElement(chat_window_input_index);
        chat_window->deleteListElement(chat_window_input_index);
        chat_window_input_index = 0xFFFFFFFF;

        chat_window->addListElement(us, "", true, false);

        /* Remove first item from list if maximum chat history is reached */
        ui32 list_elements = chat_window->getNumberOfListElements();
        if (list_elements > max_chat_history)
            chat_window->deleteListElement(0);

        chat_window_input_index = chat_window->addListElement(chat_str, "Chat> ", "chat", true, true);
        chat_window->modifyListSelectionIndex(chat_window_input_index);
    }
}

void Client::cycle()
{
    if (!isActive()) return;
    if (!user) { client_socket->close(); return; };
    if (!user->isActive() && client_socket) { client_socket->close(); return; };
    if (identified) identify_window = SP<InterfaceElementWindow>();

    config_interface->cycle();

    ts.cycle();

    /* Check if client has resized their terminal, adjust
       if necessary. */
    do_full_redraw = false;
    ui32 w, h;
    ts.getTerminalSize(&w, &h);
    if (w != buffer_terminal.getWidth() || h != buffer_terminal.getHeight())
    {
        /* Do not allow larger than 300x300 terminals. */
        w = (w > 300) ? 300 : w;
        h = (h > 300) ? 300 : h;

        buffer_terminal.resize(w, h);
        buffer_terminal.feedString("\x1b[2J", 4);
        do_full_redraw = true;

        ts.sendPacket("\x1b[2J", 4);

        interface->setTerminalSize(w, h);
    }

    SP<Slot> sp_slot = slot.lock();
    if (sp_slot)
        sp_slot->unloadToWindow(game_window);

    interface->refresh();
    interface->cycle();

    const Terminal& client_t = interface->getTerminal();

    if (packet_pending && ts.isPacketCancellable(packet_pending_index))
        ts.cancelPacket(packet_pending_index);
    else if (deltas.size() > 0)
    {
        buffer_terminal.copyPreserve(&last_client_terminal);
        deltas.clear();
        packet_pending = false;
        packet_pending_index = 0;
    }

    if (last_client_terminal.getWidth() != client_t.getWidth() ||
        last_client_terminal.getHeight() != client_t.getHeight())
        last_client_terminal.resize(client_t.getWidth(), client_t.getHeight());
    last_client_terminal.copyPreserve(&client_t);

    if (!do_full_redraw)
        deltas = client_t.restrictedUpdateCycle(&buffer_terminal); 
    else
        deltas = client_t.updateCycle();
    if (deltas.size() > 0)
    {
        packet_pending_index = ts.sendPacket(deltas.c_str(), deltas.size());
        packet_pending = true;
        /* If full redraw, throw away the packet indices so that
           we can't discard this package */
        if (do_full_redraw)
        {
            packet_pending = false;
            packet_pending_index = 0;
            deltas.clear();
            buffer_terminal.copyPreserve(&client_t);
        }
    }

    ts.cycle();

    char buf[500];
    size_t buf_size = 500;
    do
    {
        bool active = ts.receive((void*) buf, &buf_size);
        if (buf_size == 0) break;

        size_t i1;
        for (i1 = 0; i1 < buf_size; i1++)
            interface->pushKeyPress((ui32) ((unsigned char) buf[i1]), false);
    }
    while(buf_size == 500);

    cycleChat();
}

void Client::setGlobalChatLogger(SP<Logger> global_chat)
{
    this->global_chat = global_chat;
    global_chat_reader = global_chat->createReader();
};

SP<Logger> Client::getGlobalChatLogger() const
{
    return global_chat;
}

bool Client::identifySelectFunction(ui32 index)
{
    string select_type = identify_window->getListElementData(index);
    if (select_type == "nickname")
    {
        /* Some restrictions on name. */
        UnicodeString suggested_name = identify_window->getListElement(index);
        /* Remove leading and trailing whitespace */
        suggested_name.trim();
        /* Reject nicknames with embedded whitespace */
        i32 pos = suggested_name.indexOf(' ');
        if (pos != -1)
        {
            suggested_name.findAndReplace(' ', '_');
            identify_window->modifyListElementText(index, suggested_name);
            return false;
        }

        ui32 str_len = suggested_name.countChar32();
        if (str_len == 0) return false; /* no empty nicks! */
        if (str_len > 20)
        {
            /* no overly long nicks either! */
            suggested_name.truncate(20);
            identify_window->modifyListElementText(index, suggested_name);
            return false; 
        }

        /* The string has no whitespace, is at most 20 characters long but does have at least 1 character.
           This is an acceptable nickname. */
        nickname = suggested_name;

        /* Then, check from database what sort of user information is available. */
        SP<ConfigurationDatabase> cdb = configuration.lock();

        /* If there is no configuraition, just do the standard non-permanent login. */
        if (!cdb)
        {
            clientIdentified();
            return true;
        }

        SP<User> user = cdb->loadUserData(nickname);
        if (!user)
        {
            /* No user information? Then we ask the user whether they want a permanent
             * or non-permanent account. */
            identify_window->setTitle("New user - Password");
            identify_window->deleteAllListElements();
            identify_window->addListElementUTF8("If you want to make this a permanent account, enter your password here.", "dummy", false, false);
            identify_window->addListElementUTF8("Otherwise, leave them empty; your user data will not be preserved.", "dummy", false, false);
            password1_index = identify_window->addListElementUTF8("", "Password: ", "new_password1", true, true);
            password2_index = identify_window->addListElementUTF8("", "Retype password: ", "new_password2", true, true);
            identify_window->modifyListSelectionIndex(password1_index);
        }
        else
        {
            /* Something wrong with this user if this evaluates to true. */
            if (user->getPasswordHash() == "")
                return true;

            /* Normal log-in */
            /* Ask for password. */
            identify_window->setTitle("Existing user - Password");
            identify_window->deleteAllListElements();
            password1_index = identify_window->addListElementUTF8("", "Password: ", "password1", true, true);
            identify_window->modifyListSelectionIndex(password1_index);
        }
    } /* if "nickname" */
    else if (select_type == "password1")
    {
        /* Don't allow the client to check for passwords in shorter than 1 second intervals. */
        if (nanoclock() - password_enter_time < 1000000000) return true;

        UnicodeString password1 = identify_window->getListElement(password1_index);

        SP<ConfigurationDatabase> cdb = configuration.lock();
        /* Refuse to log the user in this case, if database does not exist. */
        /* (Different from new user handling when database doesn't exist, I think there's a higher chance
         *  of malicious user impersonating someone if it's allowed here than there). */
        if (!cdb)
        {
            client_socket->close();
            return true;
        }
        SP<User> user = cdb->loadUserData(nickname);
        /* The user *should* exist, disconnect the client if it doesn't. */
        if (!user)
        {
            client_socket->close();
            return true;
        }
        password_enter_time = nanoclock();

        if (!user->verifyPassword(password1))
        {
            /* Invalid password! */
            identify_window->modifyListElementText(password1_index, "");
            return true;
        }

        this->user = user;
        clientIdentified();
        return true;
    }
    else if (select_type == "new_password1")
        identify_window->modifyListSelectionIndex(identify_window->getListSelectionIndex()+1);
    else if (select_type == "new_password2")
    {
        /* Check that passwords match. */
        UnicodeString password1 = identify_window->getListElement(password1_index);
        UnicodeString password2 = identify_window->getListElement(password2_index);
        if (password1 != password2)
        {
            identify_window->modifyListSelectionIndex(password1_index);
            identify_window->modifyListElementTextUTF8(password1_index, "");
            identify_window->modifyListElementTextUTF8(password2_index, "");
            return true;
        }
        /* Empty passwords? Make non-permanent login. */
        if (password1.countChar32() == 0)
        {
            clientIdentified();
            return true;
        }

        /* User wants to be a permanent user. */
        /* FINE! Let's create them and add them to database */
        if (!user) user = SP<User>(new User);

        SP<ConfigurationDatabase> cdb = configuration.lock();
        if (!cdb)
        {
            /* Oops, no configuration database? Should not happen at this point. */
            /* Set safe values for user and just log them in. */
            user->setPasswordSalt("");
            user->setPasswordHash("");
            user->setAdmin(false);
            clientIdentified();
            return true;
        }

        unsigned char random_salt[8];
        makeRandomBytes(random_salt, 8);
        user->setPasswordSalt(bytes_to_hex(data1D((char*) random_salt, 8)));

        user->setName(nickname);
        user->setAdmin(false);
        user->setPassword(password1);

        cdb->saveUserData(user);
        clientIdentified();
    }
    

    return true;
}

bool Client::chatSelectFunction(ui32 index)
{
    UnicodeString chat_message = chat_window->getListElement(chat_window_input_index);
    if (chat_message.countChar32() == 0) return false;

    chat_window->modifyListElementText(chat_window_input_index, "");
    chat_window->modifyListSelectionIndex(chat_window_input_index);

    UnicodeString prefix;
    prefix = UnicodeString::fromUTF8("<") +
             nickname +
             UnicodeString::fromUTF8("> ");
    global_chat->logMessage(prefix + chat_message);
    return false;
};

/* Callback for game window input */
void Client::gameInputFunction(ui32 keycode, bool special_key)
{
    SP<Slot> sp_slot = slot.lock();
    if (sp_slot)
        sp_slot->feedInput(keycode, special_key);
}

/* Used as a callback function for element window input. */
bool Client::chatRestrictFunction(ui32* keycode, ui32* cursor)
{
    /* Don't allow cursor to move before 6 characters. */
    //if ((*cursor) < 6) { (*cursor) = 6; return false; };
    if ((*keycode) == 0) return true;

    /* Restrict minimum length to 6 characters. (length of "Chat> ") */
    UnicodeString us = chat_window->getListElement(chat_window_input_index);

    return true;
}

void Client::updateNicklistWindowForAll()
{
    vector<WP<Client> >::iterator i1;
    for (i1 = clients->begin(); i1 != clients->end(); i1++)
    {
        SP<Client> c = (*i1).lock();
        if (c)
            c->updateNicklistWindow();
    }
}

void Client::updateNicklistWindow()
{
    if (!nicklist_window) return;

    ui32 nicklist_window_index = nicklist_window->getListSelectionIndex();
    nicklist_window->deleteAllListElements();
    if (!clients) return;

    vector<UnicodeString> nicks;
    nicks.reserve(clients->size());

    vector<WP<Client> >::iterator i1;
    for (i1 = clients->begin(); i1 != clients->end(); i1++)
    {
        SP<Client> c = (*i1).lock();
        if (!c)
        {
            clients->erase(i1);
            i1--;
            continue;
        };
        UnicodeString us = c->nickname;
        if (us.countChar32() == 0) continue;

        nicks.push_back(c->nickname);
    }
    sort(nicks.begin(), nicks.end());

    vector<UnicodeString>::iterator i2;
    bool found_index = false;
    int index = 0;
    for (i2 = nicks.begin(); i2 != nicks.end(); i2++)
    {
        index = nicklist_window->addListElement((*i2), "", true);
        if (index == nicklist_window_index) found_index = true;
    }
    if (!found_index)
        nicklist_window->modifyListSelectionIndex(index);
}

void Client::clientIdentified()
{
    game_window = interface->createInterface2DWindow();
    nicklist_window = interface->createInterfaceElementWindow();
    chat_window = interface->createInterfaceElementWindow();

    chat_window->setHint("chat");

    /* Set callbacks for chat window */
    function2<bool, ui32*, ui32*> bound_function = bind(&Client::chatRestrictFunction, this, _1, _2);
    chat_window->setInputCallback(bound_function);
    function1<bool, ui32> bound_select_callback = bind(&Client::chatSelectFunction, this, _1);
    chat_window->setListCallback(bound_select_callback);

    /* And for game window */
    function2<void, ui32, bool> bound_function2 = bind(&Client::gameInputFunction, this, _1, _2);
    game_window->setInputCallback(bound_function2);

    /* 10x10 is small, and a good default. */
    game_window->setMinimumSize(10, 10);
    game_window->setTitle("Game");

    chat_window_input_index = chat_window->addListElementUTF8("", "Chat> ", "chat", true, true);
    chat_window->modifyListSelectionIndex(chat_window_input_index);
    chat_window->setTitle("Chat");

    config_window = config_interface->getUserWindow();
    user->setName(nickname);
    config_interface->setUser(user);

    identified = true;

    nicklist_window->setTitle("Local players");
    nicklist_window->setHint("nicklist");

    updateNicklistWindowForAll();
}

void Client::setClientVector(vector<WP<Client> >* clients)
{
    this->clients = clients;
}

void Client::setConfigurationDatabase(WP<ConfigurationDatabase> configuration_database)
{
    configuration = configuration_database;
    config_interface->setConfigurationDatabase(configuration.lock());
}

bool Client::shouldShutdown() const
{
    if (config_interface->shouldShutdown()) return true;
    return false;
}

