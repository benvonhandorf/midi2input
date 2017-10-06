#include <iostream>
#include <iomanip>
#include <fstream>
#include <queue>
#include <sstream>
#include <cstring>

#include <unistd.h>

extern "C" {
    #include <lua5.2/lua.h>
    #include <lua5.2/lauxlib.h>
    #include <lua5.2/lualib.h>
}

#include "argh.h"
#include "log.h"

#include "midi.h"

#ifdef WITH_JACK
    #include "jack.h"
#endif
#ifdef WITH_ALSA
    #include "alsa.h"
#endif

#ifdef WITH_XORG
    #include "x11.h"
#endif

namespace midi2input {
    lua_State *L;

    #ifdef WITH_ALSA
    alsa_singleton *alsa = nullptr;
    #endif

    #ifdef WITH_JACK
    jack_singleton *jack = nullptr;
    #endif
}

const char *helptext =
"USAGE: ./midi2input [options]"
"GENERAL OPTIONS:"
"   -h  --help      Print usage and exit"
"   -v  --verbose   Output more information"
"   -c  --config    Specify config file, default = ~/.config/midi2input.lua"
"   -a  --alsa      Use ALSA midi backend "
"   -j  --jack      Use Jack midi backend ";

static int
lua_midi_send( lua_State *L )
{
    midi_event event;
    for( int i = 0; i < 3; ++i ){
        lua_pushnumber( L, i + 1 );
        lua_gettable( L, -2 );
        event[ i ] =  static_cast<unsigned char>( luaL_checknumber( L, -1 ) );
        lua_pop( L, 1 );
    }

    #ifdef WITH_ALSA
    if( midi2input::alsa )
        if( midi2input::alsa->valid )
            midi2input::alsa->midi_send( event );
    #endif

    #ifdef WITH_JACK
    if( midi2input::jack )
        if( midi2input::jack->valid )
            midi2input::jack->midi_send( event );
    #endif
    return 0;
}

static int
lua_exec( lua_State *L )
{
    std::string command;
    command = luaL_checkstring( L, -1 );
    LOG( INFO ) << "exec: " << command;

    FILE *in;
    char buff[512];
    if(! (in = popen( command.c_str(), "r" ))){
        return 1;
    }
    while( fgets(buff, sizeof(buff), in) != nullptr ){
        LOG( INFO ) << buff;
    }
    pclose( in );
    return 0;
}

bool
load_config( const std::string &name )
{
    auto L = midi2input::L;
    // load configuration from a priority list of locations
    // * specified from the command line
    // * configuration folder $HOME/.config/
    // * home directory $HOME/
    std::string filepath;
    std::queue<std::string> paths;

    if(! name.empty() ) paths.push( name );

    // configuration folder ~/.config/
    filepath = std::string( getenv( "HOME" ) ) + "/.config/midi2input.lua";
    paths.push( filepath );

    // configuration folder ~/.config/
    filepath = std::string( getenv( "HOME" ) ) + "/.midi2input.lua";
    paths.push( filepath );

    std::ifstream tFile;
    while(! paths.empty() ){
        tFile.open( paths.front().c_str(), std::ifstream::in );
        if( tFile.is_open() ){
            tFile.close();
            break;
        }
        paths.pop();
    }
    if( paths.empty() ) return false;

    if( luaL_loadfile( L, paths.front().c_str() ) || lua_pcall( L, 0, 0, 0 ) ){
        LOG( ERROR ) << "cannot run configuration file: " << lua_tostring( L, -1 );
        return false;
    }
    LOG( INFO ) << "Using: " << paths.front();
    return true;
}



int32_t
processEvent( const midi_event &event )
{
    auto L = midi2input::L;
    lua_getglobal( L, "midi_recv" );
    lua_pushnumber( L, event[0] );
    lua_pushnumber( L, event[1] );
    lua_pushnumber( L, event[2] );
    if( lua_pcall( L, 3, 0, 0 ) != 0 )
        LOG( ERROR ) << "call to function 'event_in' failed" << lua_tostring( L, -1 );
    return 0;
}

int
main( int argc, const char **argv )
{
    argh::parser cmdl( argc, argv );

    // Options Parsing
    // ===============
    // setup logging level.
    if( cmdl[{ "-v", "--verbose" }] )
    //if( options[ QUIET ] )
    //    LOG::SetDefaultLoggerLevel( LOG::CHECK );

    if( cmdl[{"-h", "--help"}] ){
        LOG(INFO) << helptext;
        exit( 0 );
    }

    /* ============================== Lua =============================== */
    // --config
    LOG( INFO ) << "Parsing cmd line options";
    std::string luaScript;
    if( cmdl[{"-c", "--config"}] )
    {
        luaScript = "";
    } else luaScript = "~/.config/midi2input.lua";

    LOG( INFO ) << "Initialising Lua";

    midi2input::L = luaL_newstate();
    auto L = midi2input::L;
    luaL_openlibs( L );

    lua_pushcfunction( L, lua_midi_send );
    lua_setglobal( L, "midi_send" );

    lua_pushcfunction( L, lua_exec );
    lua_setglobal( L, "exec" );

    LOG( INFO ) << "Lua: Loading configuration file";
    if(! load_config( luaScript ) ){
        LOG( FATAL ) << "Unable to open configuration file, expecting ~/.config/midi2input.lua, or -c switch.";
        exit( -1 );

    }

    /* ============================== ALSA ============================== */
    if( cmdl[{"-a","--alsa"}] ){
    #ifdef WITH_ALSA
        midi2input::alsa = alsa_singleton::getInstance( true );
        if( midi2input::alsa->valid )
            midi2input::alsa->set_eventProcessor( processEvent );
    #else
        LOG( ERROR ) << "Not compiled with ALSA midi backend";
        exit(-1);
    #endif
    }

    /* ============================== Jack ============================== */
    if( cmdl[{"-j","--jack"}] ){
    #ifdef WITH_JACK
        midi2input::jack = jack_singleton::getInstance( true );
        if( midi2input::jack->valid )
            midi2input::jack->set_eventProcessor( processEvent );
    #else
        LOG( ERROR ) << "Not compiled with Jack midi backend";
        exit(-1);
    #endif
    }

    /* ============================= X11 ================================ */
    #ifdef WITH_XORG
    if( initialise( midi2input::L ) ) exit(-1);
    #endif


    /* =========================== Main Loop ============================ */
    LOG( INFO ) << "Main: Entering sleep, waiting for events";
    while( true )
    {
        #ifdef WITH_XORG
        detect_window();
        #endif

        #ifdef WITH_ALSA
        if( midi2input::alsa ) if( midi2input::alsa->valid )
            midi2input::alsa->midi_recv();
        #endif

        //TODO something to know when to quit.
        //TODO inotify to monitor and reload configuration

        #if !defined WITH_ALSA && !defined WITH_JACK
        LOG( ERROR ) << "no midi backend compiled into binary, nothing to do.";
        break;
        #endif
        sleep( 1 );
    }

    lua_close( L );
    exit( 0 );
}


