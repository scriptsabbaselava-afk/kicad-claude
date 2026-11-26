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

#ifndef CLAUDE_CHAT_PANEL_H
#define CLAUDE_CHAT_PANEL_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/panel.h>
#include <wx/timer.h>

#include <claude_chat/claude_chat_config.h>
#include <claude_chat/mcp_client.h>

class wxHtmlWindow;
class wxTextCtrl;
class wxButton;
class wxStaticText;
class wxAuiPaneInfo;
class wxWindow;

/**
 * What an editor frame tells the chat panel about itself.  All callbacks are invoked on
 * the GUI thread.
 */
struct CLAUDE_CHAT_HOOKS
{
    wxString                  m_editorName;   ///< "Schematic Editor" / "PCB Editor"
    wxString                  m_toolboxes;    ///< MCP toolboxes to open at start
    std::function<wxString()> m_documentPath; ///< Full path of the open document (may be empty)
    std::function<wxString()> m_projectPath;  ///< Full path of the .kicad_pro (may be empty)
    std::function<bool()>     m_isModified;   ///< Document has unsaved changes
    std::function<bool()>     m_save;         ///< Save the document
    std::function<void()>     m_reload;       ///< Reload the document from disk
};


/**
 * Side panel with a chat to Claude.  Claude drives KiCad through the tools of the KiCad
 * MCP server, which this panel runs as a child process.
 */
class CLAUDE_CHAT_PANEL : public wxPanel
{
public:
    CLAUDE_CHAT_PANEL( wxWindow* aParent, const CLAUDE_CHAT_HOOKS& aHooks );
    ~CLAUDE_CHAT_PANEL() override;

    static wxString      PaneName() { return wxS( "ClaudeChat" ); }
    static wxAuiPaneInfo DefaultPaneInfo( wxWindow* aFrame );

    /// Stop the worker and the MCP server.  Safe to call more than once.
    void Shutdown();

    /// Re-read the settings (after the Preferences dialog was closed).
    void ReloadConfig();

private:
    enum class ENTRY_KIND
    {
        USER,
        ASSISTANT,
        TOOL,
        INFO,
        ERROR_MSG
    };

    struct ENTRY
    {
        ENTRY_KIND m_kind;
        wxString   m_text;
    };

    void onSend( wxCommandEvent& aEvent );
    void onStop( wxCommandEvent& aEvent );
    void onNewChat( wxCommandEvent& aEvent );
    void onSettings( wxCommandEvent& aEvent );
    void onInputKey( wxKeyEvent& aEvent );
    void onTimer( wxTimerEvent& aEvent );
    void onSysColourChanged( wxSysColourChangedEvent& aEvent );

    void sendMessage();
    void addEntry( ENTRY_KIND aKind, const wxString& aText );
    void render();
    void setBusy( bool aBusy, const wxString& aStatus = wxEmptyString );
    bool ensureIpcApiEnabled();

    // ---- worker thread side ----
    void workerMain( wxString aUserBlock, wxString aApiKey, wxString aDocPath );
    bool ensureTools( wxString& aError );
    void rebuildTools( const nlohmann::json& aMcpTools );
    bool callClaude( const wxString& aApiKey, nlohmann::json& aResponse, wxString& aError );
    nlohmann::json runTool( const nlohmann::json& aToolUse );
    std::string systemPrompt() const;

    /// Post a closure to the GUI thread; it is dropped if the panel is gone.
    void post( std::function<void()> aFunc );
    /// Run a closure on the GUI thread and wait for it (gives up if cancelled).
    bool runOnGui( std::function<void()> aFunc );
    void postEntry( ENTRY_KIND aKind, const wxString& aText );

    CLAUDE_CHAT_HOOKS  m_hooks;
    CLAUDE_CHAT_CONFIG m_cfg;

    wxHtmlWindow* m_history;
    wxTextCtrl*   m_input;
    wxButton*     m_sendBtn;
    wxButton*     m_stopBtn;
    wxButton*     m_newBtn;
    wxButton*     m_settingsBtn;
    wxStaticText* m_status;
    wxTimer       m_timer;

    std::vector<ENTRY> m_entries;

    std::thread                        m_worker;
    std::atomic<bool>                  m_cancel;
    std::atomic<bool>                  m_busy;
    std::shared_ptr<std::atomic<bool>> m_alive;

    MCP_CLIENT     m_mcp;
    wxString       m_serverSignature; ///< settings the running server was started with
    nlohmann::json m_tools;     ///< tool definitions in Anthropic format
    nlohmann::json m_messages;  ///< conversation in Anthropic format
};

class wxAuiManager;

/// Create the chat panel and dock it on the right of an editor frame.
CLAUDE_CHAT_PANEL* AddClaudeChatPane( wxAuiManager& aAuiMgr, wxWindow* aFrame,
                                      const CLAUDE_CHAT_HOOKS& aHooks );

/// Make sure the pane is visible (a restored AUI perspective may hide unknown panes).
void ShowClaudeChatPane( wxAuiManager& aAuiMgr );

/// Stop the chat worker and the MCP server of an editor frame (call from its destructor).
void ShutdownClaudeChatPane( wxAuiManager& aAuiMgr );

#endif // CLAUDE_CHAT_PANEL_H
