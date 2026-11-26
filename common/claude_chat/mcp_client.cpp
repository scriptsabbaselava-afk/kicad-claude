/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <claude_chat/mcp_client.h>
#include <claude_chat/claude_chat_config.h>

#include <chrono>
#include <thread>

#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/process.h>
#include <wx/stdpaths.h>
#include <wx/stream.h>
#include <wx/utils.h>

static const size_t STDERR_TAIL_MAX = 8192;
static const int    INIT_TIMEOUT_SEC = 90;     // first start may compile python caches
static const int    LIST_TIMEOUT_SEC = 60;
static const int    CALL_TIMEOUT_SEC = 300;    // DRC / exports can be slow


static long long nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>( steady_clock::now().time_since_epoch() ).count();
}


/**
 * wxProcess whose lifetime we control: it is only deleted by MCP_CLIENT, except when it was
 * "orphaned" (killed while still running), in which case it deletes itself on termination.
 */
class MCP_PROCESS : public wxProcess
{
public:
    MCP_PROCESS( std::shared_ptr<std::atomic<bool>> aExited ) :
            wxProcess( wxPROCESS_REDIRECT ),
            m_exited( std::move( aExited ) ),
            m_orphaned( false )
    {}

    void OnTerminate( int aPid, int aStatus ) override
    {
        *m_exited = true;

        if( m_orphaned )
            delete this;
    }

    std::shared_ptr<std::atomic<bool>> m_exited;
    bool                               m_orphaned;
};


MCP_CLIENT::MCP_CLIENT() :
        m_process( nullptr ),
        m_pid( 0 ),
        m_exited( std::make_shared<std::atomic<bool>>( true ) ),
        m_nextId( 1 ),
        m_initialized( false ),
        m_toolsChanged( false )
{
}


MCP_CLIENT::~MCP_CLIENT()
{
    Stop();
}


bool MCP_CLIENT::IsRunning() const
{
    return m_process && !*m_exited;
}


bool MCP_CLIENT::Start( const CLAUDE_CHAT_CONFIG& aConfig, const wxString& aToolboxes,
                        wxString& aError )
{
    Stop();

    if( !wxFileName::FileExists( aConfig.m_server ) )
    {
        aError = wxString::Format( TR_UTF8( "Serveur MCP introuvable : %s\n"
                                      "Relancez l'installateur ou corrigez le chemin dans "
                                      "Préférences > Claude (IA)." ),
                                   aConfig.m_server );
        return false;
    }

    wxExecuteEnv env;
    wxGetEnvMap( &env.env );

    wxString sep = wxFileName::GetPathSeparator();
#ifdef __WXMSW__
    wxString pathSep = wxS( ";" );
#else
    wxString pathSep = wxS( ":" );
#endif

    // Make node, KiCad's own tools (kicad-cli) and KiCad's python findable by the server.
    wxString path = env.env[wxS( "PATH" )];
    wxFileName nodeFn( aConfig.m_node );
    wxFileName exeFn( wxStandardPaths::Get().GetExecutablePath() );

    if( nodeFn.IsAbsolute() )
        path = nodeFn.GetPath() + pathSep + path;

    path = exeFn.GetPath() + pathSep + path;
#ifndef __WXMSW__
    path += pathSep + wxS( "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin" );
#endif
    env.env[wxS( "PATH" )] = path;

    env.env[wxS( "KICAD_PYTHON" )] = aConfig.m_kicadPython;

    if( !aConfig.m_pythonPath.IsEmpty() )
    {
        wxString old = env.env[wxS( "PYTHONPATH" )];
        env.env[wxS( "PYTHONPATH" )] =
                old.IsEmpty() ? aConfig.m_pythonPath : aConfig.m_pythonPath + pathSep + old;
    }

#ifdef __WXMAC__
    // Let python packages that dlopen() native libraries (cairocffi) find KiCad's own copies.
    {
        wxString exePath = exeFn.GetFullPath();
        int      pos = exePath.Find( wxS( ".app/Contents/" ) );

        if( pos != wxNOT_FOUND )
        {
            wxString frameworks = exePath.Left( pos ) + wxS( ".app/Contents/Frameworks" );
            wxString old = env.env[wxS( "DYLD_FALLBACK_LIBRARY_PATH" )];
            env.env[wxS( "DYLD_FALLBACK_LIBRARY_PATH" )] =
                    old.IsEmpty() ? frameworks : frameworks + wxS( ":" ) + old;
        }
    }
#endif

    env.env[wxS( "PYTHONUNBUFFERED" )] = wxS( "1" );
    env.env[wxS( "LOG_LEVEL" )] = wxS( "info" );

    wxString toolboxes = wxS( "core" );

    if( !aToolboxes.IsEmpty() )
        toolboxes += wxS( "," ) + aToolboxes;

    if( !aConfig.m_toolboxes.IsEmpty() )
        toolboxes += wxS( "," ) + aConfig.m_toolboxes;

    env.env[wxS( "KICAD_MCP_TOOLBOXES" )] = toolboxes;

    // Run from the server's repository root (parent of dist/)
    wxFileName serverFn( aConfig.m_server );
    wxFileName root( serverFn.GetPath(), wxEmptyString );
    root.RemoveLastDir();
    env.cwd = root.GetPath();

    m_exited = std::make_shared<std::atomic<bool>>( false );
    m_process = new MCP_PROCESS( m_exited );

    wxString     node = aConfig.m_node;
    wxString     server = aConfig.m_server;
    const wchar_t* argv[] = { node.wc_str(), server.wc_str(), nullptr };

    int flags = wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE | wxEXEC_MAKE_GROUP_LEADER;
    m_pid = wxExecute( argv, flags, m_process, &env );

    if( m_pid <= 0 )
    {
        // wxExecute deletes nothing on failure; the process object is ours.
        delete m_process;
        m_process = nullptr;
        *m_exited = true;
        aError = wxString::Format( TR_UTF8( "Impossible de lancer le serveur MCP avec « %s ».\n"
                                      "Node.js est-il installé ?" ),
                                   aConfig.m_node );
        return false;
    }

    m_stdoutBuf.clear();
    {
        std::lock_guard<std::mutex> lock( m_stderrMutex );
        m_stderrTail.clear();
    }
    m_initialized = false;
    m_toolsChanged = false;
    (void) sep;
    return true;
}


void MCP_CLIENT::Stop()
{
    if( !m_process )
        return;

    if( !*m_exited )
    {
        // Killing is asynchronous: let the process object delete itself once wx notices.
        m_process->m_orphaned = true;
        wxProcess::Kill( m_pid, wxSIGKILL, wxKILL_CHILDREN );
    }
    else
    {
        delete m_process;
    }

    m_process = nullptr;
    m_pid = 0;
    m_initialized = false;
}


void MCP_CLIENT::DrainStderr()
{
    std::lock_guard<std::mutex> lock( m_stderrMutex );

    if( !m_process )
        return;

    wxInputStream* err = m_process->GetErrorStream();

    if( !err )
        return;

    char buf[4096];

    while( err->CanRead() )
    {
        err->Read( buf, sizeof( buf ) );
        size_t n = err->LastRead();

        if( n == 0 )
            break;

        m_stderrTail.append( buf, n );
    }

    if( m_stderrTail.size() > STDERR_TAIL_MAX )
        m_stderrTail.erase( 0, m_stderrTail.size() - STDERR_TAIL_MAX );
}


wxString MCP_CLIENT::RecentStderr()
{
    std::lock_guard<std::mutex> lock( m_stderrMutex );
    return wxString::FromUTF8( m_stderrTail );
}


wxString MCP_CLIENT::deadServerMessage()
{
    wxString tail = RecentStderr();
    wxString msg = TR_UTF8( "Le serveur MCP KiCad s'est arrêté de façon inattendue." );

    if( !tail.IsEmpty() )
    {
        // Keep the last lines only
        if( tail.length() > 1500 )
            tail = wxS( "..." ) + tail.Right( 1500 );

        msg += wxS( "\n" ) + TR_UTF8( "Derniers messages du serveur :" ) + wxS( "\n" ) + tail;
    }

    return msg;
}


bool MCP_CLIENT::send( const nlohmann::json& aMsg, wxString& aError )
{
    if( !IsRunning() )
    {
        aError = deadServerMessage();
        return false;
    }

    std::string line = aMsg.dump( -1, ' ', false, nlohmann::json::error_handler_t::replace );
    line += '\n';

    wxOutputStream* out = m_process->GetOutputStream();
    out->Write( line.data(), line.size() );

    if( out->LastWrite() != line.size() )
    {
        aError = deadServerMessage();
        return false;
    }

    return true;
}


bool MCP_CLIENT::readLine( std::string& aLine, long long aDeadlineMs,
                           const std::atomic<bool>& aCancel, wxString& aError )
{
    char buf[8192];

    while( true )
    {
        size_t nl = m_stdoutBuf.find( '\n' );

        if( nl != std::string::npos )
        {
            aLine = m_stdoutBuf.substr( 0, nl );
            m_stdoutBuf.erase( 0, nl + 1 );
            return true;
        }

        if( aCancel )
        {
            aError = TR_UTF8( "Arrêté." );
            return false;
        }

        DrainStderr();

        wxInputStream* in = m_process ? m_process->GetInputStream() : nullptr;
        bool           gotData = false;

        if( in && in->CanRead() )
        {
            in->Read( buf, sizeof( buf ) );
            size_t n = in->LastRead();

            if( n > 0 )
            {
                m_stdoutBuf.append( buf, n );
                gotData = true;
            }
        }

        if( gotData )
            continue;

        if( *m_exited || !in )
        {
            aError = deadServerMessage();
            return false;
        }

        if( nowMs() > aDeadlineMs )
        {
            aError = TR_UTF8( "Le serveur MCP KiCad n'a pas répondu à temps." );
            return false;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
}


bool MCP_CLIENT::request( const std::string& aMethod, const nlohmann::json& aParams,
                          nlohmann::json& aResult, int aTimeoutSec,
                          const std::atomic<bool>& aCancel, wxString& aError )
{
    int id = m_nextId++;

    nlohmann::json msg = { { "jsonrpc", "2.0" }, { "id", id }, { "method", aMethod },
                           { "params", aParams } };

    if( !send( msg, aError ) )
        return false;

    long long deadline = nowMs() + aTimeoutSec * 1000LL;

    while( true )
    {
        std::string line;

        if( !readLine( line, deadline, aCancel, aError ) )
            return false;

        if( line.empty() || line[0] != '{' )
            continue;   // not JSON-RPC (stray output)

        nlohmann::json in;

        try
        {
            in = nlohmann::json::parse( line );
        }
        catch( ... )
        {
            continue;
        }

        if( in.contains( "method" ) )
        {
            std::string method = in["method"].get<std::string>();

            if( in.contains( "id" ) )
            {
                // A request from the server.  We only support ping.
                nlohmann::json reply = { { "jsonrpc", "2.0" }, { "id", in["id"] } };

                if( method == "ping" )
                    reply["result"] = nlohmann::json::object();
                else
                    reply["error"] = { { "code", -32601 }, { "message", "Method not found" } };

                send( reply, aError );
            }
            else if( method == "notifications/tools/list_changed" )
            {
                m_toolsChanged = true;
            }

            continue;
        }

        if( !in.contains( "id" ) || !in["id"].is_number_integer() || in["id"].get<int>() != id )
            continue;

        if( in.contains( "error" ) )
        {
            std::string err = in["error"].value( "message", std::string( "unknown error" ) );
            aError = wxString::Format( TR_UTF8( "Erreur MCP : %s" ), wxString::FromUTF8( err ) );
            return false;
        }

        aResult = in.value( "result", nlohmann::json::object() );
        return true;
    }
}


bool MCP_CLIENT::Initialize( const std::atomic<bool>& aCancel, wxString& aError )
{
    nlohmann::json params = {
        { "protocolVersion", "2025-06-18" },
        { "capabilities", nlohmann::json::object() },
        { "clientInfo", { { "name", "kicad-claude-chat" }, { "version", "1.0" } } }
    };

    nlohmann::json result;

    if( !request( "initialize", params, result, INIT_TIMEOUT_SEC, aCancel, aError ) )
        return false;

    nlohmann::json note = { { "jsonrpc", "2.0" }, { "method", "notifications/initialized" } };

    if( !send( note, aError ) )
        return false;

    m_initialized = true;
    return true;
}


bool MCP_CLIENT::ListTools( nlohmann::json& aTools, const std::atomic<bool>& aCancel,
                            wxString& aError )
{
    aTools = nlohmann::json::array();
    nlohmann::json params = nlohmann::json::object();

    // tools/list may be paginated
    for( int page = 0; page < 50; ++page )
    {
        nlohmann::json result;

        if( !request( "tools/list", params, result, LIST_TIMEOUT_SEC, aCancel, aError ) )
            return false;

        if( result.contains( "tools" ) && result["tools"].is_array() )
        {
            for( const nlohmann::json& t : result["tools"] )
                aTools.push_back( t );
        }

        if( result.contains( "nextCursor" ) && result["nextCursor"].is_string() )
            params["cursor"] = result["nextCursor"];
        else
            break;
    }

    m_toolsChanged = false;
    return true;
}


bool MCP_CLIENT::CallTool( const std::string& aName, const nlohmann::json& aArgs,
                           nlohmann::json& aResult, const std::atomic<bool>& aCancel,
                           wxString& aError )
{
    nlohmann::json params = { { "name", aName },
                              { "arguments", aArgs.is_object() ? aArgs
                                                               : nlohmann::json::object() } };

    return request( "tools/call", params, aResult, CALL_TIMEOUT_SEC, aCancel, aError );
}
