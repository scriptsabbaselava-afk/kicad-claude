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

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>


PANEL_CLAUDE_SETTINGS::PANEL_CLAUDE_SETTINGS( wxWindow* aParent ) :
        RESETTABLE_PANEL( aParent ),
        m_deleteKey( false )
{
    wxBoxSizer* top = new wxBoxSizer( wxVERTICAL );

    // ---- API key ----
    wxStaticBoxSizer* keyBox = new wxStaticBoxSizer( wxVERTICAL, this,
                                                     TR_UTF8( "Clé API Anthropic" ) );
    wxBoxSizer* keyRow = new wxBoxSizer( wxHORIZONTAL );

    m_apiKey = new wxTextCtrl( keyBox->GetStaticBox(), wxID_ANY, wxEmptyString,
                               wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD );
    m_apiKey->SetHint( TR_UTF8( "sk-ant-…  (laisser vide pour garder la clé actuelle)" ) );
    keyRow->Add( m_apiKey, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );

    wxButton* forget = new wxButton( keyBox->GetStaticBox(), wxID_ANY,
                                     TR_UTF8( "Oublier la clé" ) );
    keyRow->Add( forget, 0, wxALIGN_CENTER_VERTICAL );
    keyBox->Add( keyRow, 0, wxEXPAND | wxALL, FromDIP( 5 ) );

    m_keyStatus = new wxStaticText( keyBox->GetStaticBox(), wxID_ANY, wxEmptyString );
    keyBox->Add( m_keyStatus, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 5 ) );

#if defined( __WXMAC__ )
    wxString where = TR_UTF8( "le trousseau macOS" );
#elif defined( __WXMSW__ )
    wxString where = TR_UTF8( "le Gestionnaire d'identification Windows" );
#else
    wxString where = TR_UTF8( "le trousseau du système (libsecret)" );
#endif

    wxStaticText* keyHelp = new wxStaticText(
            keyBox->GetStaticBox(), wxID_ANY,
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

    forget->Bind( wxEVT_BUTTON,
                  [this]( wxCommandEvent& )
                  {
                      m_deleteKey = true;
                      m_apiKey->Clear();
                      m_keyStatus->SetLabel( TR_UTF8( "La clé sera supprimée en validant." ) );
                  } );
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


bool PANEL_CLAUDE_SETTINGS::TransferDataToWindow()
{
    CLAUDE_CHAT_CONFIG cfg = CLAUDE_CHAT_CONFIG::Load();

    m_apiKey->Clear();
    m_model->SetValue( cfg.m_model );
    m_node->SetValue( cfg.m_node );
    m_server->SetValue( cfg.m_server );
    m_python->SetValue( cfg.m_kicadPython );
    m_pythonPath->SetValue( cfg.m_pythonPath );
    m_deleteKey = false;
    updateKeyStatus();
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
    m_model->SetValue( cfg.m_model );
    m_node->SetValue( cfg.m_node );
    m_server->SetValue( cfg.m_server );
    m_python->SetValue( cfg.m_kicadPython );
    m_pythonPath->SetValue( cfg.m_pythonPath );
}
