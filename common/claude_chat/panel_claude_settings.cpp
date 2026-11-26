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

#include <claude_chat/panel_claude_settings.h>
#include <claude_chat/claude_chat_config.h>

#include <functional>

#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/weakref.h>


/// A detached child process that calls back on the GUI thread when it ends.
class CLAUDE_CMD_PROCESS : public wxProcess
{
public:
    CLAUDE_CMD_PROCESS( std::function<void( int )> aOnEnd ) :
            wxProcess( wxPROCESS_DEFAULT ),
            m_onEnd( std::move( aOnEnd ) )
    {}

    void OnTerminate( int aPid, int aStatus ) override
    {
        std::function<void( int )> onEnd = m_onEnd;
        wxTheApp->CallAfter( [onEnd, aStatus]() { onEnd( aStatus ); } );
        delete this;
    }

private:
    std::function<void( int )> m_onEnd;
};


static wxString quoteArg( const wxString& aArg )
{
    if( aArg.Contains( wxS( " " ) ) && !aArg.StartsWith( wxS( "\"" ) ) )
        return wxS( "\"" ) + aArg + wxS( "\"" );

    return aArg;
}


PANEL_CLAUDE_SETTINGS::PANEL_CLAUDE_SETTINGS( wxWindow* aParent ) :
        RESETTABLE_PANEL( aParent ),
        m_deleteKey( false )
{
    wxBoxSizer* top = new wxBoxSizer( wxVERTICAL );

    // ---- How to connect ----
    wxArrayString modes;
    modes.Add( TR_UTF8( "Mon compte Claude (abonnement Pro ou Max, via Claude Code)" ) );
    modes.Add( TR_UTF8( "Clé API Anthropic (facturation à l'usage)" ) );
    m_backend = new wxRadioBox( this, wxID_ANY, TR_UTF8( "Connexion à Claude" ), wxDefaultPosition,
                                wxDefaultSize, modes, 1, wxRA_SPECIFY_COLS );
    top->Add( m_backend, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    // ---- Claude account ----
    wxStaticBoxSizer* accBox = new wxStaticBoxSizer( wxVERTICAL, this,
                                                     TR_UTF8( "Compte Claude" ) );
    wxWindow* accParent = accBox->GetStaticBox();

    wxBoxSizer* pathRow = new wxBoxSizer( wxHORIZONTAL );
    pathRow->Add( new wxStaticText( accParent, wxID_ANY, TR_UTF8( "Claude Code :" ) ), 0,
                  wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 5 ) );
    m_claudePath = new wxTextCtrl( accParent, wxID_ANY );
    pathRow->Add( m_claudePath, 1, wxALIGN_CENTER_VERTICAL );
    accBox->Add( pathRow, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    wxBoxSizer* accButtons = new wxBoxSizer( wxHORIZONTAL );
    m_installBtn = new wxButton( accParent, wxID_ANY, TR_UTF8( "Installer Claude Code" ) );
    m_loginBtn = new wxButton( accParent, wxID_ANY,
                               TR_UTF8( "Se connecter avec mon compte Claude" ) );
    m_checkBtn = new wxButton( accParent, wxID_ANY, TR_UTF8( "Vérifier" ) );
    accButtons->Add( m_installBtn, 0, wxRIGHT, FromDIP( 5 ) );
    accButtons->Add( m_loginBtn, 0, wxRIGHT, FromDIP( 5 ) );
    accButtons->Add( m_checkBtn, 0 );
    accBox->Add( accButtons, 0, wxLEFT | wxRIGHT, FromDIP( 5 ) );

    m_accountStatus = new wxStaticText( accParent, wxID_ANY, wxEmptyString );
    accBox->Add( m_accountStatus, 0, wxALL, FromDIP( 5 ) );

    wxStaticText* accHelp = new wxStaticText(
            accParent, wxID_ANY,
            TR_UTF8( "Claude Code est l'outil officiel d'Anthropic qui permet d'utiliser votre "
                     "abonnement Claude. La connexion se fait sur la page d'Anthropic, dans "
                     "votre navigateur ; KiCad ne voit jamais votre mot de passe." ) );
    accHelp->Wrap( FromDIP( 560 ) );
    accBox->Add( accHelp, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 5 ) );

    top->Add( accBox, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    // ---- API key ----
    wxStaticBoxSizer* keyBox = new wxStaticBoxSizer( wxVERTICAL, this,
                                                     TR_UTF8( "Clé API Anthropic" ) );
    wxWindow*   keyParent = keyBox->GetStaticBox();
    wxBoxSizer* keyRow = new wxBoxSizer( wxHORIZONTAL );

    m_apiKey = new wxTextCtrl( keyParent, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxDefaultSize, wxTE_PASSWORD );
    m_apiKey->SetHint( TR_UTF8( "sk-ant-…  (laisser vide pour garder la clé actuelle)" ) );
    keyRow->Add( m_apiKey, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );

    m_forgetBtn = new wxButton( keyParent, wxID_ANY, TR_UTF8( "Oublier la clé" ) );
    keyRow->Add( m_forgetBtn, 0, wxALIGN_CENTER_VERTICAL );
    keyBox->Add( keyRow, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    m_keyStatus = new wxStaticText( keyParent, wxID_ANY, wxEmptyString );
    keyBox->Add( m_keyStatus, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 5 ) );

#if defined( __WXMAC__ )
    wxString where = TR_UTF8( "le trousseau macOS" );
#elif defined( __WXMSW__ )
    wxString where = TR_UTF8( "le Gestionnaire d'identification Windows" );
#else
    wxString where = TR_UTF8( "le trousseau du système (libsecret)" );
#endif

    wxStaticText* keyHelp = new wxStaticText(
            keyParent, wxID_ANY,
            wxString::Format( TR_UTF8( "La clé est rangée dans %s, jamais dans un fichier de "
                                       "KiCad. Créez-en une sur console.anthropic.com." ),
                              where ) );
    keyHelp->Wrap( FromDIP( 560 ) );
    keyBox->Add( keyHelp, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 5 ) );

    top->Add( keyBox, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    // ---- Model and server ----
    wxStaticBoxSizer* srvBox = new wxStaticBoxSizer( wxVERTICAL, this,
                                                     TR_UTF8( "Modèle et serveur MCP" ) );
    wxFlexGridSizer*  grid = new wxFlexGridSizer( 2, FromDIP( 5 ), FromDIP( 5 ) );
    grid->AddGrowableCol( 1 );

    auto addRow = [&]( const wxString& aLabel ) -> wxWindow*
    {
        grid->Add( new wxStaticText( srvBox->GetStaticBox(), wxID_ANY, aLabel ), 0,
                   wxALIGN_CENTER_VERTICAL );
        return srvBox->GetStaticBox();
    };

    wxArrayString models;
    models.Add( wxS( "claude-opus-5-5" ) );
    models.Add( wxS( "claude-sonnet-5-5" ) );
    models.Add( wxS( "claude-haiku-5-5" ) );
    models.Add( wxS( "claude-fable-5-2" ) );

    m_model = new wxComboBox( addRow( TR_UTF8( "Modèle :" ) ), wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize, models );
    grid->Add( m_model, 1, wxEXPAND );

    m_node = new wxTextCtrl( addRow( TR_UTF8( "Node.js :" ) ), wxID_ANY );
    grid->Add( m_node, 1, wxEXPAND );

    m_server = new wxTextCtrl( addRow( TR_UTF8( "Serveur MCP (index.js) :" ) ), wxID_ANY );
    grid->Add( m_server, 1, wxEXPAND );

    m_python = new wxTextCtrl( addRow( TR_UTF8( "Python de KiCad :" ) ), wxID_ANY );
    grid->Add( m_python, 1, wxEXPAND );

    m_pythonPath = new wxTextCtrl( addRow( TR_UTF8( "PYTHONPATH :" ) ), wxID_ANY );
    grid->Add( m_pythonPath, 1, wxEXPAND );

    srvBox->Add( grid, 0, wxEXPAND | wxALL, FromDIP( 5 ) );
    top->Add( srvBox, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    SetSizer( top );

    // ---- Events ----
    m_backend->Bind( wxEVT_RADIOBOX, [this]( wxCommandEvent& ) { updateEnabledState(); } );

    m_forgetBtn->Bind( wxEVT_BUTTON,
                       [this]( wxCommandEvent& )
                       {
                           m_deleteKey = true;
                           m_apiKey->Clear();
                           m_keyStatus->SetLabel( TR_UTF8( "La clé sera supprimée en validant." ) );
                       } );

    m_checkBtn->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { updateAccountStatus(); } );

    m_loginBtn->Bind( wxEVT_BUTTON,
                      [this]( wxCommandEvent& )
                      {
                          runClaudeCommand( { m_claudePath->GetValue().Trim().Trim( false ),
                                              wxS( "auth" ), wxS( "login" ), wxS( "--claudeai" ) },
                                            TR_UTF8( "Connexion en cours : terminez-la dans la page "
                                                     "qui s'est ouverte dans votre navigateur…" ),
                                            true );
                      } );

    m_installBtn->Bind( wxEVT_BUTTON,
                        [this]( wxCommandEvent& )
                        {
#ifdef __WXMSW__
                            std::vector<wxString> argv = {
                                wxS( "powershell.exe" ), wxS( "-NoProfile" ),
                                wxS( "-ExecutionPolicy" ), wxS( "Bypass" ), wxS( "-Command" ),
                                wxS( "irm https://claude.ai/install.ps1 | iex" ) };
#else
                            std::vector<wxString> argv = {
                                wxS( "/bin/bash" ), wxS( "-c" ),
                                wxS( "curl -fsSL https://claude.ai/install.sh | bash" ) };
#endif
                            runClaudeCommand( argv,
                                              TR_UTF8( "Installation de Claude Code en cours "
                                                       "(une à deux minutes)…" ),
                                              true );
                        } );
}


void PANEL_CLAUDE_SETTINGS::runClaudeCommand( const std::vector<wxString>& aArgv,
                                              const wxString& aRunningText, bool aShowConsole )
{
    if( aArgv.empty() || aArgv[0].IsEmpty() )
        return;

    std::vector<const wchar_t*> argv;

    for( const wxString& a : aArgv )
        argv.push_back( a.wc_str() );

    argv.push_back( nullptr );

    m_accountStatus->SetLabel( aRunningText );
    m_installBtn->Disable();
    m_loginBtn->Disable();

    wxWeakRef<PANEL_CLAUDE_SETTINGS> self( this );

    CLAUDE_CMD_PROCESS* proc = new CLAUDE_CMD_PROCESS(
            [self]( int aStatus )
            {
                if( !self )
                    return;

                // A fresh install puts Claude Code in its default place.
                if( !wxFileName::FileExists( self->m_claudePath->GetValue() ) )
                    self->m_claudePath->SetValue( CLAUDE_CHAT_CONFIG::DefaultClaudePath() );

                self->m_installBtn->Enable();
                self->m_loginBtn->Enable();
                self->updateAccountStatus();
            } );

    int flags = wxEXEC_ASYNC | ( aShowConsole ? wxEXEC_SHOW_CONSOLE : wxEXEC_HIDE_CONSOLE );

    if( wxExecute( argv.data(), flags, proc ) <= 0 )
    {
        delete proc;
        m_installBtn->Enable();
        m_loginBtn->Enable();
        m_accountStatus->SetLabel( wxString::Format( TR_UTF8( "Impossible de lancer « %s »." ),
                                                     aArgv[0] ) );
    }
}


void PANEL_CLAUDE_SETTINGS::updateAccountStatus()
{
    wxString claude = m_claudePath->GetValue().Trim().Trim( false );

    if( claude.IsEmpty() || ( wxFileName( claude ).IsAbsolute() && !wxFileName::FileExists( claude ) ) )
    {
        m_accountStatus->SetLabel( TR_UTF8( "Claude Code n'est pas installé : cliquez sur "
                                            "« Installer Claude Code »." ) );
        Layout();
        return;
    }

    wxArrayString output, errors;
    long          rc = wxExecute( quoteArg( claude ) + wxS( " auth status --json" ), output, errors,
                                  wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE );
    wxString      text;

    for( const wxString& l : output )
        text += l + wxS( "\n" );

    bool loggedIn = false;

    try
    {
        nlohmann::json j = nlohmann::json::parse( text.ToStdString( wxConvUTF8 ) );
        loggedIn = j.value( "loggedIn", false );
    }
    catch( ... )
    {
    }

    if( rc < 0 )
        m_accountStatus->SetLabel( TR_UTF8( "Impossible de lancer Claude Code : vérifiez le chemin." ) );
    else if( loggedIn )
        m_accountStatus->SetLabel( TR_UTF8( "✓ Connecté à votre compte Claude." ) );
    else
        m_accountStatus->SetLabel( TR_UTF8( "Pas connecté : cliquez sur « Se connecter avec mon "
                                            "compte Claude »." ) );

    Layout();
}


void PANEL_CLAUDE_SETTINGS::updateKeyStatus()
{
    wxString key;

    if( CLAUDE_CHAT_CONFIG::GetApiKey( key ) )
    {
        wxString tail = key.Length() > 4 ? key.Right( 4 ) : wxString();
        m_keyStatus->SetLabel( wxString::Format( TR_UTF8( "Une clé est enregistrée (…%s)." ),
                                                 tail ) );
    }
    else
    {
        m_keyStatus->SetLabel( TR_UTF8( "Aucune clé enregistrée." ) );
    }
}


void PANEL_CLAUDE_SETTINGS::updateEnabledState()
{
    bool account = m_backend->GetSelection() == 0;

    m_claudePath->Enable( account );
    m_installBtn->Enable( account );
    m_loginBtn->Enable( account );
    m_checkBtn->Enable( account );
    m_apiKey->Enable( !account );
    m_forgetBtn->Enable( !account );
}


bool PANEL_CLAUDE_SETTINGS::TransferDataToWindow()
{
    CLAUDE_CHAT_CONFIG cfg = CLAUDE_CHAT_CONFIG::Load();

    m_backend->SetSelection( cfg.UsesClaudeCode() ? 0 : 1 );
    m_claudePath->SetValue( cfg.m_claudePath );
    m_apiKey->Clear();
    m_model->SetValue( cfg.m_model );
    m_node->SetValue( cfg.m_node );
    m_server->SetValue( cfg.m_server );
    m_python->SetValue( cfg.m_kicadPython );
    m_pythonPath->SetValue( cfg.m_pythonPath );
    m_deleteKey = false;
    updateKeyStatus();
    updateEnabledState();

    if( cfg.UsesClaudeCode() )
        updateAccountStatus();

    return true;
}


bool PANEL_CLAUDE_SETTINGS::TransferDataFromWindow()
{
    wxString key = m_apiKey->GetValue().Trim().Trim( false );

    if( !key.IsEmpty() )
    {
        if( !key.StartsWith( wxS( "sk-ant-" ) ) )
        {
            wxMessageBox( TR_UTF8( "Cette clé ne ressemble pas à une clé API Anthropic "
                                   "(elle doit commencer par « sk-ant- »)." ),
                          TR_UTF8( "Claude" ), wxOK | wxICON_WARNING, this );
            return false;
        }

        if( !CLAUDE_CHAT_CONFIG::SetApiKey( key ) )
        {
            wxMessageBox( TR_UTF8( "Impossible d'enregistrer la clé dans le trousseau du "
                                   "système." ),
                          TR_UTF8( "Claude" ), wxOK | wxICON_ERROR, this );
            return false;
        }
    }
    else if( m_deleteKey )
    {
        CLAUDE_CHAT_CONFIG::DeleteApiKey();
    }

    CLAUDE_CHAT_CONFIG cfg;
    cfg.m_backend = m_backend->GetSelection() == 0 ? wxS( "claude-code" ) : wxS( "api" );
    cfg.m_claudePath = m_claudePath->GetValue().Trim().Trim( false );
    cfg.m_model = m_model->GetValue().Trim().Trim( false );
    cfg.m_node = m_node->GetValue().Trim().Trim( false );
    cfg.m_server = m_server->GetValue().Trim().Trim( false );
    cfg.m_kicadPython = m_python->GetValue().Trim().Trim( false );
    cfg.m_pythonPath = m_pythonPath->GetValue().Trim().Trim( false );
    cfg.m_toolboxes = CLAUDE_CHAT_CONFIG::Load().m_toolboxes;
    cfg.Save();

    m_apiKey->Clear();
    return true;
}


void PANEL_CLAUDE_SETTINGS::ResetPanel()
{
    // Defaults are whatever auto-detection finds when the fields are empty.
    CLAUDE_CHAT_CONFIG empty;
    empty.Save();

    CLAUDE_CHAT_CONFIG cfg = CLAUDE_CHAT_CONFIG::Load();
    m_backend->SetSelection( cfg.UsesClaudeCode() ? 0 : 1 );
    m_claudePath->SetValue( cfg.m_claudePath );
    m_model->SetValue( cfg.m_model );
    m_node->SetValue( cfg.m_node );
    m_server->SetValue( cfg.m_server );
    m_python->SetValue( cfg.m_kicadPython );
    m_pythonPath->SetValue( cfg.m_pythonPath );
    updateEnabledState();
}
