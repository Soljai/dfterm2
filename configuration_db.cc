#include "configuration_db.hpp"
#include <string>
#include <cstdio>
#include <iostream>
#include <boost/bind.hpp>
#include <openssl/sha.h>
#include "hash.hpp"
#include "sqlite3.h"
#include <vector>
#include "configuration_primitives.hpp"
#include <boost/function.hpp>
#include "slot.hpp"

using namespace dfterm;
using namespace std;
using namespace boost;

ConfigurationDatabase::ConfigurationDatabase()
{
    db = (sqlite3*) 0;
}

ConfigurationDatabase::~ConfigurationDatabase()
{
    if (db)
        sqlite3_close(db);
    db = (sqlite3*) 0;
}

OpenStatus ConfigurationDatabase::open(const UnicodeString &filename)
{
    if (db) sqlite3_close(db);
    db = (sqlite3*) 0;

    string filename_utf8 = TO_UTF8(filename);

    /* Test if the database exists on the disk. */
    bool database_exists = true;
    int result = sqlite3_open_v2(filename_utf8.c_str(), &db, SQLITE_OPEN_READONLY, (const char*) 0);
    if (result)
        database_exists = false;
    else
        sqlite3_close(db);
    db = (sqlite3*) 0;

    result = sqlite3_open(filename_utf8.c_str(), &db);
    if (result)
        return Failure;

    /* Create tables. These calls fail if they already exist. */
    result = sqlite3_exec(db, "CREATE TABLE Users(Name TEXT, PasswordSHA512 TEXT, PasswordSalt TEXT, Admin TEXT);", 0, 0, 0);
    result = sqlite3_exec(db, "CREATE TABLE Slotprofiles(Name TEXT, Width TEXT, Height TEXT, Path TEXT, WorkingPath TEXT, SlotType TEXT, AllowedWatchers TEXT, AllowedLaunchers TEXT, AllowedPlayers TEXT, ForbiddenWatchers TEXT, ForbiddenLaunchers TEXT, ForbiddenPlayers TEXT, MaxSlots TEXT);", 0, 0, 0);
    /* Create an admin user, if database was created. */
    if (!database_exists)
        return OkCreatedNewDatabase;

    return Ok;
}

int ConfigurationDatabase::slotprofileNameListDataCallback(vector<UnicodeString>* name_list, void* v_self, int argc, char** argv, char** colname)
{
    if (!name_list) return 0;

    int i;
    for (i = 0; i < argc; i++)
    {
        if (!argv[i]) continue;
        
        if (!strcmp(colname[i], "Name"))
        {
            string name_utf8 = argv[i];
            (*name_list).push_back(UnicodeString::fromUTF8(name_utf8));
        }
    }

    return 0;
}

int ConfigurationDatabase::slotprofileDataCallback(SlotProfile* sp, void* v_self, int argc, char** argv, char** colname)
{
    if (!sp) return 0;

    int i;
    for (i = 0; i < argc; i++)
    {
        if (!argv[i]) continue;

        if (!strcmp(colname[i], "Name"))
            sp->setNameUTF8(argv[i]);
        else if (!strcmp(colname[i], "Width"))
            sp->setWidth(strtol(argv[i], NULL, 10));
        else if (!strcmp(colname[i], "Height"))
            sp->setHeight(strtol(argv[i], NULL, 10));
        else if (!strcmp(colname[i], "Path"))
            sp->setExecutable(argv[i]);
        else if (!strcmp(colname[i], "WorkingPath"))
            sp->setWorkingPath(argv[i]);
        else if (!strcmp(colname[i], "SlotType"))
        {
            SlotType st = InvalidSlotType;
            for (st = (SlotType) 0; st != InvalidSlotType; st = (SlotType) ((size_t) st + 1))
                if (!strcmp(argv[i], SlotNames[(size_t) st].c_str()))
                    break;
            sp->setSlotType(st);
        }
        else if (!strcmp(colname[i], "AllowedWatchers"))
            sp->setAllowedWatchers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "AllowedLaunchers"))
            sp->setAllowedLaunchers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "AllowedPlayers"))
            sp->setAllowedPlayers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "ForbiddenWatchers"))
            sp->setForbiddenWatchers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "ForbiddenLaunchers"))
            sp->setForbiddenLaunchers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "ForbiddenPlayers"))
            sp->setForbiddenPlayers(UserGroup::unSerialize(argv[i]));
        else if (!strcmp(colname[i], "MaxSlots"))
            sp->setMaxSlots(strtol(argv[i], NULL, 10));
    }

    return 0;
};

int ConfigurationDatabase::userDataCallback(string* name, string* password_hash, string* password_salt, bool* admin, void* v_self, int argc, char** argv, char** colname)
{
    (*name).clear();
    (*password_hash).clear();
    (*password_salt).clear();
    (*admin) = false;

    int i;
    for (i = 0; i < argc; i++)
    {
        if (!argv[i]) continue;

        if (!strcmp(colname[i], "Name"))
            (*name) = argv[i];
        else if (!strcmp(colname[i], "PasswordSHA512"))
            (*password_hash) = argv[i];
        else if (!strcmp(colname[i], "PasswordSalt"))
            (*password_salt) = argv[i];
        else if (!strcmp(colname[i], "Admin"))
            if (!strcmp(argv[i], "Yes"))
                (*admin) = true;
    }

    return 0;
};

int ConfigurationDatabase::userListDataCallback(vector<SP<User> >* user_list, void* v_self, int argc, char** argv, char** colname)
{
    string name, password_hash, password_salt;
    bool admin = false;

    int i;
    for (i = 0; i < argc; i++)
    {
        if (!argv[i]) continue;

        if (!strcmp(colname[i], "Name"))
            name = argv[i];
        else if (!strcmp(colname[i], "PasswordSHA512"))
            password_hash = argv[i];
        else if (!strcmp(colname[i], "PasswordSalt"))
            password_salt = argv[i];
        else if (!strcmp(colname[i], "Admin"))
            if (!strcmp(argv[i], "Yes"))
                admin = true;
    }

    if (name.size() <= 0) return 0;

    SP<User> user(new User);
    user->setPasswordSalt(password_salt);
    user->setPasswordHash(password_hash);
    user->setName(UnicodeString::fromUTF8(name));
    user->setAdmin(admin);

    (*user_list).push_back(user);
    return 0;
};

static int c_callback(void* a, int b, char** c, char** d)
{
    function4<int, void*, int, char**, char**>* sql_callback_function = 
    (function4<int, void*, int, char**, char**>*) a;

    return (*sql_callback_function)((void*) 0, b, c, d);
}

vector<SP<User> > ConfigurationDatabase::loadAllUserData()
{
    if (!db) return vector<SP<User> >();

    vector<SP<User> > result_users;

    function4<int, void*, int, char**, char**> sql_callback_function;
    sql_callback_function = boost::bind(&ConfigurationDatabase::userListDataCallback, this, &result_users, _1, _2, _3, _4);

    string statement;
    statement = string("SELECT Name, PasswordSHA512, PasswordSalt, Admin FROM Users;");
    int result = sqlite3_exec(db, statement.c_str(), c_callback, (void*) &sql_callback_function, 0);
    if (result != SQLITE_OK) return vector<SP<User> >();

    return result_users;
}

void ConfigurationDatabase::deleteSlotProfileData(const UnicodeString &name)
{
    if (!db) return;

    string name_utf8 = TO_UTF8(name);
    if (escape_sql_string(name_utf8).size() < 1) return;

    string statement = string("DELETE FROM Slotprofiles WHERE Name = \'") + escape_sql_string(name_utf8) + string("\';"); 
    sqlite3_exec(db, statement.c_str(), 0, 0, 0);
}

void ConfigurationDatabase::deleteUserData(const UnicodeString &name)
{
    if (!db) return;

    string name_utf8 = TO_UTF8(name);
    if (escape_sql_string(name_utf8).size() < 1) return;

    string statement = string("DELETE FROM Users WHERE Name = \'") + escape_sql_string(name_utf8) + string("\';"); 
    sqlite3_exec(db, statement.c_str(), 0, 0, 0);
}

SP<User> ConfigurationDatabase::loadUserData(const UnicodeString &name)
{
    if (!db) return SP<User>();

    string name_utf8 = TO_UTF8(name);
    if (escape_sql_string(name_utf8).size() < 1) return SP<User>();

    string r_name, r_password_hash, r_password_salt;
    bool r_admin = false;

    function4<int, void*, int, char**, char**> sql_callback_function;
    sql_callback_function = boost::bind(&ConfigurationDatabase::userDataCallback, this, &r_name, &r_password_hash, &r_password_salt, &r_admin, _1, _2, _3, _4);

    string statement;
    statement = string("SELECT Name, PasswordSHA512, PasswordSalt, Admin FROM Users WHERE Name = \'") + escape_sql_string(name_utf8) + string("\';");
    int result = sqlite3_exec(db, statement.c_str(), c_callback, (void*) &sql_callback_function, 0);
    if (result != SQLITE_OK) return SP<User>();
    if (r_name.size() == 0) return SP<User>();

    SP<User> user(new User);
    user->setPasswordSalt(r_password_salt);
    user->setPasswordHash(r_password_hash);
    user->setName(UnicodeString::fromUTF8(r_name));
    user->setAdmin(r_admin);

    return user;
}

void ConfigurationDatabase::saveUserData(User* user)
{
    if (!db) return;
    if (!user) return;

    string name_utf8 = TO_UTF8(user->getName());
    if (escape_sql_string(name_utf8).size() < 1) return;

    string statement;
    statement = string("DELETE FROM Users WHERE Name = \'") + escape_sql_string(name_utf8) + string("\';"); 
    int result = sqlite3_exec(db, statement.c_str(), 0, 0, 0);

    string admin_str("No");
    if (user->isAdmin()) admin_str = "Yes";
        
    statement = string("INSERT INTO Users(Name, PasswordSHA512, PasswordSalt, Admin) VALUES(\'") + escape_sql_string(name_utf8) + string("\', \'") + escape_sql_string(user->getPasswordHash()) + string("\', \'") + escape_sql_string(user->getPasswordSalt()) + string("\', \'") + admin_str + string("\');");
    result = sqlite3_exec(db, statement.c_str(), 0, 0, 0);
};

void ConfigurationDatabase::saveSlotProfileData(SlotProfile* slotprofile)
{
    if (!db) return;
    if (!slotprofile) return;

    string name = slotprofile->getNameUTF8();

    string statement;
    statement = string("DELETE FROM Slotprofiles WHERE NAME = \'") + escape_sql_string(name) + string("\';");
    int result = sqlite3_exec(db, statement.c_str(), 0, 0, 0);

    stringstream ss;
    ss << "INSERT INTO Slotprofiles(Name, Width, Height, Path, WorkingPath, SlotType, AllowedWatchers, AllowedLaunchers, AllowedPlayers, ForbiddenWatchers, ForbiddenLaunchers, ForbiddenPlayers, MaxSlots) VALUES(\'" << 
    escape_sql_string(slotprofile->getNameUTF8()) << "\',\'" << 
    slotprofile->getWidth() << "\',\'" <<
    slotprofile->getHeight() << "\',\'" <<
    escape_sql_string(slotprofile->getExecutableUTF8()) << "\',\'" <<
    escape_sql_string(slotprofile->getWorkingPathUTF8()) << "\',\'" <<
    escape_sql_string(SlotNames[(size_t) slotprofile->getSlotType()]) << "\',\'" <<
    escape_sql_string(slotprofile->getAllowedWatchers().serialize()) << "\',\'" <<
    escape_sql_string(slotprofile->getAllowedLaunchers().serialize()) << "\',\'" <<
    escape_sql_string(slotprofile->getAllowedPlayers().serialize()) << "\',\'" <<
    escape_sql_string(slotprofile->getForbiddenWatchers().serialize()) << "\',\'" <<
    escape_sql_string(slotprofile->getForbiddenLaunchers().serialize()) << "\',\'" <<
    escape_sql_string(slotprofile->getForbiddenPlayers().serialize()) << "\',\'" <<
    slotprofile->getMaxSlots() << "\');";

    char* errormsg = (char*) 0;
    result = sqlite3_exec(db, ss.str().c_str(), 0, 0, &errormsg);
    if (result != SQLITE_OK)
        LOG(Error, "Error while executing SQL statement \"" << ss.str() << "\": " << errormsg);
    if (errormsg) sqlite3_free(errormsg);
};

vector<UnicodeString> ConfigurationDatabase::loadSlotProfileNames()
{
    if (!db) return vector<UnicodeString>();

    vector<UnicodeString> name_list;

    function4<int, void*, int, char**, char**> sql_callback_function;
    sql_callback_function = boost::bind(&ConfigurationDatabase::slotprofileNameListDataCallback, this, &name_list, _1, _2, _3, _4);

    char* errormsg = (char*) 0;

    string statement;
    statement = string("SELECT Name FROM Slotprofiles;");
    int result = sqlite3_exec(db, statement.c_str(), c_callback, (void*) &sql_callback_function, &errormsg);
    if (result != SQLITE_OK)
    {
        LOG(Error, "Error while executing SQL statement \"" << statement << "\": " << errormsg);
        if (errormsg) sqlite3_free(errormsg);
        return name_list;
    }
    if (errormsg) sqlite3_free(errormsg);

    return name_list;
}

SP<SlotProfile> ConfigurationDatabase::loadSlotProfileData(const UnicodeString &name)
{
    if (!db) return SP<SlotProfile>();

    string name_utf8 = TO_UTF8(name);

    SlotProfile sp;

    function4<int, void*, int, char**, char**> sql_callback_function;
    sql_callback_function = boost::bind(&ConfigurationDatabase::slotprofileDataCallback, this, &sp, _1, _2, _3, _4);

    char* errormsg = (char*) 0;

    string statement;
    statement = string("SELECT Name, Width, Height, Path, WorkingPath, SlotType, AllowedWatchers, AllowedLaunchers, AllowedPlayers, ForbiddenWatchers, ForbiddenLaunchers, ForbiddenPlayers, MaxSlots FROM Slotprofiles WHERE Name = \'") + escape_sql_string(name_utf8) + string("\';");
    int result = sqlite3_exec(db, statement.c_str(), c_callback, (void*) &sql_callback_function, &errormsg);
    if (result != SQLITE_OK)
    {
        LOG(Error, "Error while executing SQL statement \"" << statement << "\": " << errormsg);
        if (errormsg) sqlite3_free(errormsg);
        return SP<SlotProfile>();
    }
    if (errormsg) sqlite3_free(errormsg);

    return SP<SlotProfile>(new SlotProfile(sp));
};

data1D dfterm::escape_sql_string(const data1D &str)
{
    data1D result;
    result.reserve(str.size());
    size_t str_size = str.size();

    size_t i1;
    for (i1 = 0; i1 < str_size; i1++)
    {
        if (str[i1] == '\'')
            result.append("\'\'");
        else if (str[i1] == 0)
            continue;
        else
            result.push_back(str[i1]);
    }

    return result;
}