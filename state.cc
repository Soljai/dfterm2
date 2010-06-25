#include "state.hpp"
#include <iostream>
#include <sstream>
#include "nanoclock.hpp"
#include "logger.hpp"

using namespace dfterm;
using namespace std;
using namespace boost;
using namespace trankesbel;

static bool state_initialized = false;

static ui64 running_counter = 1;

State::State()
{
    state_initialized = true;
    global_chat = SP<Logger>(new Logger);
    ticks_per_second = 20;
    close = false;
};

State::~State()
{
    state_initialized = false;
};

WP<SlotProfile> State::getSlotProfile(UnicodeString name)
{
    lock_guard<recursive_mutex> lock(slotprofiles_mutex);

    vector<SP<SlotProfile> >::iterator i1;
    for (i1 = slotprofiles.begin(); i1 != slotprofiles.end(); i1++)
        if ((*i1) && (*i1)->getName() == name)
            return (*i1);
    return WP<SlotProfile>();
}
    
WP<Slot> State::getSlot(UnicodeString name)
{
    lock_guard<recursive_mutex> lock(slots_mutex);

    vector<SP<Slot> >::iterator i1;
    for (i1 = slots.begin(); i1 != slots.end(); i1++)
        if ((*i1) && (*i1)->getName() == name)
            return (*i1);
    return WP<Slot>();
}

vector<WP<Slot> > State::getSlots()
{
    lock_guard<recursive_mutex> lock(slots_mutex);

    vector<WP<Slot> > wp_slots;
    vector<SP<Slot> >::iterator i1;
    for (i1 = slots.begin(); i1 != slots.end(); i1++)
        wp_slots.push_back(*i1);
    return wp_slots;
}

vector<WP<SlotProfile> > State::getSlotProfiles()
{
    lock_guard<recursive_mutex> lock(slotprofiles_mutex);

    vector<WP<SlotProfile> > wp_slots;
    vector<SP<SlotProfile> >::iterator i1;
    for (i1 = slotprofiles.begin(); i1 != slotprofiles.end(); i1++)
        wp_slots.push_back(*i1);
    return wp_slots;
}

SP<State> State::createState()
{
    if (state_initialized) return SP<State>();
    SP<State> newstate(new State);
    newstate->self = newstate;

    return newstate;
};

bool State::setDatabase(UnicodeString database_file)
{
	return setDatabaseUTF8(TO_UTF8(database_file));
}

bool State::setDatabaseUTF8(string database_file)
{
    unique_lock<recursive_mutex> clock(configuration_mutex);

    /* Configuration */
    configuration = SP<ConfigurationDatabase>(new ConfigurationDatabase);
    OpenStatus d_result = configuration->openUTF8(database_file);
    if (d_result == Failure)
    {
        LOG(Error, "Failed to open database file " << database_file);
        return false;
    }
    if (d_result == OkCreatedNewDatabase)
    {
        LOG(Note, "Created a new database from scratch. You should add an admin account to configure dfterm2.");
        LOG(Note, "You need to use the command line tool dfterm2_configure for that. Close dfterm2 and then");
        LOG(Note, "add an account like this: ");
        LOG(Note, "dfterm2_configure --adduser (user name) (password) admin");
        LOG(Note, "For example:");
        LOG(Note, "dfterm2_configure --adduser Adeon s3cr3t_p4ssw0rd admin");
        LOG(Note, "This will create a new admin account for you.");
        LOG(Note, "If you are not using the default database (if you don't know then you are using it), use");
        LOG(Note, "the --database switch to modify the correct database.");
        return true;
    }
    clock.unlock();

    unique_lock<recursive_mutex> lock(slots_mutex);
    slots.clear();
    lock.unlock();

    lock_guard<recursive_mutex> lock2(slotprofiles_mutex);
    slotprofiles.clear();

    vector<UnicodeString> profile_list = configuration->loadSlotProfileNames();
    vector<UnicodeString>::iterator i1;
    for (i1 = profile_list.begin(); i1 != profile_list.end(); i1++)
    {
        SP<SlotProfile> sp = configuration->loadSlotProfileData(*i1);
        if (!sp) continue;

        addSlotProfile(sp);
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
        LOG(Error, "Listening on telnet service " << address.getHumanReadablePlainUTF8() << " failed.");
        return false;
    }
    LOG(Note, "Telnet service started on address " << address.getHumanReadablePlainUTF8());
    listening_sockets.insert(s);

    socketevents.addSocket(s);

    return true;
}

void State::setTicksPerSecond(::uint64_t ticks_per_second)
{
    this->ticks_per_second = ticks_per_second;
}

void State::destroyClient(UnicodeString nickname, SP<Client> exclude)
{
    lock_guard<recursive_mutex> lock(clients_mutex);

    bool update_nicklists = false;

    size_t i1, len = clients.size();
    for (i1 = 0; i1 < len; i1++)
    {
        if (clients[i1] == exclude) continue;

        if (clients[i1] && clients[i1]->getUser()->getName() == nickname)
        {
            clients.erase(clients.begin() + i1);
            clients_weak.erase(clients_weak.begin() + i1);
            LOG(Note, "Disconnected a duplicate connection for user " << clients[i1]->getUser()->getNameUTF8());
            update_nicklists = true;
            break;
        }
    }

    if (!update_nicklists) return;

    len = clients.size();
    for (i1 = 0; i1 < len; i1++)
        if (clients[i1])
            clients[i1]->updateClients();
}

void State::addSlotProfile(SP<SlotProfile> sp)
{
    slotprofiles.push_back(sp);
};

void State::deleteSlotProfile(SP<SlotProfile> slotprofile)
{
    if (!slotprofile) return;

    size_t i1, len = slots.size();
    for (i1 = 0; i1 < len; i1++)
    {
        if (!slots[i1]) continue;

        SP<SlotProfile> sp = slots[i1]->getSlotProfile().lock();
        if (!sp || slotprofile != sp) continue;

        slots.erase(slots.begin() + i1);
        len--;
        i1--;
    };

    len = slotprofiles.size();
    for (i1 = 0; i1 < len; i1++)
    {
        if (slotprofiles[i1] == slotprofile)
        {
            slotprofiles.erase(slotprofiles.begin() + i1);
            i1--;
            len--;
        }
    }
}

void State::updateSlotProfile(SP<SlotProfile> target, const SlotProfile &source)
{
    if (!target) return;

    (*target.get()) = source;

    size_t i1, len = slots.size();
    for (i1 = 0; i1 < len; i1++)
    {
        if (!slots[i1]) continue;

        SP<SlotProfile> slotprofile = slots[i1]->getSlotProfile().lock();
        if (!slotprofile) continue;

        size_t i2, len2 = clients.size();
        for (i2 = 0; i2 < len2; i2++)
        {
            if (!clients[i2]) continue;

            if (clients[i2]->getSlot().lock() != slots[i1])
                continue;

            SP<User> user = clients[i2]->getUser();
            if (!user) continue;

            if (!isAllowedWatcher(user, slots[i1]))
                clients[i2]->setSlot(SP<Slot>());
        }
    }
}

bool State::hasSlotProfile(UnicodeString name)
{
    if (name.countChar32() == 0) return true;

    vector<SP<SlotProfile> >::iterator i1;
    for (i1 = slotprofiles.begin(); i1 != slotprofiles.end(); i1++)
        if ((*i1)->getName() == name) return true;
    return false;
}

bool State::isAllowedLauncher(SP<User> launcher, SP<SlotProfile> slot_profile)
{
    UserGroup allowed_launchers = slot_profile->getAllowedLaunchers();
    UserGroup forbidden_launchers = slot_profile->getForbiddenLaunchers();
    
    if (forbidden_launchers.hasUser(launcher->getName()))
        return false;
    if (!allowed_launchers.hasUser(launcher->getName()) && !allowed_launchers.hasLauncher())
        return false;
    return true;
}

bool State::isAllowedPlayer(SP<User> user, SP<Slot> slot)
{
    bool not_allowed_by_being_launcher = false;
    SP<SlotProfile> sp_slotprofile = slot->getSlotProfile().lock();
    if (!sp_slotprofile)
    {
        LOG(Error, "State::isAllowedPlayer(), no slot profile associated with slot " << slot->getNameUTF8());
        return false;
    }

    UserGroup allowed_players = sp_slotprofile->getAllowedPlayers();
    UserGroup forbidden_players = sp_slotprofile->getForbiddenPlayers();
    SP<User> launcher = slot->getLauncher().lock();
    if (launcher && launcher == user)
    {
        if (forbidden_players.hasLauncher())
            return false;
        if (!allowed_players.hasLauncher())
            not_allowed_by_being_launcher = true;
    }
    if (forbidden_players.hasUser(user->getName()))
        return false;
    if (!allowed_players.hasUser(user->getName()) && (not_allowed_by_being_launcher || launcher != user))
        return false;
    return true;
};

bool State::isAllowedWatcher(SP<User> user, SP<Slot> slot)
{
    bool not_allowed_by_being_launcher = false;
    SP<SlotProfile> sp_slotprofile = slot->getSlotProfile().lock();
    if (!sp_slotprofile)
    {
        LOG(Error, "State::isAllowedWatcher(), no slot profile associated with slot " << slot->getNameUTF8());
        return false;
    }

    UserGroup allowed_watchers = sp_slotprofile->getAllowedWatchers();
    UserGroup forbidden_watchers = sp_slotprofile->getForbiddenWatchers();
    SP<User> launcher = slot->getLauncher().lock();
    if (launcher && launcher == user)
    {
        if (forbidden_watchers.hasLauncher())
            return false;
        if (!allowed_watchers.hasLauncher())
            not_allowed_by_being_launcher = true;
    }
    if (forbidden_watchers.hasUser(user->getName()))
        return false;
    if (!allowed_watchers.hasUser(user->getName()) && (not_allowed_by_being_launcher || launcher != user))
        return false;
    return true;
};

bool State::setUserToSlot(SP<User> user, UnicodeString slot_name)
{
    /* Find the user from client list */
    SP<Client> client;
    vector<SP<Client> >::iterator i1;
    for (i1 = clients.begin(); i1 != clients.end(); i1++)
        if ( (*i1)->getUser() == user)
        {
            client = (*i1);
            break;
        }

    string slot_name_utf8 = TO_UTF8(slot_name_utf8);

    if (!client)
    {
        LOG(Error, "User " << user->getNameUTF8() << " attempted to watch slot " << slot_name_utf8 << " but there is no associated client connected.");
        return false;
    }

    client->setSlot(SP<Slot>());

    if (slot_name.countChar32() == 0) return true;

    WP<Slot> slot = getSlot(slot_name);
    SP<Slot> sp_slot = slot.lock();
    if (!sp_slot)
    {
        LOG(Error, "Slot join requested by " << user->getNameUTF8() << " from interface but no such slot is in state. Slot name " << slot_name_utf8);
        return false;
    }

    SP<SlotProfile> sp_slotprofile = sp_slot->getSlotProfile().lock();
    if (!sp_slotprofile)
    {
        LOG(Error, "Slot join requested by " << user->getNameUTF8() << " from interface but the slot has no slot profile associated with it. Slot name " << slot_name_utf8);
        return false;
    }

    /* Check if this client is allowed to watch this. */
    if (!isAllowedWatcher(user, sp_slot))
    {
        LOG(Error, "Slot join requested by " << user->getNameUTF8() << " but they are not allowed to do that. Slot name " << slot_name_utf8);
        return false;
    }

    client->setSlot(sp_slot);
    LOG(Note, "User " << user->getNameUTF8() << " is now watching slot " << slot_name_utf8);
    return true;
}

bool State::launchSlotNoCheck(SP<SlotProfile> slot_profile, SP<User> launcher)
{
    lock_guard<recursive_mutex> lock(slotprofiles_mutex);
    lock_guard<recursive_mutex> lock2(slots_mutex);

    if (!launcher) launcher = SP<User>(new User);

    if (!isAllowedLauncher(launcher, slot_profile))
    {
        LOG(Error, "User " << launcher->getNameUTF8() << " attempted to launch a slot from slot profile " << slot_profile->getNameUTF8() << " but they are not allowed to do that.");
        return false;
    }

    /* Check that there are not too many slots of this slot profile */
    ui32 num_slots = 0;
    vector<SP<Slot> >::iterator i1;
    for (i1 = slots.begin(); i1 != slots.end(); i1++)
        if ((*i1) && (*i1)->getSlotProfile().lock() == slot_profile)
            num_slots++;
    if (num_slots >= slot_profile->getMaxSlots())
    {
        LOG(Error, "User " << launcher->getNameUTF8() << " attempted to launch a slot but maximum number of slots of this slot profile has been reached. Slot profile name " << slot_profile->getNameUTF8());
        return false;
    }

    stringstream rcs;
    rcs << running_counter;
    running_counter++;

    SP<Slot> slot = Slot::createSlot(slot_profile->getSlotType());
    slot->setState(self);
    slot->setSlotProfile(slot_profile);
    slot->setLauncher(launcher);
    string name_utf8 = slot_profile->getNameUTF8() + string(" - ") + launcher->getNameUTF8() + string(":") + rcs.str();
    slot->setNameUTF8(name_utf8);
    if (!slot)
    {
        LOG(Error, "Slot::createSlot() failed with slot profile " << slot_profile->getNameUTF8());
        return false;
    }
    slot->setParameter("path", slot_profile->getExecutable());
    slot->setParameter("work", slot_profile->getWorkingPath());

    stringstream ss_w, ss_h;
    ss_w << slot_profile->getWidth();
    ss_h << slot_profile->getHeight();

    slot->setParameter("w", UnicodeString::fromUTF8(ss_w.str()));
    slot->setParameter("h", UnicodeString::fromUTF8(ss_h.str()));

    LOG(Note, "Launched a slot from slot profile " << slot_profile->getNameUTF8());

    slots.push_back(slot);

    /* Put the user to watch the just launched slot */
    setUserToSlotUTF8(launcher, name_utf8);

    return true;
}

bool State::launchSlot(SP<SlotProfile> slot_profile, SP<User> launcher)
{
    lock_guard<recursive_mutex> lock(slotprofiles_mutex);

    if (!slot_profile)
    {
        if (launcher)
		    { LOG(Error, "User " << launcher->getNameUTF8() << " attempted to launch a null slot profile."); }
        else
		    { LOG(Error, "Null user attempted to launch a null slot profile."); }
        return false;
    }

    vector<SP<SlotProfile> >::iterator i1;
    for (i1 = slotprofiles.begin(); i1 != slotprofiles.end(); i1++)
        if ((*i1) == slot_profile)
            return launchSlotNoCheck(*i1, launcher);
    if (launcher)
	    { LOG(Error, "User " << launcher->getNameUTF8() << " attempted to launch a slot profile that does not exist in slot profile list."); }
    else
	    { LOG(Error, "Null user attempted to launch a slot profile that does not exist in slot profile list."); }
    return false;
}

bool State::launchSlot(UnicodeString slot_profile_name, SP<User> launcher)
{
    vector<SP<SlotProfile> >::iterator i1;
    for (i1 = slotprofiles.begin(); i1 != slotprofiles.end(); i1++)
        if ((*i1) && (*i1)->getName() == slot_profile_name)
            return launchSlotNoCheck(*i1, launcher);
    string utf8_name = TO_UTF8(slot_profile_name);
    if (launcher)
	    { LOG(Error, "User " << launcher->getNameUTF8() << " attempted to launch a slot profile with name " << utf8_name << " that does not exist in slot profile list."); }
    else
	    { LOG(Error, "Null user attempted to launch a slot profile with name " << utf8_name << " that does not exist in slot profile list."); }
    return false;
}

void State::notifyAllClients()
{
    lock_guard<recursive_mutex> lock(clients_mutex);
    vector<SP<Client> >::iterator i1;
    for (i1 = clients.begin(); i1 != clients.end(); i1++)
        if (*i1 && (*i1)->getSocket())
            socketevents.forceEvent((*i1)->getSocket());
}

void State::notifyClient(SP<Client> client)
{
    if (!client) return;

    lock_guard<recursive_mutex> lock(clients_mutex);
    vector<SP<Client> >::iterator i1;
    for (i1 = clients.begin(); i1 != clients.end(); i1++)
        if ((*i1) == client)
        {
            socketevents.forceEvent(client->getSocket());
            return;
        }

}

void State::notifyClient(SP<User> user)
{
    if (!user) return;

    lock_guard<recursive_mutex> lock(clients_mutex);
    vector<SP<Client> >::iterator i1;
    for (i1 = clients.begin(); i1 != clients.end(); i1++)
        if ((*i1) && (*i1)->getUser()->getNameUTF8() == user->getNameUTF8())
        {
            socketevents.forceEvent((*i1)->getSocket());
            return;
        }
}

void State::notifyClient(SP<Socket> socket)
{
    socketevents.forceEvent(socket);
}

void State::pruneInactiveSlots()
{
    lock_guard<recursive_mutex> lock(slots_mutex);

    /* Prune inactive slots */
    size_t i2, len = slots.size();
    for (i2 = 0; i2 < len; i2++)
    {
        if (!slots[i2] || !slots[i2]->isAlive())
        {
            if (!slots[i2])
			    { LOG(Note, "Removed a null slot from slot list."); }
            else
			    { LOG(Note, "Removed slot " << slots[i2]->getNameUTF8() << " from slot list."); }
                
            unique_lock<recursive_mutex> lock2(clients_mutex);
            vector<SP<Client> >::iterator i1;
            for (i1 = clients.begin(); i1 != clients.end(); i1++)
            {
                if (!(*i1)) continue;

                if ((*i1)->getSlot().lock() == slots[i2])
                {
                    (*i1)->setSlot(SP<Slot>());
                    notifyClient((*i1)->getSocket());
                }
            }
            lock2.unlock();

            slots.erase(slots.begin() + i2);
            len--;
            i2--;
            continue;
        }
    }
}

void State::pruneInactiveClients()
{
    lock_guard<recursive_mutex> lock(clients_mutex);

    bool changes = false;

    /* Prune inactive clients */
    size_t i2, len = clients.size();
    for (i2 = 0; i2 < len; i2++)
    {
        if (!clients[i2]->isActive())
        {
            clients.erase(clients.begin() + i2);
            clients_weak.erase(clients_weak.begin() + i2);
            len--;
            i2--;
            LOG(Note, "Pruned an inactive connection.");
            changes = true;
            continue;
        }
    }

    if (!changes) return;

    len = clients.size();
    for (i2 = 0; i2 < len; i2++)
        clients[i2]->updateClients();
    notifyAllClients();
}

void State::client_signal_function(WP<Client> client, SP<Socket> from_where)
{
    SP<Client> sp_cli = client.lock();
    if (!sp_cli || !from_where || !from_where->active()) return;

    sp_cli->cycle();
    if (sp_cli->shouldShutdown()) close = true;
}

void State::new_connection(SP<Socket> listening_socket)
{
    /* Check for incoming connections. */
    SP<Socket> new_connection(new Socket);
    bool got_connection = listening_socket->accept(new_connection.get());
    if (got_connection)
    {
        SP<Client> new_client = Client::createClient(new_connection);
        new_client->setState(self);
        new_client->setConfigurationDatabase(configuration);
        new_client->setGlobalChatLogger(global_chat);

        lock_guard<recursive_mutex> lock(clients_mutex);

        clients.push_back(new_client);
        clients_weak.push_back(new_client);
        new_client->setClientVector(&clients_weak);
        LOG(Note, "New connection from " << new_connection->getAddress().getHumanReadablePlainUTF8());

        socketevents.addSocket(new_connection);
        size_t i1, len = clients.size();
        for (i1 = 0; i1 < len; i1++)
            if (clients[i1])
                clients[i1]->updateClients();
    }
}

void State::loop()
{
    /* Use these for timing ticks */
    ::uint64_t start_time;
    const ::uint64_t tick_time = 1000000000 / ticks_per_second;

    close = false;
    while(!close)
    {
        pruneInactiveClients();
        pruneInactiveSlots();
        flush_messages();
        SP<Socket> s = socketevents.getEvent(500000000);
        if (!s) continue;

        /* Test if it's a listening socket */
        set<SP<Socket> >::iterator i2 = listening_sockets.find(s);
        if (i2 != listening_sockets.end())
        {
            new_connection(s);
            continue;
        }

        lock_guard<recursive_mutex> lock(clients_mutex);
        vector<SP<Client> >::iterator i1;
        for (i1 = clients.begin(); i1 != clients.end(); i1++)
        {
            if (!(*i1)) continue;
            if ((*i1)->getSocket() == s)
            {
                (*i1)->cycle();
                if ((*i1)->shouldShutdown()) close = true;
                break;
            }
        }
    }
}

void State::signalSlotData(SP<Slot> who)
{
    lock_guard<recursive_mutex> lock(clients_mutex);
    vector<SP<Client> >::iterator i1;
    for (i1 = clients.begin(); i1 != clients.end(); i1++)
    {
        if (!(*i1)) continue;

        if ((*i1)->getSlot().lock() == who)
            (*i1)->cycle();
    }
}

