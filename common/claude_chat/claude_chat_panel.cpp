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

#include <claude_chat/claude_chat_panel.h>

#include <chrono>
#include <future>
#include <map>

#include <eda_base_frame.h>
#include <kicad_curl/kicad_curl_easy.h>
#include <pgm_base.h>
#include <settings/common_settings.h>
#include <string_utils.h>

#ifdef KICAD_IPC_API
#include <api/api_server.h>
#endif

#include <wx/app.h>
#include <wx/aui/framemanager.h>
#include <wx/button.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/html/htmlwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

using json = nlohmann::json;

static const char*  API_URL = "https://api.anthropic.com/v1/messages";
static const char*  API_VERSION = "2026-10-21";
static const char*  FALLBACK_BETA = "server-side-fallback-2026-07-01";
static const int    MAX_TOKENS = 16000;
static const int    MAX_AGENT_STEPS = 60;
static const size_t MAX_TOOL_TEXT = 60000;


// ---------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------

static wxString htmlEscape( const wxString& aText )
{
    wxString out;
    out.reserve( aText.length() );

    for( wxUniChar c : aText )
    {
        switch( (wxChar) c )
        {
        case '&':  out += wxS( "&amp;" );  break;
        case '<':  out += wxS( "&lt;" );   break;
        case '>':  out += wxS( "&gt;" );   break;
        case '"':  out += wxS( "&quot;" ); break;
        case '\n': out += wxS( "<br>" );   break;
        default:   out += c;               break;
        }
    }

    return out;
}


static wxString colourHtml( const wxColour& aColour )
{
    return aColour.GetAsString( wxC2S_HTML_SYNTAX );
}


static wxColour blend( const wxColour& aA, const wxColour& aB, double aT )
{
    return wxColour( (unsigned char) ( aA.Red() * ( 1 - aT ) + aB.Red() * aT ),
                     (unsigned char) ( aA.Green() * ( 1 - aT ) + aB.Green() * aT ),
                     (unsigned char) ( aA.Blue() * ( 1 - aT ) + aB.Blue() * aT ) );
}


static wxString shorten( const wxString& aText, size_t aMax )
{
    wxString t = aText;
    t.Replace( wxS( "\n" ), wxS( " " ) );

    if( t.length() > aMax )
        t = t.Left( aMax ) + wxS( "…" );

    return t;
}


/// Modification times of the KiCad design files next to aDocPath.
static std::map<wxString, wxDateTime> designFileTimes( const wxString& aDocPath )
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


static bool modelSupportsFallbacks( const wxString& aModel )
{
    return aModel.StartsWith( wxS( "claude-opus-5" ) ) || aModel.StartsWith( wxS( "claude-fable-5" ) )
           || aModel == wxS( "claude-sonnet-5-5" );
}


// ---------------------------------------------------------------------------------------
// Construction / UI
// ---------------------------------------------------------------------------------------

wxAuiPaneInfo CLAUDE_CHAT_PANEL::DefaultPaneInfo( wxWindow* aFrame )
{
    return wxAuiPaneInfo()
            .Name( PaneName() )
            .Caption( TR_UTF8( "Claude" ) )
            .Right()
            .Layer( 10 )      // outermost column on the right
            .Position( 0 )
            .CaptionVisible( true )
            .PaneBorder( true )
            .TopDockable( false )
            .BottomDockable( false )
            .CloseButton( false )   // always visible
            .Floatable( false )
            .MinSize( aFrame->FromDIP( wxSize( 280, 200 ) ) )
            .BestSize( aFrame->FromDIP( wxSize( 380, -1 ) ) )
            .Show( true );
}


CLAUDE_CHAT_PANEL::CLAUDE_CHAT_PANEL( wxWindow* aParent, const CLAUDE_CHAT_HOOKS& aHooks ) :
        wxPanel( aParent, wxID_ANY ),
        m_hooks( aHooks ),
        m_cfg( CLAUDE_CHAT_CONFIG::Load() ),
        m_timer( this ),
        m_cancel( false ),
        m_busy( false ),
        m_alive( std::make_shared<std::atomic<bool>>( true ) ),
        m_tools( json::array() ),
        m_messages( json::array() )
{
    wxBoxSizer* top = new wxBoxSizer( wxVERTICAL );

    m_history = new wxHtmlWindow( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxHW_SCROLLBAR_AUTO | wxBORDER_NONE );
    m_history->SetBorders( FromDIP( 6 ) );
    top->Add( m_history, 1, wxEXPAND );

    m_status = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_status->SetForegroundColour( wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT ) );
    top->Add( m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 4 ) );

    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              wxSize( -1, FromDIP( 70 ) ), wxTE_MULTILINE | wxTE_RICH2 );
    m_input->SetHint( TR_UTF8( "Demandez quelque chose à Claude… (Entrée pour envoyer, "
                         "Maj+Entrée pour aller à la ligne)" ) );
    top->Add( m_input, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    wxBoxSizer* buttons = new wxBoxSizer( wxHORIZONTAL );
    m_settingsBtn = new wxButton( this, wxID_ANY, TR_UTF8( "Réglages" ), wxDefaultPosition,
                                  wxDefaultSize, wxBU_EXACTFIT );
    m_newBtn = new wxButton( this, wxID_ANY, TR_UTF8( "Nouveau" ), wxDefaultPosition, wxDefaultSize,
                             wxBU_EXACTFIT );
    m_stopBtn = new wxButton( this, wxID_ANY, TR_UTF8( "Arrêter" ), wxDefaultPosition, wxDefaultSize,
                              wxBU_EXACTFIT );
    m_sendBtn = new wxButton( this, wxID_ANY, TR_UTF8( "Envoyer" ) );
    m_sendBtn->SetDefault();

    buttons->Add( m_settingsBtn, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->Add( m_newBtn, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->AddStretchSpacer();
    buttons->Add( m_stopBtn, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->Add( m_sendBtn, 0 );
    top->Add( buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 4 ) );

    SetSizer( top );

    m_sendBtn->Bind( wxEVT_BUTTON, &CLAUDE_CHAT_PANEL::onSend, this );
    m_stopBtn->Bind( wxEVT_BUTTON, &CLAUDE_CHAT_PANEL::onStop, this );
    m_newBtn->Bind( wxEVT_BUTTON, &CLAUDE_CHAT_PANEL::onNewChat, this );
    m_settingsBtn->Bind( wxEVT_BUTTON, &CLAUDE_CHAT_PANEL::onSettings, this );
    m_input->Bind( wxEVT_KEY_DOWN, &CLAUDE_CHAT_PANEL::onInputKey, this );
    Bind( wxEVT_TIMER, &CLAUDE_CHAT_PANEL::onTimer, this );
    Bind( wxEVT_SYS_COLOUR_CHANGED, &CLAUDE_CHAT_PANEL::onSysColourChanged, this );
    m_history->Bind( wxEVT_HTML_LINK_CLICKED,
                     []( wxHtmlLinkEvent& aEvent )
                     {
                         wxLaunchDefaultBrowser( aEvent.GetLinkInfo().GetHref() );
                     } );

    setBusy( false );

    addEntry( ENTRY_KIND::INFO,
              wxString::Format( TR_UTF8( "Claude est prêt dans l'%s. Il agit sur KiCad grâce au serveur "
                                   "MCP KiCad ; chaque action s'affiche ici." ),
                                m_hooks.m_editorName ) );

    if( m_cfg.UsesClaudeCode() )
    {
        addEntry( ENTRY_KIND::INFO, TR_UTF8( "Mode : votre compte Claude (abonnement Pro ou Max), "
                                             "via Claude Code. Première fois : cliquez sur "
                                             "« Réglages » puis « Se connecter avec mon compte "
                                             "Claude »." ) );
    }
    else
    {
        wxString key;

        if( !CLAUDE_CHAT_CONFIG::GetApiKey( key ) )
        {
            addEntry( ENTRY_KIND::INFO, TR_UTF8( "Aucune clé API Anthropic n'est configurée : "
                                                 "cliquez sur « Réglages » pour l'ajouter. Elle "
                                                 "est rangée dans le trousseau du système, "
                                                 "jamais dans un fichier." ) );
        }
    }

    ensureIpcApiEnabled();

    m_timer.Start( 500 );
}


CLAUDE_CHAT_PANEL::~CLAUDE_CHAT_PANEL()
{
    Shutdown();
}


void CLAUDE_CHAT_PANEL::Shutdown()
{
    m_cancel = true;

    if( m_worker.joinable() )
        m_worker.join();

    m_timer.Stop();
    m_claudeCode.Kill();
    m_mcp.Stop();
    *m_alive = false;
}


void CLAUDE_CHAT_PANEL::ReloadConfig()
{
    m_cfg = CLAUDE_CHAT_CONFIG::Load();

    if( !m_busy )
    {
        // Restart the server with the new settings on the next message.
        m_mcp.Stop();
        m_tools = json::array();
    }
}


bool CLAUDE_CHAT_PANEL::ensureIpcApiEnabled()
{
#ifdef KICAD_IPC_API
    COMMON_SETTINGS* settings = Pgm().GetCommonSettings();

    if( settings && !settings->m_Api.enable_server )
    {
        settings->m_Api.enable_server = true;

        if( !Pgm().GetApiServer().Running() )
            Pgm().GetApiServer().Start();

        addEntry( ENTRY_KIND::INFO, TR_UTF8( "Le serveur d'API IPC de KiCad a été activé "
                                       "(Préférences > Plugins) pour que Claude puisse "
                                       "agir en direct." ) );
    }

    return true;
#else
    addEntry( ENTRY_KIND::INFO, TR_UTF8( "Cette version de KiCad est compilée sans API IPC : Claude "
                                   "travaillera directement sur les fichiers." ) );
    return false;
#endif
}


void CLAUDE_CHAT_PANEL::setBusy( bool aBusy, const wxString& aStatus )
{
    m_busy = aBusy;
    m_sendBtn->Enable( !aBusy );
    m_stopBtn->Enable( aBusy );
    m_newBtn->Enable( !aBusy );
    m_settingsBtn->Enable( !aBusy );
    m_status->SetLabel( aStatus );
    Layout();
}


void CLAUDE_CHAT_PANEL::addEntry( ENTRY_KIND aKind, const wxString& aText )
{
    m_entries.push_back( { aKind, aText } );
    render();
}


void CLAUDE_CHAT_PANEL::render()
{
    wxColour bg = wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW );
    wxColour fg = wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT );
    wxColour grey = blend( fg, bg, 0.45 );
    wxColour userBg = blend( bg, wxColour( 70, 130, 220 ), 0.18 );
    wxColour toolBg = blend( bg, fg, 0.06 );
    wxColour errBg = blend( bg, wxColour( 220, 60, 60 ), 0.18 );
    wxColour errFg = blend( fg, wxColour( 220, 40, 40 ), 0.6 );
    wxColour link = blend( fg, wxColour( 60, 120, 230 ), 0.8 );

    wxString html = wxString::Format( wxS( "<html><body bgcolor='%s' text='%s' link='%s'>" ),
                                      colourHtml( bg ), colourHtml( fg ), colourHtml( link ) );

    for( const ENTRY& e : m_entries )
    {
        switch( e.m_kind )
        {
        case ENTRY_KIND::USER:
            html += wxString::Format( wxS( "<table width='100%%' cellpadding='6' cellspacing='0'>"
                                           "<tr><td bgcolor='%s'><b>%s</b><br>%s</td></tr>"
                                           "</table><br>" ),
                                      colourHtml( userBg ), TR_UTF8( "Vous" ), htmlEscape( e.m_text ) );
            break;

        case ENTRY_KIND::ASSISTANT:
        {
            wxString md;
            ConvertMarkdown2Html( e.m_text, md );
            html += wxString::Format( wxS( "<b>Claude</b><br>%s<br>" ), md );
            break;
        }

        case ENTRY_KIND::TOOL:
            html += wxString::Format( wxS( "<table width='100%%' cellpadding='3' cellspacing='0'>"
                                           "<tr><td bgcolor='%s'><font size='-1' color='%s'>"
                                           "<tt>%s</tt></font></td></tr></table>" ),
                                      colourHtml( toolBg ), colourHtml( grey ),
                                      htmlEscape( e.m_text ) );
            break;

        case ENTRY_KIND::INFO:
            html += wxString::Format( wxS( "<font size='-1' color='%s'><i>%s</i></font><br><br>" ),
                                      colourHtml( grey ), htmlEscape( e.m_text ) );
            break;

        case ENTRY_KIND::ERROR_MSG:
            html += wxString::Format( wxS( "<table width='100%%' cellpadding='6' cellspacing='0'>"
                                           "<tr><td bgcolor='%s'><font color='%s'><b>%s</b> %s"
                                           "</font></td></tr></table><br>" ),
                                      colourHtml( errBg ), colourHtml( errFg ), TR_UTF8( "Erreur :" ),
                                      htmlEscape( e.m_text ) );
            break;
        }
    }

    html += wxS( "</body></html>" );

    m_history->Freeze();
    m_history->SetPage( html );

    int x = 0, y = 0;
    m_history->GetVirtualSize( &x, &y );
    m_history->Scroll( 0, y );
    m_history->Thaw();

    // Some platforms only know the final height after layout.
    CallAfter(
            [this]()
            {
                int vx = 0, vy = 0;
                m_history->GetVirtualSize( &vx, &vy );
                m_history->Scroll( 0, vy );
            } );
}


void CLAUDE_CHAT_PANEL::onSysColourChanged( wxSysColourChangedEvent& aEvent )
{
    render();
    aEvent.Skip();
}


void CLAUDE_CHAT_PANEL::onTimer( wxTimerEvent& aEvent )
{
    // Keep the server's stderr pipe empty while nobody is talking to it.
    if( !m_busy )
        m_mcp.DrainStderr();
}


void CLAUDE_CHAT_PANEL::onInputKey( wxKeyEvent& aEvent )
{
    if( ( aEvent.GetKeyCode() == WXK_RETURN || aEvent.GetKeyCode() == WXK_NUMPAD_ENTER )
        && !aEvent.ShiftDown() )
    {
        if( !m_busy )
            sendMessage();

        return;
    }

    aEvent.Skip();
}


void CLAUDE_CHAT_PANEL::onSend( wxCommandEvent& aEvent )
{
    sendMessage();
}


void CLAUDE_CHAT_PANEL::onStop( wxCommandEvent& aEvent )
{
    m_cancel = true;
    m_status->SetLabel( TR_UTF8( "Arrêt en cours…" ) );
}


void CLAUDE_CHAT_PANEL::onNewChat( wxCommandEvent& aEvent )
{
    if( m_busy )
        return;

    m_messages = json::array();
    m_claudeSession.Clear();
    m_entries.clear();
    addEntry( ENTRY_KIND::INFO, TR_UTF8( "Nouvelle conversation." ) );
}


void CLAUDE_CHAT_PANEL::onSettings( wxCommandEvent& aEvent )
{
    EDA_BASE_FRAME* frame = dynamic_cast<EDA_BASE_FRAME*>( wxGetTopLevelParent( this ) );

    if( frame )
        frame->ShowPreferences( TR_UTF8( "Claude (IA)" ), wxEmptyString );

    ReloadConfig();

    wxString key;

    if( CLAUDE_CHAT_CONFIG::GetApiKey( key ) )
        m_status->SetLabel( TR_UTF8( "Clé API enregistrée." ) );
}


// ---------------------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------------------

void CLAUDE_CHAT_PANEL::sendMessage()
{
    if( m_busy )
        return;

    wxString text = m_input->GetValue().Trim().Trim( false );

    if( text.IsEmpty() )
        return;

    if( m_worker.joinable() )
        m_worker.join();

    // Pick up changes made in Preferences > Claude (IA) since the last message.
    m_cfg = CLAUDE_CHAT_CONFIG::Load();

    if( m_cfg.UsesClaudeCode() )
    {
        sendViaClaudeCode( text );
        return;
    }

    wxString apiKey;

    if( !CLAUDE_CHAT_CONFIG::GetApiKey( apiKey ) )
    {
        addEntry( ENTRY_KIND::ERROR_MSG, TR_UTF8( "Aucune clé API Anthropic. Cliquez sur « Réglages » "
                                            "et collez votre clé (elle commence par sk-ant-)." ) );
        return;
    }

    wxString signature = m_cfg.m_node + wxS( "|" ) + m_cfg.m_server + wxS( "|" )
                         + m_cfg.m_kicadPython + wxS( "|" ) + m_cfg.m_pythonPath + wxS( "|" )
                         + m_cfg.m_toolboxes;

    if( m_mcp.IsRunning() && signature != m_serverSignature )
        m_mcp.Stop();

    // Start (or restart after a crash) the MCP server; wxExecute must run on the GUI thread.
    if( !m_mcp.IsRunning() )
    {
        m_serverSignature = signature;

        wxString err;

        if( !m_mcp.Start( m_cfg, m_hooks.m_toolboxes, err ) )
        {
            addEntry( ENTRY_KIND::ERROR_MSG, err );
            return;
        }

        m_tools = json::array();
    }

    m_input->Clear();
    addEntry( ENTRY_KIND::USER, text );

    wxString doc = m_hooks.m_documentPath ? m_hooks.m_documentPath() : wxString();
    wxString context = buildContext( text );

    m_cancel = false;
    setBusy( true, TR_UTF8( "Claude réfléchit…" ) );

    m_worker = std::thread( &CLAUDE_CHAT_PANEL::workerMain, this, context, apiKey, doc );
}


wxString CLAUDE_CHAT_PANEL::buildContext( const wxString& aText )
{
    // Tell Claude what is open in this editor.
    wxString doc = m_hooks.m_documentPath ? m_hooks.m_documentPath() : wxString();
    wxString pro = m_hooks.m_projectPath ? m_hooks.m_projectPath() : wxString();
    wxString context = wxString::Format( wxS( "<kicad_context>\neditor: %s\n" ),
                                         m_hooks.m_editorName );

    context += wxS( "open_document: " ) + ( doc.IsEmpty() ? wxString( wxS( "(none)" ) ) : doc )
               + wxS( "\n" );
    context += wxS( "project: " ) + ( pro.IsEmpty() ? wxString( wxS( "(none)" ) ) : pro )
               + wxS( "\n" );
    context += wxS( "</kicad_context>\n\n" ) + aText;
    return context;
}


// ---------------------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------------------

void CLAUDE_CHAT_PANEL::post( std::function<void()> aFunc )
{
    std::shared_ptr<std::atomic<bool>> alive = m_alive;

    wxTheApp->CallAfter(
            [alive, aFunc]()
            {
                if( *alive )
                    aFunc();
            } );
}


bool CLAUDE_CHAT_PANEL::runOnGui( std::function<void()> aFunc )
{
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    std::shared_ptr<std::promise<void>> done = std::make_shared<std::promise<void>>();
    std::future<void>                   future = done->get_future();

    wxTheApp->CallAfter(
            [alive, aFunc, done]()
            {
                if( *alive )
                    aFunc();

                done->set_value();
            } );

    while( future.wait_for( std::chrono::milliseconds( 50 ) ) != std::future_status::ready )
    {
        if( m_cancel )
            return false;
    }

    return true;
}


void CLAUDE_CHAT_PANEL::postEntry( ENTRY_KIND aKind, const wxString& aText )
{
    post( [this, aKind, aText]() { addEntry( aKind, aText ); } );
}


void CLAUDE_CHAT_PANEL::rebuildTools( const json& aMcpTools )
{
    m_tools = json::array();

    for( const json& t : aMcpTools )
    {
        if( !t.contains( "name" ) )
            continue;

        json schema = t.value( "inputSchema", json::object() );

        if( !schema.is_object() )
            schema = json::object();

        schema.erase( "$schema" );

        if( !schema.contains( "type" ) )
            schema["type"] = "object";

        std::string desc = t.value( "description", std::string() );

        m_tools.push_back( { { "name", t["name"] },
                             { "description", desc },
                             { "input_schema", schema } } );
    }
}


bool CLAUDE_CHAT_PANEL::ensureTools( wxString& aError )
{
    if( !m_mcp.IsInitialized() )
    {
        post( [this]() { m_status->SetLabel( TR_UTF8( "Démarrage du serveur MCP KiCad…" ) ); } );

        if( !m_mcp.Initialize( m_cancel, aError ) )
            return false;
    }

    if( m_tools.empty() || m_mcp.ConsumeToolsChanged() )
    {
        json mcpTools;

        if( !m_mcp.ListTools( mcpTools, m_cancel, aError ) )
            return false;

        rebuildTools( mcpTools );
    }

    return true;
}


std::string CLAUDE_CHAT_PANEL::systemPrompt() const
{
    // Kept constant for the whole session so that prompt caching works.
    std::string editor = m_hooks.m_editorName.ToStdString( wxConvUTF8 );

    return "You are Claude, built into a chat panel docked inside KiCad (" + editor + "). "
           "You operate KiCad for the user through the tools of the KiCad MCP server.\n\n"
           "How to work:\n"
           "- Each user message starts with a <kicad_context> block giving the document and "
           "project open in the editor. Work on that document unless the user says otherwise; "
           "do not ask the user for paths you already have.\n"
           "- Inspect before you modify: list the schematic components or board components to "
           "find the exact references, pin numbers and positions you need.\n"
           "- The editor saves the open document before your tool calls and reloads it "
           "automatically when a design file changes on disk. Never ask the user to reload.\n"
           "- Only a core set of tools is loaded. Use search_tools, list_toolboxes and "
           "open_toolbox to load more; tools you open become callable on your next step.\n"
           "- After significant modifications, run the Electrical Rules Check (run_erc) on "
           "schematics or the Design Rules Check (run_drc) on boards, then report the result "
           "briefly: number of errors and warnings and the important ones.\n"
           "- If a tool fails, explain the failure in plain words and what the user can do.\n"
           "- Be concise. Answer in the language the user writes in.";
}


bool CLAUDE_CHAT_PANEL::callClaude( const wxString& aApiKey, json& aResponse, wxString& aError )
{
    json body = { { "model", m_cfg.m_model.ToStdString( wxConvUTF8 ) },
                  { "max_tokens", MAX_TOKENS },
                  { "system", systemPrompt() },
                  { "messages", m_messages },
                  { "cache_control", { { "type", "ephemeral" } } } };

    if( !m_tools.empty() )
        body["tools"] = m_tools;

    bool fallbacks = modelSupportsFallbacks( m_cfg.m_model );

    if( fallbacks )
        body["fallbacks"] = "default";

    std::string payload = body.dump( -1, ' ', false, json::error_handler_t::replace );

    const int delays[] = { 3, 8, 20 };

    for( int attempt = 0; ; ++attempt )
    {
        KICAD_CURL_EASY curl;
        // KICAD_CLAUDE_API_URL is only meant for testing against a local server.
        wxString urlOverride;
        std::string url = API_URL;

        if( wxGetEnv( wxS( "KICAD_CLAUDE_API_URL" ), &urlOverride ) && !urlOverride.IsEmpty() )
            url = urlOverride.ToStdString( wxConvUTF8 );

        curl.SetURL( url );
        curl.SetUserAgent( "KiCad-Claude-Chat/1.0" );
        curl.SetHeader( "x-api-key", aApiKey.ToStdString( wxConvUTF8 ) );
        curl.SetHeader( "anthropic-version", API_VERSION );
        curl.SetHeader( "content-type", "application/json" );

        if( fallbacks )
            curl.SetHeader( "anthropic-beta", FALLBACK_BETA );

        curl.SetPostFields( payload );
        curl.SetConnectTimeout( 30 );
        curl.SetTimeout( 900 );
        curl.SetTransferCallback(
                [this]( size_t, size_t, size_t, size_t ) -> int
                {
                    return m_cancel ? 1 : 0;
                },
                0 );

        int      code = curl.Perform();
        int      status = code == 0 ? curl.GetResponseStatusCode() : 0;
        bool     retryable = false;
        wxString apiMessage;

        if( m_cancel )
        {
            aError = TR_UTF8( "Arrêté." );
            return false;
        }

        if( code != 0 )
        {
            aError = wxString::Format( TR_UTF8( "Impossible de joindre api.anthropic.com (%s). "
                                          "Vérifiez votre connexion Internet." ),
                                       wxString::FromUTF8( curl.GetErrorText( code ) ) );
            retryable = true;
        }
        else
        {
            try
            {
                aResponse = json::parse( curl.GetBuffer() );
            }
            catch( ... )
            {
                aResponse = json::object();
            }

            if( status == 200 )
                return true;

            if( aResponse.contains( "error" ) && aResponse["error"].is_object() )
                apiMessage = wxString::FromUTF8( aResponse["error"].value( "message", "" ) );

            switch( status )
            {
            case 401:
                aError = TR_UTF8( "Clé API refusée (401). Elle est invalide ou révoquée : "
                            "changez-la dans « Réglages »." );
                return false;

            case 403:
                aError = TR_UTF8( "Accès refusé (403) : " ) + apiMessage;
                return false;

            case 400:
            case 404:
            case 413:
                aError = wxString::Format( TR_UTF8( "Requête refusée par l'API (%d) : %s" ), status,
                                           apiMessage );
                return false;

            case 429:
                aError = TR_UTF8( "Limite de débit de l'API atteinte (429)." );
                retryable = true;
                break;

            default:
                aError = wxString::Format( TR_UTF8( "Erreur du service Anthropic (%d) : %s" ), status,
                                           apiMessage );
                retryable = status >= 500;
                break;
            }
        }

        if( !retryable || attempt >= 2 )
            return false;

        int wait = delays[attempt];
        post( [this, wait]()
              {
                  m_status->SetLabel( wxString::Format( TR_UTF8( "Nouvel essai dans %d s…" ), wait ) );
              } );

        for( int i = 0; i < wait * 10; ++i )
        {
            if( m_cancel )
            {
                aError = TR_UTF8( "Arrêté." );
                return false;
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        }
    }
}


json CLAUDE_CHAT_PANEL::runTool( const json& aToolUse )
{
    std::string name = aToolUse.value( "name", std::string() );
    json        input = aToolUse.value( "input", json::object() );
    std::string id = aToolUse.value( "id", std::string() );

    wxString argText = wxString::FromUTF8( input.dump( -1, ' ', false,
                                                       json::error_handler_t::replace ) );
    postEntry( ENTRY_KIND::TOOL, wxString::Format( wxS( "▸ %s %s" ), wxString::FromUTF8( name ),
                                                   shorten( argText, 160 ) ) );
    post( [this, name]()
          {
              m_status->SetLabel( wxString::Format( TR_UTF8( "Outil en cours : %s" ),
                                                    wxString::FromUTF8( name ) ) );
          } );

    json     result;
    wxString err;
    json     content = json::array();
    bool     isError = false;

    if( !m_mcp.CallTool( name, input, result, m_cancel, err ) )
    {
        isError = true;
        content.push_back( { { "type", "text" }, { "text", err.ToStdString( wxConvUTF8 ) } } );
    }
    else
    {
        isError = result.value( "isError", false );
        size_t textBudget = MAX_TOOL_TEXT;

        if( result.contains( "content" ) && result["content"].is_array() )
        {
            for( const json& c : result["content"] )
            {
                std::string type = c.value( "type", std::string() );

                if( type == "text" )
                {
                    std::string text = c.value( "text", std::string() );

                    if( text.size() > textBudget )
                    {
                        size_t total = text.size();
                        text = text.substr( 0, textBudget );
                        text += "\n[... result truncated by the KiCad chat panel: "
                                + std::to_string( total ) + " characters in total]";
                    }

                    textBudget -= std::min( textBudget, text.size() );
                    content.push_back( { { "type", "text" }, { "text", text } } );
                }
                else if( type == "image" && c.contains( "data" ) )
                {
                    content.push_back( { { "type", "image" },
                                         { "source", { { "type", "base64" },
                                                       { "media_type", c.value( "mimeType",
                                                                                "image/png" ) },
                                                       { "data", c["data"] } } } } );
                }
            }
        }

        if( content.empty() )
            content.push_back( { { "type", "text" }, { "text", "(no output)" } } );
    }

    // Show a one-line outcome in the chat
    wxString summary;

    if( content[0].value( "type", "" ) == "text" )
        summary = wxString::FromUTF8( content[0].value( "text", "" ) );

    postEntry( ENTRY_KIND::TOOL, wxString::Format( wxS( "   %s %s" ),
                                                   isError ? wxS( "✗" ) : wxS( "✓" ),
                                                   shorten( summary, 200 ) ) );

    json block = { { "type", "tool_result" }, { "tool_use_id", id }, { "content", content } };

    if( isError )
        block["is_error"] = true;

    return block;
}


void CLAUDE_CHAT_PANEL::workerMain( wxString aUserBlock, wxString aApiKey, wxString aDocPath )
{
    wxString error;
    auto     finish = [this]( const wxString& aError )
    {
        post( [this, aError]()
              {
                  if( !aError.IsEmpty() )
                      addEntry( ENTRY_KIND::ERROR_MSG, aError );

                  setBusy( false );
              } );
    };

    if( !ensureTools( error ) )
    {
        if( !m_cancel )
        {
            error = TR_UTF8( "Le serveur MCP KiCad ne répond pas.\n" ) + error;
            post( [this]() { m_mcp.Stop(); } );   // restart it on the next message
        }

        finish( error );
        return;
    }

    size_t rollback = m_messages.size();

    m_messages.push_back( { { "role", "user" },
                            { "content", json::array( { { { "type", "text" },
                                                          { "text", aUserBlock.ToStdString(
                                                                            wxConvUTF8 ) } } } ) } } );

    for( int step = 0; step < MAX_AGENT_STEPS; ++step )
    {
        post( [this]() { m_status->SetLabel( TR_UTF8( "Claude réfléchit…" ) ); } );

        if( m_mcp.ConsumeToolsChanged() )
        {
            json mcpTools;

            if( m_mcp.ListTools( mcpTools, m_cancel, error ) )
                rebuildTools( mcpTools );
        }

        json response;

        if( !callClaude( aApiKey, response, error ) )
        {
            // Drop the unanswered user turn so the conversation stays valid.
            if( m_messages.size() == rollback + 1 )
                m_messages.erase( m_messages.begin() + rollback, m_messages.end() );
            else if( !m_messages.empty() && m_messages.back()["role"] == "assistant" )
                m_messages.push_back( { { "role", "user" },
                                        { "content", "(the previous request failed)" } } );

            finish( error );
            return;
        }

        json content = response.value( "content", json::array() );
        std::string stopReason = response.value( "stop_reason", std::string() );

        m_messages.push_back( { { "role", "assistant" }, { "content", content } } );

        for( const json& block : content )
        {
            if( block.value( "type", "" ) == "text" )
            {
                wxString text = wxString::FromUTF8( block.value( "text", "" ) ).Trim();

                if( !text.IsEmpty() )
                    postEntry( ENTRY_KIND::ASSISTANT, text );
            }
        }

        if( stopReason == "tool_use" )
        {
            // Save the user's pending edits so the MCP server sees them, and remember the
            // design files' timestamps to know whether to reload afterwards.
            wxString docPath = aDocPath;
            bool     saved = false;

            runOnGui( [&]()
                      {
                          if( m_hooks.m_documentPath )
                              docPath = m_hooks.m_documentPath();

                          if( m_hooks.m_isModified && m_hooks.m_isModified() && m_hooks.m_save )
                              saved = m_hooks.m_save();
                      } );

            if( saved )
                postEntry( ENTRY_KIND::INFO, TR_UTF8( "Document enregistré avant l'action de Claude." ) );

            std::map<wxString, wxDateTime> before = designFileTimes( docPath );
            json                           results = json::array();

            for( const json& block : content )
            {
                if( block.value( "type", "" ) != "tool_use" )
                    continue;

                if( m_cancel )
                {
                    results.push_back( { { "type", "tool_result" },
                                         { "tool_use_id", block.value( "id", "" ) },
                                         { "content", "Cancelled by the user." },
                                         { "is_error", true } } );
                    continue;
                }

                results.push_back( runTool( block ) );
            }

            m_messages.push_back( { { "role", "user" }, { "content", results } } );

            if( designFileTimes( docPath ) != before )
            {
                runOnGui( [this]()
                          {
                              if( m_hooks.m_reload )
                                  m_hooks.m_reload();
                          } );
                postEntry( ENTRY_KIND::INFO, TR_UTF8( "Document rechargé depuis le disque." ) );
            }

            if( m_cancel )
            {
                finish( TR_UTF8( "Arrêté par l'utilisateur." ) );
                return;
            }

            if( !m_mcp.IsRunning() )
            {
                post( [this]() { m_mcp.Stop(); } );
                finish( TR_UTF8( "Le serveur MCP KiCad s'est arrêté. Il sera relancé au prochain "
                           "message.\n" ) + m_mcp.RecentStderr().Right( 800 ) );
                return;
            }

            continue;
        }

        if( stopReason == "pause_turn" )
            continue;

        if( stopReason == "max_tokens" )
            finish( TR_UTF8( "La réponse a été coupée (limite de longueur atteinte)." ) );
        else if( stopReason == "refusal" )
            finish( TR_UTF8( "Claude a refusé de traiter cette demande." ) );
        else
            finish( wxEmptyString );

        return;
    }

    finish( TR_UTF8( "Arrêt : trop d'étapes d'affilée. Reformulez ou découpez la demande." ) );
}


// ---------------------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------------------

CLAUDE_CHAT_PANEL* AddClaudeChatPane( wxAuiManager& aAuiMgr, wxWindow* aFrame,
                                      const CLAUDE_CHAT_HOOKS& aHooks )
{
    CLAUDE_CHAT_PANEL* panel = new CLAUDE_CHAT_PANEL( aFrame, aHooks );
    aAuiMgr.AddPane( panel, CLAUDE_CHAT_PANEL::DefaultPaneInfo( aFrame ) );
    return panel;
}


void ShowClaudeChatPane( wxAuiManager& aAuiMgr )
{
    wxAuiPaneInfo& pane = aAuiMgr.GetPane( CLAUDE_CHAT_PANEL::PaneName() );

    if( pane.IsOk() )
    {
        pane.Show( true );
        pane.CloseButton( false );
    }
}


void ShutdownClaudeChatPane( wxAuiManager& aAuiMgr )
{
    wxAuiPaneInfo& pane = aAuiMgr.GetPane( CLAUDE_CHAT_PANEL::PaneName() );

    if( pane.IsOk() )
    {
        if( CLAUDE_CHAT_PANEL* panel = dynamic_cast<CLAUDE_CHAT_PANEL*>( pane.window ) )
            panel->Shutdown();
    }
}
