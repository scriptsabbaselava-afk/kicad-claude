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

/*
 * "Claude account" backend of the chat panel.
 *
 * Anthropic lets a Claude subscription (Pro / Max) be used through Claude Code, its official
 * command line tool, not through third-party API clients.  In this mode each message is
 * handed to the user's own Claude Code ("claude -p ... --output-format stream-json"),
 * which runs the KiCad MCP server itself.  The panel shows the streamed events.
 */

#include <claude_chat/claude_chat_panel.h>
#include <claude_chat/mcp_client.h>

#include <map>

#include <settings/settings_manager.h>

#include <wx/dir.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>

using json = nlohmann::json;

static const int    CLAUDE_CODE_IDLE_TIMEOUT_SEC = 1800;
static const char*  KICAD_SERVER_NAME = "kicad";


static wxString shortenText( const wxString& aText, size_t aMax )
{
    wxString t = aText;
    t.Replace( wxS( "\n" ), wxS( " " ) );

    if( t.length() > aMax )
        t = t.Left( aMax ) + wxS( "…" );

    return t;
}


static std::map<wxString, wxDateTime> designTimes( const wxString& aDocPath )
{
    std::map<wxString, wxDateTime> times;

    if( aDocPath.IsEmpty() )
        return times;

    wxFileName doc( aDocPath );
    wxDir      dir( doc.GetPath() );

    if( !dir.IsOpened() )
        return times;

    for( const wxString& spec : { wxString( wxS( "*.kicad_sch" ) ), wxString( wxS( "*.kicad_pcb" ) ) } )
    {
        wxString name;

        for( bool ok = dir.GetFirst( &name, spec, wxDIR_FILES ); ok; ok = dir.GetNext( &name ) )
        {
            wxFileName fn( doc.GetPath(), name );
            times[fn.GetFullPath()] = fn.GetModificationTime();
        }
    }

    return times;
}


/// Text of a tool_result block, whose content is a string or a list of content blocks.
static wxString toolResultText( const json& aBlock )
{
    if( !aBlock.contains( "content" ) )
        return wxEmptyString;

    const json& c = aBlock["content"];

    if( c.is_string() )
        return wxString::FromUTF8( c.get<std::string>() );

    wxString out;

    if( c.is_array() )
    {
        for( const json& part : c )
        {
            if( part.value( "type", "" ) == "text" )
                out += wxString::FromUTF8( part.value( "text", "" ) ) + wxS( " " );
        }
    }

    return out;
}


static wxString friendlyClaudeCodeError( const wxString& aText )
{
    wxString lower = aText.Lower();

    if( lower.Contains( wxS( "not logged in" ) ) || lower.Contains( wxS( "/login" ) )
        || lower.Contains( wxS( "invalid api key" ) ) || lower.Contains( wxS( "oauth" ) )
        || lower.Contains( wxS( "authentication" ) ) || lower.Contains( wxS( "401" ) ) )
    {
        return TR_UTF8( "Vous n'êtes pas connecté à votre compte Claude. Cliquez sur « Réglages » "
                        "puis sur « Se connecter avec mon compte Claude »." );
    }

    if( lower.Contains( wxS( "limit" ) ) )
        return TR_UTF8( "Limite d'utilisation de votre abonnement Claude atteinte : " ) + aText;

    return aText;
}


bool CLAUDE_CHAT_PANEL::writeClaudeCodeMcpConfig( const wxString& aPath, wxString& aError )
{
    wxExecuteEnv env;
    MCP_CLIENT::BuildServerEnv( m_cfg, m_hooks.m_toolboxes, env );

    json envJson = json::object();

    for( const char* name : { "PATH", "KICAD_PYTHON", "PYTHONPATH", "PYTHONUNBUFFERED",
                              "LOG_LEVEL", "KICAD_MCP_TOOLBOXES", "DYLD_FALLBACK_LIBRARY_PATH",
                              "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "SYSTEMROOT" } )
    {
        wxString key = wxString::FromUTF8( name );

        if( env.env.count( key ) && !env.env[key].IsEmpty() )
            envJson[name] = env.env[key].ToStdString( wxConvUTF8 );
    }

    json cfg = { { "mcpServers",
                   { { KICAD_SERVER_NAME,
                       { { "type", "stdio" },
                         { "command", m_cfg.m_node.ToStdString( wxConvUTF8 ) },
                         { "args", json::array( { m_cfg.m_server.ToStdString( wxConvUTF8 ) } ) },
                         { "env", envJson } } } } } };

    wxFFile file( aPath, wxS( "w" ) );

    if( !file.IsOpened() || !file.Write( wxString::FromUTF8( cfg.dump( 2 ) ), wxConvUTF8 ) )
    {
        aError = wxString::Format( TR_UTF8( "Impossible d'écrire %s" ), aPath );
        return false;
    }

    return true;
}


void CLAUDE_CHAT_PANEL::sendViaClaudeCode( const wxString& aText )
{
    wxFileName claude( m_cfg.m_claudePath );

    if( claude.IsAbsolute() && !claude.FileExists() )
    {
        addEntry( ENTRY_KIND::ERROR_MSG,
                  TR_UTF8( "Claude Code n'est pas installé. Cliquez sur « Réglages » puis sur "
                           "« Installer Claude Code », et connectez-vous avec votre compte "
                           "Claude." ) );
        return;
    }

    if( !wxFileName::FileExists( m_cfg.m_server ) )
    {
        addEntry( ENTRY_KIND::ERROR_MSG,
                  wxString::Format( TR_UTF8( "Serveur MCP introuvable : %s" ), m_cfg.m_server ) );
        return;
    }

    wxString mcpConfig = wxFileName( SETTINGS_MANAGER::GetUserSettingsPath(),
                                     wxS( "claude_chat_mcp.json" ) ).GetFullPath();
    wxString err;

    if( !writeClaudeCodeMcpConfig( mcpConfig, err ) )
    {
        addEntry( ENTRY_KIND::ERROR_MSG, err );
        return;
    }

    m_input->Clear();
    addEntry( ENTRY_KIND::USER, aText );

    wxString doc = m_hooks.m_documentPath ? m_hooks.m_documentPath() : wxString();
    wxString context = buildContext( aText );

    // Claude Code edits files behind KiCad's back: save pending edits first.
    if( m_hooks.m_isModified && m_hooks.m_isModified() && m_hooks.m_save && m_hooks.m_save() )
        addEntry( ENTRY_KIND::INFO, TR_UTF8( "Document enregistré avant l'action de Claude." ) );

    std::vector<wxString> argv = {
        m_cfg.m_claudePath,
        wxS( "-p" ),
        wxS( "--output-format" ), wxS( "stream-json" ),
        wxS( "--verbose" ),
        wxS( "--mcp-config" ), mcpConfig,
        wxS( "--strict-mcp-config" ),
        wxS( "--allowedTools" ), wxS( "mcp__kicad" ),
        wxS( "--tools" ), wxS( "Read" ),
        wxS( "--append-system-prompt" ), wxString::FromUTF8( systemPrompt() )
    };

    if( !m_cfg.m_model.IsEmpty() )
    {
        argv.push_back( wxS( "--model" ) );
        argv.push_back( m_cfg.m_model );
    }

    if( !m_claudeSession.IsEmpty() )
    {
        argv.push_back( wxS( "--resume" ) );
        argv.push_back( m_claudeSession );
    }

    wxExecuteEnv env;
    MCP_CLIENT::BuildServerEnv( m_cfg, m_hooks.m_toolboxes, env );

    // Run Claude Code from the project folder (or the home folder).
    wxString pro = m_hooks.m_projectPath ? m_hooks.m_projectPath() : wxString();
    env.cwd = !pro.IsEmpty() && wxFileName( pro ).DirExists() ? wxFileName( pro ).GetPath()
                                                               : wxGetHomeDir();

    if( !m_claudeCode.Start( argv, env, err ) )
    {
        addEntry( ENTRY_KIND::ERROR_MSG,
                  wxString::Format( TR_UTF8( "Impossible de lancer Claude Code (%s). Vérifiez le "
                                             "chemin dans Réglages, ou installez-le." ),
                                    m_cfg.m_claudePath ) );
        return;
    }

    m_claudeCode.WriteAllAndClose( context.ToStdString( wxConvUTF8 ) );

    m_cancel = false;
    setBusy( true, TR_UTF8( "Claude réfléchit… (compte Claude)" ) );
    m_worker = std::thread( &CLAUDE_CHAT_PANEL::workerClaudeCode, this, doc );
}


void CLAUDE_CHAT_PANEL::workerClaudeCode( wxString aDocPath )
{
    auto finish = [this]( const wxString& aError )
    {
        post( [this, aError]()
              {
                  if( !aError.IsEmpty() )
                      addEntry( ENTRY_KIND::ERROR_MSG, aError );

                  setBusy( false );
              } );
    };

    std::map<wxString, wxDateTime> times = designTimes( aDocPath );
    bool                           gotResult = false;
    wxString                       resultError;

    while( true )
    {
        std::string line;
        auto        r = m_claudeCode.ReadLine( line, CLAUDE_CODE_IDLE_TIMEOUT_SEC, m_cancel );

        if( r == CLAUDE_CHILD_PROCESS::READ_RESULT::CANCELLED )
        {
            post( [this]() { m_claudeCode.Kill(); } );
            finish( TR_UTF8( "Arrêté par l'utilisateur." ) );
            return;
        }

        if( r == CLAUDE_CHILD_PROCESS::READ_RESULT::TIMEOUT )
        {
            post( [this]() { m_claudeCode.Kill(); } );
            finish( TR_UTF8( "Claude Code ne répond plus." ) );
            return;
        }

        if( r == CLAUDE_CHILD_PROCESS::READ_RESULT::EXITED )
            break;

        json ev;

        try
        {
            ev = json::parse( line );
        }
        catch( ... )
        {
            continue;
        }

        std::string type = ev.value( "type", "" );

        if( type == "system" && ev.value( "subtype", "" ) == "init" )
        {
            if( ev.contains( "session_id" ) && ev["session_id"].is_string() )
                m_claudeSession = wxString::FromUTF8( ev["session_id"].get<std::string>() );

            if( ev.contains( "mcp_servers" ) && ev["mcp_servers"].is_array() )
            {
                for( const json& srv : ev["mcp_servers"] )
                {
                    if( srv.value( "name", "" ) == KICAD_SERVER_NAME
                        && srv.value( "status", "" ) != "connected" )
                    {
                        postEntry( ENTRY_KIND::ERROR_MSG,
                                   wxString::Format( TR_UTF8( "Le serveur MCP KiCad n'a pas pu "
                                                              "démarrer (%s)." ),
                                                     wxString::FromUTF8( srv.value( "status",
                                                                                    "" ) ) ) );
                    }
                }
            }
        }
        else if( type == "assistant" && ev.contains( "message" ) )
        {
            const json content = ev["message"].value( "content", json::array() );

            for( const json& block : content )
            {
                std::string btype = block.value( "type", "" );

                if( btype == "text" )
                {
                    wxString text = wxString::FromUTF8( block.value( "text", "" ) ).Trim();

                    if( !text.IsEmpty() )
                        postEntry( ENTRY_KIND::ASSISTANT, text );
                }
                else if( btype == "tool_use" )
                {
                    wxString name = wxString::FromUTF8( block.value( "name", "" ) );
                    name.Replace( wxS( "mcp__kicad__" ), wxEmptyString );

                    wxString args = wxString::FromUTF8(
                            block.value( "input", json::object() )
                                    .dump( -1, ' ', false, json::error_handler_t::replace ) );

                    postEntry( ENTRY_KIND::TOOL,
                               wxString::Format( wxS( "▸ %s %s" ), name, shortenText( args, 160 ) ) );
                    post( [this, name]()
                          {
                              m_status->SetLabel( wxString::Format( TR_UTF8( "Outil en cours : %s" ),
                                                                    name ) );
                          } );
                }
            }
        }
        else if( type == "user" && ev.contains( "message" ) )
        {
            const json content = ev["message"].value( "content", json::array() );

            if( !content.is_array() )
                continue;

            bool hadResult = false;

            for( const json& block : content )
            {
                if( block.value( "type", "" ) != "tool_result" )
                    continue;

                hadResult = true;
                bool isError = block.value( "is_error", false );
                postEntry( ENTRY_KIND::TOOL,
                           wxString::Format( wxS( "   %s %s" ),
                                             isError ? wxS( "✗" ) : wxS( "✓" ),
                                             shortenText( toolResultText( block ), 200 ) ) );
            }

            if( hadResult )
            {
                post( [this]() { m_status->SetLabel( TR_UTF8( "Claude réfléchit… (compte Claude)" ) ); } );

                std::map<wxString, wxDateTime> now = designTimes( aDocPath );

                if( now != times )
                {
                    times = now;
                    runOnGui( [this]()
                              {
                                  if( m_hooks.m_reload )
                                      m_hooks.m_reload();
                              } );
                    postEntry( ENTRY_KIND::INFO, TR_UTF8( "Document rechargé depuis le disque." ) );
                }
            }
        }
        else if( type == "result" )
        {
            gotResult = true;

            if( ev.contains( "session_id" ) && ev["session_id"].is_string() )
                m_claudeSession = wxString::FromUTF8( ev["session_id"].get<std::string>() );

            if( ev.value( "is_error", false ) || ev.value( "subtype", "success" ) != "success" )
            {
                wxString text = wxString::FromUTF8( ev.value( "result", "" ) );

                if( text.IsEmpty() )
                    text = wxString::FromUTF8( ev.value( "subtype", "error" ) );

                resultError = friendlyClaudeCodeError( text );
            }
        }
    }

    if( !gotResult )
    {
        wxString tail = m_claudeCode.StderrTail().Trim();

        if( tail.length() > 800 )
            tail = wxS( "…" ) + tail.Right( 800 );

        wxString msg = friendlyClaudeCodeError( tail );

        if( msg == tail )
            msg = TR_UTF8( "Claude Code s'est arrêté sans répondre." )
                  + ( tail.IsEmpty() ? wxString() : wxS( "\n" ) + tail );

        // A broken session id would fail every following message: start afresh.
        m_claudeSession.Clear();
        finish( msg );
        return;
    }

    finish( resultError );
}
